/* Leno GUI - Linux 平台实现 (X11)
 * 使用 Xlib + 软件渲染
 *
 * 窗口管理: XCreateWindow + 事件选择
 * 渲染: XImage 像素缓冲区 + XPutImage
 * 事件: XPending / XNextEvent 轮询
 *
 * 参考 SDL3 X11 源码 (src/video/x11/) 实现:
 *   - 剪贴板: 参考 SDL_x11clipboard.c (简化为 XStoreBytes/XFetchBytes)
 *   - 光标:   参考 SDL_x11mouse.c (XDefineCursor 创建空光标)
 *   - 透明度: 参考 SDL_x11window.c (_NET_WM_WINDOW_OPACITY)
 *   - 键盘:   参考 SDL_x11keyboard.c (XQueryKeymap)
 *   - 鼠标:   参考 SDL_x11mouse.c (XQueryPointer)
 *   - 计时器: 参考 SDL_timer.c (clock_gettime CLOCK_MONOTONIC)
 *   - DPI:    参考 SDL_x11modes.c (XDisplayWidthMM 计算)
 */

#include "leno_guis.h"

#ifdef __linux__

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <pthread.h>
#include <unistd.h>

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

static pthread_mutex_t g_event_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 事件过滤器（借鉴 SDL3） */
typedef struct LenoGUIEventFilter {
    void* userdata;
    int (*filter)(void* userdata, LenoGUIEvent* event);
    struct LenoGUIEventFilter* next;
} LenoGUIEventFilter;

static LenoGUIEventFilter* g_event_filters = NULL;

/* 添加事件过滤器 */
void leno_gui_platform_add_event_filter(void* userdata, int (*filter)(void*, LenoGUIEvent*)) {
    if (!filter) return;
    LenoGUIEventFilter* f = (LenoGUIEventFilter*)malloc(sizeof(LenoGUIEventFilter));
    if (!f) return;
    f->userdata = userdata;
    f->filter = filter;
    pthread_mutex_lock(&g_event_mutex);
    f->next = g_event_filters;
    g_event_filters = f;
    pthread_mutex_unlock(&g_event_mutex);
}

/* 移除事件过滤器 */
void leno_gui_platform_remove_event_filter(void* userdata, int (*filter)(void*, LenoGUIEvent*)) {
    pthread_mutex_lock(&g_event_mutex);
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
    pthread_mutex_unlock(&g_event_mutex);
}

/* 从对象池获取或分配新节点 */
static LenoGUIEventEntry* event_entry_alloc(void) {
    pthread_mutex_lock(&g_event_mutex);
    LenoGUIEventEntry* entry = g_event_free_list;
    if (entry) {
        g_event_free_list = entry->next;
        memset(entry, 0, sizeof(LenoGUIEventEntry));
    }
    pthread_mutex_unlock(&g_event_mutex);
    
    if (!entry) {
        entry = (LenoGUIEventEntry*)calloc(1, sizeof(LenoGUIEventEntry));
    }
    return entry;
}

/* 回收节点到对象池 */
static void event_entry_free(LenoGUIEventEntry* entry) {
    if (!entry) return;
    pthread_mutex_lock(&g_event_mutex);
    entry->next = g_event_free_list;
    g_event_free_list = entry;
    pthread_mutex_unlock(&g_event_mutex);
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
    
    pthread_mutex_lock(&g_event_mutex);
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
    pthread_mutex_unlock(&g_event_mutex);
}

static int event_queue_pop(LenoGUIEvent* ev) {
    pthread_mutex_lock(&g_event_mutex);
    LenoGUIEventEntry* entry = g_event_queue_head;
    if (!entry) {
        pthread_mutex_unlock(&g_event_mutex);
        return 0;
    }
    
    g_event_queue_head = entry->next;
    if (!g_event_queue_head) {
        g_event_queue_tail = NULL;
    }
    g_event_count--;
    pthread_mutex_unlock(&g_event_mutex);
    
    *ev = entry->event;
    event_entry_free(entry);
    return 1;
}

/* 清理事件队列和对象池 */
static void event_queue_cleanup(void) {
    pthread_mutex_lock(&g_event_mutex);
    
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
    
    pthread_mutex_unlock(&g_event_mutex);
}

/* ===== 窗口 ID 计数器 ===== */

static int g_next_window_id = 1;

/* ===== 全局 X11 连接 ===== */

static Display* g_display = NULL;
static int g_screen = 0;
static Atom g_wm_delete_window = None;
static int g_gui_initialized = 0;

/* ===== 窗口列表（用于事件处理查找窗口） ===== */

#define MAX_GUI_WINDOWS 16

static LenoGUIPlatformWindow* g_window_list[MAX_GUI_WINDOWS];
static int g_window_count = 0;

static void window_list_add(LenoGUIPlatformWindow* win) {
    if (g_window_count < MAX_GUI_WINDOWS) {
        g_window_list[g_window_count++] = win;
    }
}

static void window_list_remove(LenoGUIPlatformWindow* win) {
    for (int i = 0; i < g_window_count; i++) {
        if (g_window_list[i] == win) {
            for (int j = i; j < g_window_count - 1; j++) {
                g_window_list[j] = g_window_list[j + 1];
            }
            g_window_count--;
            break;
        }
    }
}

static LenoGUIPlatformWindow* window_list_find(Window xwindow) {
    for (int i = 0; i < g_window_count; i++) {
        if (g_window_list[i] && g_window_list[i]->xwindow == xwindow) {
            return g_window_list[i];
        }
    }
    return NULL;
}

/* ===== 平台窗口结构 ===== */

struct LenoGUIPlatformWindow {
    Window xwindow;
    int window_id;
    int width;
    int height;
    int should_close;
    int is_fullscreen;
    int is_borderless;
    int is_minimized;       /* 跟踪最小化状态（参考 SDL3） */
    int is_hiding;          /* 区分 UnmapNotify 是最小化还是隐藏（参考 SDL3） */
    int drag_area_enabled;
    int drag_area_x;
    int drag_area_y;
    int drag_area_w;
    int drag_area_h;
    /* 双击检测状态 */
    unsigned long last_click_time[3];   /* 上次点击时间（毫秒，索引0=左键,1=中键,2=右键） */
    int last_click_x[3];                /* 上次点击X坐标 */
    int last_click_y[3];                /* 上次点击Y坐标 */
    int click_count[3];                 /* 当前点击计数（用于双击检测） */
};

/* ===== 平台渲染器结构 ===== */

struct LenoGUIPlatformRenderer {
    LenoGUIPlatformWindow* window;
    XImage* ximage;
    uint32_t* pixels;
    int width;
    int height;
    uint8_t draw_r;
    uint8_t draw_g;
    uint8_t draw_b;
    uint8_t draw_a;
    GC gc;
    /* 视口偏移和尺寸（参考 SDL3 SDL_SetRenderViewport） */
    int vp_x, vp_y, vp_w, vp_h;
    /* 裁剪矩形（参考 SDL3 SDL_SetRenderClipRect） */
    int clip_x, clip_y, clip_w, clip_h;
    int clip_enabled;
    int needs_resize;  /* 窗口大小变化标志，参考 SDL3 surface_valid */
};

/* ===== 平台图像结构 ===== */

struct LenoGUIPlatformImage {
    uint32_t* pixels;
    int width;
    int height;
    int pitch;
};

#include "leno_guis_swrender.c"
#include "leno_guis_image.c"

/* ===== X11 KeySym 到 Leno 键码映射 ===== */

