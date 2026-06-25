/* Leno GUI - Windows 平台实现
 * 使用 Win32 API + GDI 双缓冲
 *
 * 窗口管理: CreateWindowExW + 自定义窗口过程
 * 渲染: DIB Section 后备缓冲区 + BitBlt
 * 事件: PeekMessage 轮询 + 全局事件队列
 */

#include "leno_guis.h"

#ifdef _WIN32

#include <windows.h>
#include <imm.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ===== 事件队列（借鉴 SDL3：链表 + 对象池）===== */

typedef struct LenoGUIEventEntry {
    LenoGUIEvent event;
    struct LenoGUIEventEntry* next;
} LenoGUIEventEntry;

static LenoGUIEventEntry* g_event_queue_head = NULL;
static LenoGUIEventEntry* g_event_queue_tail = NULL;
static int g_event_count = 0;
static int g_event_max_seen = 0;

/* 对象池：空闲事件节点 */
static LenoGUIEventEntry* g_event_free_list = NULL;

static CRITICAL_SECTION g_event_cs;
static int g_cs_initialized = 0;

/* 文件对话框结果（通过事件队列从工作线程投递到主线程） */
typedef struct {
    char** files;
    int nfiles;
    int filter_index;
} FileDialogResult;

static FileDialogResult* g_filedlg_result = NULL;
static CRITICAL_SECTION g_filedlg_result_cs;

/* 事件过滤器（借鉴 SDL3） */
typedef struct LenoGUIEventFilter {
    void* userdata;
    int (*filter)(void* userdata, LenoGUIEvent* event);
    struct LenoGUIEventFilter* next;
} LenoGUIEventFilter;

static LenoGUIEventFilter* g_event_filters = NULL;

static void event_queue_init(void) {
    if (!g_cs_initialized) {
        InitializeCriticalSection(&g_event_cs);
        g_cs_initialized = 1;
    }
    g_event_queue_head = NULL;
    g_event_queue_tail = NULL;
    g_event_count = 0;
    g_event_max_seen = 0;
    g_event_free_list = NULL;
    g_event_filters = NULL;
}

/* 从对象池获取或分配新节点 */
static LenoGUIEventEntry* event_entry_alloc(void) {
    EnterCriticalSection(&g_event_cs);
    LenoGUIEventEntry* entry = g_event_free_list;
    if (entry) {
        g_event_free_list = entry->next;
        memset(entry, 0, sizeof(LenoGUIEventEntry));
    }
    LeaveCriticalSection(&g_event_cs);
    
    if (!entry) {
        entry = (LenoGUIEventEntry*)calloc(1, sizeof(LenoGUIEventEntry));
    }
    return entry;
}

/* 回收节点到对象池 */
static void event_entry_free(LenoGUIEventEntry* entry) {
    if (!entry) return;
    EnterCriticalSection(&g_event_cs);
    entry->next = g_event_free_list;
    g_event_free_list = entry;
    LeaveCriticalSection(&g_event_cs);
}

/* 添加事件过滤器 */
void leno_gui_platform_add_event_filter(void* userdata, int (*filter)(void*, LenoGUIEvent*)) {
    if (!filter) return;
    LenoGUIEventFilter* f = (LenoGUIEventFilter*)malloc(sizeof(LenoGUIEventFilter));
    if (!f) return;
    f->userdata = userdata;
    f->filter = filter;
    EnterCriticalSection(&g_event_cs);
    f->next = g_event_filters;
    g_event_filters = f;
    LeaveCriticalSection(&g_event_cs);
}

/* 移除事件过滤器 */
void leno_gui_platform_remove_event_filter(void* userdata, int (*filter)(void*, LenoGUIEvent*)) {
    EnterCriticalSection(&g_event_cs);
    LenoGUIEventFilter** current = &g_event_filters;
    while (*current) {
        LenoGUIEventFilter* f = *current;
        if (f->userdata == userdata && f->filter == filter) {
            *current = f->next;
            free(f);
            break;
        }
        current = &f->next;
    }
    LeaveCriticalSection(&g_event_cs);
}

static void event_queue_push(const LenoGUIEvent* ev) {
    /* 运行事件过滤器 */
    LenoGUIEvent filtered = *ev;
    LenoGUIEventFilter* f = g_event_filters;
    while (f) {
        if (f->filter && !f->filter(f->userdata, &filtered)) {
            return; /* 过滤器阻止事件 */
        }
        f = f->next;
    }
    
    LenoGUIEventEntry* entry = event_entry_alloc();
    if (!entry) return;
    
    entry->event = filtered;
    entry->next = NULL;
    
    EnterCriticalSection(&g_event_cs);
    if (g_event_queue_tail) {
        g_event_queue_tail->next = entry;
    } else {
        g_event_queue_head = entry;
    }
    g_event_queue_tail = entry;
    g_event_count++;
    if (g_event_count > g_event_max_seen) {
        g_event_max_seen = g_event_count;
    }
    LeaveCriticalSection(&g_event_cs);
}

static int event_queue_pop(LenoGUIEvent* ev) {
    EnterCriticalSection(&g_event_cs);
    LenoGUIEventEntry* entry = g_event_queue_head;
    if (!entry) {
        LeaveCriticalSection(&g_event_cs);
        return 0;
    }
    
    g_event_queue_head = entry->next;
    if (!g_event_queue_head) {
        g_event_queue_tail = NULL;
    }
    g_event_count--;
    LeaveCriticalSection(&g_event_cs);
    
    *ev = entry->event;
    event_entry_free(entry);
    return 1;
}

/* 清理事件队列和对象池 */
static void event_queue_cleanup(void) {
    EnterCriticalSection(&g_event_cs);
    
    /* 清理队列 */
    LenoGUIEventEntry* entry = g_event_queue_head;
    while (entry) {
        LenoGUIEventEntry* next = entry->next;
        free(entry);
        entry = next;
    }
    g_event_queue_head = NULL;
    g_event_queue_tail = NULL;
    
    /* 清理对象池 */
    entry = g_event_free_list;
    while (entry) {
        LenoGUIEventEntry* next = entry->next;
        free(entry);
        entry = next;
    }
    g_event_free_list = NULL;
    
    /* 清理过滤器 */
    LenoGUIEventFilter* f = g_event_filters;
    while (f) {
        LenoGUIEventFilter* next = f->next;
        free(f);
        f = next;
    }
    g_event_filters = NULL;
    
    LeaveCriticalSection(&g_event_cs);
}

/* ===== 窗口计数器（用于 window_id） ===== */

static int g_next_window_id = 1;

/* ===== 平台窗口结构 ===== */

struct LenoGUIPlatformWindow {
    HWND hwnd;
    int window_id;
    int width;
    int height;
    int should_close;
    int is_fullscreen;
    DWORD saved_style;
    RECT saved_rect;
    LenoGUIPlatformRenderer* renderer;  /* 关联的渲染器，用于 WM_PAINT 直接渲染 */
    int is_borderless;                  /* 是否无边框窗口 */
    int drag_area_enabled;              /* 是否启用拖动区域限制 */
    RECT drag_area;                     /* 可拖动区域（客户区坐标） */
    int drag_drop_enabled;              /* 是否启用拖放（参考 SDL3） */
    int min_w;                          /* 最小宽度 */
    int min_h;                          /* 最小高度 */
    int max_w;                          /* 最大宽度 */
    int max_h;                          /* 最大高度 */
    int no_maximize;                    /* 禁止最大化（移除按钮+阻止行为） */
    HCURSOR custom_cursor;              /* 自定义光标句柄 */
    /* 键盘状态跟踪（参考 SDL3 prev/curr 按键状态） */
    uint8_t prev_keys[256];             /* 上一帧按键状态 */
    uint8_t curr_keys[256];             /* 当前帧按键状态 */
    int key_states_valid;               /* 按键状态是否有效（已执行过 update） */
};

/* ===== 平台渲染器结构 ===== */

struct LenoGUIPlatformRenderer {
    LenoGUIPlatformWindow* window;
    HDC back_dc;
    HBITMAP back_bitmap;
    HBITMAP old_bitmap;
    uint32_t* pixels;
    int width;
    int height;
    int pitch;         /* 每行字节数（借鉴 SDL3 surface pitch） */
    uint8_t draw_r;
    uint8_t draw_g;
    uint8_t draw_b;
    uint8_t draw_a;
    int vp_x, vp_y, vp_w, vp_h;
    int clip_x, clip_y, clip_w, clip_h;
    int clip_enabled;
    int needs_resize;  /* 窗口大小变化标志，参考 SDL3 surface_valid */
    
    /* 逻辑呈现模式（借鉴 SDL3） */
    int logical_w;     /* 逻辑宽度 */
    int logical_h;     /* 逻辑高度 */
    int logical_mode;  /* 呈现模式 */
    float scale_x;     /* X 轴缩放 */
    float scale_y;     /* Y 轴缩放 */
    int logical_enabled; /* 是否启用逻辑大小 */
};

/* ===== 平台图像结构 ===== */

struct LenoGUIPlatformImage {
    uint32_t* pixels;
    int width;
    int height;
    int pitch;
    int access;           /* 访问模式: STATIC/STREAMING/TARGET */
    /* 渲染目标支持 */
    HDC target_dc;        /* 渲染目标 DC */
    HBITMAP target_bitmap;
    HBITMAP old_bitmap;
};

/* 渲染目标定义在 swrender.c */

#include "leno_guis_swrender.c"
#include "leno_guis_image.c"

/* ===== 文本输入控制（参考 SDL_StartTextInput） ===== */
static int g_text_input_active = 0;

/* ===== IME 状态（参考 SDL3 Windows IME） ===== */
static int g_ime_composing = 0;
static int g_ime_caret_x = 0;
static int g_ime_caret_y = 0;
#define IME_RESULT_BUF 16
static WCHAR g_ime_result_chars[IME_RESULT_BUF] = { 0 };
static int g_ime_result_count = 0;

/* ===== 窗口类注册 ===== */

static const wchar_t* LENO_GUI_CLASS = L"LenoGUIWindow";
static int g_class_registered = 0;
static int g_gui_initialized = 0;

/* ===== 光标管理（参考 SDL3） ===== */
static HCURSOR g_system_cursors[LENO_GUI_CURSOR_COUNT] = { NULL };
static HCURSOR g_custom_cursor = NULL;
static int g_custom_cursor_id = 0;
static int g_current_system_cursor = LENO_GUI_CURSOR_DEFAULT;

/* 初始化系统光标表 */
static void init_system_cursors(void) {
    if (g_system_cursors[LENO_GUI_CURSOR_DEFAULT]) return; /* 已初始化 */
    g_system_cursors[LENO_GUI_CURSOR_DEFAULT]    = LoadCursorW(NULL, (LPCWSTR)(ULONG_PTR)IDC_ARROW);
    g_system_cursors[LENO_GUI_CURSOR_TEXT]       = LoadCursorW(NULL, (LPCWSTR)(ULONG_PTR)IDC_IBEAM);
    g_system_cursors[LENO_GUI_CURSOR_WAIT]       = LoadCursorW(NULL, (LPCWSTR)(ULONG_PTR)IDC_WAIT);
    g_system_cursors[LENO_GUI_CURSOR_CROSSHAIR]  = LoadCursorW(NULL, (LPCWSTR)(ULONG_PTR)IDC_CROSS);
    g_system_cursors[LENO_GUI_CURSOR_PROGRESS]   = LoadCursorW(NULL, (LPCWSTR)(ULONG_PTR)IDC_APPSTARTING);
    g_system_cursors[LENO_GUI_CURSOR_RESIZE_NWSE]= LoadCursorW(NULL, (LPCWSTR)(ULONG_PTR)IDC_SIZENWSE);
    g_system_cursors[LENO_GUI_CURSOR_RESIZE_NESW]= LoadCursorW(NULL, (LPCWSTR)(ULONG_PTR)IDC_SIZENESW);
    g_system_cursors[LENO_GUI_CURSOR_RESIZE_EW]  = LoadCursorW(NULL, (LPCWSTR)(ULONG_PTR)IDC_SIZEWE);
    g_system_cursors[LENO_GUI_CURSOR_RESIZE_NS]  = LoadCursorW(NULL, (LPCWSTR)(ULONG_PTR)IDC_SIZENS);
    g_system_cursors[LENO_GUI_CURSOR_MOVE]       = LoadCursorW(NULL, (LPCWSTR)(ULONG_PTR)IDC_SIZEALL);
    g_system_cursors[LENO_GUI_CURSOR_NOT_ALLOWED]= LoadCursorW(NULL, (LPCWSTR)(ULONG_PTR)IDC_NO);
    g_system_cursors[LENO_GUI_CURSOR_POINTER]    = LoadCursorW(NULL, (LPCWSTR)(ULONG_PTR)IDC_HAND);
}

