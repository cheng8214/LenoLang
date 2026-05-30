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

/* ===== 事件队列 ===== */

#define MAX_GUI_EVENTS 512

static LenoGUIEvent g_event_queue[MAX_GUI_EVENTS];
static int g_event_head = 0;
static int g_event_tail = 0;
static int g_event_count = 0;

static pthread_mutex_t g_event_mutex = PTHREAD_MUTEX_INITIALIZER;

static void event_queue_push(const LenoGUIEvent* ev) {
    pthread_mutex_lock(&g_event_mutex);
    if (g_event_count < MAX_GUI_EVENTS) {
        g_event_queue[g_event_tail] = *ev;
        g_event_tail = (g_event_tail + 1) % MAX_GUI_EVENTS;
        g_event_count++;
    }
    pthread_mutex_unlock(&g_event_mutex);
}

static int event_queue_pop(LenoGUIEvent* ev) {
    pthread_mutex_lock(&g_event_mutex);
    if (g_event_count == 0) {
        pthread_mutex_unlock(&g_event_mutex);
        return 0;
    }
    *ev = g_event_queue[g_event_head];
    g_event_head = (g_event_head + 1) % MAX_GUI_EVENTS;
    g_event_count--;
    pthread_mutex_unlock(&g_event_mutex);
    return 1;
}

/* ===== 窗口 ID 计数器 ===== */

static int g_next_window_id = 1;

/* ===== 全局 X11 连接 ===== */

static Display* g_display = NULL;
static int g_screen = 0;
static Atom g_wm_delete_window = None;
static int g_gui_initialized = 0;

/* ===== 平台窗口结构 ===== */

struct LenoGUIPlatformWindow {
    Window xwindow;
    int window_id;
    int width;
    int height;
    int should_close;
    int is_fullscreen;
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
};

/* ===== 平台纹理结构 ===== */

struct LenoGUIPlatformTexture {
    uint32_t* pixels;
    int width;
    int height;
    int pitch;
};

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

/* ===== 软件渲染辅助函数（与 win32 共用逻辑，接受渲染器指针） ===== */

/* 绘制单个像素点，应用视口偏移和裁剪矩形 */
static void sw_draw_point(LenoGUIPlatformRenderer* ren, int x, int y, uint32_t color) {
    /* 逻辑坐标转物理坐标（加上视口偏移） */
    int px = x + ren->vp_x;
    int py = y + ren->vp_y;
    /* 视口边界裁剪 */
    if (px < ren->vp_x || px >= ren->vp_x + ren->vp_w) return;
    if (py < ren->vp_y || py >= ren->vp_y + ren->vp_h) return;
    /* 裁剪矩形检查（仅在启用时） */
    if (ren->clip_enabled) {
        if (px < ren->clip_x || px >= ren->clip_x + ren->clip_w) return;
        if (py < ren->clip_y || py >= ren->clip_y + ren->clip_h) return;
    }
    /* 缓冲区边界保护 */
    if (px >= 0 && px < ren->width && py >= 0 && py < ren->height) {
        ren->pixels[py * ren->width + px] = color;
    }
}