static int keysym_to_leno_key(KeySym ks) {
    if (ks >= 'a' && ks <= 'z') return (int)(ks - 32);
    if (ks >= 'A' && ks <= 'Z') return (int)ks;
    if (ks >= '0' && ks <= '9') return (int)ks;
    switch (ks) {
        case XK_Return:     return LENO_GUI_KEY_RETURN;
        case XK_Escape:     return LENO_GUI_KEY_ESCAPE;
        case XK_BackSpace:  return LENO_GUI_KEY_BACKSPACE;
        case XK_Tab:        return LENO_GUI_KEY_TAB;
        case XK_space:      return LENO_GUI_KEY_SPACE;
        case XK_Delete:     return LENO_GUI_KEY_DELETE;
        case XK_Left:       return LENO_GUI_KEY_LEFT;
        case XK_Right:      return LENO_GUI_KEY_RIGHT;
        case XK_Up:         return LENO_GUI_KEY_UP;
        case XK_Down:       return LENO_GUI_KEY_DOWN;
        case XK_Insert:     return LENO_GUI_KEY_INSERT;
        case XK_Home:       return LENO_GUI_KEY_HOME;
        case XK_End:        return LENO_GUI_KEY_END;
        case XK_Page_Up:    return LENO_GUI_KEY_PAGEUP;
        case XK_Page_Down:  return LENO_GUI_KEY_PAGEDOWN;
        case XK_F1:         return LENO_GUI_KEY_F1;
        case XK_F2:         return LENO_GUI_KEY_F2;
        case XK_F3:         return LENO_GUI_KEY_F3;
        case XK_F4:         return LENO_GUI_KEY_F4;
        case XK_F5:         return LENO_GUI_KEY_F5;
        case XK_F6:         return LENO_GUI_KEY_F6;
        case XK_F7:         return LENO_GUI_KEY_F7;
        case XK_F8:         return LENO_GUI_KEY_F8;
        case XK_F9:         return LENO_GUI_KEY_F9;
        case XK_F10:        return LENO_GUI_KEY_F10;
        case XK_F11:        return LENO_GUI_KEY_F11;
        case XK_F12:        return LENO_GUI_KEY_F12;
        case XK_Shift_L:    return LENO_GUI_KEY_LSHIFT;
        case XK_Shift_R:    return LENO_GUI_KEY_RSHIFT;
        case XK_Control_L:  return LENO_GUI_KEY_LCTRL;
        case XK_Control_R:  return LENO_GUI_KEY_RCTRL;
        case XK_Alt_L:      return LENO_GUI_KEY_LALT;
        case XK_Alt_R:      return LENO_GUI_KEY_RALT;
        case XK_Caps_Lock:  return LENO_GUI_KEY_CAPSLOCK;
        case XK_Num_Lock:   return LENO_GUI_KEY_NUMLOCK;
        default:            return LENO_GUI_KEY_UNKNOWN;
    }
}

/* Leno 键码到 X11 KeySym 的反向映射（用于 get_key_state） */
static KeySym leno_key_to_keysym(int key) {
    if (key >= 'A' && key <= 'Z') return (KeySym)key;
    if (key >= '0' && key <= '9') return (KeySym)key;
    switch (key) {
        case LENO_GUI_KEY_RETURN:    return XK_Return;
        case LENO_GUI_KEY_ESCAPE:    return XK_Escape;
        case LENO_GUI_KEY_BACKSPACE: return XK_BackSpace;
        case LENO_GUI_KEY_TAB:       return XK_Tab;
        case LENO_GUI_KEY_SPACE:     return XK_space;
        case LENO_GUI_KEY_DELETE:    return XK_Delete;
        case LENO_GUI_KEY_LEFT:      return XK_Left;
        case LENO_GUI_KEY_RIGHT:     return XK_Right;
        case LENO_GUI_KEY_UP:        return XK_Up;
        case LENO_GUI_KEY_DOWN:      return XK_Down;
        case LENO_GUI_KEY_INSERT:    return XK_Insert;
        case LENO_GUI_KEY_HOME:      return XK_Home;
        case LENO_GUI_KEY_END:       return XK_End;
        case LENO_GUI_KEY_PAGEUP:    return XK_Page_Up;
        case LENO_GUI_KEY_PAGEDOWN:  return XK_Page_Down;
        case LENO_GUI_KEY_F1:        return XK_F1;
        case LENO_GUI_KEY_F2:        return XK_F2;
        case LENO_GUI_KEY_F3:        return XK_F3;
        case LENO_GUI_KEY_F4:        return XK_F4;
        case LENO_GUI_KEY_F5:        return XK_F5;
        case LENO_GUI_KEY_F6:        return XK_F6;
        case LENO_GUI_KEY_F7:        return XK_F7;
        case LENO_GUI_KEY_F8:        return XK_F8;
        case LENO_GUI_KEY_F9:        return XK_F9;
        case LENO_GUI_KEY_F10:       return XK_F10;
        case LENO_GUI_KEY_F11:       return XK_F11;
        case LENO_GUI_KEY_F12:       return XK_F12;
        case LENO_GUI_KEY_LSHIFT:    return XK_Shift_L;
        case LENO_GUI_KEY_RSHIFT:    return XK_Shift_R;
        case LENO_GUI_KEY_LCTRL:     return XK_Control_L;
        case LENO_GUI_KEY_RCTRL:     return XK_Control_R;
        case LENO_GUI_KEY_LALT:      return XK_Alt_L;
        case LENO_GUI_KEY_RALT:      return XK_Alt_R;
        case LENO_GUI_KEY_CAPSLOCK:  return XK_Caps_Lock;
        case LENO_GUI_KEY_NUMLOCK:   return XK_Num_Lock;
        default:                     return NoSymbol;
    }
}

static int get_mod_flags(XKeyEvent* ev) {
    int mod = 0;
    if (ev->state & ShiftMask)   mod |= LENO_GUI_MOD_SHIFT;
    if (ev->state & ControlMask) mod |= LENO_GUI_MOD_CTRL;
    if (ev->state & Mod1Mask)    mod |= LENO_GUI_MOD_ALT;
    if (ev->state & Mod4Mask)    mod |= LENO_GUI_MOD_SUPER;
    return mod;
}

static uint64_t get_timestamp_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

/* ===== 平台 API 实现 ===== */

int leno_gui_platform_init(void) {
    if (g_gui_initialized) return 1;

    g_display = XOpenDisplay(NULL);
    if (!g_display) return 0;

    g_screen = DefaultScreen(g_display);
    g_wm_delete_window = XInternAtom(g_display, "WM_DELETE_WINDOW", False);

    g_event_head = 0;
    g_event_tail = 0;
    g_event_count = 0;

    g_gui_initialized = 1;
    return 1;
}

void leno_gui_platform_quit(void) {
    if (!g_gui_initialized) return;
    
    /* 清理事件队列和对象池 */
    event_queue_cleanup();
    
    if (g_display) {
        XCloseDisplay(g_display);
        g_display = NULL;
    }
    g_gui_initialized = 0;
}