/* ===== Win32 虚拟键码到 Leno 键码映射 ===== */

static int vk_to_leno_key(WPARAM vk) {
    if (vk >= 'A' && vk <= 'Z') return (int)vk;
    if (vk >= '0' && vk <= '9') return (int)vk;
    switch (vk) {
        case VK_RETURN:    return LENO_GUI_KEY_RETURN;
        case VK_ESCAPE:    return LENO_GUI_KEY_ESCAPE;
        case VK_BACK:      return LENO_GUI_KEY_BACKSPACE;
        case VK_TAB:       return LENO_GUI_KEY_TAB;
        case VK_SPACE:     return LENO_GUI_KEY_SPACE;
        case VK_DELETE:    return LENO_GUI_KEY_DELETE;
        case VK_LEFT:      return LENO_GUI_KEY_LEFT;
        case VK_RIGHT:     return LENO_GUI_KEY_RIGHT;
        case VK_UP:        return LENO_GUI_KEY_UP;
        case VK_DOWN:      return LENO_GUI_KEY_DOWN;
        case VK_INSERT:    return LENO_GUI_KEY_INSERT;
        case VK_HOME:      return LENO_GUI_KEY_HOME;
        case VK_END:       return LENO_GUI_KEY_END;
        case VK_PRIOR:     return LENO_GUI_KEY_PAGEUP;
        case VK_NEXT:      return LENO_GUI_KEY_PAGEDOWN;
        case VK_F1:        return LENO_GUI_KEY_F1;
        case VK_F2:        return LENO_GUI_KEY_F2;
        case VK_F3:        return LENO_GUI_KEY_F3;
        case VK_F4:        return LENO_GUI_KEY_F4;
        case VK_F5:        return LENO_GUI_KEY_F5;
        case VK_F6:        return LENO_GUI_KEY_F6;
        case VK_F7:        return LENO_GUI_KEY_F7;
        case VK_F8:        return LENO_GUI_KEY_F8;
        case VK_F9:        return LENO_GUI_KEY_F9;
        case VK_F10:       return LENO_GUI_KEY_F10;
        case VK_F11:       return LENO_GUI_KEY_F11;
        case VK_F12:       return LENO_GUI_KEY_F12;
        case VK_LSHIFT:    return LENO_GUI_KEY_LSHIFT;
        case VK_RSHIFT:    return LENO_GUI_KEY_RSHIFT;
        case VK_LCONTROL:  return LENO_GUI_KEY_LCTRL;
        case VK_RCONTROL:  return LENO_GUI_KEY_RCTRL;
        case VK_LMENU:     return LENO_GUI_KEY_LALT;
        case VK_RMENU:     return LENO_GUI_KEY_RALT;
        case VK_CAPITAL:   return LENO_GUI_KEY_CAPSLOCK;
        case VK_NUMLOCK:   return LENO_GUI_KEY_NUMLOCK;
        default:           return LENO_GUI_KEY_UNKNOWN;
    }
}

static int get_mod_flags(void) {
    int mod = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000)   mod |= LENO_GUI_MOD_SHIFT;
    if (GetKeyState(VK_CONTROL) & 0x8000) mod |= LENO_GUI_MOD_CTRL;
    if (GetKeyState(VK_MENU) & 0x8000)    mod |= LENO_GUI_MOD_ALT;
    if (GetKeyState(VK_LWIN) & 0x8000 || GetKeyState(VK_RWIN) & 0x8000)
        mod |= LENO_GUI_MOD_SUPER;
    return mod;
}

/* ===== 事件发送辅助函数（借鉴 SDL3，减少重复代码）===== */

static void push_window_event(LenoGUIPlatformWindow* win, int type, int data1, int data2) {
    LenoGUIEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.timestamp = GetTickCount64();
    ev.window_id = win ? win->window_id : 0;
    ev.data1 = data1;
    ev.data2 = data2;
    event_queue_push(&ev);
}

static void push_key_event(LenoGUIPlatformWindow* win, int type, int keycode, int scancode, int mod, int repeat) {
    LenoGUIEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.timestamp = GetTickCount64();
    ev.window_id = win ? win->window_id : 0;
    ev.key = keycode;
    ev.scancode = scancode;
    ev.mod_flags = mod;
    ev.repeat = repeat;
    event_queue_push(&ev);
}

static void push_mouse_motion_event(LenoGUIPlatformWindow* win, float x, float y, float xrel, float yrel) {
    LenoGUIEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = LENO_GUI_EVT_MOUSE_MOVE;
    ev.timestamp = GetTickCount64();
    ev.window_id = win ? win->window_id : 0;
    ev.mouse_x = x;
    ev.mouse_y = y;
    ev.mouse_xrel = xrel;
    ev.mouse_yrel = yrel;
    event_queue_push(&ev);
}

static void push_mouse_button_event(LenoGUIPlatformWindow* win, int type, int button, int clicks, float x, float y) {
    LenoGUIEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.timestamp = GetTickCount64();
    ev.window_id = win ? win->window_id : 0;
    ev.mouse_x = x;
    ev.mouse_y = y;
    ev.mouse_button = button;
    ev.mouse_clicks = clicks;
    event_queue_push(&ev);
}

static void push_mouse_wheel_event(LenoGUIPlatformWindow* win, float x, float y, float mouse_x, float mouse_y) {
    LenoGUIEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = LENO_GUI_EVT_MOUSE_WHEEL;
    ev.timestamp = GetTickCount64();
    ev.window_id = win ? win->window_id : 0;
    ev.wheel_x = x;
    ev.wheel_y = y;
    ev.mouse_x = mouse_x;
    ev.mouse_y = mouse_y;
    event_queue_push(&ev);
}

/* ===== 窗口过程 ===== */