/* Bresenham 直线算法，逐点调用 sw_draw_point 自动裁剪 */
static void sw_draw_line(LenoGUIPlatformRenderer* ren, int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        sw_draw_point(ren, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* 绘制矩形边框（四条边） */
static void sw_draw_rect(LenoGUIPlatformRenderer* ren, int x, int y, int w, int h, uint32_t color) {
    for (int i = 0; i < w; i++) {
        sw_draw_point(ren, x + i, y, color);
        sw_draw_point(ren, x + i, y + h - 1, color);
    }
    for (int i = 0; i < h; i++) {
        sw_draw_point(ren, x, y + i, color);
        sw_draw_point(ren, x + w - 1, y + i, color);
    }
}

/* 填充矩形（优化：计算裁剪区域后批量写入像素行） */
static void sw_fill_rect(LenoGUIPlatformRenderer* ren, int x, int y, int w, int h, uint32_t color) {
    /* 逻辑坐标转物理坐标 */
    int px = x + ren->vp_x;
    int py = y + ren->vp_y;
    /* 计算有效绘制区域（与视口求交集） */
    int x1 = px > ren->vp_x ? px : ren->vp_x;
    int y1 = py > ren->vp_y ? py : ren->vp_y;
    int x2 = (px + w) < (ren->vp_x + ren->vp_w) ? (px + w) : (ren->vp_x + ren->vp_w);
    int y2 = (py + h) < (ren->vp_y + ren->vp_h) ? (py + h) : (ren->vp_y + ren->vp_h);
    /* 裁剪矩形进一步限制 */
    if (ren->clip_enabled) {
        if (x1 < ren->clip_x) x1 = ren->clip_x;
        if (y1 < ren->clip_y) y1 = ren->clip_y;
        if (x2 > ren->clip_x + ren->clip_w) x2 = ren->clip_x + ren->clip_w;
        if (y2 > ren->clip_y + ren->clip_h) y2 = ren->clip_y + ren->clip_h;
    }
    /* 缓冲区边界保护 */
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > ren->width) x2 = ren->width;
    if (y2 > ren->height) y2 = ren->height;
    if (x1 >= x2 || y1 >= y2) return;
    /* 逐行批量写入 */
    for (int row = y1; row < y2; row++) {
        uint32_t* row_ptr = ren->pixels + row * ren->width;
        for (int col = x1; col < x2; col++) {
            row_ptr[col] = color;
        }
    }
}

/* 纹理 blit（支持 Alpha 混合），应用视口偏移和裁剪 */
static void sw_blit_texture(LenoGUIPlatformRenderer* ren,
                             const uint32_t* src, int sw_val, int sh_val, int spitch,
                             int dx, int dy) {
    for (int y = 0; y < sh_val; y++) {
        int py = dy + y + ren->vp_y;
        /* 视口裁剪 */
        if (py < ren->vp_y || py >= ren->vp_y + ren->vp_h) continue;
        /* 裁剪矩形 */
        if (ren->clip_enabled && (py < ren->clip_y || py >= ren->clip_y + ren->clip_h)) continue;
        /* 缓冲区边界 */
        if (py < 0 || py >= ren->height) continue;
        for (int x = 0; x < sw_val; x++) {
            int px = dx + x + ren->vp_x;
            if (px < ren->vp_x || px >= ren->vp_x + ren->vp_w) continue;
            if (ren->clip_enabled && (px < ren->clip_x || px >= ren->clip_x + ren->clip_w)) continue;
            if (px < 0 || px >= ren->width) continue;
            uint32_t src_pixel = src[y * (spitch / 4) + x];
            uint8_t sa = (src_pixel >> 24) & 0xFF;
            if (sa == 0) continue;
            if (sa == 255) {
                ren->pixels[py * ren->width + px] = src_pixel;
            } else {
                /* Alpha 混合 */
                uint32_t dst_pixel = ren->pixels[py * ren->width + px];
                uint8_t dr = (dst_pixel >> 16) & 0xFF;
                uint8_t dg = (dst_pixel >> 8) & 0xFF;
                uint8_t db = dst_pixel & 0xFF;
                uint8_t sr = (src_pixel >> 16) & 0xFF;
                uint8_t sg = (src_pixel >> 8) & 0xFF;
                uint8_t sb = src_pixel & 0xFF;
                uint8_t inv_a = 255 - sa;
                dr = (uint8_t)((sr * sa + dr * inv_a) / 255);
                dg = (uint8_t)((sg * sa + dg * inv_a) / 255);
                db = (uint8_t)((sb * sa + db * inv_a) / 255);
                ren->pixels[py * ren->width + px] = LENO_GUI_PIXEL(dr, dg, db, 255);
            }
        }
    }
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

    XFlush(g_display);
    return win;
}

void leno_gui_platform_destroy_window(LenoGUIPlatformWindow* win) {
    if (!win) return;
    if (g_display && win->xwindow) {
        XDestroyWindow(g_display, win->xwindow);
        XFlush(g_display);
    }
    free(win);
}

void leno_gui_platform_show_window(LenoGUIPlatformWindow* win) {
    if (!win || !g_display || !win->xwindow) return;
    XMapWindow(g_display, win->xwindow);
    XFlush(g_display);
}

void leno_gui_platform_hide_window(LenoGUIPlatformWindow* win) {
    if (!win || !g_display || !win->xwindow) return;
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

void leno_gui_platform_get_window_size(LenoGUIPlatformWindow* win, int* w, int* h) {
    if (!win) { if (w) *w = 0; if (h) *h = 0; return; }
    if (w) *w = win->width;
    if (h) *h = win->height;
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

int leno_gui_platform_window_should_close(LenoGUIPlatformWindow* win) {
    return win ? win->should_close : 1;
}

void leno_gui_platform_set_window_should_close(LenoGUIPlatformWindow* win, int val) {
    if (win) win->should_close = val;
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

void leno_gui_platform_render_clear(LenoGUIPlatformRenderer* ren) {
    if (!ren || !ren->pixels) return;
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    int total = ren->width * ren->height;
    for (int i = 0; i < total; i++) {
        ren->pixels[i] = color;
    }
}

void leno_gui_platform_render_present(LenoGUIPlatformRenderer* ren) {
    if (!ren || !ren->pixels || !g_display || !ren->window || !ren->window->xwindow) return;
    XPutImage(g_display, ren->window->xwindow, ren->gc, ren->ximage,
              0, 0, 0, 0, ren->width, ren->height);
    XFlush(g_display);
}

void leno_gui_platform_set_draw_color(LenoGUIPlatformRenderer* ren, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!ren) return;
    ren->draw_r = r;
    ren->draw_g = g;
    ren->draw_b = b;
    ren->draw_a = a;
}

void leno_gui_platform_render_draw_point(LenoGUIPlatformRenderer* ren, int x, int y) {
    if (!ren || !ren->pixels) return;
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    sw_draw_point(ren, x, y, color);
}

void leno_gui_platform_render_draw_line(LenoGUIPlatformRenderer* ren, int x1, int y1, int x2, int y2) {
    if (!ren || !ren->pixels) return;
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    sw_draw_line(ren, x1, y1, x2, y2, color);
}

void leno_gui_platform_render_draw_rect(LenoGUIPlatformRenderer* ren, int x, int y, int w, int h) {
    if (!ren || !ren->pixels) return;
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    sw_draw_rect(ren, x, y, w, h, color);
}

void leno_gui_platform_render_fill_rect(LenoGUIPlatformRenderer* ren, int x, int y, int w, int h) {
    if (!ren || !ren->pixels) return;
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    sw_fill_rect(ren, x, y, w, h, color);
}

void leno_gui_platform_get_renderer_size(LenoGUIPlatformRenderer* ren, int* w, int* h) {
    if (!ren) { if (w) *w = 0; if (h) *h = 0; return; }
    if (w) *w = ren->width;
    if (h) *h = ren->height;
}

/* ===== 纹理 ===== */

LenoGUIPlatformTexture* leno_gui_platform_create_texture(LenoGUIPlatformRenderer* ren, int w, int h) {
    (void)ren;
    LenoGUIPlatformTexture* tex = (LenoGUIPlatformTexture*)calloc(1, sizeof(LenoGUIPlatformTexture));
    if (!tex) return NULL;
    tex->width = w;
    tex->height = h;
    tex->pitch = w * 4;
    tex->pixels = (uint32_t*)calloc(w * h, sizeof(uint32_t));
    if (!tex->pixels) {
        free(tex);
        return NULL;
    }
    return tex;
}

void leno_gui_platform_destroy_texture(LenoGUIPlatformTexture* tex) {
    if (!tex) return;
    if (tex->pixels) free(tex->pixels);
    free(tex);
}

void leno_gui_platform_render_texture(LenoGUIPlatformRenderer* ren, LenoGUIPlatformTexture* tex, int x, int y) {
    if (!ren || !ren->pixels || !tex || !tex->pixels) return;
    sw_blit_texture(ren, tex->pixels, tex->width, tex->height, tex->pitch, x, y);
}

void leno_gui_platform_update_texture(LenoGUIPlatformTexture* tex, const void* data, int pitch) {
    if (!tex || !tex->pixels || !data) return;
    const uint32_t* src = (const uint32_t*)data;
    for (int y = 0; y < tex->height; y++) {
        memcpy(tex->pixels + y * tex->width, (const uint8_t*)src + y * pitch, tex->width * 4);
    }
}

int leno_gui_platform_texture_width(LenoGUIPlatformTexture* tex) {
    return tex ? tex->width : 0;
}

int leno_gui_platform_texture_height(LenoGUIPlatformTexture* tex) {
    return tex ? tex->height : 0;
}

/* ===== 画圆（Bresenham 中点圆算法，与 win32 实现一致） ===== */

void leno_gui_platform_render_draw_circle(LenoGUIPlatformRenderer* ren, int cx, int cy, int radius) {
    if (!ren || !ren->pixels || radius <= 0) return;
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    /* Bresenham 中点圆算法：利用八分对称性只计算 1/8 圆弧 */
    int x = 0, y = radius;
    int d = 3 - 2 * radius;
    while (x <= y) {
        /* 八分对称绘制 8 个点 */
        sw_draw_point(ren, cx + x, cy + y, color);
        sw_draw_point(ren, cx - x, cy + y, color);
        sw_draw_point(ren, cx + x, cy - y, color);
        sw_draw_point(ren, cx - x, cy - y, color);
        sw_draw_point(ren, cx + y, cy + x, color);
        sw_draw_point(ren, cx - y, cy + x, color);
        sw_draw_point(ren, cx + y, cy - x, color);
        sw_draw_point(ren, cx - y, cy - x, color);
        if (d < 0) {
            d += 4 * x + 6;
        } else {
            d += 4 * (x - y) + 10;
            y--;
        }
        x++;
    }
}

/* 填充圆（水平扫描线填充，与 win32 实现一致） */
void leno_gui_platform_render_fill_circle(LenoGUIPlatformRenderer* ren, int cx, int cy, int radius) {
    if (!ren || !ren->pixels || radius <= 0) return;
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    /* 使用 Bresenham 算法计算圆的边界，然后水平扫描线填充 */
    int x = 0, y = radius;
    int d = 3 - 2 * radius;
    while (x <= y) {
        /* 每对对称点之间画水平线 */
        sw_draw_line(ren, cx - x, cy + y, cx + x, cy + y, color);
        sw_draw_line(ren, cx - x, cy - y, cx + x, cy - y, color);
        sw_draw_line(ren, cx - y, cy + x, cx + y, cy + x, color);
        sw_draw_line(ren, cx - y, cy - x, cx + y, cy - x, color);
        if (d < 0) {
            d += 4 * x + 6;
        } else {
            d += 4 * (x - y) + 10;
            y--;
        }
        x++;
    }
}

/* 绘制圆角矩形边框（与 win32 实现一致） */
void leno_gui_platform_render_draw_rounded_rect(LenoGUIPlatformRenderer* ren, int x, int y, int w, int h, int radius) {
    if (!ren || !ren->pixels || w <= 0 || h <= 0) return;
    /* 圆角半径不能超过短边的一半 */
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    if (radius <= 0) {
        leno_gui_platform_render_draw_rect(ren, x, y, w, h);
        return;
    }
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    /* 四条直线边 */
    sw_draw_line(ren, x + radius, y, x + w - 1 - radius, y, color);
    sw_draw_line(ren, x + radius, y + h - 1, x + w - 1 - radius, y + h - 1, color);
    sw_draw_line(ren, x, y + radius, x, y + h - 1 - radius, color);
    sw_draw_line(ren, x + w - 1, y + radius, x + w - 1, y + h - 1 - radius, color);
    /* 四个圆角（四分之一圆弧） */
    int cx1 = x + radius, cy1 = y + radius;
    int cx2 = x + w - 1 - radius, cy2 = y + radius;
    int cx3 = x + radius, cy3 = y + h - 1 - radius;
    int cx4 = x + w - 1 - radius, cy4 = y + h - 1 - radius;
    int ax = 0, ay = radius;
    int dd = 3 - 2 * radius;
    while (ax <= ay) {
        sw_draw_point(ren, cx1 - ax, cy1 - ay, color);
        sw_draw_point(ren, cx1 - ay, cy1 - ax, color);
        sw_draw_point(ren, cx2 + ax, cy2 - ay, color);
        sw_draw_point(ren, cx2 + ay, cy2 - ax, color);
        sw_draw_point(ren, cx3 - ax, cy3 + ay, color);
        sw_draw_point(ren, cx3 - ay, cy3 + ax, color);
        sw_draw_point(ren, cx4 + ax, cy4 + ay, color);
        sw_draw_point(ren, cx4 + ay, cy4 + ax, color);
        if (dd < 0) {
            dd += 4 * ax + 6;
        } else {
            dd += 4 * (ax - ay) + 10;
            ay--;
        }
        ax++;
    }
}

/* 填充圆角矩形（与 win32 实现一致） */
void leno_gui_platform_render_fill_rounded_rect(LenoGUIPlatformRenderer* ren, int x, int y, int w, int h, int radius) {
    if (!ren || !ren->pixels || w <= 0 || h <= 0) return;
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    if (radius <= 0) {
        leno_gui_platform_render_fill_rect(ren, x, y, w, h);
        return;
    }
    /* 中间矩形区域直接填充 */
    leno_gui_platform_render_fill_rect(ren, x, y + radius, w, h - 2 * radius);
    /* 上下两条矩形条 */
    leno_gui_platform_render_fill_rect(ren, x + radius, y, w - 2 * radius, radius);
    leno_gui_platform_render_fill_rect(ren, x + radius, y + h - radius, w - 2 * radius, radius);
    /* 四个圆角用扫描线填充 */
    int ax = 0, ay = radius;
    int dd = 3 - 2 * radius;
    while (ax <= ay) {
        /* 左上角 */
        sw_draw_line(ren, x + radius - ay, y + radius - ax, x + radius - 1, y + radius - ax, LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a));
        sw_draw_line(ren, x + radius - ax, y + radius - ay, x + radius - 1, y + radius - ay, LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a));
        /* 右上角 */
        sw_draw_line(ren, x + w - radius, y + radius - ax, x + w - radius + ay - 1, y + radius - ax, LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a));
        sw_draw_line(ren, x + w - radius, y + radius - ay, x + w - radius + ax - 1, y + radius - ay, LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a));
        /* 左下角 */
        sw_draw_line(ren, x + radius - ay, y + h - radius + ax, x + radius - 1, y + h - radius + ax, LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a));
        sw_draw_line(ren, x + radius - ax, y + h - radius + ay, x + radius - 1, y + h - radius + ay, LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a));
        /* 右下角 */
        sw_draw_line(ren, x + w - radius, y + h - radius + ax, x + w - radius + ay - 1, y + h - radius + ax, LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a));
        sw_draw_line(ren, x + w - radius, y + h - radius + ay, x + w - radius + ax - 1, y + h - radius + ay, LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a));
        if (dd < 0) {
            dd += 4 * ax + 6;
        } else {
            dd += 4 * (ax - ay) + 10;
            ay--;
        }
        ax++;
    }
}

