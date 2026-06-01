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
    uint8_t draw_r;
    uint8_t draw_g;
    uint8_t draw_b;
    uint8_t draw_a;
    int vp_x, vp_y, vp_w, vp_h;
    int clip_x, clip_y, clip_w, clip_h;
    int clip_enabled;
    int needs_resize;  /* 窗口大小变化标志，参考 SDL3 surface_valid */
};

/* ===== 平台纹理结构 ===== */

struct LenoGUIPlatformTexture {
    uint32_t* pixels;
    int width;
    int height;
    int pitch;
};

#include "leno_guis_swrender.c"

/* ===== 窗口类注册 ===== */

static const wchar_t* LENO_GUI_CLASS = L"LenoGUIWindow";
static int g_class_registered = 0;
static int g_gui_initialized = 0;

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
            push_window_event(win, LENO_GUI_EVT_WINDOW_RESIZE, (int)LOWORD(lparam), (int)HIWORD(lparam));
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
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            
            /* 直接绘制后备缓冲区到窗口 */
            if (win && win->renderer && win->renderer->back_dc && hdc) {
                RECT clientRect;
                GetClientRect(hwnd, &clientRect);
                int win_w = clientRect.right - clientRect.left;
                int win_h = clientRect.bottom - clientRect.top;
                
                if (win->renderer->width == win_w && win->renderer->height == win_h) {
                    BitBlt(hdc, 0, 0, win->renderer->width, win->renderer->height,
                           win->renderer->back_dc, 0, 0, SRCCOPY);
                } else if (win_w > 0 && win_h > 0) {
                    SetStretchBltMode(hdc, COLORONCOLOR);
                    StretchBlt(hdc, 0, 0, win_w, win_h,
                               win->renderer->back_dc, 0, 0,
                               win->renderer->width, win->renderer->height, SRCCOPY);
                }
            }
            
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
            LenoGUIEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = LENO_GUI_EVT_TEXT_INPUT;
            ev.timestamp = GetTickCount64();
            ev.window_id = win ? win->window_id : 0;
            if (wparam < 0x10000) {
                ev.text[0] = (char)(wparam & 0xFF);
                ev.text[1] = '\0';
            }
            event_queue_push(&ev);
            return 0;
        }
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
        default:
            return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
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

    event_queue_init();

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
    
    /* 清理事件队列和对象池 */
    event_queue_cleanup();
    
    if (g_cs_initialized) {
        DeleteCriticalSection(&g_event_cs);
        g_cs_initialized = 0;
    }
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

    DWORD style = WS_OVERLAPPEDWINDOW;
    if (flags & LENO_GUI_WIN_BORDERLESS) {
        style = WS_POPUP;
        if (flags & LENO_GUI_WIN_RESIZABLE) {
            style |= WS_THICKFRAME;
        }
    }
    if (!(flags & LENO_GUI_WIN_RESIZABLE)) style &= ~WS_THICKFRAME;
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
    ren->draw_r = 0;
    ren->draw_g = 0;
    ren->draw_b = 0;
    ren->draw_a = 255;
    ren->vp_x = 0; ren->vp_y = 0; ren->vp_w = ren->width; ren->vp_h = ren->height;
    ren->clip_x = 0; ren->clip_y = 0; ren->clip_w = ren->width; ren->clip_h = ren->height;
    ren->clip_enabled = 0;

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
static int ensure_renderer_size(LenoGUIPlatformRenderer* ren) {
    if (!ren || !ren->window || !ren->window->hwnd) return 0;
    
    RECT rect;
    if (!GetClientRect(ren->window->hwnd, &rect)) return 0;
    int win_w = rect.right - rect.left;
    int win_h = rect.bottom - rect.top;
    
    /* 如果窗口大小与渲染器不匹配，自动调整 */
    if (win_w != ren->width || win_h != ren->height) {
        if (win_w > 0 && win_h > 0) {
            return leno_gui_platform_renderer_resize(ren, win_w, win_h);
        }
    }
    return 1;
}

void leno_gui_platform_render_present(LenoGUIPlatformRenderer* ren) {
    if (!ren || !ren->window || !ren->window->hwnd || !ren->back_dc) return;
    
    /* 自动检查并调整渲染器大小（借鉴 SDL3） */
    if (!ensure_renderer_size(ren)) return;
    
    HWND hwnd = ren->window->hwnd;
    HDC hdc = GetDC(hwnd);
    if (hdc) {
        /* 现在渲染器大小应该与窗口匹配，直接使用 BitBlt */
        BitBlt(hdc, 0, 0, ren->width, ren->height, ren->back_dc, 0, 0, SRCCOPY);
        ReleaseDC(hwnd, hdc);
    }
    ValidateRect(hwnd, NULL);
}

int leno_gui_platform_renderer_resize(LenoGUIPlatformRenderer* ren, int w, int h) {
    if (!ren || w <= 0 || h <= 0) return 0;
    destroy_dib_section(&ren->back_dc, &ren->back_bitmap, &ren->old_bitmap);
    ren->pixels = NULL;
    ren->width = w;
    ren->height = h;
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
};

LenoGUIPlatformFont* leno_gui_platform_load_font(const char* name, int size) {
    if (size <= 0) size = 16;
    int wsize = MultiByteToWideChar(CP_UTF8, 0, name, -1, NULL, 0);
    wchar_t* wname = (wchar_t*)malloc(wsize * sizeof(wchar_t));
    if (!wname) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, wsize);
    HFONT hfont = CreateFontW(
        -size, 0,
        0, 0,
        FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        wname
    );
    free(wname);
    if (!hfont) return NULL;
    LenoGUIPlatformFont* font = (LenoGUIPlatformFont*)calloc(1, sizeof(LenoGUIPlatformFont));
    if (!font) { DeleteObject(hfont); return NULL; }
    font->hfont = hfont;
    font->size = size;
    return font;
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
    RECT rc = { x + ren->vp_x, y + ren->vp_y, ren->width, ren->height };
    DrawTextW(ren->back_dc, wtext, -1, &rc, DT_LEFT | DT_TOP | DT_NOCLIP);
    free(wtext);
    SelectObject(ren->back_dc, old_font);
}

void leno_gui_platform_text_size_font(LenoGUIPlatformFont* font, const char* text, int* w, int* h) {
    if (!font || !font->hfont || !text) { if (w) *w = 0; if (h) *h = 0; return; }
    HDC dc = GetDC(NULL);
    HFONT old_font = (HFONT)SelectObject(dc, font->hfont);
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    wchar_t* wtext = (wchar_t*)malloc(wlen * sizeof(wchar_t));
    if (!wtext) { SelectObject(dc, old_font); ReleaseDC(NULL, dc); if (w) *w = 0; if (h) *h = 0; return; }
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wtext, wlen);
    RECT rc = {0, 0, 0, 0};
    DrawTextW(dc, wtext, -1, &rc, DT_LEFT | DT_TOP | DT_CALCRECT);
    free(wtext);
    SelectObject(dc, old_font);
    ReleaseDC(NULL, dc);
    if (w) *w = rc.right - rc.left;
    if (h) *h = rc.bottom - rc.top;
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

#endif /* _WIN32 */