static LRESULT CALLBACK leno_gui_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    LenoGUIPlatformWindow* win = (LenoGUIPlatformWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_CLOSE: {
            if (win) win->should_close = 1;
            push_window_event(win, LENO_GUI_EVT_WINDOW_CLOSE, 0, 0);
            return 0;
        }
        case WM_DESTROY: {
            if (win) win->should_close = 1;
            push_window_event(win, LENO_GUI_EVT_QUIT, 0, 0);
            return 0;
        }
        case WM_SIZE: {
            if (win) {
                win->width = LOWORD(lparam);
                win->height = HIWORD(lparam);
            }
            if (wparam == SIZE_MINIMIZED) {
                push_window_event(win, LENO_GUI_EVT_WINDOW_MINIMIZED, (int)LOWORD(lparam), (int)HIWORD(lparam));
            } else if (wparam == SIZE_MAXIMIZED) {
                push_window_event(win, LENO_GUI_EVT_WINDOW_MAXIMIZED, (int)LOWORD(lparam), (int)HIWORD(lparam));
            } else if (wparam == SIZE_RESTORED) {
                push_window_event(win, LENO_GUI_EVT_WINDOW_RESTORED, (int)LOWORD(lparam), (int)HIWORD(lparam));
            } else {
                push_window_event(win, LENO_GUI_EVT_WINDOW_RESIZE, (int)LOWORD(lparam), (int)HIWORD(lparam));
            }
            return 0;
        }
        case WM_MOVE: {
            push_window_event(win, LENO_GUI_EVT_WINDOW_MOVE, (int)(short)LOWORD(lparam), (int)(short)HIWORD(lparam));
            return 0;
        }
        case WM_SETFOCUS: {
            push_window_event(win, LENO_GUI_EVT_WINDOW_FOCUS, 0, 0);
            return 0;
        }
        case WM_KILLFOCUS: {
            push_window_event(win, LENO_GUI_EVT_WINDOW_UNFOCUS, 0, 0);
            return 0;
        }
        case WM_ERASEBKGND: {
            /* 禁止 Windows 擦除背景，避免闪烁 */
            return 1;
        }
        case WM_PAINT: {
            /* 所有绘制由 render_present 处理，WM_PAINT 只需验证区域 */
            /* 这样可以避免 WM_PAINT 和 render_present 之间的绘制冲突 */
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ENTERSIZEMOVE: {
            /* 进入模态循环（拖动/调整大小） */
            /* 启动定时器保持内容更新（参考 SDL3） */
            SetTimer(hwnd, 1, 33, NULL);  /* 约 30 FPS */
            return 0;
        }
        case WM_SIZING: {
            /* 调整大小过程中：只更新窗口大小，不调整渲染器 */
            if (win) {
                RECT clientRect;
                GetClientRect(hwnd, &clientRect);
                win->width = clientRect.right - clientRect.left;
                win->height = clientRect.bottom - clientRect.top;
            }
            /* 让 Windows 继续处理默认行为 */
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }
        case WM_EXITSIZEMOVE: {
            /* 退出模态循环，停止定时器 */
            KillTimer(hwnd, 1);
            /* 拖动结束，标记渲染器需要调整大小 */
            if (win && win->renderer) {
                win->renderer->needs_resize = 1;
            }
            /* 发送 resize 事件，让应用程序知道窗口大小变化了 */
            if (win) {
                RECT clientRect;
                GetClientRect(hwnd, &clientRect);
                push_window_event(win, LENO_GUI_EVT_WINDOW_RESIZE, 
                                  clientRect.right - clientRect.left,
                                  clientRect.bottom - clientRect.top);
            }
            return 0;
        }
        case WM_TIMER: {
            /* 模态循环中的定时器，调用主循环回调（参考 SDL3 SDL_OnWindowLiveResizeUpdate） */
            if (wparam == 1) {
                /* 直接调用主循环回调，让应用程序重新绘制 */
                leno_gui_platform_iterate_main_callbacks();
            }
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            push_key_event(win, LENO_GUI_EVT_KEY_DOWN, vk_to_leno_key(wparam),
                           (int)((lparam >> 16) & 0xFF), get_mod_flags(), (lparam >> 30) & 1);
            return 0;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            push_key_event(win, LENO_GUI_EVT_KEY_UP, vk_to_leno_key(wparam),
                           (int)((lparam >> 16) & 0xFF), get_mod_flags(), 0);
            return 0;
        }
        case WM_CHAR: {
            /* 参考 SDL3：IME 合成期间由 WM_IME_COMPOSITION/GCS_RESULTSTR 负责输入，
               忽略 WM_CHAR 避免重复或插入拼音字符 */
            if (g_ime_composing) {
                return 0;
            }
            /* 抑制 IME 确认结果后紧跟的重复 WM_CHAR（微软拼音等会再发一次） */
            if (g_ime_result_count > 0) {
                int i;
                for (i = 0; i < g_ime_result_count; i++) {
                    if (g_ime_result_chars[i] == (WCHAR)wparam) {
                        g_ime_result_chars[i] = g_ime_result_chars[--g_ime_result_count];
                        return 0;
                    }
                }
            }
            /* 非合成字符输入：英文、数字、符号等 */
            if (wparam >= 0x20 || wparam == '\t') {
                LenoGUIEvent ev;
                memset(&ev, 0, sizeof(ev));
                ev.type = LENO_GUI_EVT_TEXT_INPUT;
                ev.timestamp = GetTickCount64();
                ev.window_id = win ? win->window_id : 0;
                WCHAR wc = (WCHAR)wparam;
                int len = WideCharToMultiByte(CP_UTF8, 0, &wc, 1, ev.text, sizeof(ev.text) - 1, NULL, NULL);
                ev.text[len > 0 ? len : 0] = '\0';
                event_queue_push(&ev);
            }
            return 0;
        }
        /* ===== IME 中文输入法支持（参考 SDL3）===== */
        case WM_IME_SETCONTEXT:
            /* 当前未实现自绘候选窗，保留系统默认 IME UI 以便用户看到输入法 */
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        case WM_IME_STARTCOMPOSITION: {
            g_ime_composing = 1;
            HIMC himc = ImmGetContext(hwnd);
            if (himc) {
                COMPOSITIONFORM cf;
                memset(&cf, 0, sizeof(cf));
                cf.dwStyle = CFS_POINT;
                cf.ptCurrentPos.x = g_ime_caret_x;
                cf.ptCurrentPos.y = g_ime_caret_y;
                ImmSetCompositionWindow(himc, &cf);
                ImmReleaseContext(hwnd, himc);
            }
            break;
        }
        case WM_IME_COMPOSITION: {
            HIMC himc = ImmGetContext(hwnd);
            if (!himc) break;
            /* GCS_RESULTSTR：输入法已确认的最终中文字符串 */
            if (wparam & GCS_RESULTSTR) {
                LONG len = ImmGetCompositionStringW(himc, GCS_RESULTSTR, NULL, 0);
                if (len > 0) {
                    int nchars = len / sizeof(WCHAR);
                    WCHAR* wstr = (WCHAR*)malloc((nchars + 1) * sizeof(WCHAR));
                    if (wstr) {
                        LONG got = ImmGetCompositionStringW(himc, GCS_RESULTSTR, wstr, len);
                        if (got == len) {
                            wstr[nchars] = L'\0';
                            /* 记录 IME 结果字符，用于抑制紧跟的重复 WM_CHAR */
                            g_ime_result_count = 0;
                            int k;
                            for (k = 0; k < nchars && k < IME_RESULT_BUF; k++) {
                                g_ime_result_chars[g_ime_result_count++] = wstr[k];
                            }
                            LenoGUIEvent ev;
                            memset(&ev, 0, sizeof(ev));
                            ev.type = LENO_GUI_EVT_TEXT_INPUT;
                            ev.timestamp = GetTickCount64();
                            ev.window_id = win ? win->window_id : 0;
                            int ul = WideCharToMultiByte(CP_UTF8, 0, wstr, -1,
                                ev.text, sizeof(ev.text) - 1, NULL, NULL);
                            if (ul > 0) {
                                ev.text[ul - 1] = '\0';
                                event_queue_push(&ev);
                            }
                        }
                        free(wstr);
                    }
                }
            }
            ImmReleaseContext(hwnd, himc);
            break;
        }
        case WM_IME_ENDCOMPOSITION:
            g_ime_composing = 0;
            break;
        case WM_MOUSEMOVE: {
            float x = (float)(short)LOWORD(lparam);
            float y = (float)(short)HIWORD(lparam);
            static float last_x = 0, last_y = 0;
            float xrel = x - last_x;
            float yrel = y - last_y;
            last_x = x;
            last_y = y;
            push_mouse_motion_event(win, x, y, xrel, yrel);
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_RBUTTONDOWN: {
            float x = (float)(short)LOWORD(lparam);
            float y = (float)(short)HIWORD(lparam);
            int button;
            if (msg == WM_LBUTTONDOWN)      button = LENO_GUI_MOUSE_LEFT;
            else if (msg == WM_MBUTTONDOWN) button = LENO_GUI_MOUSE_MIDDLE;
            else                            button = LENO_GUI_MOUSE_RIGHT;
            /* 单击：clicks = 1 */
            push_mouse_button_event(win, LENO_GUI_EVT_MOUSE_DOWN, button, 1, x, y);
            /* 无边框窗口拖动支持：左键在可拖动区域时启动拖动 */
            if (win && win->is_borderless && msg == WM_LBUTTONDOWN) {
                int mx = (short)LOWORD(lparam);
                int my = (short)HIWORD(lparam);
                int can_drag = 0;
                if (win->drag_area_enabled) {
                    if (mx >= win->drag_area.left && mx < win->drag_area.right &&
                        my >= win->drag_area.top && my < win->drag_area.bottom) {
                        can_drag = 1;
                    }
                } else {
                    can_drag = 1;
                }
                if (can_drag) {
                    ReleaseCapture();
                    SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                }
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK:
        case WM_MBUTTONDBLCLK:
        case WM_RBUTTONDBLCLK: {
            float x = (float)(short)LOWORD(lparam);
            float y = (float)(short)HIWORD(lparam);
            int button;
            if (msg == WM_LBUTTONDBLCLK)      button = LENO_GUI_MOUSE_LEFT;
            else if (msg == WM_MBUTTONDBLCLK) button = LENO_GUI_MOUSE_MIDDLE;
            else                              button = LENO_GUI_MOUSE_RIGHT;
            /* 双击：clicks = 2 */
            push_mouse_button_event(win, LENO_GUI_EVT_MOUSE_DOWN, button, 2, x, y);
            return 0;
        }
        case WM_LBUTTONUP:
        case WM_MBUTTONUP:
        case WM_RBUTTONUP: {
            float x = (float)(short)LOWORD(lparam);
            float y = (float)(short)HIWORD(lparam);
            int button;
            if (msg == WM_LBUTTONUP)        button = LENO_GUI_MOUSE_LEFT;
            else if (msg == WM_MBUTTONUP)   button = LENO_GUI_MOUSE_MIDDLE;
            else                            button = LENO_GUI_MOUSE_RIGHT;
            push_mouse_button_event(win, LENO_GUI_EVT_MOUSE_UP, button, 0, x, y);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            POINT pt;
            pt.x = (short)LOWORD(lparam);
            pt.y = (short)HIWORD(lparam);
            ScreenToClient(hwnd, &pt);
            short delta = GET_WHEEL_DELTA_WPARAM(wparam);
            push_mouse_wheel_event(win, 0.0f, (float)delta / WHEEL_DELTA, (float)pt.x, (float)pt.y);
            return 0;
        }
        case WM_MOUSEHWHEEL: {
            POINT pt;
            pt.x = (short)LOWORD(lparam);
            pt.y = (short)HIWORD(lparam);
            ScreenToClient(hwnd, &pt);
            short delta = GET_WHEEL_DELTA_WPARAM(wparam);
            push_mouse_wheel_event(win, (float)delta / WHEEL_DELTA, 0.0f, (float)pt.x, (float)pt.y);
            return 0;
        }
        case WM_SHOWWINDOW: {
            push_window_event(win, wparam ? LENO_GUI_EVT_WINDOW_SHOW : LENO_GUI_EVT_WINDOW_HIDE, 0, 0);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            if (win) {
                MINMAXINFO* mmi = (MINMAXINFO*)lparam;
                if (win->min_w > 0) mmi->ptMinTrackSize.x = win->min_w;
                if (win->min_h > 0) mmi->ptMinTrackSize.y = win->min_h;
                if (win->max_w > 0) mmi->ptMaxTrackSize.x = win->max_w;
                if (win->max_h > 0) mmi->ptMaxTrackSize.y = win->max_h;
                if (win->no_maximize) {
                    /* 阻止最大化：将最大化尺寸限制为当前窗口尺寸 */
                    RECT wr;
                    if (GetWindowRect(win->hwnd, &wr)) {
                        mmi->ptMaxSize.x = wr.right - wr.left;
                        mmi->ptMaxSize.y = wr.bottom - wr.top;
                    }
                }
            }
            return 0;
        }
        case WM_DROPFILES: {
            /* 文件拖放处理（参考 SDL3 WM_DROPFILES） */
            if (win) {
                HDROP drop = (HDROP)wparam;
                UINT count = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
                LenoGUIEvent ev;
                memset(&ev, 0, sizeof(ev));
                ev.type = LENO_GUI_EVT_DROP_BEGIN;
                ev.timestamp = GetTickCount64();
                ev.window_id = win->window_id;
                event_queue_push(&ev);

                for (UINT i = 0; i < count; i++) {
                    UINT size = DragQueryFileW(drop, i, NULL, 0) + 1;
                    wchar_t* buffer = (wchar_t*)malloc(size * sizeof(wchar_t));
                    if (buffer) {
                        if (DragQueryFileW(drop, i, buffer, size)) {
                            memset(&ev, 0, sizeof(ev));
                            ev.type = LENO_GUI_EVT_DROP_FILE;
                            ev.timestamp = GetTickCount64();
                            ev.window_id = win->window_id;
                            WideCharToMultiByte(CP_UTF8, 0, buffer, -1,
                                                ev.drop_file, sizeof(ev.drop_file), NULL, NULL);
                            ev.drop_file[sizeof(ev.drop_file) - 1] = '\0';
                            event_queue_push(&ev);
                        }
                        free(buffer);
                    }
                }
                memset(&ev, 0, sizeof(ev));
                ev.type = LENO_GUI_EVT_DROP_COMPLETE;
                ev.timestamp = GetTickCount64();
                ev.window_id = win->window_id;
                event_queue_push(&ev);

                DragFinish(drop);
            }
            return 0;
        }
        case WM_SETCURSOR: {
            /* 光标设置（参考 SDL3） */
            if (LOWORD(lparam) == HTCLIENT) {
                if (g_custom_cursor) {
                    SetCursor(g_custom_cursor);
                    return TRUE;
                }
                /* 使用当前系统光标 */
                init_system_cursors();
                if (g_system_cursors[g_current_system_cursor]) {
                    SetCursor(g_system_cursors[g_current_system_cursor]);
                    return TRUE;
                }
                return DefWindowProcW(hwnd, msg, wparam, lparam);
            }
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }
        default:
            return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/* ===== DIB Section 创建 ===== */

static int create_dib_section(int w, int h, HDC* out_dc, HBITMAP* out_bitmap,
                              HBITMAP* out_old_bitmap, uint32_t** out_pixels) {
    HDC screen_dc = GetDC(NULL);
    *out_dc = CreateCompatibleDC(screen_dc);
    ReleaseDC(NULL, screen_dc);
    if (!*out_dc) return 0;

    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    *out_bitmap = CreateDIBSection(*out_dc, &bmi, DIB_RGB_COLORS, (void**)out_pixels, NULL, 0);
    if (!*out_bitmap) {
        DeleteDC(*out_dc);
        *out_dc = NULL;
        return 0;
    }

    *out_old_bitmap = (HBITMAP)SelectObject(*out_dc, *out_bitmap);
    return 1;
}

static void destroy_dib_section(HDC* dc, HBITMAP* bitmap, HBITMAP* old_bitmap) {
    if (*dc) {
        if (*old_bitmap) SelectObject(*dc, *old_bitmap);
        if (*bitmap) DeleteObject(*bitmap);
        DeleteDC(*dc);
        *dc = NULL;
        *bitmap = NULL;
        *old_bitmap = NULL;
    }
}

/* ===== 平台 API 实现 ===== */

int leno_gui_platform_init(void) {
    if (g_gui_initialized) return 1;

    /* 提升系统定时器精度到 1ms，使 Sleep(1) 真正只睡 ~1ms */
    timeBeginPeriod(1);

    event_queue_init();
    InitializeCriticalSection(&g_filedlg_result_cs);

    if (!g_class_registered) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = leno_gui_wndproc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hIcon = LoadIconW(NULL, (LPCWSTR)(ULONG_PTR)IDI_APPLICATION);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)(ULONG_PTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = LENO_GUI_CLASS;
        if (!RegisterClassExW(&wc)) return 0;
        g_class_registered = 1;
    }

    g_gui_initialized = 1;
    return 1;
}

void leno_gui_platform_quit(void) {
    if (!g_gui_initialized) return;
    g_gui_initialized = 0;
    
    /* 恢复系统定时器默认精度 */
    timeEndPeriod(1);

    /* 清理事件队列和对象池 */
    event_queue_cleanup();
    
    if (g_cs_initialized) {
        DeleteCriticalSection(&g_event_cs);
        g_cs_initialized = 0;
    }
    
    /* 清理文件对话框结果 */
    EnterCriticalSection(&g_filedlg_result_cs);
    if (g_filedlg_result) {
        if (g_filedlg_result->files) {
            for (int i = 0; i < g_filedlg_result->nfiles; i++) {
                free(g_filedlg_result->files[i]);
            }
            free(g_filedlg_result->files);
        }
        free(g_filedlg_result);
        g_filedlg_result = NULL;
    }
    LeaveCriticalSection(&g_filedlg_result_cs);
    DeleteCriticalSection(&g_filedlg_result_cs);
}

LenoGUIPlatformWindow* leno_gui_platform_create_window(const char* title, int w, int h, int flags) {
    if (!g_gui_initialized) return NULL;

    LenoGUIPlatformWindow* win = (LenoGUIPlatformWindow*)calloc(1, sizeof(LenoGUIPlatformWindow));
    if (!win) return NULL;

    win->window_id = g_next_window_id++;
    win->width = w;
    win->height = h;
    win->should_close = 0;
    win->is_fullscreen = 0;
    win->is_borderless = (flags & LENO_GUI_WIN_BORDERLESS) ? 1 : 0;
    win->drag_area_enabled = 0;
    win->drag_drop_enabled = 0;
    win->min_w = 0;
    win->min_h = 0;
    win->max_w = 0;
    win->max_h = 0;
    win->no_maximize = (flags & LENO_GUI_WIN_NO_MAXIMIZE) ? 1 : 0;
    win->custom_cursor = NULL;

    DWORD style;
    if (flags & LENO_GUI_WIN_BORDERLESS) {
        style = WS_POPUP;
        if (flags & LENO_GUI_WIN_RESIZABLE) {
            style |= WS_THICKFRAME;
        }
    } else {
        /* 显式组装窗口样式，避免 WS_OVERLAPPEDWINDOW 宏的隐式行为 */
        style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        if (flags & LENO_GUI_WIN_RESIZABLE) {
            style |= WS_THICKFRAME;
        }
        if (!(flags & LENO_GUI_WIN_NO_MAXIMIZE)) {
            style |= WS_MAXIMIZEBOX;
        }
    }
    if (flags & LENO_GUI_WIN_MINIMIZED) style |= WS_MINIMIZE;
    if (flags & LENO_GUI_WIN_MAXIMIZED) style |= WS_MAXIMIZE;
    if (flags & LENO_GUI_WIN_HIDDEN) style &= ~WS_VISIBLE;
    else style |= WS_VISIBLE;

    RECT rect = { 0, 0, w, h };
    AdjustWindowRect(&rect, style, FALSE);

    wchar_t* wtitle = NULL;
    int wtitle_len = MultiByteToWideChar(CP_UTF8, 0, title, -1, NULL, 0);
    if (wtitle_len > 0) {
        wtitle = (wchar_t*)malloc(wtitle_len * sizeof(wchar_t));
        MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, wtitle_len);
    }

    win->hwnd = CreateWindowExW(
        0,
        LENO_GUI_CLASS,
        wtitle ? wtitle : L"Leno GUI",
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        NULL, NULL, GetModuleHandleW(NULL), NULL
    );

    free(wtitle);

    if (!win->hwnd) {
        free(win);
        return NULL;
    }

    SetWindowLongPtrW(win->hwnd, GWLP_USERDATA, (LONG_PTR)win);

    /* 如果禁用最大化，需强制刷新窗口框架以生效 */
    if (flags & LENO_GUI_WIN_NO_MAXIMIZE) {
        SetWindowPos(win->hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }

    if (flags & LENO_GUI_WIN_ALWAYS_ON_TOP) {
        SetWindowPos(win->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }

    return win;
}

void leno_gui_platform_destroy_window(LenoGUIPlatformWindow* win) {
    if (!win) return;
    if (win->hwnd) {
        SetWindowLongPtrW(win->hwnd, GWLP_USERDATA, 0);
        DestroyWindow(win->hwnd);
        win->hwnd = NULL;
    }
    free(win);
}

void leno_gui_platform_show_window(LenoGUIPlatformWindow* win) {
    if (!win || !win->hwnd) return;
    ShowWindow(win->hwnd, SW_SHOW);
}

void leno_gui_platform_hide_window(LenoGUIPlatformWindow* win) {
    if (!win || !win->hwnd) return;
    ShowWindow(win->hwnd, SW_HIDE);
}

void leno_gui_platform_set_window_title(LenoGUIPlatformWindow* win, const char* title) {
    if (!win || !win->hwnd) return;
    wchar_t* wtitle = NULL;
    int wtitle_len = MultiByteToWideChar(CP_UTF8, 0, title, -1, NULL, 0);
    if (wtitle_len > 0) {
        wtitle = (wchar_t*)malloc(wtitle_len * sizeof(wchar_t));
        MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, wtitle_len);
        SetWindowTextW(win->hwnd, wtitle);
        free(wtitle);
    }
}

void leno_gui_platform_set_window_size(LenoGUIPlatformWindow* win, int w, int h) {
    if (!win || !win->hwnd) return;
    RECT rect = { 0, 0, w, h };
    DWORD style = (DWORD)GetWindowLongW(win->hwnd, GWL_STYLE);
    AdjustWindowRect(&rect, style, FALSE);
    SetWindowPos(win->hwnd, NULL, 0, 0, rect.right - rect.left, rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER);
    win->width = w;
    win->height = h;
}

void leno_gui_platform_set_window_position(LenoGUIPlatformWindow* win, int x, int y) {
    if (!win || !win->hwnd) return;
    SetWindowPos(win->hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

void leno_gui_platform_get_window_position(LenoGUIPlatformWindow* win, int* x, int* y) {
    if (!win || !win->hwnd) { if (x) *x = 0; if (y) *y = 0; return; }
    RECT rect;
    GetWindowRect(win->hwnd, &rect);
    if (x) *x = rect.left;
    if (y) *y = rect.top;
}

void leno_gui_platform_set_window_fullscreen(LenoGUIPlatformWindow* win, int fullscreen) {
    if (!win || !win->hwnd) return;
    if (fullscreen && !win->is_fullscreen) {
        win->saved_style = (DWORD)GetWindowLongW(win->hwnd, GWL_STYLE);
        GetWindowRect(win->hwnd, &win->saved_rect);
        DWORD style = win->saved_style & ~(WS_CAPTION | WS_THICKFRAME);
        SetWindowLongW(win->hwnd, GWL_STYLE, style);
        int cx = GetSystemMetrics(SM_CXSCREEN);
        int cy = GetSystemMetrics(SM_CYSCREEN);
        SetWindowPos(win->hwnd, HWND_TOPMOST, 0, 0, cx, cy, SWP_FRAMECHANGED);
        win->is_fullscreen = 1;
    } else if (!fullscreen && win->is_fullscreen) {
        SetWindowLongW(win->hwnd, GWL_STYLE, win->saved_style);
        SetWindowPos(win->hwnd, NULL,
                     win->saved_rect.left, win->saved_rect.top,
                     win->saved_rect.right - win->saved_rect.left,
                     win->saved_rect.bottom - win->saved_rect.top,
                     SWP_FRAMECHANGED | SWP_NOZORDER);
        win->is_fullscreen = 0;
    }
}

void leno_gui_platform_set_window_drag_area(LenoGUIPlatformWindow* win, int x, int y, int w, int h) {
    if (!win) return;
    win->drag_area_enabled = 1;
    win->drag_area.left = x;
    win->drag_area.top = y;
    win->drag_area.right = x + w;
    win->drag_area.bottom = y + h;
}

void leno_gui_platform_clear_window_drag_area(LenoGUIPlatformWindow* win) {
    if (!win) return;
    win->drag_area_enabled = 0;
}

/* ===== 渲染器 ===== */

LenoGUIPlatformRenderer* leno_gui_platform_create_renderer(LenoGUIPlatformWindow* win) {
    if (!win || !win->hwnd) return NULL;

    LenoGUIPlatformRenderer* ren = (LenoGUIPlatformRenderer*)calloc(1, sizeof(LenoGUIPlatformRenderer));
    if (!ren) return NULL;

    ren->window = win;
    ren->width = win->width;
    ren->height = win->height;
    ren->pitch = win->width * 4;  /* 每行 4 字节 * 宽度 */
    ren->draw_r = 0;
    ren->draw_g = 0;
    ren->draw_b = 0;
    ren->draw_a = 255;
    ren->vp_x = 0; ren->vp_y = 0; ren->vp_w = ren->width; ren->vp_h = ren->height;
    ren->clip_x = 0; ren->clip_y = 0; ren->clip_w = ren->width; ren->clip_h = ren->height;
    ren->clip_enabled = 0;
    
    /* 初始化逻辑呈现模式（借鉴 SDL3） */
    ren->logical_w = win->width;
    ren->logical_h = win->height;
    ren->logical_mode = LENO_GUI_LOGICAL_PRESENTATION_DISABLED;
    ren->scale_x = 1.0f;
    ren->scale_y = 1.0f;
    ren->logical_enabled = 0;

    if (!create_dib_section(ren->width, ren->height, &ren->back_dc, &ren->back_bitmap,
                            &ren->old_bitmap, &ren->pixels)) {
        free(ren);
        return NULL;
    }

    /* 将渲染器关联到窗口，用于 WM_PAINT 直接渲染 */
    win->renderer = ren;

    return ren;
}

void leno_gui_platform_destroy_renderer(LenoGUIPlatformRenderer* ren) {
    if (!ren) return;
    destroy_dib_section(&ren->back_dc, &ren->back_bitmap, &ren->old_bitmap);
    ren->pixels = NULL;
    free(ren);
}

/* 检查并自动调整渲染器大小（借鉴 SDL3 的 surface_valid 检查） */
static int check_and_resize_renderer(LenoGUIPlatformRenderer* ren) {
    if (!ren || !ren->window || !ren->window->hwnd) return 0;
    
    /* 逻辑呈现模式下，渲染器大小保持逻辑大小，不自动调整 */
    if (ren->logical_enabled && ren->logical_mode != LENO_GUI_LOGICAL_PRESENTATION_DISABLED) {
        /* 只需要检查是否需要重置为逻辑大小 */
        if (ren->width != ren->logical_w || ren->height != ren->logical_h) {
            return leno_gui_platform_renderer_resize(ren, ren->logical_w, ren->logical_h);
        }
        return 1;
    }
    
    /* 检查窗口大小是否变化 */
    RECT rect;
    if (!GetClientRect(ren->window->hwnd, &rect)) return 0;
    int new_width = rect.right - rect.left;
    int new_height = rect.bottom - rect.top;
    
    /* 如果大小没有变化且不需要调整，直接返回 */
    if (!ren->needs_resize && ren->width == new_width && ren->height == new_height) {
        return 1;
    }
    
    /* 只有在需要调整且大小有效时才调整 */
    if (new_width > 0 && new_height > 0) {
        return leno_gui_platform_renderer_resize(ren, new_width, new_height);
    }
    return 1;
}

/* 强制检查窗口大小并调整渲染器（用于 present 前） */
/* 保留供将来使用 */
/* static int ensure_renderer_size(LenoGUIPlatformRenderer* ren) { ... } */

void leno_gui_platform_render_present(LenoGUIPlatformRenderer* ren) {
    if (!ren || !ren->window || !ren->window->hwnd || !ren->back_dc) return;

    HWND hwnd = ren->window->hwnd;
    HDC hdc = GetDC(hwnd);
    if (!hdc) return;

    /* 获取窗口实际大小 */
    RECT clientRect;
    GetClientRect(hwnd, &clientRect);
    int win_w = clientRect.right - clientRect.left;
    int win_h = clientRect.bottom - clientRect.top;

    if (ren->logical_enabled && ren->logical_mode != LENO_GUI_LOGICAL_PRESENTATION_DISABLED) {
        /* 逻辑呈现模式：使用内存DC双缓冲，避免直接在屏幕上清屏导致闪烁 */

        /* 计算目标矩形 */
        int dst_x = 0, dst_y = 0, dst_w = win_w, dst_h = win_h;

        switch (ren->logical_mode) {
            case LENO_GUI_LOGICAL_PRESENTATION_STRETCH:
                dst_x = 0; dst_y = 0;
                dst_w = win_w; dst_h = win_h;
                break;

            case LENO_GUI_LOGICAL_PRESENTATION_LETTERBOX:
                {
                    float scale_x = (float)win_w / ren->logical_w;
                    float scale_y = (float)win_h / ren->logical_h;
                    float scale = (scale_x < scale_y) ? scale_x : scale_y;
                    dst_w = (int)(ren->logical_w * scale);
                    dst_h = (int)(ren->logical_h * scale);
                    dst_x = (win_w - dst_w) / 2;
                    dst_y = (win_h - dst_h) / 2;
                }
                break;

            case LENO_GUI_LOGICAL_PRESENTATION_OVERSCAN:
                {
                    float scale_x = (float)win_w / ren->logical_w;
                    float scale_y = (float)win_h / ren->logical_h;
                    float scale = (scale_x > scale_y) ? scale_x : scale_y;
                    dst_w = (int)(ren->logical_w * scale);
                    dst_h = (int)(ren->logical_h * scale);
                    dst_x = (win_w - dst_w) / 2;
                    dst_y = (win_h - dst_h) / 2;
                }
                break;

            case LENO_GUI_LOGICAL_PRESENTATION_INTEGER_SCALE:
                {
                    int scale = (int)((float)win_w / ren->logical_w);
                    int scale_y = (int)((float)win_h / ren->logical_h);
                    if (scale_y < scale) scale = scale_y;
                    if (scale < 1) scale = 1;
                    dst_w = ren->logical_w * scale;
                    dst_h = ren->logical_h * scale;
                    dst_x = (win_w - dst_w) / 2;
                    dst_y = (win_h - dst_h) / 2;
                }
                break;
        }

        /* 创建内存DC作为双缓冲 */
        HDC mem_dc = CreateCompatibleDC(hdc);
        if (mem_dc) {
            HBITMAP mem_bitmap = CreateCompatibleBitmap(hdc, win_w, win_h);
            if (mem_bitmap) {
                HBITMAP old_bitmap = (HBITMAP)SelectObject(mem_dc, mem_bitmap);

                /* 在内存DC中绘制：先清屏 */
                RECT rect = {0, 0, win_w, win_h};
                FillRect(mem_dc, &rect, (HBRUSH)GetStockObject(BLACK_BRUSH));

                /* 在内存DC中绘制：再StretchBlt */
                SetStretchBltMode(mem_dc, ren->logical_mode == LENO_GUI_LOGICAL_PRESENTATION_INTEGER_SCALE ?
                                  COLORONCOLOR : HALFTONE);
                StretchBlt(mem_dc, dst_x, dst_y, dst_w, dst_h,
                           ren->back_dc, 0, 0, ren->logical_w, ren->logical_h, SRCCOPY);

                /* 一次性将内存DC复制到屏幕 */
                BitBlt(hdc, 0, 0, win_w, win_h, mem_dc, 0, 0, SRCCOPY);

                SelectObject(mem_dc, old_bitmap);
                DeleteObject(mem_bitmap);
            }
            DeleteDC(mem_dc);
        }
    } else {
        /* 正常模式：直接复制 */
        if (ren->width == win_w && ren->height == win_h) {
            BitBlt(hdc, 0, 0, ren->width, ren->height, ren->back_dc, 0, 0, SRCCOPY);
        } else if (win_w > 0 && win_h > 0) {
            SetStretchBltMode(hdc, COLORONCOLOR);
            StretchBlt(hdc, 0, 0, win_w, win_h,
                       ren->back_dc, 0, 0,
                       ren->width, ren->height, SRCCOPY);
        }
    }

    ReleaseDC(hwnd, hdc);
    ValidateRect(hwnd, NULL);
}

int leno_gui_platform_renderer_resize(LenoGUIPlatformRenderer* ren, int w, int h) {
    if (!ren || w <= 0 || h <= 0) return 0;
    destroy_dib_section(&ren->back_dc, &ren->back_bitmap, &ren->old_bitmap);
    ren->pixels = NULL;
    ren->width = w;
    ren->height = h;
    ren->pitch = w * 4;  /* 更新 pitch */
    ren->vp_x = 0; ren->vp_y = 0;
    ren->vp_w = w; ren->vp_h = h;
    ren->clip_x = 0; ren->clip_y = 0;
    ren->clip_w = w; ren->clip_h = h;
    ren->clip_enabled = 0;
    if (!create_dib_section(ren->width, ren->height, &ren->back_dc, &ren->back_bitmap,
                            &ren->old_bitmap, &ren->pixels)) {
        return 0;
    }
    ren->needs_resize = 0;
    return 1;
}

/* ===== 逻辑呈现模式 API（借鉴 SDL3）===== */

void leno_gui_platform_set_logical_size(LenoGUIPlatformRenderer* ren, int w, int h) {
    if (!ren || w <= 0 || h <= 0) return;
    ren->logical_w = w;
    ren->logical_h = h;
    ren->logical_enabled = 1;
    
    /* 重新创建 DIB section 为逻辑大小 */
    destroy_dib_section(&ren->back_dc, &ren->back_bitmap, &ren->old_bitmap);
    if (!create_dib_section(w, h, &ren->back_dc, &ren->back_bitmap, &ren->old_bitmap, &ren->pixels)) {
        ren->logical_enabled = 0;
        return;
    }
    
    /* 更新渲染器尺寸为逻辑尺寸 */
    ren->width = w;
    ren->height = h;
    ren->pitch = w * 4;
    ren->vp_x = 0; ren->vp_y = 0;
    ren->vp_w = w; ren->vp_h = h;
    ren->clip_x = 0; ren->clip_y = 0;
    ren->clip_w = w; ren->clip_h = h;
}

void leno_gui_platform_get_logical_size(LenoGUIPlatformRenderer* ren, int* w, int* h) {
    if (!ren) {
        if (w) *w = 0;
        if (h) *h = 0;
        return;
    }
    if (w) *w = ren->logical_w;
    if (h) *h = ren->logical_h;
}

void leno_gui_platform_set_logical_presentation(LenoGUIPlatformRenderer* ren, int mode) {
    if (!ren) return;
    if (mode < LENO_GUI_LOGICAL_PRESENTATION_DISABLED || mode > LENO_GUI_LOGICAL_PRESENTATION_INTEGER_SCALE)
        return;
    ren->logical_mode = mode;
    if (mode != LENO_GUI_LOGICAL_PRESENTATION_DISABLED && !ren->logical_enabled) {
        /* 如果启用逻辑模式但未设置大小，使用当前大小 */
        leno_gui_platform_set_logical_size(ren, ren->width, ren->height);
    }
}

int leno_gui_platform_get_logical_presentation(LenoGUIPlatformRenderer* ren) {
    return ren ? ren->logical_mode : LENO_GUI_LOGICAL_PRESENTATION_DISABLED;
}

void leno_gui_platform_get_logical_viewport(LenoGUIPlatformRenderer* ren, int* x, int* y, int* w, int* h) {
    if (!ren || !ren->logical_enabled) {
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = 0;
        if (h) *h = 0;
        return;
    }
    
    /* 获取窗口实际客户区大小 */
    int win_w = ren->window->width;
    int win_h = ren->window->height;
    if (ren->window->hwnd) {
        RECT clientRect;
        GetClientRect(ren->window->hwnd, &clientRect);
        win_w = clientRect.right - clientRect.left;
        win_h = clientRect.bottom - clientRect.top;
    }
    
    /* 计算实际显示区域 */
    int dst_x = 0, dst_y = 0, dst_w = win_w, dst_h = win_h;
    
    switch (ren->logical_mode) {
        case LENO_GUI_LOGICAL_PRESENTATION_LETTERBOX:
            {
                float scale_x = (float)win_w / ren->logical_w;
                float scale_y = (float)win_h / ren->logical_h;
                float scale = (scale_x < scale_y) ? scale_x : scale_y;
                dst_w = (int)(ren->logical_w * scale);
                dst_h = (int)(ren->logical_h * scale);
                dst_x = (win_w - dst_w) / 2;
                dst_y = (win_h - dst_h) / 2;
            }
            break;
        case LENO_GUI_LOGICAL_PRESENTATION_INTEGER_SCALE:
            {
                int scale = (int)((float)win_w / ren->logical_w);
                int scale_y = (int)((float)win_h / ren->logical_h);
                if (scale_y < scale) scale = scale_y;
                if (scale < 1) scale = 1;
                dst_w = ren->logical_w * scale;
                dst_h = ren->logical_h * scale;
                dst_x = (win_w - dst_w) / 2;
                dst_y = (win_h - dst_h) / 2;
            }
            break;
    }
    
    if (x) *x = dst_x;
    if (y) *y = dst_y;
    if (w) *w = dst_w;
    if (h) *h = dst_h;
}

void leno_gui_platform_reset_logical_size(LenoGUIPlatformRenderer* ren) {
    if (!ren) return;
    ren->logical_enabled = 0;
    ren->logical_mode = LENO_GUI_LOGICAL_PRESENTATION_DISABLED;
    
    /* 恢复窗口实际大小 */
    int w = ren->window->width;
    int h = ren->window->height;
    
    destroy_dib_section(&ren->back_dc, &ren->back_bitmap, &ren->old_bitmap);
    if (create_dib_section(w, h, &ren->back_dc, &ren->back_bitmap, &ren->old_bitmap, &ren->pixels)) {
        ren->width = w;
        ren->height = h;
        ren->pitch = w * 4;
        ren->vp_w = w;
        ren->vp_h = h;
        ren->clip_w = w;
        ren->clip_h = h;
    }
}

/* ===== 事件 ===== */

static void pump_win32_messages(void) {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

int leno_gui_platform_poll_event(LenoGUIEvent* event) {
    pump_win32_messages();
    return event_queue_pop(event);
}

int leno_gui_platform_wait_event(LenoGUIEvent* event, int timeout_ms) {
    if (timeout_ms <= 0) {
        MsgWaitForMultipleObjects(0, NULL, FALSE, INFINITE, QS_ALLEVENTS);
    } else {
        MsgWaitForMultipleObjects(0, NULL, FALSE, (DWORD)timeout_ms, QS_ALLEVENTS);
    }
    pump_win32_messages();
    return event_queue_pop(event);
}

/* ===== 显示器信息 ===== */

void leno_gui_platform_get_display_size(int* w, int* h) {
    if (w) *w = GetSystemMetrics(SM_CXSCREEN);
    if (h) *h = GetSystemMetrics(SM_CYSCREEN);
}

/* ===== 输入状态查询（参考 SDL3 SDL_GetKeyboardState / SDL_GetMouseState） ===== */

/* 查询指定按键是否按下（使用 Win32 GetAsyncKeyState） */
int leno_gui_platform_get_key_state(int key) {
    /* 将 Leno 键码映射回 Win32 虚拟键码 */
    int vk = 0;
    if (key >= 'A' && key <= 'Z') vk = key;
    else if (key >= '0' && key <= '9') vk = key;
    else {
        switch (key) {
            case LENO_GUI_KEY_RETURN:    vk = VK_RETURN; break;
            case LENO_GUI_KEY_ESCAPE:    vk = VK_ESCAPE; break;
            case LENO_GUI_KEY_BACKSPACE: vk = VK_BACK; break;
            case LENO_GUI_KEY_TAB:       vk = VK_TAB; break;
            case LENO_GUI_KEY_SPACE:     vk = VK_SPACE; break;
            case LENO_GUI_KEY_DELETE:    vk = VK_DELETE; break;
            case LENO_GUI_KEY_LEFT:      vk = VK_LEFT; break;
            case LENO_GUI_KEY_RIGHT:     vk = VK_RIGHT; break;
            case LENO_GUI_KEY_UP:        vk = VK_UP; break;
            case LENO_GUI_KEY_DOWN:      vk = VK_DOWN; break;
            case LENO_GUI_KEY_LSHIFT:    vk = VK_LSHIFT; break;
            case LENO_GUI_KEY_RSHIFT:    vk = VK_RSHIFT; break;
            case LENO_GUI_KEY_LCTRL:     vk = VK_LCONTROL; break;
            case LENO_GUI_KEY_RCTRL:     vk = VK_RCONTROL; break;
            case LENO_GUI_KEY_LALT:      vk = VK_LMENU; break;
            case LENO_GUI_KEY_RALT:      vk = VK_RMENU; break;
            default: return 0;
        }
    }
    return (GetAsyncKeyState(vk) & 0x8000) ? 1 : 0;
}

/* ===== 键盘状态跟踪（参考 SDL3 prev/curr 按键状态数组） ===== */

/* 辅助：将 Leno 键码映射到数组索引 [0..255] */
static int leno_key_to_index(int key) {
    if (key >= 'A' && key <= 'Z') return key;
    if (key >= '0' && key <= '9') return key;
    switch (key) {
        case LENO_GUI_KEY_RETURN:    return VK_RETURN;
        case LENO_GUI_KEY_ESCAPE:    return VK_ESCAPE;
        case LENO_GUI_KEY_BACKSPACE: return VK_BACK;
        case LENO_GUI_KEY_TAB:       return VK_TAB;
        case LENO_GUI_KEY_SPACE:     return VK_SPACE;
        case LENO_GUI_KEY_DELETE:    return VK_DELETE;
        case LENO_GUI_KEY_LEFT:      return VK_LEFT;
        case LENO_GUI_KEY_RIGHT:     return VK_RIGHT;
        case LENO_GUI_KEY_UP:        return VK_UP;
        case LENO_GUI_KEY_DOWN:      return VK_DOWN;
        case LENO_GUI_KEY_INSERT:    return VK_INSERT;
        case LENO_GUI_KEY_HOME:      return VK_HOME;
        case LENO_GUI_KEY_END:       return VK_END;
        case LENO_GUI_KEY_PAGEUP:    return VK_PRIOR;
        case LENO_GUI_KEY_PAGEDOWN:  return VK_NEXT;
        case LENO_GUI_KEY_F1: case LENO_GUI_KEY_F2: case LENO_GUI_KEY_F3:
        case LENO_GUI_KEY_F4: case LENO_GUI_KEY_F5: case LENO_GUI_KEY_F6:
        case LENO_GUI_KEY_F7: case LENO_GUI_KEY_F8: case LENO_GUI_KEY_F9:
        case LENO_GUI_KEY_F10: case LENO_GUI_KEY_F11: case LENO_GUI_KEY_F12:
            return VK_F1 + (key - LENO_GUI_KEY_F1);
        case LENO_GUI_KEY_LSHIFT:    return VK_LSHIFT;
        case LENO_GUI_KEY_RSHIFT:    return VK_RSHIFT;
        case LENO_GUI_KEY_LCTRL:     return VK_LCONTROL;
        case LENO_GUI_KEY_RCTRL:     return VK_RCONTROL;
        case LENO_GUI_KEY_LALT:      return VK_LMENU;
        case LENO_GUI_KEY_RALT:      return VK_RMENU;
        case LENO_GUI_KEY_CAPSLOCK:  return VK_CAPITAL;
        case LENO_GUI_KEY_NUMLOCK:   return VK_NUMLOCK;
        default: return -1;
    }
}

/* 更新键盘状态（在 poll 前调用，比较 prev/curr 用于 is_pressed/is_released 判断） */
void leno_gui_platform_update_key_states(void) {
    /* 对所有活跃窗口更新按键状态 */
    /* 简单实现：扫描所有窗口，更新第一个活跃窗口的状态 */
    LenoGUIPlatformWindow* win = NULL;
    /* 查找第一个存在的窗口 */
    HWND hwnd = FindWindowW(L"LenoGUIWindow", NULL);
    if (hwnd) {
        win = (LenoGUIPlatformWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
    if (!win) return;

    /* 保存上一帧状态 */
    memcpy(win->prev_keys, win->curr_keys, sizeof(win->prev_keys));

    /* 读取当前帧状态（使用 GetAsyncKeyState 扫描所有 VK） */
    for (int i = 0; i < 256; i++) {
        win->curr_keys[i] = (GetAsyncKeyState(i) & 0x8000) ? 1 : 0;
    }
    win->key_states_valid = 1;
}

/* 检查按键是否刚被按下 */
int leno_gui_platform_is_key_pressed(int key) {
    int idx = leno_key_to_index(key);
    if (idx < 0 || idx >= 256) return 0;

    /* 查找活跃窗口 */
    HWND hwnd = FindWindowW(L"LenoGUIWindow", NULL);
    if (!hwnd) return 0;
    LenoGUIPlatformWindow* win = (LenoGUIPlatformWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!win || !win->key_states_valid) return 0;

    /* pressed = 当前按下 && 上一帧未按下 */
    return (win->curr_keys[idx] && !win->prev_keys[idx]) ? 1 : 0;
}

/* 检查按键是否刚被释放 */
int leno_gui_platform_is_key_released(int key) {
    int idx = leno_key_to_index(key);
    if (idx < 0 || idx >= 256) return 0;

    HWND hwnd = FindWindowW(L"LenoGUIWindow", NULL);
    if (!hwnd) return 0;
    LenoGUIPlatformWindow* win = (LenoGUIPlatformWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!win || !win->key_states_valid) return 0;

    /* released = 当前未按下 && 上一帧按下 */
    return (!win->curr_keys[idx] && win->prev_keys[idx]) ? 1 : 0;
}

/* ===== 文本输入控制（参考 SDL_StartTextInput / SDL_StopTextInput） ===== */

void leno_gui_platform_start_text_input(void) {
    g_text_input_active = 1;
    /* 参考 SDL3：显式关联默认 IME 上下文到当前焦点窗口，确保 IME 可接收输入 */
    HWND hwnd = GetForegroundWindow();
    if (hwnd) {
        HIMC himc = ImmGetContext(hwnd);
        if (!himc) {
            himc = ImmCreateContext();
            if (himc) {
                ImmAssociateContext(hwnd, himc);
            }
        } else {
            ImmReleaseContext(hwnd, himc);
        }
    }
}

void leno_gui_platform_stop_text_input(void) {
    g_text_input_active = 0;
}

int leno_gui_platform_is_text_input_active(void) {
    return g_text_input_active;
}

void leno_gui_platform_set_ime_caret_pos(int x, int y) {
    g_ime_caret_x = x;
    g_ime_caret_y = y;
    /* 如果已经在合成中，立即更新 IME 候选窗/合成窗位置 */
    if (g_ime_composing) {
        HWND hwnd = GetFocus();
        if (!hwnd) hwnd = GetForegroundWindow();
        if (hwnd) {
            HIMC himc = ImmGetContext(hwnd);
            if (himc) {
                COMPOSITIONFORM cf;
                memset(&cf, 0, sizeof(cf));
                cf.dwStyle = CFS_POINT;
                cf.ptCurrentPos.x = x;
                cf.ptCurrentPos.y = y;
                ImmSetCompositionWindow(himc, &cf);
                ImmReleaseContext(hwnd, himc);
            }
        }
    }
}

/* 查询鼠标状态：位置和按钮 */
int leno_gui_platform_get_mouse_state(int* x, int* y, int* buttons) {
    POINT pt;
    GetCursorPos(&pt);
    if (x) *x = pt.x;
    if (y) *y = pt.y;
    int btns = 0;
    if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) btns |= 0x01;
    if (GetAsyncKeyState(VK_MBUTTON) & 0x8000) btns |= 0x02;
    if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) btns |= 0x04;
    if (buttons) *buttons = btns;
    return btns;
}

/* ===== 剪贴板（参考 SDL3 SDL_GetClipboardText / SDL_SetClipboardText） ===== */

/* 获取剪贴板文本内容 */
char* leno_gui_platform_get_clipboard_text(void) {
    if (!OpenClipboard(NULL)) return NULL;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (!h) {
        CloseClipboard();
        /* 尝试获取 ANSI 文本 */
        HANDLE ha = GetClipboardData(CF_TEXT);
        if (!ha) return NULL;
        char* ansi = (char*)GlobalLock(ha);
        if (!ansi) { CloseClipboard(); return NULL; }
        char* result = _strdup(ansi);
        GlobalUnlock(ha);
        CloseClipboard();
        return result;
    }
    wchar_t* wtext = (wchar_t*)GlobalLock(h);
    if (!wtext) { CloseClipboard(); return NULL; }
    /* 宽字符转 UTF-8 */
    int len = WideCharToMultiByte(CP_UTF8, 0, wtext, -1, NULL, 0, NULL, NULL);
    char* result = NULL;
    if (len > 0) {
        result = (char*)malloc(len);
        WideCharToMultiByte(CP_UTF8, 0, wtext, -1, result, len, NULL, NULL);
    }
    GlobalUnlock(h);
    CloseClipboard();
    return result;
}

/* 设置剪贴板文本内容 */
void leno_gui_platform_set_clipboard_text(const char* text) {
    if (!text) return;
    /* UTF-8 转宽字符 */
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (wlen <= 0) return;
    wchar_t* wtext = (wchar_t*)malloc(wlen * sizeof(wchar_t));
    if (!wtext) return;
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wtext, wlen);
    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, wlen * sizeof(wchar_t));
        if (h) {
            wchar_t* ptr = (wchar_t*)GlobalLock(h);
            if (ptr) {
                memcpy(ptr, wtext, wlen * sizeof(wchar_t));
                GlobalUnlock(h);
                SetClipboardData(CF_UNICODETEXT, h);
            }
        }
        CloseClipboard();
    }
    free(wtext);
}

/* ===== 光标控制（参考 SDL3 SDL_ShowCursor / SDL_HideCursor） ===== */

/* 显示或隐藏光标 */
void leno_gui_platform_show_cursor(int show) {
    ShowCursor(show ? TRUE : FALSE);
}

/* ===== 窗口透明度（参考 SDL3 SDL_SetWindowOpacity） ===== */

/* 设置窗口透明度（0.0 完全透明 ~ 1.0 完全不透明） */
void leno_gui_platform_set_window_opacity(LenoGUIPlatformWindow* win, float opacity) {
    if (!win || !win->hwnd) return;
    /* 透明度值限制在 [0, 1] 范围 */
    if (opacity < 0.0f) opacity = 0.0f;
    if (opacity > 1.0f) opacity = 1.0f;
    /* 需要添加 WS_EX_LAYERED 扩展样式才能使用透明度 */
    LONG ex_style = GetWindowLongW(win->hwnd, GWL_EXSTYLE);
    SetWindowLongW(win->hwnd, GWL_EXSTYLE, ex_style | WS_EX_LAYERED);
    /* 设置分层窗口透明度 */
    SetLayeredWindowAttributes(win->hwnd, 0, (BYTE)(opacity * 255.0f), LWA_ALPHA);
}

/* ===== 字体操作（系统字体渲染，参考 SDL3 SDL_RenderDebugText + GDI） ===== */

struct LenoGUIPlatformFont {
    HFONT hfont;
    int size;
    int internal_leading;   /* 字体内聚留白，光标需跳过 */
};

LenoGUIPlatformFont* leno_gui_platform_load_font(const char* name, int size) {
    if (size <= 0) size = 16;
    HFONT hfont = NULL;
    /* 尝试列表：用户指定的 → 系统备选 */
    const char* fallbacks[] = { name, "Consolas", "Arial", "Segoe UI", NULL };
    for (int i = 0; fallbacks[i] && !hfont; i++) {
        int wsize = MultiByteToWideChar(CP_UTF8, 0, fallbacks[i], -1, NULL, 0);
        if (wsize <= 0) continue;
        wchar_t* wname = (wchar_t*)malloc(wsize * sizeof(wchar_t));
        if (!wname) continue;
        MultiByteToWideChar(CP_UTF8, 0, fallbacks[i], -1, wname, wsize);
        hfont = CreateFontW(
            -size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, wname
        );
        free(wname);
    }
    if (!hfont) { fprintf(stderr, "[FONT] FAIL: %s (size %d)\n", name, size); return NULL; }
    LenoGUIPlatformFont* font = (LenoGUIPlatformFont*)calloc(1, sizeof(LenoGUIPlatformFont));
    if (!font) { DeleteObject(hfont); return NULL; }
    font->hfont = hfont;
    font->size = size;
    /* 测量字体内聚留白 */
    {
        HDC dc = GetDC(NULL);
        HFONT old_f = (HFONT)SelectObject(dc, hfont);
        TEXTMETRICW tm;
        if (GetTextMetricsW(dc, &tm)) {
            font->internal_leading = tm.tmInternalLeading;
        } else {
            font->internal_leading = size / 8;  /* 回退估算 */
        }
        SelectObject(dc, old_f);
        ReleaseDC(NULL, dc);
    }
    return font;
}

int leno_gui_platform_font_internal_leading(LenoGUIPlatformFont* font) {
    return font ? font->internal_leading : 0;
}

int leno_gui_platform_text_width_utf8(LenoGUIPlatformFont* font, const char* text, int byte_len) {
    if (!font || !font->hfont || !text || byte_len <= 0) return 0;
    char* sub = (char*)malloc((size_t)byte_len + 1);
    if (!sub) return 0;
    memcpy(sub, text, (size_t)byte_len); sub[byte_len] = '\0';
    int wlen = MultiByteToWideChar(CP_UTF8, 0, sub, -1, NULL, 0);
    if (wlen <= 1) { free(sub); return 0; }
    wchar_t* wtext = (wchar_t*)malloc((size_t)wlen * sizeof(wchar_t));
    if (!wtext) { free(sub); return 0; }
    MultiByteToWideChar(CP_UTF8, 0, sub, -1, wtext, wlen);
    free(sub);
    /* Scintilla 方式：用 DrawTextW(DT_CALCRECT) 代替 GetTextExtentExPointW，
       确保与绘制使用完全相同的布局引擎和 DC */
    HDC dc = GetDC(NULL);
    HFONT old_f = (HFONT)SelectObject(dc, font->hfont);
    RECT rc = {0, 0, 0, 0};
    DrawTextW(dc, wtext, -1, &rc, DT_LEFT | DT_TOP | DT_CALCRECT | DT_NOPREFIX | DT_SINGLELINE);
    SelectObject(dc, old_f);
    ReleaseDC(NULL, dc);
    free(wtext);
    return rc.right - rc.left;
}

void leno_gui_platform_destroy_font(LenoGUIPlatformFont* font) {
    if (!font) return;
    if (font->hfont) DeleteObject(font->hfont);
    free(font);
}

void leno_gui_platform_draw_text_font(LenoGUIPlatformRenderer* ren, LenoGUIPlatformFont* font, const char* text, int x, int y) {
    if (!ren || !ren->back_dc || !font || !font->hfont || !text) return;
    HFONT old_font = (HFONT)SelectObject(ren->back_dc, font->hfont);
    COLORREF color = RGB(ren->draw_r, ren->draw_g, ren->draw_b);
    SetTextColor(ren->back_dc, color);
    SetBkMode(ren->back_dc, TRANSPARENT);
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    wchar_t* wtext = (wchar_t*)malloc(wlen * sizeof(wchar_t));
    if (!wtext) { SelectObject(ren->back_dc, old_font); return; }
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wtext, wlen);
    RECT rc = { x + ren->vp_x, y + ren->vp_y, 999999, 999999 };
    DrawTextW(ren->back_dc, wtext, -1, &rc, DT_LEFT | DT_TOP | DT_NOCLIP | DT_SINGLELINE | DT_NOPREFIX);
    free(wtext);
    SelectObject(ren->back_dc, old_font);
}

void leno_gui_platform_text_size_font(LenoGUIPlatformRenderer* ren, LenoGUIPlatformFont* font, const char* text, int* w, int* h) {
    if (!font || !font->hfont || !text) { if (w) *w = 0; if (h) *h = 0; return; }
    /* Scintilla 架构：测量和绘制必须在同一个 DC 上进行，避免中文等字符出现 1~2px 偏差 */
    HDC dc = (ren && ren->back_dc) ? ren->back_dc : GetDC(NULL);
    int release_dc = (ren && ren->back_dc) ? 0 : 1;
    HFONT old_font = (HFONT)SelectObject(dc, font->hfont);
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    wchar_t* wtext = (wchar_t*)malloc(wlen * sizeof(wchar_t));
    if (!wtext) { SelectObject(dc, old_font); if (release_dc) ReleaseDC(NULL, dc); if (w) *w = 0; if (h) *h = 0; return; }
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wtext, wlen);
    /* 与渲染一致，使用 DrawTextW；DT_NOPREFIX 防止 & 被解释，DT_SINGLELINE 禁止空格处自动换行 */
    RECT rc = {0, 0, 0, 0};
    DrawTextW(dc, wtext, -1, &rc, DT_LEFT | DT_TOP | DT_CALCRECT | DT_NOPREFIX | DT_SINGLELINE);
    free(wtext);
    SelectObject(dc, old_font);
    if (release_dc) ReleaseDC(NULL, dc);
    if (w) *w = rc.right - rc.left;
    if (h) *h = rc.bottom - rc.top;
}

/* ===== 高精度计时器（参考 SDL3 SDL_GetTicks / SDL_GetPerformanceCounter） ===== */

/* 获取自系统启动以来的毫秒数 */
uint64_t leno_gui_platform_get_ticks(void) {
    return GetTickCount64();
}

/* 获取高精度性能计数器值 */
uint64_t leno_gui_platform_get_performance_counter(void) {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return (uint64_t)li.QuadPart;
}

/* 获取高精度性能计数器频率（每秒计数次数） */
uint64_t leno_gui_platform_get_performance_frequency(void) {
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    return (uint64_t)li.QuadPart;
}

/* 延迟指定毫秒数 */
void leno_gui_platform_delay(uint32_t ms) {
    Sleep(ms);
}

/* ===== 显示器 DPI 查询 ===== */

/* 获取主显示器 DPI 缩放值 */
float leno_gui_platform_get_display_dpi(void) {
    /* 使用 Win32 DPI 感知 API（Windows 10 1607+） */
    HANDLE hmod = GetModuleHandleW(L"user32.dll");
    if (hmod) {
        typedef UINT(WINAPI* GetDpiForSystem_t)(void);
        GetDpiForSystem_t pGetDpiForSystem = (GetDpiForSystem_t)(void*)GetProcAddress(hmod, "GetDpiForSystem");
        if (pGetDpiForSystem) {
            return (float)pGetDpiForSystem();
        }
    }
    /* 回退方案：通过 DC 获取 DPI */
    HDC hdc = GetDC(NULL);
    float dpi = 96.0f;
    if (hdc) {
        dpi = (float)GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(NULL, hdc);
    }
    return dpi;
}

/* ===== 系统光标设置（参考 SDL3 SDL_CreateSystemCursor） ===== */

void leno_gui_platform_set_system_cursor(int cursor_type) {
    if (cursor_type < 0 || cursor_type >= LENO_GUI_CURSOR_COUNT) return;
    init_system_cursors();
    g_custom_cursor = NULL;
    g_custom_cursor_id = 0;
    g_current_system_cursor = cursor_type;
    if (g_system_cursors[cursor_type]) {
        /* 将系统光标设为全局默认，WM_SETCURSOR 会使用它 */
        /* 也可以直接调用 SetCursor */
        SetCursor(g_system_cursors[cursor_type]);
    }
}

/* ===== 自定义光标创建（参考 SDL3 SDL_CreateColorCursor） ===== */

int leno_gui_platform_create_custom_cursor(const uint32_t* pixels, int w, int h, int hot_x, int hot_y) {
    if (!pixels || w <= 0 || h <= 0 || hot_x < 0 || hot_y < 0 || hot_x >= w || hot_y >= h) return 0;

    /* 创建 AND 掩码（全透明=全1，非透明=全0） */
    int mask_size = ((w + 7) / 8) * h;
    BYTE* and_mask = (BYTE*)calloc(mask_size, 1);
    BYTE* xor_mask = (BYTE*)calloc(mask_size, 1);
    if (!and_mask || !xor_mask) {
        free(and_mask);
        free(xor_mask);
        return 0;
    }

    /* 将 32 位 ARGB 转换为 1 位单色掩码 */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int byte_idx = y * ((w + 7) / 8) + (x / 8);
            int bit_idx = 7 - (x % 8);
            uint32_t pixel = pixels[y * w + x];
            BYTE a = (BYTE)(pixel >> 24);

            if (a > 128) {
                /* 不透明：XOR 位=1 (显示颜色), AND 位=0 (保留) */
                xor_mask[byte_idx] |= (1 << bit_idx);
                and_mask[byte_idx] &= ~(1 << bit_idx);
            } else {
                /* 透明：XOR 位=0, AND 位=1 (保留背景) */
                xor_mask[byte_idx] &= ~(1 << bit_idx);
                and_mask[byte_idx] |= (1 << bit_idx);
            }
        }
    }

    /* 创建图标 */
    HICON icon = CreateIcon(GetModuleHandleW(NULL), w, h, 1, 1, and_mask, xor_mask);
    free(and_mask);
    free(xor_mask);

    if (!icon) return 0;

    /* 销毁旧光标 */
    if (g_custom_cursor) {
        DestroyCursor(g_custom_cursor);
        g_custom_cursor = NULL;
    }

    /* 从图标创建光标 */
    ICONINFO icon_info;
    if (GetIconInfo(icon, &icon_info)) {
        g_custom_cursor = (HCURSOR)CopyImage(icon, IMAGE_CURSOR, w, h, LR_COPYFROMRESOURCE);
        if (!g_custom_cursor) {
            g_custom_cursor = CreateIconIndirect(&icon_info);
        }
        if (icon_info.hbmColor) DeleteObject(icon_info.hbmColor);
        if (icon_info.hbmMask) DeleteObject(icon_info.hbmMask);
    }
    DestroyIcon(icon);

    if (!g_custom_cursor) return 0;

    g_custom_cursor_id = 1;
    SetCursor(g_custom_cursor);
    return 1;
}