/* ===== 视口和裁剪（参考 SDL3 SDL_SetRenderViewport / SDL_SetRenderClipRect） ===== */

/* 设置渲染视口：所有后续绘制坐标相对于视口左上角 */
void leno_gui_platform_set_viewport(LenoGUIPlatformRenderer* ren, int x, int y, int w, int h) {
    if (!ren) return;
    ren->vp_x = x;
    ren->vp_y = y;
    ren->vp_w = w > 0 ? w : ren->width;
    ren->vp_h = h > 0 ? h : ren->height;
}

/* 获取当前视口设置 */
void leno_gui_platform_get_viewport(LenoGUIPlatformRenderer* ren, int* x, int* y, int* w, int* h) {
    if (!ren) { if (x) *x = 0; if (y) *y = 0; if (w) *w = 0; if (h) *h = 0; return; }
    if (x) *x = ren->vp_x;
    if (y) *y = ren->vp_y;
    if (w) *w = ren->vp_w;
    if (h) *h = ren->vp_h;
}

/* 设置裁剪矩形：限制绘制区域（物理坐标） */
void leno_gui_platform_set_clip_rect(LenoGUIPlatformRenderer* ren, int x, int y, int w, int h) {
    if (!ren) return;
    ren->clip_x = x;
    ren->clip_y = y;
    ren->clip_w = w;
    ren->clip_h = h;
    ren->clip_enabled = 1;
}