LenoGUIPlatformWindow* leno_gui_platform_create_window(const char* title, int w, int h, int flags) {
    if (!g_gui_initialized || !g_display) return NULL;

    LenoGUIPlatformWindow* win = (LenoGUIPlatformWindow*)calloc(1, sizeof(LenoGUIPlatformWindow));
    if (!win) return NULL;

    win->window_id = g_next_window_id++;
    win->width = w;
    win->height = h;
    win->should_close = 0;
    win->is_fullscreen = 0;
    win->is_borderless = (flags & LENO_GUI_WIN_BORDERLESS) ? 1 : 0;
    win->drag_area_enabled = 0;

    Window root = RootWindow(g_display, g_screen);
    unsigned long bg = BlackPixel(g_display, g_screen);

    unsigned long xflags = 0;
    XSetWindowAttributes swa;
    memset(&swa, 0, sizeof(swa));
    swa.background_pixel = bg;
    swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask |
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                     StructureNotifyMask | FocusChangeMask;

    if (flags & LENO_GUI_WIN_BORDERLESS) {
        swa.override_redirect = True;
        xflags |= CWOverrideRedirect;
    }

    xflags |= CWBackPixel | CWEventMask;

    win->xwindow = XCreateWindow(g_display, root, 0, 0, w, h, 0,
                                  CopyFromParent, InputOutput, CopyFromParent,
                                  xflags, &swa);
    if (!win->xwindow) {
        free(win);
        return NULL;
    }

    XSetWMProtocols(g_display, win->xwindow, &g_wm_delete_window, 1);

    XStoreName(g_display, win->xwindow, title);

    if (flags & LENO_GUI_WIN_RESIZABLE) {
        XSizeHints* hints = XAllocSizeHints();
        if (hints) {
            hints->flags = PSize;
            hints->width = w;
            hints->height = h;
            XSetWMNormalHints(g_display, win->xwindow, hints);
            XFree(hints);
        }
    } else {
        XSizeHints* hints = XAllocSizeHints();
        if (hints) {
            hints->flags = PSize | PMinSize | PMaxSize;
            hints->width = w;
            hints->height = h;
            hints->min_width = w;
            hints->min_height = h;
            hints->max_width = w;
            hints->max_height = h;
            XSetWMNormalHints(g_display, win->xwindow, hints);
            XFree(hints);
        }
    }

    if (!(flags & LENO_GUI_WIN_HIDDEN)) {
        XMapWindow(g_display, win->xwindow);
    }

    window_list_add(win);

    XFlush(g_display);
    return win;
}

void leno_gui_platform_destroy_window(LenoGUIPlatformWindow* win) {
    if (!win) return;
    window_list_remove(win);
    if (g_display && win->xwindow) {
        XDestroyWindow(g_display, win->xwindow);
        XFlush(g_display);
    }
    free(win);
}

void leno_gui_platform_show_window(LenoGUIPlatformWindow* win) {
    if (!win || !g_display || !win->xwindow) return;
    win->is_hiding = 0;
    XMapWindow(g_display, win->xwindow);
    XFlush(g_display);
}

void leno_gui_platform_hide_window(LenoGUIPlatformWindow* win) {
    if (!win || !g_display || !win->xwindow) return;
    win->is_hiding = 1;  /* 标记为程序主动隐藏，UnmapNotify 会发 WINDOW_HIDE */
    XUnmapWindow(g_display, win->xwindow);
    XFlush(g_display);
}

void leno_gui_platform_set_window_title(LenoGUIPlatformWindow* win, const char* title) {
    if (!win || !g_display || !win->xwindow) return;
    XStoreName(g_display, win->xwindow, title);
    XFlush(g_display);
}

void leno_gui_platform_set_window_size(LenoGUIPlatformWindow* win, int w, int h) {
    if (!win || !g_display || !win->xwindow) return;
    XResizeWindow(g_display, win->xwindow, w, h);
    win->width = w;
    win->height = h;
    XFlush(g_display);
}

void leno_gui_platform_set_window_position(LenoGUIPlatformWindow* win, int x, int y) {
    if (!win || !g_display || !win->xwindow) return;
    XMoveWindow(g_display, win->xwindow, x, y);
    XFlush(g_display);
}

void leno_gui_platform_get_window_position(LenoGUIPlatformWindow* win, int* x, int* y) {
    if (!win || !g_display || !win->xwindow) { if (x) *x = 0; if (y) *y = 0; return; }
    XWindowAttributes attr;
    XGetWindowAttributes(g_display, win->xwindow, &attr);
    if (x) *x = attr.x;
    if (y) *y = attr.y;
}