void leno_gui_platform_destroy_custom_cursor(void) {
    if (g_custom_cursor) {
        DestroyCursor(g_custom_cursor);
        g_custom_cursor = NULL;
    }
    g_custom_cursor_id = 0;
}

void leno_gui_platform_set_cursor(int cursor_id) {
    if (cursor_id == 0) {
        /* 恢复系统光标 */
        g_custom_cursor = NULL;
        g_custom_cursor_id = 0;
    }
    /* cursor_id > 0 时使用自定义光标 */
}

/* ===== 拖放支持（参考 SDL3 SDL_AcceptDragAndDrop） ===== */

void leno_gui_platform_accept_drag_and_drop(LenoGUIPlatformWindow* win, int accept) {
    if (!win || !win->hwnd) return;
    win->drag_drop_enabled = accept;
    DragAcceptFiles(win->hwnd, accept ? TRUE : FALSE);
}

/* ===== 窗口最小/最大尺寸限制（参考 SDL3 SDL_SetWindowMinimumSize / SDL_SetWindowMaximumSize） ===== */

void leno_gui_platform_set_window_minimum_size(LenoGUIPlatformWindow* win, int min_w, int min_h) {
    if (!win) return;
    win->min_w = min_w;
    win->min_h = min_h;
}

void leno_gui_platform_set_window_maximum_size(LenoGUIPlatformWindow* win, int max_w, int max_h) {
    if (!win) return;
    win->max_w = max_w;
    win->max_h = max_h;
}