/* 获取当前裁剪矩形 */
void leno_gui_platform_get_clip_rect(LenoGUIPlatformRenderer* ren, int* x, int* y, int* w, int* h) {
    if (!ren) { if (x) *x = 0; if (y) *y = 0; if (w) *w = 0; if (h) *h = 0; return; }
    if (x) *x = ren->clip_x;
    if (y) *y = ren->clip_y;
    if (w) *w = ren->clip_w;
    if (h) *h = ren->clip_h;
}

/* 禁用裁剪矩形 */
void leno_gui_platform_disable_clip_rect(LenoGUIPlatformRenderer* ren) {
    if (!ren) return;
    ren->clip_enabled = 0;
}

/* ===== 纹理高级渲染 ===== */

/* 纹理源矩形渲染：从纹理中取子区域绘制到目标位置（可缩放） */
void leno_gui_platform_render_texture_src(LenoGUIPlatformRenderer* ren, LenoGUIPlatformTexture* tex,
                                          int sx, int sy, int sw_val, int sh_val, int dx, int dy, int dw, int dh) {
    if (!ren || !ren->pixels || !tex || !tex->pixels) return;
    if (sw_val <= 0 || sh_val <= 0 || dw <= 0 || dh <= 0) return;
    /* 计算缩放比例 */
    float x_scale = (float)sw_val / (float)dw;
    float y_scale = (float)sh_val / (float)dh;
    int src_pitch_int = tex->pitch / 4;
    /* 逐像素采样（最近邻插值） */
    for (int row = 0; row < dh; row++) {
        int src_y = sy + (int)(row * y_scale);
        if (src_y < 0 || src_y >= tex->height) continue;
        for (int col = 0; col < dw; col++) {
            int src_x = sx + (int)(col * x_scale);
            if (src_x < 0 || src_x >= tex->width) continue;
            uint32_t src_pixel = tex->pixels[src_y * src_pitch_int + src_x];
            uint8_t sa = (src_pixel >> 24) & 0xFF;
            if (sa == 0) continue;
            /* 使用 sw_draw_point 自动处理视口和裁剪 */
            if (sa == 255) {
                int px = dx + col + ren->vp_x;
                int py = dy + row + ren->vp_y;
                if (px >= ren->vp_x && px < ren->vp_x + ren->vp_w &&
                    py >= ren->vp_y && py < ren->vp_y + ren->vp_h) {
                    if (!ren->clip_enabled || (px >= ren->clip_x && px < ren->clip_x + ren->clip_w &&
                                                py >= ren->clip_y && py < ren->clip_y + ren->clip_h)) {
                        if (px >= 0 && px < ren->width && py >= 0 && py < ren->height) {
                            ren->pixels[py * ren->width + px] = src_pixel;
                        }
                    }
                }
            } else {
                sw_draw_point(ren, dx + col, dy + row, src_pixel);
            }
        }
    }
}