void leno_gui_platform_set_window_fullscreen(LenoGUIPlatformWindow* win, int fullscreen) {
    if (!win || !g_display || !win->xwindow) return;

    Atom wm_state = XInternAtom(g_display, "_NET_WM_STATE", True);
    Atom fullscreen_atom = XInternAtom(g_display, "_NET_WM_STATE_FULLSCREEN", True);

    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = win->xwindow;
    ev.xclient.message_type = wm_state;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = fullscreen ? 1 : 0;
    ev.xclient.data.l[1] = (long)fullscreen_atom;
    ev.xclient.data.l[2] = 0;

    XSendEvent(g_display, RootWindow(g_display, g_screen), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(g_display);

    win->is_fullscreen = fullscreen;
}

void leno_gui_platform_set_window_drag_area(LenoGUIPlatformWindow* win, int x, int y, int w, int h) {
    if (!win) return;
    win->drag_area_enabled = 1;
    win->drag_area_x = x;
    win->drag_area_y = y;
    win->drag_area_w = w;
    win->drag_area_h = h;
}

void leno_gui_platform_clear_window_drag_area(LenoGUIPlatformWindow* win) {
    if (!win) return;
    win->drag_area_enabled = 0;
}

/* ===== 渲染器 ===== */

LenoGUIPlatformRenderer* leno_gui_platform_create_renderer(LenoGUIPlatformWindow* win) {
    if (!win || !g_display || !win->xwindow) return NULL;

    LenoGUIPlatformRenderer* ren = (LenoGUIPlatformRenderer*)calloc(1, sizeof(LenoGUIPlatformRenderer));
    if (!ren) return NULL;

    ren->window = win;
    ren->width = win->width;
    ren->height = win->height;
    ren->draw_r = 0;
    ren->draw_g = 0;
    ren->draw_b = 0;
    ren->draw_a = 255;
    /* 初始化视口为整个渲染区域 */
    ren->vp_x = 0; ren->vp_y = 0; ren->vp_w = ren->width; ren->vp_h = ren->height;
    /* 初始化裁剪矩形为整个渲染区域（默认禁用） */
    ren->clip_x = 0; ren->clip_y = 0; ren->clip_w = ren->width; ren->clip_h = ren->height;
    ren->clip_enabled = 0;

    ren->pixels = (uint32_t*)calloc(ren->width * ren->height, sizeof(uint32_t));
    if (!ren->pixels) {
        free(ren);
        return NULL;
    }

    Visual* visual = DefaultVisual(g_display, g_screen);
    int depth = DefaultDepth(g_display, g_screen);

    ren->ximage = XCreateImage(g_display, visual, depth, ZPixmap, 0,
                                (char*)ren->pixels, ren->width, ren->height, 32, 0);
    if (!ren->ximage) {
        free(ren->pixels);
        free(ren);
        return NULL;
    }

    ren->gc = XCreateGC(g_display, win->xwindow, 0, NULL);

    return ren;
}

void leno_gui_platform_destroy_renderer(LenoGUIPlatformRenderer* ren) {
    if (!ren) return;
    if (ren->ximage) {
        ren->ximage->data = NULL;
        XDestroyImage(ren->ximage);
    }
    if (ren->pixels) free(ren->pixels);
    if (g_display && ren->window && ren->window->xwindow && ren->gc) {
        XFreeGC(g_display, ren->gc);
    }
    free(ren);
}

/* 检查并自动调整渲染器大小（参考 SDL3 GetWindowSurface） */
static int check_and_resize_renderer(LenoGUIPlatformRenderer* ren) {
    if (!ren || !ren->window || !g_display || !ren->window->xwindow) return 0;

    /* 获取窗口当前大小 */
    XWindowAttributes attr;
    if (!XGetWindowAttributes(g_display, ren->window->xwindow, &attr)) return 0;

    int new_width = attr.width;
    int new_height = attr.height;

    /* 如果大小变化或需要调整，重新创建渲染器 */
    if (ren->needs_resize || new_width != ren->width || new_height != ren->height) {
        if (new_width > 0 && new_height > 0) {
            return leno_gui_platform_renderer_resize(ren, new_width, new_height);
        }
    }
    return 1;
}

void leno_gui_platform_render_present(LenoGUIPlatformRenderer* ren) {
    if (!ren || !ren->pixels || !g_display || !ren->window || !ren->window->xwindow) return;

    XPutImage(g_display, ren->window->xwindow, ren->gc, ren->ximage,
              0, 0, 0, 0, ren->width, ren->height);
    XFlush(g_display);
}

/* 当窗口大小改变时，重新调整渲染器大小 */
int leno_gui_platform_renderer_resize(LenoGUIPlatformRenderer* ren, int w, int h) {
    if (!ren || !g_display || w <= 0 || h <= 0) return 0;

    /* 销毁旧的 XImage */
    if (ren->ximage) {
        ren->ximage->data = NULL;
        XDestroyImage(ren->ximage);
        ren->ximage = NULL;
    }

    /* 释放旧的像素缓冲区 */
    if (ren->pixels) {
        free(ren->pixels);
        ren->pixels = NULL;
    }

    /* 更新尺寸 */
    ren->width = w;
    ren->height = h;

    /* 重置视口为新的全尺寸 */
    ren->vp_x = 0; ren->vp_y = 0;
    ren->vp_w = w; ren->vp_h = h;

    /* 重置裁剪矩形 */
    ren->clip_x = 0; ren->clip_y = 0;
    ren->clip_w = w; ren->clip_h = h;
    ren->clip_enabled = 0;

    /* 分配新的像素缓冲区 */
    ren->pixels = (uint32_t*)calloc(ren->width * ren->height, sizeof(uint32_t));
    if (!ren->pixels) return 0;

    /* 创建新的 XImage */
    Visual* visual = DefaultVisual(g_display, g_screen);
    int depth = DefaultDepth(g_display, g_screen);

    ren->ximage = XCreateImage(g_display, visual, depth, ZPixmap, 0,
                                (char*)ren->pixels, ren->width, ren->height, 32, 0);
    if (!ren->ximage) {
        free(ren->pixels);
        ren->pixels = NULL;
        return 0;
    }

    /* 清除调整标志 */
    ren->needs_resize = 0;

    return 1;
}

/* ===== 事件 ===== */

static void pump_x11_events(void) {
    if (!g_display) return;
    while (XPending(g_display) > 0) {
        XEvent xev;
        XNextEvent(g_display, &xev);

        LenoGUIPlatformWindow* win = NULL;
        if (xev.xany.window) {
            win = window_list_find(xev.xany.window);
        }

        LenoGUIEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.timestamp = get_timestamp_ms();

        switch (xev.type) {
            case ClientMessage: {
                if ((Atom)xev.xclient.data.l[0] == g_wm_delete_window) {
                    ev.type = LENO_GUI_EVT_WINDOW_CLOSE;
                    event_queue_push(&ev);
                    LenoGUIEvent quit_ev;
                    memset(&quit_ev, 0, sizeof(quit_ev));
                    quit_ev.type = LENO_GUI_EVT_QUIT;
                    quit_ev.timestamp = get_timestamp_ms();
                    event_queue_push(&quit_ev);
                }
                break;
            }
            case ConfigureNotify: {
                ev.type = LENO_GUI_EVT_WINDOW_RESIZE;
                ev.data1 = xev.xconfigure.width;
                ev.data2 = xev.xconfigure.height;
                event_queue_push(&ev);
                break;
            }
            case Expose: {
                /* 窗口需要重绘（拖动/遮挡后暴露） */
                if (xev.xexpose.count == 0) {  /* 只处理最后一个 Expose 事件 */
                    ev.type = LENO_GUI_EVT_WINDOW_EXPOSED;
                    event_queue_push(&ev);
                }
                break;
            }
            case KeyPress: {
                ev.type = LENO_GUI_EVT_KEY_DOWN;
                KeySym ks = XkbKeycodeToKeysym(g_display, xev.xkey.keycode, 0, 0);
                ev.key = keysym_to_leno_key(ks);
                ev.scancode = xev.xkey.keycode;
                ev.mod_flags = get_mod_flags(&xev.xkey);
                event_queue_push(&ev);
                break;
            }
            case KeyRelease: {
                ev.type = LENO_GUI_EVT_KEY_UP;
                KeySym ks = XkbKeycodeToKeysym(g_display, xev.xkey.keycode, 0, 0);
                ev.key = keysym_to_leno_key(ks);
                ev.scancode = xev.xkey.keycode;
                ev.mod_flags = get_mod_flags(&xev.xkey);
                event_queue_push(&ev);
                break;
            }
            case MotionNotify: {
                ev.type = LENO_GUI_EVT_MOUSE_MOVE;
                ev.mouse_x = (float)xev.xmotion.x;
                ev.mouse_y = (float)xev.xmotion.y;
                event_queue_push(&ev);
                /* 无边框窗口拖动 */
                if (win && win->is_borderless) {
                    Window root_return, child_return;
                    int root_x, root_y, win_x, win_y;
                    unsigned int mask_return;
                    XQueryPointer(g_display, win->xwindow, &root_return, &child_return,
                                  &root_x, &root_y, &win_x, &win_y, &mask_return);
                    if (mask_return & Button1Mask) {
                        XMoveWindow(g_display, win->xwindow,
                                    root_x - xev.xmotion.x, root_y - xev.xmotion.y);
                    }
                }
                break;
            }
            case ButtonPress: {
                if (xev.xbutton.button == 4 || xev.xbutton.button == 5 ||
                    xev.xbutton.button == 6 || xev.xbutton.button == 7) {
                    ev.type = LENO_GUI_EVT_MOUSE_WHEEL;
                    ev.mouse_x = (float)xev.xbutton.x;
                    ev.mouse_y = (float)xev.xbutton.y;
                    /* 垂直滚轮: button 4=up, 5=down */
                    if (xev.xbutton.button == 4) {
                        ev.wheel_y = 1.0f; ev.wheel_x = 0.0f;
                    } else if (xev.xbutton.button == 5) {
                        ev.wheel_y = -1.0f; ev.wheel_x = 0.0f;
                    /* 水平滚轮: button 6=left, 7=right (参考 SDL_x11events.c) */
                    } else if (xev.xbutton.button == 6) {
                        ev.wheel_y = 0.0f; ev.wheel_x = -1.0f;
                    } else if (xev.xbutton.button == 7) {
                        ev.wheel_y = 0.0f; ev.wheel_x = 1.0f;
                    }
                } else {
                    ev.type = LENO_GUI_EVT_MOUSE_DOWN;
                    ev.mouse_x = (float)xev.xbutton.x;
                    ev.mouse_y = (float)xev.xbutton.y;
                    ev.mouse_button = (int)xev.xbutton.button;
                    /* 双击检测逻辑 */
                    int button_idx = (int)xev.xbutton.button - 1;
                    if (button_idx >= 0 && button_idx < 3 && win) {
                        struct timespec ts;
                        clock_gettime(CLOCK_MONOTONIC, &ts);
                        unsigned long now = (unsigned long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
                        unsigned long elapsed = now - win->last_click_time[button_idx];
                        int dx = xev.xbutton.x - win->last_click_x[button_idx];
                        int dy = xev.xbutton.y - win->last_click_y[button_idx];
                        /* 双击阈值：400ms 内，位置偏移小于 4 像素 */
                        if (elapsed < 400 && dx * dx + dy * dy < 16) {
                            win->click_count[button_idx]++;
                        } else {
                            win->click_count[button_idx] = 1;
                        }
                        win->last_click_time[button_idx] = now;
                        win->last_click_x[button_idx] = xev.xbutton.x;
                        win->last_click_y[button_idx] = xev.xbutton.y;
                        ev.mouse_clicks = win->click_count[button_idx];
                    } else {
                        ev.mouse_clicks = 1;
                    }
                    /* 无边框窗口拖动支持 */
                    if (win && win->is_borderless && xev.xbutton.button == 1) {
                        int mx = xev.xbutton.x;
                        int my = xev.xbutton.y;
                        int can_drag = 0;
                        if (win->drag_area_enabled) {
                            if (mx >= win->drag_area_x && mx < win->drag_area_x + win->drag_area_w &&
                                my >= win->drag_area_y && my < win->drag_area_y + win->drag_area_h) {
                                can_drag = 1;
                            }
                        } else {
                            can_drag = 1;
                        }
                        if (can_drag) {
                            XRaiseWindow(g_display, win->xwindow);
                            XGrabPointer(g_display, win->xwindow, True,
                                         ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                                         GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
                        }
                    }
                }
                event_queue_push(&ev);
                break;
            }
            case ButtonRelease: {
                if (xev.xbutton.button == 4 || xev.xbutton.button == 5) break;
                ev.type = LENO_GUI_EVT_MOUSE_UP;
                ev.mouse_x = (float)xev.xbutton.x;
                ev.mouse_y = (float)xev.xbutton.y;
                ev.mouse_button = (int)xev.xbutton.button;
                event_queue_push(&ev);
                /* 释放鼠标捕获 */
                if (win && win->is_borderless) {
                    XUngrabPointer(g_display, CurrentTime);
                }
                break;
            }
            case FocusIn: {
                /* 参考 SDL3 X11_DispatchFocusIn：过滤 NotifyGrab/NotifyUngrab/NotifyInferior/NotifyPointer */
                if (xev.xfocus.mode == NotifyGrab || xev.xfocus.mode == NotifyUngrab) break;
                if (xev.xfocus.detail == NotifyInferior || xev.xfocus.detail == NotifyPointer) break;
                ev.type = LENO_GUI_EVT_WINDOW_FOCUS;
                event_queue_push(&ev);
                break;
            }
            case FocusOut: {
                /* 参考 SDL3 X11_DispatchFocusOut：同样过滤 */
                if (xev.xfocus.mode == NotifyGrab || xev.xfocus.mode == NotifyUngrab) break;
                if (xev.xfocus.detail == NotifyInferior || xev.xfocus.detail == NotifyPointer) break;
                ev.type = LENO_GUI_EVT_WINDOW_UNFOCUS;
                event_queue_push(&ev);
                break;
            }
            case MapNotify: {
                /* 参考 SDL3 X11_DispatchMapNotify */
                ev.type = LENO_GUI_EVT_WINDOW_SHOW;
                event_queue_push(&ev);
                /* 如果之前是最小化状态，则发送 RESTORED 事件 */
                if (win && win->is_minimized) {
                    win->is_minimized = 0;
                    ev.type = LENO_GUI_EVT_WINDOW_RESTORED;
                    event_queue_push(&ev);
                    ev.type = LENO_GUI_EVT_WINDOW_EXPOSED;
                    event_queue_push(&ev);
                }
                break;
            }
            case UnmapNotify: {
                /* 参考 SDL3 X11_DispatchUnmapNotify：
                 * 区分最小化（用户最小化窗口）和隐藏（程序调用 hide_window）
                 * is_hiding 为 true 时是程序主动隐藏，发 WINDOW_HIDE
                 * is_hiding 为 false 时是窗口管理器最小化，发 WINDOW_MINIMIZED
                 */
                if (win && !win->is_hiding) {
                    win->is_minimized = 1;
                    ev.type = LENO_GUI_EVT_WINDOW_MINIMIZED;
                    event_queue_push(&ev);
                } else {
                    ev.type = LENO_GUI_EVT_WINDOW_HIDE;
                    event_queue_push(&ev);
                }
                break;
            }
            case ConfigureNotify: {
                /* 参考 SDL3 ConfigureNotify 处理：发送 RESIZE 和 MOVE 事件 */
                if (win) {
                    int new_w = xev.xconfigure.width;
                    int new_h = xev.xconfigure.height;
                    int new_x = xev.xconfigure.x;
                    int new_y = xev.xconfigure.y;
                    /* 检测大小变化 */
                    if (new_w != win->width || new_h != win->height) {
                        win->width = new_w;
                        win->height = new_h;
                        ev.type = LENO_GUI_EVT_WINDOW_RESIZE;
                        ev.data1 = new_w;
                        ev.data2 = new_h;
                        event_queue_push(&ev);
                    }
                }
                break;
            }
        }
    }
}

int leno_gui_platform_poll_event(LenoGUIEvent* event) {
    pump_x11_events();
    return event_queue_pop(event);
}

int leno_gui_platform_wait_event(LenoGUIEvent* event, int timeout_ms) {
    if (!g_display) return 0;
    if (timeout_ms <= 0) {
        XEvent xev;
        XPeekEvent(g_display, &xev);
    } else {
        struct timeval tv;
        fd_set fds;
        int xfd = ConnectionNumber(g_display);
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        select(xfd + 1, &fds, NULL, NULL, &tv);
    }
    pump_x11_events();
    return event_queue_pop(event);
}

/* ===== 显示器信息 ===== */

void leno_gui_platform_get_display_size(int* w, int* h) {
    if (!g_display) { if (w) *w = 0; if (h) *h = 0; return; }
    if (w) *w = DisplayWidth(g_display, g_screen);
    if (h) *h = DisplayHeight(g_display, g_screen);
}

/* ===== 输入状态查询（参考 SDL3 SDL_GetKeyboardState / SDL_GetMouseState） ===== */

/* 查询指定按键是否按下（使用 XQueryKeymap，参考 SDL_x11keyboard.c） */
int leno_gui_platform_get_key_state(int key) {
    if (!g_display) return 0;

    /* 将 Leno 键码映射到 X11 KeySym */
    KeySym ks = leno_key_to_keysym(key);
    if (ks == NoSymbol) return 0;

    /* 使用 XQueryKeymap 获取当前键盘状态（32 字节位图） */
    char keymap[32];
    XQueryKeymap(g_display, keymap);

    /* 将 KeySym 转换为 keycode，然后检查对应位 */
    KeyCode kc = XKeysymToKeycode(g_display, ks);
    if (kc == 0) return 0;

    /* keymap 中每个字节 8 位，keycode 对应 keymap[kc>>8] 的第 (kc & 7) 位 */
    int byte_idx = kc / 8;
    int bit_idx = kc % 8;
    if (byte_idx < 0 || byte_idx >= 32) return 0;

    return (keymap[byte_idx] & (1 << bit_idx)) ? 1 : 0;
}

/* ===== 键盘状态跟踪（参考 SDL3 prev/curr 按键状态数组） ===== */

static uint8_t g_linux_prev_keys[256] = {0};
static uint8_t g_linux_curr_keys[256] = {0};
static int g_linux_key_states_valid = 0;

/* 辅助：将 Leno 键码映射到 keymap 索引 */
static int leno_key_to_linux_index(int key) {
    if (!g_display) return -1;
    KeySym ks = leno_key_to_keysym(key);
    if (ks == NoSymbol) return -1;
    KeyCode kc = XKeysymToKeycode(g_display, ks);
    if (kc == 0 || kc >= 256) return -1;
    return (int)kc;
}

void leno_gui_platform_update_key_states(void) {
    if (!g_display) return;
    memcpy(g_linux_prev_keys, g_linux_curr_keys, sizeof(g_linux_prev_keys));

    char keymap[32];
    XQueryKeymap(g_display, keymap);
    for (int kc = 0; kc < 256; kc++) {
        int byte_idx = kc / 8;
        int bit_idx = kc % 8;
        if (byte_idx < 0 || byte_idx >= 32) continue;
        g_linux_curr_keys[kc] = (keymap[byte_idx] & (1 << bit_idx)) ? 1 : 0;
    }
    g_linux_key_states_valid = 1;
}

int leno_gui_platform_is_key_pressed(int key) {
    if (!g_linux_key_states_valid) return 0;
    int idx = leno_key_to_linux_index(key);
    if (idx < 0 || idx >= 256) return 0;
    return (g_linux_curr_keys[idx] && !g_linux_prev_keys[idx]) ? 1 : 0;
}

int leno_gui_platform_is_key_released(int key) {
    if (!g_linux_key_states_valid) return 0;
    int idx = leno_key_to_linux_index(key);
    if (idx < 0 || idx >= 256) return 0;
    return (!g_linux_curr_keys[idx] && g_linux_prev_keys[idx]) ? 1 : 0;
}

/* ===== 文本输入控制 ===== */

static int g_linux_text_input_active = 0;

void leno_gui_platform_start_text_input(void) {
    g_linux_text_input_active = 1;
}

void leno_gui_platform_stop_text_input(void) {
    g_linux_text_input_active = 0;
}

int leno_gui_platform_is_text_input_active(void) {
    return g_linux_text_input_active;
}

/* 查询鼠标状态：位置和按钮（使用 XQueryPointer，参考 SDL_x11mouse.c） */
int leno_gui_platform_get_mouse_state(int* x, int* y, int* buttons) {
    if (!g_display) {
        if (x) *x = 0;
        if (y) *y = 0;
        if (buttons) *buttons = 0;
        return 0;
    }

    Window root = RootWindow(g_display, g_screen);
    Window child;
    int root_x, root_y, win_x, win_y;
    unsigned int mask;

    /* XQueryPointer 查询指针位置和按钮状态 */
    if (!XQueryPointer(g_display, root, &root, &child,
                       &root_x, &root_y, &win_x, &win_y, &mask)) {
        if (x) *x = 0;
        if (y) *y = 0;
        if (buttons) *buttons = 0;
        return 0;
    }

    if (x) *x = root_x;
    if (y) *y = root_y;

    /* 从按钮掩码提取按钮状态 */
    int btns = 0;
    if (mask & Button1Mask) btns |= 0x01; /* 左键 */
    if (mask & Button2Mask) btns |= 0x02; /* 中键 */
    if (mask & Button3Mask) btns |= 0x04; /* 右键 */
    if (buttons) *buttons = btns;
    return btns;
}

/* ===== 剪贴板（参考 SDL3 SDL_x11clipboard.c，简化实现使用 XStoreBytes/XFetchBytes） ===== */

/* 获取剪贴板文本内容 */
char* leno_gui_platform_get_clipboard_text(void) {
    if (!g_display) return NULL;

    /* 首先尝试使用 XA_PRIMARY 选择区（参考 SDL_x11clipboard.c） */
    Atom selection = XA_PRIMARY;
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char* data = NULL;

    /* 尝试获取 UTF8_STRING 类型的选择内容 */
    Atom utf8_atom = XInternAtom(g_display, "UTF8_STRING", True);
    Atom target = utf8_atom != None ? utf8_atom : XA_STRING;

    /* 检查是否有选择区拥有者 */
    Window owner = XGetSelectionOwner(g_display, selection);
    if (owner != None) {
        /* 请求选择区转换（简化实现：直接尝试获取） */
        if (XGetWindowProperty(g_display, owner, target, 0, 1024 * 64 / 4, False,
                               target, &actual_type, &actual_format,
                               &nitems, &bytes_after, &data) == Success && data) {
            char* result = strdup((const char*)data);
            XFree(data);
            return result;
        }
    }

    /* 回退方案：使用 XFetchBytes 获取剪切缓冲区 */
    int nbytes = 0;
    char* buf = XFetchBytes(g_display, &nbytes);
    if (buf && nbytes > 0) {
        char* result = (char*)malloc(nbytes + 1);
        if (result) {
            memcpy(result, buf, nbytes);
            result[nbytes] = '\0';
        }
        XFree(buf);
        return result;
    }
    if (buf) XFree(buf);

    return NULL;
}

/* 设置剪贴板文本内容 */
void leno_gui_platform_set_clipboard_text(const char* text) {
    if (!text || !g_display) return;

    int len = (int)strlen(text);

    /* 使用 XStoreBytes 设置剪切缓冲区（PRIMARY 选择区的简化方案） */
    XStoreBytes(g_display, text, len);

    /* 同时尝试获取 CLIPBOARD 选择区所有权（参考 SDL_x11clipboard.c） */
    Atom clipboard_atom = XInternAtom(g_display, "CLIPBOARD", True);
    if (clipboard_atom != None) {
        /* 创建一个不可见窗口作为选择区拥有者 */
        Window owner = XCreateSimpleWindow(g_display, RootWindow(g_display, g_screen),
                                            -1, -1, 1, 1, 0, 0, 0);
        if (owner) {
            XSetSelectionOwner(g_display, clipboard_atom, owner, CurrentTime);
            XSetSelectionOwner(g_display, XA_PRIMARY, owner, CurrentTime);
            /* 注意：简化实现，不处理 SelectionRequest 事件 */
        }
    }

    XFlush(g_display);
}

/* ===== 光标控制（参考 SDL3 SDL_x11mouse.c） ===== */

/* 显示或隐藏光标（使用 XDefineCursor 创建空光标或恢复光标） */
void leno_gui_platform_show_cursor(int show) {
    if (!g_display) return;

    if (!show) {
        /* 创建透明（空）光标来隐藏鼠标 */
        static Cursor invisible_cursor = None;
        if (invisible_cursor == None) {
            /* 使用 1x1 像素的空位图创建不可见光标 */
            Pixmap pixmap = XCreatePixmap(g_display, RootWindow(g_display, g_screen), 1, 1, 1);
            XColor color;
            memset(&color, 0, sizeof(color));
            invisible_cursor = XCreatePixmapCursor(g_display, pixmap, pixmap, &color, &color, 0, 0);
            XFreePixmap(g_display, pixmap);
        }
        /* 遍历所有屏幕的根窗口设置不可见光标 */
        for (int i = 0; i < ScreenCount(g_display); i++) {
            Window root = RootWindow(g_display, i);
            XDefineCursor(g_display, root, invisible_cursor);
        }
        XFlush(g_display);
    } else {
        /* 恢复默认光标 */
        for (int i = 0; i < ScreenCount(g_display); i++) {
            Window root = RootWindow(g_display, i);
            XUndefineCursor(g_display, root);
        }
        XFlush(g_display);
    }
}

/* ===== 窗口透明度（参考 SDL3 SDL_x11window.c _NET_WM_WINDOW_OPACITY） ===== */

/* 设置窗口透明度（0.0 完全透明 ~ 1.0 完全不透明） */
void leno_gui_platform_set_window_opacity(LenoGUIPlatformWindow* win, float opacity) {
    if (!win || !g_display || !win->xwindow) return;
    /* 透明度值限制在 [0, 1] 范围 */
    if (opacity < 0.0f) opacity = 0.0f;
    if (opacity > 1.0f) opacity = 1.0f;

    /* 使用 _NET_WM_WINDOW_OPACITY 属性设置窗口透明度 */
    Atom opacity_atom = XInternAtom(g_display, "_NET_WM_WINDOW_OPACITY", True);
    if (opacity_atom == None) return;

    /* 透明度值：0xFFFFFFFF 表示完全不透明，0 表示完全透明 */
    unsigned long opacity_value = (unsigned long)(opacity * 0xFFFFFFFF);
    XChangeProperty(g_display, win->xwindow, opacity_atom, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char*)&opacity_value, 1);
    XFlush(g_display);
}

/* ===== 字体操作（Xft 系统字体渲染） ===== */

#include <X11/Xft/Xft.h>

struct LenoGUIPlatformFont {
    XftFont* xft_font;
    int size;
};

LenoGUIPlatformFont* leno_gui_platform_load_font(const char* name, int size) {
    if (!g_display || size <= 0) size = 16;
    XftFont* xft_font = XftFontOpenName(g_display, g_screen, name, size);
    if (!xft_font) {
        char fallback[256];
        snprintf(fallback, sizeof(fallback), "sans-%d", size);
        xft_font = XftFontOpenName(g_display, g_screen, fallback, size);
    }
    if (!xft_font) return NULL;
    LenoGUIPlatformFont* font = (LenoGUIPlatformFont*)calloc(1, sizeof(LenoGUIPlatformFont));
    if (!font) { XftFontClose(g_display, xft_font); return NULL; }
    font->xft_font = xft_font;
    font->size = size;
    return font;
}

void leno_gui_platform_destroy_font(LenoGUIPlatformFont* font) {
    if (!font) return;
    if (font->xft_font && g_display) XftFontClose(g_display, font->xft_font);
    free(font);
}

void leno_gui_platform_draw_text_font(LenoGUIPlatformRenderer* ren, LenoGUIPlatformFont* font, const char* text, int x, int y) {
    if (!ren || !ren->pixels || !font || !font->xft_font || !text || !g_display) return;
    Drawable drawable = XCreatePixmap(g_display, DefaultRootWindow(g_display), ren->width, ren->height, DefaultDepth(g_display, g_screen));
    XftDraw* draw = XftDrawCreate(g_display, drawable, DefaultVisual(g_display, g_screen), DefaultColormap(g_display, g_screen));
    XRenderColor xrc = { ren->draw_r << 8, ren->draw_g << 8, ren->draw_b << 8, ren->draw_a << 8 };
    XftColor xft_color;
    XftColorAllocValue(g_display, DefaultVisual(g_display, g_screen), DefaultColormap(g_display, g_screen), &xrc, &xft_color);
    XftDrawStringUtf8(draw, &xft_color, font->xft_font, x + ren->vp_x, y + ren->vp_y + font->xft_font->ascent, (const FcChar8*)text, strlen(text));
    XftColorFree(g_display, DefaultVisual(g_display, g_screen), DefaultColormap(g_display, g_screen), &xft_color);
    XftDrawDestroy(draw);
    XImage* img = XGetImage(g_display, drawable, 0, 0, ren->width, ren->height, AllPlanes, ZPixmap);
    if (img) {
        for (int py = 0; py < ren->height; py++) {
            for (int px = 0; px < ren->width; px++) {
                unsigned long pixel = XGetPixel(img, px, py);
                if (pixel != 0) {
                    ren->pixels[py * ren->width + px] = (uint32_t)pixel;
                }
            }
        }
        XDestroyImage(img);
    }
    XFreePixmap(g_display, drawable);
}

void leno_gui_platform_text_size_font(LenoGUIPlatformFont* font, const char* text, int* w, int* h) {
    if (!font || !font->xft_font || !text) { if (w) *w = 0; if (h) *h = 0; return; }
    XGlyphInfo extents;
    XftTextExtentsUtf8(g_display, font->xft_font, (const FcChar8*)text, strlen(text), &extents);
    if (w) *w = extents.width;
    if (h) *h = extents.height;
}

/* ===== 消息框（X11 没有原生消息框，使用 fprintf 输出到控制台） ===== */

/* 显示消息框：type 0=信息 1=警告 2=错误 */
int leno_gui_platform_show_message_box(const char* title, const char* message, int type) {
    const char* type_str;
    switch (type) {
        case 1:  type_str = "警告"; break;
        case 2:  type_str = "错误"; break;
        default: type_str = "信息"; break;
    }
    fprintf(stderr, "[%s] %s: %s\n", type_str, title ? title : "", message ? message : "");
    return 0;
}

/* ===== 文件对话框（参考 SDL3 SDL_zenitydialog.c：使用 Zenity 子进程） ===== */

/* 文件对话框参数，传递给工作线程 */
typedef struct {
    int type;
    int allow_many;
    int nfilters;
    char** filter_names;
    char** filter_patterns;
    char* default_path;
    char* title;
} FileDialogArgs;

/* 文件对话框结果（通过临界区保护，从工作线程传递到主线程） */
typedef struct {
    char** files;
    int nfiles;
    int filter_index;
} FileDialogResult;

static FileDialogResult* g_filedlg_result = NULL;
static pthread_mutex_t g_filedlg_result_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 释放文件对话框结果 */
static void filedlg_result_free(FileDialogResult* r) {
    if (!r) return;
    if (r->files) {
        for (int i = 0; i < r->nfiles; i++) free(r->files[i]);
        free(r->files);
    }
    free(r);
}

/* 构建 Zenity 命令行并在工作线程中执行 */
static void* file_dialog_thread_proc(void* param) {
    FileDialogArgs* args = (FileDialogArgs*)param;
    char** files = NULL;
    int nfiles = 0;

    /* 构建 Zenity 命令 */
    /* 基础命令和类型 */
    char cmd[4096] = {0};
    int cmd_len = 0;

    /* 检查 zenity 是否存在 */
    FILE* test = popen("which zenity 2>/dev/null", "r");
    if (!test) goto done;
    char test_buf[256] = {0};
    if (!fgets(test_buf, sizeof(test_buf), test)) {
        pclose(test);
        goto done;
    }
    pclose(test);
    if (test_buf[0] == '\0') goto done;

    /* 构建命令：zenity --file-selection */
    cmd_len = snprintf(cmd, sizeof(cmd), "zenity --file-selection");

    /* 标题 */
    if (args->title && args->title[0]) {
        cmd_len += snprintf(cmd + cmd_len, sizeof(cmd) - cmd_len,
                            " --title=\"%s\"", args->title);
    }

    /* 类型 */
    if (args->type == 1) { /* LENO_GUI_FILEDIALOG_SAVEFILE */
        cmd_len += snprintf(cmd + cmd_len, sizeof(cmd) - cmd_len, " --save --confirm-overwrite");
    } else if (args->type == 2) { /* LENO_GUI_FILEDIALOG_OPENFOLDER */
        cmd_len += snprintf(cmd + cmd_len, sizeof(cmd) - cmd_len, " --directory");
    }

    /* 多选 */
    if (args->allow_many && args->type == 0) {
        cmd_len += snprintf(cmd + cmd_len, sizeof(cmd) - cmd_len, " --multiple --separator=\"\\n\"");
    }

    /* 默认路径 */
    if (args->default_path && args->default_path[0]) {
        cmd_len += snprintf(cmd + cmd_len, sizeof(cmd) - cmd_len,
                            " --filename=\"%s\"", args->default_path);
    }

    /* 过滤器 */
    if (args->nfilters > 0 && args->filter_names && args->filter_patterns) {
        for (int i = 0; i < args->nfilters; i++) {
            if (args->filter_names[i] && args->filter_patterns[i]) {
                /* Zenity 格式: --file-filter="名称 | *.ext" */
                char filter_str[512];
                /* 把管道符替换成斜杠（Zenity 用 | 作为分隔符） */
                char* p = args->filter_names[i];
                while (*p) { if (*p == '|') *p = '/'; p++; }

                snprintf(filter_str, sizeof(filter_str), "%s | %s",
                         args->filter_names[i], args->filter_patterns[i]);
                cmd_len += snprintf(cmd + cmd_len, sizeof(cmd) - cmd_len,
                                    " --file-filter=\"%s\"", filter_str);
            }
        }
    }

    /* 执行 Zenity */
    FILE* fp = popen(cmd, "r");
    if (!fp) goto done;

    /* 读取输出（每行一个文件路径） */
    char line[4096];
    int cap = 8;
    files = (char**)calloc(cap + 1, sizeof(char*));
    if (!files) { pclose(fp); goto done; }

    while (fgets(line, sizeof(line), fp)) {
        /* 去除末尾换行符 */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;

        /* 扩展数组 */
        if (nfiles >= cap) {
            cap *= 2;
            char** new_files = (char**)realloc(files, (cap + 1) * sizeof(char*));
            if (!new_files) { pclose(fp); goto done; }
            files = new_files;
        }

        files[nfiles] = strdup(line);
        if (!files[nfiles]) { pclose(fp); goto done; }
        nfiles++;
    }
    files[nfiles] = NULL;
    pclose(fp);

done:
    /* 将结果通过互斥锁传递到主线程 */
    pthread_mutex_lock(&g_filedlg_result_mutex);

    /* 释放旧结果 */
    if (g_filedlg_result) {
        filedlg_result_free(g_filedlg_result);
        g_filedlg_result = NULL;
    }

    /* 存储新结果 */
    if (nfiles > 0 && files) {
        g_filedlg_result = (FileDialogResult*)calloc(1, sizeof(FileDialogResult));
        if (g_filedlg_result) {
            g_filedlg_result->files = files;
            g_filedlg_result->nfiles = nfiles;
            g_filedlg_result->filter_index = -1; /* Zenity 不支持过滤器索引 */
            files = NULL; /* 转移所有权 */
        }
    }

    pthread_mutex_unlock(&g_filedlg_result_mutex);

    /* 推送事件唤醒主循环 */
    {
        LenoGUIEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = LENO_GUI_EVT_FILEDIALOG_RESULT;
        event_queue_push(&ev);
    }

    /* 清理 */
    if (files) {
        for (int i = 0; i < nfiles; i++) free(files[i]);
        free(files);
    }
    if (args->filter_names) {
        for (int i = 0; i < args->nfilters; i++) free(args->filter_names[i]);
        free(args->filter_names);
    }
    if (args->filter_patterns) {
        for (int i = 0; i < args->nfilters; i++) free(args->filter_patterns[i]);
        free(args->filter_patterns);
    }
    free(args->default_path);
    free(args->title);
    free(args);

    return NULL;
}

void leno_gui_platform_show_file_dialog(int type, LenoGUIFileDialogCallback callback,
                                         void* userdata, LenoGUIPlatformWindow* win,
                                         const LenoGUIFileFilter* filters, int nfilters,
                                         const char* default_path, int allow_many,
                                         const char* title) {
    (void)callback; (void)userdata; (void)win;

    FileDialogArgs* args = (FileDialogArgs*)calloc(1, sizeof(FileDialogArgs));
    if (!args) {
        LenoGUIEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = LENO_GUI_EVT_FILEDIALOG_RESULT;
        event_queue_push(&ev);
        return;
    }

    args->type = type;
    args->allow_many = allow_many;
    args->nfilters = nfilters;
    args->default_path = default_path ? strdup(default_path) : NULL;
    args->title = title ? strdup(title) : NULL;

    /* 复制过滤器 */
    if (filters && nfilters > 0) {
        args->filter_names = (char**)calloc(nfilters, sizeof(char*));
        args->filter_patterns = (char**)calloc(nfilters, sizeof(char*));
        if (args->filter_names && args->filter_patterns) {
            for (int i = 0; i < nfilters; i++) {
                args->filter_names[i] = filters[i].name ? strdup(filters[i].name) : NULL;
                args->filter_patterns[i] = filters[i].pattern ? strdup(filters[i].pattern) : NULL;
            }
        }
    }

    /* 创建工作线程运行对话框 */
    pthread_t thread;
    if (pthread_create(&thread, NULL, file_dialog_thread_proc, args) != 0) {
        /* 线程创建失败，清理并推送空事件 */
        if (args->filter_names) {
            for (int i = 0; i < nfilters; i++) free(args->filter_names[i]);
            free(args->filter_names);
        }
        if (args->filter_patterns) {
            for (int i = 0; i < nfilters; i++) free(args->filter_patterns[i]);
            free(args->filter_patterns);
        }
        free(args->default_path);
        free(args->title);
        free(args);
        LenoGUIEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = LENO_GUI_EVT_FILEDIALOG_RESULT;
        event_queue_push(&ev);
        return;
    }
    pthread_detach(thread);
}

#include "guis_internal.h"

int leno_gui_platform_process_filedialog_result(void) {
    int processed = 0;

    pthread_mutex_lock(&g_filedlg_result_mutex);
    FileDialogResult* result = g_filedlg_result;
    g_filedlg_result = NULL;
    pthread_mutex_unlock(&g_filedlg_result_mutex);

    if (result) {
        /* 构建 C 字符串数组 */
        const char** c_files = (const char**)calloc((size_t)(result->nfiles + 1), sizeof(const char*));
        if (c_files) {
            for (int i = 0; i < result->nfiles; i++) {
                c_files[i] = result->files[i];
            }
            c_files[result->nfiles] = NULL;

            /* 在主线程中调用 Leno 回调 */
            process_filedialog_callback(c_files, result->nfiles, result->filter_index);

            free(c_files);
        }

        filedlg_result_free(result);
        processed = 1;
    }

    return processed;
}

/* ===== 高精度计时器（参考 SDL3 SDL_GetTicks / SDL_GetPerformanceCounter） ===== */

/* 获取自系统启动以来的毫秒数（使用 clock_gettime CLOCK_MONOTONIC） */
uint64_t leno_gui_platform_get_ticks(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* 获取高精度性能计数器值 */
uint64_t leno_gui_platform_get_performance_counter(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* 获取高精度性能计数器频率（纳秒级，1GHz） */
uint64_t leno_gui_platform_get_performance_frequency(void) {
    return 1000000000ULL;
}

/* 延迟指定毫秒数（使用 usleep） */
void leno_gui_platform_delay(uint32_t ms) {
    usleep(ms * 1000);
}

/* ===== 显示器 DPI 查询（参考 SDL3 SDL_x11modes.c XDisplayWidthMM） ===== */

/* 获取主显示器 DPI 缩放值 */
float leno_gui_platform_get_display_dpi(void) {
    if (!g_display) return 96.0f;

    /* 使用 XDisplayWidthMM 获取屏幕物理宽度（毫米） */
    int width_px = DisplayWidth(g_display, g_screen);
    int width_mm = DisplayWidthMM(g_display, g_screen);

    if (width_mm <= 0) return 96.0f;

    /* DPI = 像素数 / (毫米数 / 25.4) */
    float dpi = (float)width_px / ((float)width_mm / 25.4f);
    return dpi;
}

#endif /* __linux__ */