/* ===== 窗口图标设置（参考 SDL3 SDL_SetWindowIcon） ===== */

void leno_gui_platform_set_window_icon(LenoGUIPlatformWindow* win, const uint32_t* pixels, int w, int h) {
    if (!win || !win->hwnd || !pixels || w <= 0 || h <= 0) return;

    /* 将 ARGB 像素数据转换为 GDI 位图 */
    HDC screen_dc = GetDC(NULL);
    HDC mem_dc = CreateCompatibleDC(screen_dc);

    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; /* 自上而下 */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = NULL;
    HBITMAP color_bmp = CreateDIBSection(mem_dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (color_bmp && bits) {
        memcpy(bits, pixels, w * h * sizeof(uint32_t));
    }

    /* 创建单色掩码 */
    BYTE* mask_bits = (BYTE*)calloc(((w + 15) / 16) * 2 * h, 1);
    HBITMAP mask_bmp = NULL;
    if (mask_bits) {
        mask_bmp = CreateBitmap(w, h, 1, 1, mask_bits);
    }

    /* 创建图标 */
    ICONINFO icon_info;
    memset(&icon_info, 0, sizeof(icon_info));
    icon_info.fIcon = TRUE;
    icon_info.xHotspot = 0;
    icon_info.yHotspot = 0;
    icon_info.hbmMask = mask_bmp;
    icon_info.hbmColor = color_bmp;

    HICON icon = CreateIconIndirect(&icon_info);
    if (icon) {
        /* 设置大图标和小图标 */
        SendMessageW(win->hwnd, WM_SETICON, ICON_BIG, (LPARAM)icon);
        SendMessageW(win->hwnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);
        DestroyIcon(icon);
    }

    if (color_bmp) {
        SelectObject(mem_dc, GetStockObject(NULL_BRUSH));
        DeleteObject(color_bmp);
    }
    if (mask_bmp) DeleteObject(mask_bmp);
    free(mask_bits);
    DeleteDC(mem_dc);
    ReleaseDC(NULL, screen_dc);
}

/* ===== 窗口最大化控制 ===== */

void leno_gui_platform_set_window_maximizable(LenoGUIPlatformWindow* win, int allow) {
    if (!win || !win->hwnd) return;
    win->no_maximize = allow ? 0 : 1;
    DWORD style = GetWindowLongW(win->hwnd, GWL_STYLE);
    if (allow) {
        style |= WS_MAXIMIZEBOX;
    } else {
        style &= ~WS_MAXIMIZEBOX;
    }
    SetWindowLongW(win->hwnd, GWL_STYLE, style);
    SetWindowPos(win->hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

/* ===== 文件对话框（参考 SDL3 SDL_ShowFileDialogWithProperties） ===== */

/* 文件对话框线程参数 */
typedef struct {
    int type;                    /* 对话框类型 */
    LenoGUIFileDialogCallback callback;
    void* userdata;
    HWND hwnd;
    LenoGUIFileFilter* filters;
    int nfilters;
    char* default_path;
    int allow_many;
    char* title;
} FileDialogArgs;

/* UTF-8 字符串转宽字符 */
static wchar_t* utf8_to_wchar(const char* str) {
    if (!str) return NULL;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, str, -1, NULL, 0);
    if (wlen <= 0) return NULL;
    wchar_t* wstr = (wchar_t*)malloc(wlen * sizeof(wchar_t));
    if (wstr) {
        MultiByteToWideChar(CP_UTF8, 0, str, -1, wstr, wlen);
    }
    return wstr;
}

/* 宽字符转 UTF-8 */
static char* wchar_to_utf8(const wchar_t* wstr) {
    if (!wstr) return NULL;
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
    if (len <= 0) return NULL;
    char* str = (char*)malloc(len);
    if (str) {
        WideCharToMultiByte(CP_UTF8, 0, wstr, -1, str, len, NULL, NULL);
    }
    return str;
}

/* 构建过滤器字符串（Win32 格式：双 null 终止） */
static wchar_t* build_filter_string(const LenoGUIFileFilter* filters, int nfilters) {
    if (!filters || nfilters <= 0) return NULL;

    /* 计算所需缓冲区大小 */
    size_t total_len = 0;
    for (int i = 0; i < nfilters; i++) {
        total_len += MultiByteToWideChar(CP_UTF8, 0, filters[i].name, -1, NULL, 0);
        total_len += MultiByteToWideChar(CP_UTF8, 0, filters[i].pattern, -1, NULL, 0);
    }
    total_len += 1; /* 额外的 null 终止 */

    wchar_t* filter_str = (wchar_t*)malloc(total_len * sizeof(wchar_t));
    if (!filter_str) return NULL;

    wchar_t* ptr = filter_str;
    for (int i = 0; i < nfilters; i++) {
        /* 名称 */
        int nlen = MultiByteToWideChar(CP_UTF8, 0, filters[i].name, -1, ptr,
                                        (int)(total_len - (ptr - filter_str)));
        ptr += nlen; /* 包含 null */

        /* 模式 */
        MultiByteToWideChar(CP_UTF8, 0, filters[i].pattern, -1, ptr,
                            (int)(total_len - (ptr - filter_str)));
        ptr += wcslen(ptr) + 1;
    }
    *ptr = L'\0'; /* 双 null 终止 */

    return filter_str;
}

/* 文件对话框工作线程 */
static DWORD WINAPI file_dialog_thread_proc(LPVOID param) {
    FileDialogArgs* args = (FileDialogArgs*)param;
    int filter_index = -1;
    char** files = NULL;
    int nfiles = 0;

    wchar_t* title_w = utf8_to_wchar(args->title);
    wchar_t* filter_w = build_filter_string(args->filters, args->nfilters);

    /* 使用 GetOpenFileName / GetSaveFileName */
    {
        wchar_t filebuffer[65536] = { 0 };

        if (args->default_path) {
            wchar_t* default_w = utf8_to_wchar(args->default_path);
            if (default_w) {
                /* 检查是否是文件夹路径 */
                size_t len = wcslen(default_w);
                if (len > 0 && (default_w[len-1] == L'\\' || default_w[len-1] == L'/')) {
                    /* 文件夹路径，不使用 lpstrFile 初始化 */
                } else {
                    MultiByteToWideChar(CP_UTF8, 0, args->default_path, -1, filebuffer, 65536);
                    /* 转换正斜杠 */
                    for (int i = 0; filebuffer[i]; i++) {
                        if (filebuffer[i] == L'/') filebuffer[i] = L'\\';
                    }
                }
                free(default_w);
            }
        }

        OPENFILENAMEW ofn;
        memset(&ofn, 0, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = args->hwnd;
        ofn.lpstrFilter = filter_w;
        ofn.lpstrFile = filebuffer;
        ofn.nMaxFile = 65536;
        ofn.lpstrTitle = title_w;
        ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
        if (args->allow_many) ofn.Flags |= OFN_ALLOWMULTISELECT;
        if (args->type == LENO_GUI_FILEDIALOG_SAVEFILE) ofn.Flags |= OFN_OVERWRITEPROMPT;

        BOOL result;
        if (args->type == LENO_GUI_FILEDIALOG_SAVEFILE) {
            result = GetSaveFileNameW(&ofn);
        } else {
            result = GetOpenFileNameW(&ofn);
        }

        if (result) {
            if (!(ofn.Flags & OFN_ALLOWMULTISELECT) || args->type == LENO_GUI_FILEDIALOG_SAVEFILE) {
                /* 单个文件 */
                char* chosen = wchar_to_utf8(ofn.lpstrFile);
                if (chosen) {
                    files = (char**)calloc(2, sizeof(char*));
                    nfiles = 1;
                    files[0] = chosen;
                }
            } else {
                /* 多个文件：目录\0文件1\0文件2\0\0 */
                wchar_t* file_ptr = ofn.lpstrFile;
                size_t dir_len = wcslen(file_ptr);
                char* dir = wchar_to_utf8(file_ptr);
                file_ptr += dir_len + 1;

                if (*file_ptr) {
                    /* 有多个文件 */
                    while (*file_ptr) {
                        size_t fname_len = wcslen(file_ptr);
                        char* fname = wchar_to_utf8(file_ptr);
                        if (fname) {
                            char* fullpath = (char*)malloc(strlen(dir) + strlen(fname) + 2);
                            if (fullpath) {
                                sprintf(fullpath, "%s\\%s", dir, fname);
                                char** new_files = (char**)realloc(files, (nfiles + 2) * sizeof(char*));
                                if (new_files) {
                                    files = new_files;
                                    files[nfiles] = fullpath;
                                    nfiles++;
                                    files[nfiles] = NULL;
                                } else {
                                    free(fullpath);
                                }
                            }
                            free(fname);
                        }
                        file_ptr += fname_len + 1;
                    }
                } else {
                    /* 只有一个文件 */
                    files = (char**)calloc(2, sizeof(char*));
                    nfiles = 1;
                    files[0] = dir;
                    dir = NULL; /* 不释放 */
                }
                if (dir) free(dir);
            }
            filter_index = (int)(ofn.nFilterIndex - 1);
        }
    }

    /* 将结果通过临界区投递到主线程 */
    EnterCriticalSection(&g_filedlg_result_cs);
    /* 释放旧结果 */
    if (g_filedlg_result) {
        if (g_filedlg_result->files) {
            for (int i = 0; i < g_filedlg_result->nfiles; i++) {
                free(g_filedlg_result->files[i]);
            }
            free(g_filedlg_result->files);
        }
        free(g_filedlg_result);
        g_filedlg_result = NULL;
    }
    /* 存储新结果 */
    if (nfiles > 0 && files) {
        g_filedlg_result = (FileDialogResult*)calloc(1, sizeof(FileDialogResult));
        if (g_filedlg_result) {
            g_filedlg_result->files = files;
            g_filedlg_result->nfiles = nfiles;
            g_filedlg_result->filter_index = filter_index;
            files = NULL; /* 转移所有权，不再在下面释放 */
        }
    }
    LeaveCriticalSection(&g_filedlg_result_cs);

    /* 推送一个空事件来唤醒主循环 */
    {
        LenoGUIEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = LENO_GUI_EVT_FILEDIALOG_RESULT;
        ev.timestamp = GetTickCount64();
        event_queue_push(&ev);
    }

    /* 清理未被转移的文件 */
    if (files) {
        for (int i = 0; i < nfiles; i++) {
            free(files[i]);
        }
        free(files);
    }
    free(title_w);
    free(filter_w);
    free(args->default_path);
    free(args->title);
    free(args->filters);
    free(args);

    return 0;
}

void leno_gui_platform_show_file_dialog(int type, LenoGUIFileDialogCallback callback,
                                         void* userdata, LenoGUIPlatformWindow* win,
                                         const LenoGUIFileFilter* filters, int nfilters,
                                         const char* default_path, int allow_many,
                                         const char* title) {
    FileDialogArgs* args = (FileDialogArgs*)calloc(1, sizeof(FileDialogArgs));
    if (!args) {
        /* 内存分配失败，推送空结果事件 */
        LenoGUIEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = LENO_GUI_EVT_FILEDIALOG_RESULT;
        ev.timestamp = GetTickCount64();
        event_queue_push(&ev);
        return;
    }

    args->type = type;
    args->callback = callback;
    args->userdata = userdata;
    args->hwnd = (win && win->hwnd) ? win->hwnd : NULL;
    args->allow_many = allow_many;
    args->nfilters = nfilters;
    args->default_path = default_path ? _strdup(default_path) : NULL;
    args->title = title ? _strdup(title) : NULL;

    /* 复制过滤器 */
    if (filters && nfilters > 0) {
        args->filters = (LenoGUIFileFilter*)malloc(nfilters * sizeof(LenoGUIFileFilter));
        if (args->filters) {
            for (int i = 0; i < nfilters; i++) {
                args->filters[i].name = filters[i].name ? _strdup(filters[i].name) : NULL;
                args->filters[i].pattern = filters[i].pattern ? _strdup(filters[i].pattern) : NULL;
            }
        }
    }

    /* 创建线程运行对话框 */
    HANDLE thread = CreateThread(NULL, 0, file_dialog_thread_proc, args, 0, NULL);
    if (!thread) {
        /* 线程创建失败，清理并通过事件队列通知 */
        if (args->filters) {
            for (int i = 0; i < nfilters; i++) {
                free((void*)args->filters[i].name);
                free((void*)args->filters[i].pattern);
            }
            free(args->filters);
        }
        free(args->default_path);
        free(args->title);
        free(args);
        /* 推送空结果事件 */
        LenoGUIEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = LENO_GUI_EVT_FILEDIALOG_RESULT;
        ev.timestamp = GetTickCount64();
        event_queue_push(&ev);
        return;
    }
    CloseHandle(thread);
}

/* ===== 消息框（参考 SDL3 SDL_ShowSimpleMessageBox） ===== */

/* 显示系统消息框：type 0=信息 1=警告 2=错误 */
int leno_gui_platform_show_message_box(const char* title, const char* message, int type) {
    /* UTF-8 转宽字符 */
    int wtlen = MultiByteToWideChar(CP_UTF8, 0, title, -1, NULL, 0);
    int wmlen = MultiByteToWideChar(CP_UTF8, 0, message, -1, NULL, 0);
    wchar_t* wtitle = NULL;
    wchar_t* wmsg = NULL;
    if (wtlen > 0) {
        wtitle = (wchar_t*)malloc(wtlen * sizeof(wchar_t));
        MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, wtlen);
    }
    if (wmlen > 0) {
        wmsg = (wchar_t*)malloc(wmlen * sizeof(wchar_t));
        MultiByteToWideChar(CP_UTF8, 0, message, -1, wmsg, wmlen);
    }
    /* 根据类型选择图标 */
    UINT mb_type = MB_OK;
    switch (type) {
        case 1: mb_type |= MB_ICONWARNING; break;
        case 2: mb_type |= MB_ICONERROR; break;
        default: mb_type |= MB_ICONINFORMATION; break;
    }
    int result = MessageBoxW(NULL, wmsg, wtitle, mb_type);
    free(wtitle);
    free(wmsg);
    return result;
}

/* ===== 文件对话框结果处理（由主循环调用，线程安全） ===== */

#include "guis_internal.h"

int leno_gui_platform_process_filedialog_result(void) {
    int processed = 0;
    
    EnterCriticalSection(&g_filedlg_result_cs);
    FileDialogResult* result = g_filedlg_result;
    g_filedlg_result = NULL;
    LeaveCriticalSection(&g_filedlg_result_cs);
    
    if (result) {
        /* 构建 C 字符串数组 */
        const char** files = (const char**)calloc((size_t)(result->nfiles + 1), sizeof(const char*));
        if (files) {
            for (int i = 0; i < result->nfiles; i++) {
                files[i] = result->files[i];
            }
            files[result->nfiles] = NULL;
            
            /* 在主线程中调用 Leno 回调 */
            process_filedialog_callback(files, result->nfiles, result->filter_index);
            
            free(files);
        }
        
        /* 清理结果 */
        if (result->files) {
            for (int i = 0; i < result->nfiles; i++) {
                free(result->files[i]);
            }
            free(result->files);
        }
        free(result);
        processed = 1;
    }
    
    return processed;
}

#endif /* _WIN32 */