/* 纹理旋转/翻转渲染（参考 SDL3 SDL_RenderTextureRotated） */
void leno_gui_platform_render_texture_rotated(LenoGUIPlatformRenderer* ren, LenoGUIPlatformTexture* tex,
                                               int x, int y, double angle, int flip) {
    if (!ren || !ren->pixels || !tex || !tex->pixels) return;
    /* 角度转弧度 */
    double rad = angle * 3.14159265358979323846 / 180.0;
    double cos_a = cos(rad);
    double sin_a = sin(rad);
    int tw = tex->width;
    int th = tex->height;
    /* 旋转中心为纹理中心 */
    double cx = tw / 2.0;
    double cy = th / 2.0;
    int src_pitch_int = tex->pitch / 4;
    /* 遍历目标区域（包围盒），反向映射到源纹理 */
    for (int dy = -th; dy <= th; dy++) {
        for (int dx = -tw; dx <= tw; dx++) {
            /* 应用翻转 */
            double fx = dx, fy = dy;
            if (flip & LENO_GUI_FLIP_HORIZONTAL) fx = -fx;
            if (flip & LENO_GUI_FLIP_VERTICAL) fy = -fy;
            /* 反向旋转：从目标坐标计算源坐标 */
            double src_x = cos_a * fx + sin_a * fy + cx;
            double src_y = -sin_a * fx + cos_a * fy + cy;
            /* 最近邻采样 */
            int isx = (int)(src_x + 0.5);
            int isy = (int)(src_y + 0.5);
            if (isx < 0 || isx >= tw || isy < 0 || isy >= th) continue;
            uint32_t src_pixel = tex->pixels[isy * src_pitch_int + isx];
            uint8_t sa = (src_pixel >> 24) & 0xFF;
            if (sa == 0) continue;
            /* 绘制到目标位置（加上旋转中心偏移） */
            sw_draw_point(ren, x + dx, y + dy, src_pixel);
        }
    }
}

/* ===== 事件 ===== */

static void pump_x11_events(void) {
    if (!g_display) return;
    while (XPending(g_display) > 0) {
        XEvent xev;
        XNextEvent(g_display, &xev);

        LenoGUIPlatformWindow* win = NULL;
        if (xev.xany.window) {
            /* 查找匹配的窗口 - 简化实现 */
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
                break;
            }
            case ButtonPress: {
                if (xev.xbutton.button == 4 || xev.xbutton.button == 5) {
                    ev.type = LENO_GUI_EVT_MOUSE_WHEEL;
                    ev.mouse_x = (float)xev.xbutton.x;
                    ev.mouse_y = (float)xev.xbutton.y;
                    ev.wheel_y = (xev.xbutton.button == 4) ? 1.0f : -1.0f;
                    ev.wheel_x = 0.0f;
                } else {
                    ev.type = LENO_GUI_EVT_MOUSE_DOWN;
                    ev.mouse_x = (float)xev.xbutton.x;
                    ev.mouse_y = (float)xev.xbutton.y;
                    ev.mouse_button = (int)xev.xbutton.button;
                    ev.mouse_clicks = 1;
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
                break;
            }
            case FocusIn: {
                ev.type = LENO_GUI_EVT_WINDOW_FOCUS;
                event_queue_push(&ev);
                break;
            }
            case FocusOut: {
                ev.type = LENO_GUI_EVT_WINDOW_UNFOCUS;
                event_queue_push(&ev);
                break;
            }
            case MapNotify: {
                ev.type = LENO_GUI_EVT_WINDOW_SHOW;
                event_queue_push(&ev);
                break;
            }
            case UnmapNotify: {
                ev.type = LENO_GUI_EVT_WINDOW_HIDE;
                event_queue_push(&ev);
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
