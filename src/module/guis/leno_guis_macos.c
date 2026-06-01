/* Leno GUI - macOS 平台实现 (Cocoa)
 * 使用 Objective-C runtime + NSWindow/NSView
 *
 * 窗口管理: NSWindow + NSView
 * 渲染: NSBitmapImageRep 像素缓冲区 + NSView display
 * 事件: NSEvent 轮询 + 全局事件队列
 *
 * 注意: 本文件使用 Objective-C runtime API (objc_msgSend 等)
 *       以便在纯 .c 文件中调用 Cocoa API
 */

#include "leno_guis.h"

#ifdef __APPLE__

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <pthread.h>
#include <objc/objc.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <CoreGraphics/CGGeometry.h>
#include <CoreGraphics/CGEvent.h>
#include <mach/mach_time.h>

/* ===== 事件队列 ===== */

#define MAX_GUI_EVENTS 512

static LenoGUIEvent g_event_queue[MAX_GUI_EVENTS];
static int g_event_head = 0;
static int g_event_tail = 0;
static int g_event_count = 0;

static pthread_mutex_t g_event_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 向事件队列推入一个事件 */
static void event_queue_push(const LenoGUIEvent* ev) {
    pthread_mutex_lock(&g_event_mutex);
    if (g_event_count < MAX_GUI_EVENTS) {
        g_event_queue[g_event_tail] = *ev;
        g_event_tail = (g_event_tail + 1) % MAX_GUI_EVENTS;
        g_event_count++;
    }
    pthread_mutex_unlock(&g_event_mutex);
}

/* 从事件队列弹出一个事件，返回 1 表示成功，0 表示队列为空 */
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

/* ===== 平台窗口结构 ===== */

struct LenoGUIPlatformWindow {
    id ns_window;
    id ns_view;
    int window_id;
    int width;
    int height;
    int should_close;
    int is_fullscreen;
    int is_borderless;
    int drag_area_enabled;
    int drag_area_x;
    int drag_area_y;
    int drag_area_w;
    int drag_area_h;
};

/* ===== 平台渲染器结构（包含视口/裁剪字段） ===== */

struct LenoGUIPlatformRenderer {
    LenoGUIPlatformWindow* window;
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

/* ===== macOS 键码到 Leno 键码映射 ===== */

static int macos_keycode_to_leno(unsigned short keycode) {
    switch (keycode) {
        case 36: return LENO_GUI_KEY_RETURN;
        case 53: return LENO_GUI_KEY_ESCAPE;
        case 51: return LENO_GUI_KEY_BACKSPACE;
        case 48: return LENO_GUI_KEY_TAB;
        case 49: return LENO_GUI_KEY_SPACE;
        case 117: return LENO_GUI_KEY_DELETE;
        case 123: return LENO_GUI_KEY_LEFT;
        case 124: return LENO_GUI_KEY_RIGHT;
        case 126: return LENO_GUI_KEY_UP;
        case 125: return LENO_GUI_KEY_DOWN;
        case 122: return LENO_GUI_KEY_F1;
        case 120: return LENO_GUI_KEY_F2;
        case 99:  return LENO_GUI_KEY_F3;
        case 118: return LENO_GUI_KEY_F4;
        case 96:  return LENO_GUI_KEY_F5;
        case 97:  return LENO_GUI_KEY_F6;
        case 98:  return LENO_GUI_KEY_F7;
        case 100: return LENO_GUI_KEY_F8;
        case 101: return LENO_GUI_KEY_F9;
        case 109: return LENO_GUI_KEY_F10;
        case 103: return LENO_GUI_KEY_F11;
        case 111: return LENO_GUI_KEY_F12;
        case 56:  return LENO_GUI_KEY_LSHIFT;
        case 60:  return LENO_GUI_KEY_RSHIFT;
        case 59:  return LENO_GUI_KEY_LCTRL;
        case 62:  return LENO_GUI_KEY_RCTRL;
        case 58:  return LENO_GUI_KEY_LALT;
        case 61:  return LENO_GUI_KEY_RALT;
        case 57:  return LENO_GUI_KEY_CAPSLOCK;
        default:  return LENO_GUI_KEY_UNKNOWN;
    }
}

/* ===== 平台 API 实现 ===== */

/* 初始化 Cocoa GUI 子系统 */
int leno_gui_platform_init(void) {
    if (g_gui_initialized) return 1;

    id pool = objc_msgSend(objc_getClass("NSAutoreleasePool"), sel_registerName("new"));
    (void)pool;

    id app = objc_msgSend(objc_getClass("NSApplication"), sel_registerName("sharedApplication"));
    if (!app) return 0;

    objc_msgSend(app, sel_registerName("setActivationPolicy:"), 0);

    g_event_head = 0;
    g_event_tail = 0;
    g_event_count = 0;

    g_gui_initialized = 1;
    return 1;
}

/* 关闭 Cocoa GUI 子系统 */
void leno_gui_platform_quit(void) {
    if (!g_gui_initialized) return;
    g_gui_initialized = 0;
}

/* 创建平台窗口 */
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

    id ns_string = objc_msgSend(objc_getClass("NSString"),
                                 sel_registerName("stringWithUTF8String:"), title);

    unsigned int style_mask = 1 | 2 | 4 | 8;
    if (flags & LENO_GUI_WIN_BORDERLESS) {
        style_mask = 0;
        if (flags & LENO_GUI_WIN_RESIZABLE) {
            style_mask |= 4; /* NSWindowStyleMaskResizable */
        }
    }
    if (!(flags & LENO_GUI_WIN_RESIZABLE)) style_mask &= ~4;

    NSRect frame = {{0, 0}, {w, h}};
    id ns_window = objc_msgSend(objc_getClass("NSWindow"),
                                 sel_registerName("initWithContentRect:styleMask:backing:defer:"),
                                 frame, style_mask, 2, NO);
    if (!ns_window) {
        free(win);
        return NULL;
    }

    objc_msgSend(ns_window, sel_registerName("setTitle:"), ns_string);
    objc_msgSend(ns_window, sel_registerName("setMinSize:"), (NSRect){{0, 0}, {100, 100}});
    objc_msgSend(ns_window, sel_registerName("center"));

    id ns_view = objc_msgSend(ns_window, sel_registerName("contentView"));

    win->ns_window = ns_window;
    win->ns_view = ns_view;

    if (!(flags & LENO_GUI_WIN_HIDDEN)) {
        objc_msgSend(ns_window, sel_registerName("makeKeyAndOrderFront:"), nil);
    }

    if (flags & LENO_GUI_WIN_ALWAYS_ON_TOP) {
        objc_msgSend(ns_window, sel_registerName("setLevel:"), 3);
    }

    window_list_add(win);

    return win;
}

/* 销毁平台窗口 */
void leno_gui_platform_destroy_window(LenoGUIPlatformWindow* win) {
    if (!win) return;
    window_list_remove(win);
    if (win->ns_window) {
        objc_msgSend(win->ns_window, sel_registerName("close"));
    }
    free(win);
}

/* 显示窗口 */
void leno_gui_platform_show_window(LenoGUIPlatformWindow* win) {
    if (!win || !win->ns_window) return;
    objc_msgSend(win->ns_window, sel_registerName("makeKeyAndOrderFront:"), nil);
}

/* 隐藏窗口 */
void leno_gui_platform_hide_window(LenoGUIPlatformWindow* win) {
    if (!win || !win->ns_window) return;
    objc_msgSend(win->ns_window, sel_registerName("orderOut:"), nil);
}

/* 设置窗口标题 */
void leno_gui_platform_set_window_title(LenoGUIPlatformWindow* win, const char* title) {
    if (!win || !win->ns_window) return;
    id ns_string = objc_msgSend(objc_getClass("NSString"),
                                 sel_registerName("stringWithUTF8String:"), title);
    objc_msgSend(win->ns_window, sel_registerName("setTitle:"), ns_string);
}

/* 设置窗口客户区大小 */
void leno_gui_platform_set_window_size(LenoGUIPlatformWindow* win, int w, int h) {
    if (!win || !win->ns_window) return;
    NSRect frame = {{0, 0}, {w, h}};
    objc_msgSend(win->ns_window, sel_registerName("setContentSize:"), frame.size);
    win->width = w;
    win->height = h;
}

/* 设置窗口位置（屏幕坐标） */
void leno_gui_platform_set_window_position(LenoGUIPlatformWindow* win, int x, int y) {
    if (!win || !win->ns_window) return;
    objc_msgSend(win->ns_window, sel_registerName("setFrameOrigin:"), (NSPoint){{x, y}});
}

/* 获取窗口位置（屏幕坐标） */
void leno_gui_platform_get_window_position(LenoGUIPlatformWindow* win, int* x, int* y) {
    if (!win || !win->ns_window) { if (x) *x = 0; if (y) *y = 0; return; }
    NSRect frame = ((NSRect(*)(id, SEL))objc_msgSend)(win->ns_window, sel_registerName("frame"));
    if (x) *x = (int)frame.origin.x;
    if (y) *y = (int)frame.origin.y;
}

/* 设置窗口全屏状态 */
void leno_gui_platform_set_window_fullscreen(LenoGUIPlatformWindow* win, int fullscreen) {
    if (!win || !win->ns_window) return;
    objc_msgSend(win->ns_window, sel_registerName("setFullScreen:"), fullscreen);
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

/* 创建渲染器，初始化像素缓冲区和视口/裁剪默认值 */
LenoGUIPlatformRenderer* leno_gui_platform_create_renderer(LenoGUIPlatformWindow* win) {
    if (!win || !win->ns_window) return NULL;

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

    ren->pixels = (uint32_t*)calloc(ren->width * ren->height, sizeof(uint32_t));
    if (!ren->pixels) {
        free(ren);
        return NULL;
    }

    return ren;
}

/* 销毁渲染器，释放像素缓冲区 */
void leno_gui_platform_destroy_renderer(LenoGUIPlatformRenderer* ren) {
    if (!ren) return;
    if (ren->pixels) free(ren->pixels);
    free(ren);
}

/* 检查并自动调整渲染器大小（参考 SDL3 GetWindowSurface） */
static int check_and_resize_renderer(LenoGUIPlatformRenderer* ren) {
    if (!ren || !ren->window || !ren->window->ns_window) return 0;

    /* 获取窗口当前大小 */
    id content_view = ((id(*)(id, SEL))objc_msgSend)(ren->window->ns_window, sel_registerName("contentView"));
    if (!content_view) return 0;

    NSRect frame = ((NSRect(*)(id, SEL))objc_msgSend)(content_view, sel_registerName("frame"));
    int new_width = (int)frame.size.width;
    int new_height = (int)frame.size.height;

    /* 如果大小变化或需要调整，重新创建渲染器 */
    if (ren->needs_resize || new_width != ren->width || new_height != ren->height) {
        if (new_width > 0 && new_height > 0) {
            return leno_gui_platform_renderer_resize(ren, new_width, new_height);
        }
    }
    return 1;
}

/* 将像素缓冲区内容呈现到窗口（通过 NSBitmapImageRep 中转） */
void leno_gui_platform_render_present(LenoGUIPlatformRenderer* ren) {
    if (!ren || !ren->pixels || !ren->window || !ren->window->ns_view) return;

    id bitmap = objc_msgSend(objc_getClass("NSBitmapImageRep"),
                              sel_registerName("initWithBitmapDataPlanes:width:height:bitsPerSample:samplePerPixel:hasAlpha:isPlanar:colorSpaceName:bitmapFormat:bytesPerRow:bitsPerPixel:"),
                              NULL, ren->width, ren->height, 8, 4, YES, NO,
                              objc_msgSend(objc_getClass("NSCalibratedRGBColorSpace"), sel_registerName("name")),
                              0, ren->width * 4, 32);

    unsigned char* bmp_data = (unsigned char*)objc_msgSend(bitmap, sel_registerName("bitmapData"));
    if (bmp_data) {
        for (int y = 0; y < ren->height; y++) {
            for (int x = 0; x < ren->width; x++) {
                uint32_t pixel = ren->pixels[y * ren->width + x];
                uint8_t r = (pixel >> 16) & 0xFF;
                uint8_t g = (pixel >> 8) & 0xFF;
                uint8_t b = pixel & 0xFF;
                uint8_t a = (pixel >> 24) & 0xFF;
                int idx = (y * ren->width + x) * 4;
                bmp_data[idx + 0] = r;
                bmp_data[idx + 1] = g;
                bmp_data[idx + 2] = b;
                bmp_data[idx + 3] = a;
            }
        }
    }

    objc_msgSend(ren->window->ns_view, sel_registerName("display"));
}

/* 当窗口大小改变时，重新调整渲染器大小 */
int leno_gui_platform_renderer_resize(LenoGUIPlatformRenderer* ren, int w, int h) {
    if (!ren || w <= 0 || h <= 0) return 0;

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

    /* 清除调整标志 */
    ren->needs_resize = 0;

    return 1;
}

/* ===== Cocoa 事件处理（参考 SDL3） ===== */

/* NSEvent 类型定义 */
#define NSEventTypeLeftMouseDown        1
#define NSEventTypeLeftMouseUp          2
#define NSEventTypeRightMouseDown       3
#define NSEventTypeRightMouseUp         4
#define NSEventTypeMouseMoved           5
#define NSEventTypeLeftMouseDragged     6
#define NSEventTypeRightMouseDragged    7
#define NSEventTypeMouseEntered         8
#define NSEventTypeMouseExited          9
#define NSEventTypeKeyDown              10
#define NSEventTypeKeyUp                11
#define NSEventTypeFlagsChanged         12
#define NSEventTypeScrollWheel          22
#define NSEventTypeOtherMouseDown       25
#define NSEventTypeOtherMouseUp         26
#define NSEventTypeOtherMouseDragged    27

/* 事件掩码 */
#define NSEventMaskAny                  0xFFFFFFFFULL

/* 查找窗口映射表 */
static LenoGUIPlatformWindow* find_window_by_ns_window(id ns_window) {
    for (int i = 0; i < g_window_count; i++) {
        if (g_window_list[i] && g_window_list[i]->ns_window == ns_window) {
            return g_window_list[i];
        }
    }
    return NULL;
}

/* 将 NSEvent 转换为 LenoGUIEvent 并推入队列 */
static void pump_cocoa_event(id ns_event) {
    if (!ns_event) return;

    LenoGUIEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.timestamp = leno_gui_platform_get_ticks();

    /* 获取事件类型 */
    NSUInteger event_type = ((NSUInteger(*)(id, SEL))objc_msgSend)(ns_event, sel_registerName("type"));

    /* 获取窗口 */
    id ns_window = ((id(*)(id, SEL))objc_msgSend)(ns_event, sel_registerName("window"));
    LenoGUIPlatformWindow* win = NULL;
    if (ns_window) {
        win = find_window_by_ns_window(ns_window);
    }
    ev.window_id = win ? win->window_id : 0;

    /* 获取鼠标位置（适用于鼠标事件） */
    NSPoint location = ((NSPoint(*)(id, SEL))objc_msgSend)(ns_event, sel_registerName("locationInWindow"));

    /* 根据事件类型处理 */
    switch (event_type) {
        case NSEventTypeKeyDown: {
            ev.type = LENO_GUI_EVT_KEY_DOWN;
            /* 获取按键码 */
            unsigned short keycode = ((unsigned short(*)(id, SEL))objc_msgSend)(ns_event, sel_registerName("keyCode"));
            ev.key = macos_keycode_to_leno(keycode);
            /* 获取修饰键状态 */
            NSUInteger mod_flags = ((NSUInteger(*)(id, SEL))objc_msgSend)(ns_event, sel_registerName("modifierFlags"));
            ev.mod_flags = 0;
            if (mod_flags & 0x10000) ev.mod_flags |= LENO_GUI_MOD_SHIFT;
            if (mod_flags & 0x80000) ev.mod_flags |= LENO_GUI_MOD_CTRL;
            if (mod_flags & 0x20000) ev.mod_flags |= LENO_GUI_MOD_ALT;
            event_queue_push(&ev);
            break;
        }
        case NSEventTypeKeyUp: {
            ev.type = LENO_GUI_EVT_KEY_UP;
            unsigned short keycode = ((unsigned short(*)(id, SEL))objc_msgSend)(ns_event, sel_registerName("keyCode"));
            ev.key = macos_keycode_to_leno(keycode);
            event_queue_push(&ev);
            break;
        }
        case NSEventTypeLeftMouseDown:
        case NSEventTypeRightMouseDown:
        case NSEventTypeOtherMouseDown: {
            ev.type = LENO_GUI_EVT_MOUSE_DOWN;
            ev.mouse_x = (float)location.x;
            ev.mouse_y = (float)location.y;
            if (event_type == NSEventTypeLeftMouseDown) ev.mouse_button = LENO_GUI_MOUSE_LEFT;
            else if (event_type == NSEventTypeRightMouseDown) ev.mouse_button = LENO_GUI_MOUSE_RIGHT;
            else ev.mouse_button = LENO_GUI_MOUSE_MIDDLE;
            ev.mouse_clicks = 1;
            /* 无边框窗口拖动支持 */
            if (win && win->is_borderless && event_type == NSEventTypeLeftMouseDown) {
                int mx = (int)location.x;
                int my = (int)(win->height - location.y); /* 转换为左下角坐标 */
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
                    /* macOS 无边框窗口拖动：使用 performWindowDragWithEvent */
                    objc_msgSend(win->ns_window, sel_registerName("performWindowDragWithEvent:"), ns_event);
                }
            }
            event_queue_push(&ev);
            break;
        }
        case NSEventTypeLeftMouseUp:
        case NSEventTypeRightMouseUp:
        case NSEventTypeOtherMouseUp: {
            ev.type = LENO_GUI_EVT_MOUSE_UP;
            ev.mouse_x = (float)location.x;
            ev.mouse_y = (float)location.y;
            if (event_type == NSEventTypeLeftMouseUp) ev.mouse_button = LENO_GUI_MOUSE_LEFT;
            else if (event_type == NSEventTypeRightMouseUp) ev.mouse_button = LENO_GUI_MOUSE_RIGHT;
            else ev.mouse_button = LENO_GUI_MOUSE_MIDDLE;
            event_queue_push(&ev);
            break;
        }
        case NSEventTypeMouseMoved:
        case NSEventTypeLeftMouseDragged:
        case NSEventTypeRightMouseDragged:
        case NSEventTypeOtherMouseDragged: {
            ev.type = LENO_GUI_EVT_MOUSE_MOVE;
            ev.mouse_x = (float)location.x;
            ev.mouse_y = (float)location.y;
            event_queue_push(&ev);
            break;
        }
        case NSEventTypeScrollWheel: {
            ev.type = LENO_GUI_EVT_MOUSE_WHEEL;
            ev.mouse_x = (float)location.x;
            ev.mouse_y = (float)location.y;
            CGFloat delta_y = ((CGFloat(*)(id, SEL))objc_msgSend)(ns_event, sel_registerName("deltaY"));
            CGFloat delta_x = ((CGFloat(*)(id, SEL))objc_msgSend)(ns_event, sel_registerName("deltaX"));
            ev.wheel_y = (float)delta_y;
            ev.wheel_x = (float)delta_x;
            event_queue_push(&ev);
            break;
        }
    }
}

/* 泵送 Cocoa 事件（参考 SDL3 Cocoa_PumpEvents） */
static void pump_cocoa_events(void) {
    id app = objc_msgSend(objc_getClass("NSApplication"), sel_registerName("sharedApplication"));
    if (!app) return;

    /* 使用 nextEventMatchingMask 获取事件 */
    SEL next_event_sel = sel_registerName("nextEventMatchingMask:untilDate:inMode:dequeue:");

    /* 获取当前时间（用于非阻塞轮询） */
    id distant_past = objc_msgSend(objc_getClass("NSDate"), sel_registerName("distantPast"));

    /* 事件模式 */
    id event_tracking_mode = objc_msgSend(objc_getClass("NSString"),
                                           sel_registerName("stringWithUTF8String:"),
                                           "NSEventTrackingRunLoopMode");
    id default_mode = objc_msgSend(objc_getClass("NSString"),
                                    sel_registerName("stringWithUTF8String:"),
                                    "kCFRunLoopDefaultMode");

    /* 循环处理所有待处理事件 */
    for (;;) {
        id event = ((id(*)(id, SEL, NSUInteger, id, id, BOOL))objc_msgSend)(
            app, next_event_sel,
            NSEventMaskAny,
            distant_past,
            default_mode,
            YES);

        if (!event) break;

        /* 处理事件 */
        pump_cocoa_event(event);

        /* 将事件发送给应用（让系统处理窗口管理等） */
        objc_msgSend(app, sel_registerName("sendEvent:"), event);
    }

    /* 更新窗口（处理重绘等） */
    objc_msgSend(app, sel_registerName("updateWindows"));

    /* 发送窗口暴露事件（简化实现，实际需要监听 NSWindowDidExposeNotification） */
    /* 这里我们简单地定期发送暴露事件，让应用程序重绘 */
    static uint64_t last_expose_time = 0;
    uint64_t now = leno_gui_platform_get_ticks();
    if (now - last_expose_time > 100) {  /* 每 100ms 检查一次 */
        last_expose_time = now;
        /* 简化处理：发送暴露事件给所有窗口 */
        /* 实际应该检查窗口是否可见且需要重绘 */
    }
}

/* 发送窗口暴露事件（供外部调用） */
void leno_gui_platform_send_expose_event(LenoGUIPlatformWindow* win) {
    if (!win) return;
    LenoGUIEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = LENO_GUI_EVT_WINDOW_EXPOSED;
    ev.timestamp = leno_gui_platform_get_ticks();
    ev.window_id = win->window_id;
    event_queue_push(&ev);
}

/* ===== 事件 ===== */

/* 轮询事件队列 */
int leno_gui_platform_poll_event(LenoGUIEvent* event) {
    /* 先泵送 Cocoa 事件 */
    pump_cocoa_events();
    return event_queue_pop(event);
}

/* 等待事件（带超时） */
int leno_gui_platform_wait_event(LenoGUIEvent* event, int timeout_ms) {
    id app = objc_msgSend(objc_getClass("NSApplication"), sel_registerName("sharedApplication"));
    if (!app) return 0;

    /* 先检查队列中是否已有事件 */
    if (event_queue_pop(event)) return 1;

    /* 使用 nextEventMatchingMask 等待事件 */
    SEL next_event_sel = sel_registerName("nextEventMatchingMask:untilDate:inMode:dequeue:");

    /* 计算超时时间 */
    id until_date;
    if (timeout_ms <= 0) {
        until_date = objc_msgSend(objc_getClass("NSDate"), sel_registerName("distantFuture"));
    } else {
        until_date = objc_msgSend(objc_getClass("NSDate"),
                                   sel_registerName("dateWithTimeIntervalSinceNow:"),
                                   (double)timeout_ms / 1000.0);
    }

    id default_mode = objc_msgSend(objc_getClass("NSString"),
                                    sel_registerName("stringWithUTF8String:"),
                                    "kCFRunLoopDefaultMode");

    /* 等待事件 */
    id ns_event = ((id(*)(id, SEL, NSUInteger, id, id, BOOL))objc_msgSend)(
        app, next_event_sel,
        NSEventMaskAny,
        until_date,
        default_mode,
        YES);

    if (ns_event) {
        pump_cocoa_event(ns_event);
        objc_msgSend(app, sel_registerName("sendEvent:"), ns_event);
    }

    return event_queue_pop(event);
}

/* ===== 显示器信息 ===== */

/* 获取主显示器尺寸 */
void leno_gui_platform_get_display_size(int* w, int* h) {
    id screen = objc_msgSend(objc_getClass("NSScreen"), sel_registerName("mainScreen"));
    if (screen) {
        NSRect frame = ((NSRect(*)(id, SEL))objc_msgSend)(screen, sel_registerName("frame"));
        if (w) *w = (int)frame.size.width;
        if (h) *h = (int)frame.size.height;
    } else {
        if (w) *w = 0;
        if (h) *h = 0;
    }
}

/* ===== 输入状态查询（参考 SDL3 SDL_GetKeyboardState / SDL_GetMouseState） ===== */

/* Leno 键码到 macOS 虚拟键码的反向映射表 */
static int leno_key_to_macos_keycode(int key) {
    if (key >= 'A' && key <= 'Z') return key;
    if (key >= '0' && key <= '9') return key;
    switch (key) {
        case LENO_GUI_KEY_RETURN:    return 36;
        case LENO_GUI_KEY_ESCAPE:    return 53;
        case LENO_GUI_KEY_BACKSPACE: return 51;
        case LENO_GUI_KEY_TAB:       return 48;
        case LENO_GUI_KEY_SPACE:     return 49;
        case LENO_GUI_KEY_DELETE:    return 117;
        case LENO_GUI_KEY_LEFT:      return 123;
        case LENO_GUI_KEY_RIGHT:     return 124;
        case LENO_GUI_KEY_UP:        return 126;
        case LENO_GUI_KEY_DOWN:      return 125;
        case LENO_GUI_KEY_F1:        return 122;
        case LENO_GUI_KEY_F2:        return 120;
        case LENO_GUI_KEY_F3:        return 99;
        case LENO_GUI_KEY_F4:        return 118;
        case LENO_GUI_KEY_F5:        return 96;
        case LENO_GUI_KEY_F6:        return 97;
        case LENO_GUI_KEY_F7:        return 98;
        case LENO_GUI_KEY_F8:        return 100;
        case LENO_GUI_KEY_F9:        return 101;
        case LENO_GUI_KEY_F10:       return 109;
        case LENO_GUI_KEY_F11:       return 103;
        case LENO_GUI_KEY_F12:       return 111;
        case LENO_GUI_KEY_LSHIFT:    return 56;
        case LENO_GUI_KEY_RSHIFT:    return 60;
        case LENO_GUI_KEY_LCTRL:     return 59;
        case LENO_GUI_KEY_RCTRL:     return 62;
        case LENO_GUI_KEY_LALT:      return 58;
        case LENO_GUI_KEY_RALT:      return 61;
        case LENO_GUI_KEY_CAPSLOCK:  return 57;
        default: return -1;
    }
}

/* 查询指定按键是否按下（使用 CGEventSourceKeyState，参考 SDL3 Cocoa 键盘实现） */
int leno_gui_platform_get_key_state(int key) {
    int keycode = leno_key_to_macos_keycode(key);
    if (keycode < 0) return 0;
    /* CGEventSourceKeyState 使用 CoreGraphics 查询按键状态 */
    bool pressed = CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, (CGKeyCode)keycode);
    return pressed ? 1 : 0;
}

/* 查询鼠标状态：位置和按钮（参考 SDL3 Cocoa_GetGlobalMouseState） */
int leno_gui_platform_get_mouse_state(int* x, int* y, int* buttons) {
    /* 使用 NSEvent mouseLocation 获取全局鼠标位置 */
    NSPoint loc = ((NSPoint(*)(id, SEL))objc_msgSend)(
        objc_getClass("NSEvent"), sel_registerName("mouseLocation"));
    /* macOS 坐标系 Y 轴从底部向上，需要翻转 */
    id screen = objc_msgSend(objc_getClass("NSScreen"), sel_registerName("mainScreen"));
    int screen_height = 0;
    if (screen) {
        NSRect frame = ((NSRect(*)(id, SEL))objc_msgSend)(screen, sel_registerName("frame"));
        screen_height = (int)frame.size.height;
    }
    if (x) *x = (int)loc.x;
    if (y) *y = screen_height - (int)loc.y;
    /* 使用 NSEvent pressedMouseButtons 获取按钮状态 */
    NSUInteger cocoa_buttons = ((NSUInteger(*)(id, SEL))objc_msgSend)(
        objc_getClass("NSEvent"), sel_registerName("pressedMouseButtons"));
    int btns = 0;
    if (cocoa_buttons & (1 << 0)) btns |= 0x01;
    if (cocoa_buttons & (1 << 1)) btns |= 0x04;
    if (cocoa_buttons & (1 << 2)) btns |= 0x02;
    if (buttons) *buttons = btns;
    return btns;
}

/* ===== 剪贴板（参考 SDL3 SDL_cocoaclipboard.m） ===== */

/* 获取剪贴板文本内容（使用 NSPasteboard） */
char* leno_gui_platform_get_clipboard_text(void) {
    id pasteboard = objc_msgSend(objc_getClass("NSPasteboard"), sel_registerName("generalPasteboard"));
    if (!pasteboard) return NULL;
    /* 获取剪贴板中的字符串类型数据 */
    id ns_string_type = objc_msgSend(objc_getClass("NSPasteboard"), sel_registerName("name"));
    (void)ns_string_type;
    /* 使用 stringForType: 获取文本 */
    SEL string_for_type = sel_registerName("stringForType:");
    id ns_string_type_str = objc_msgSend(objc_getClass("NSString"),
                                          sel_registerName("stringWithUTF8String:"), "public.utf8-plain-text");
    id ns_str = objc_msgSend(pasteboard, string_for_type, ns_string_type_str);
    if (!ns_str) {
        /* 尝试使用 NSPasteboardTypeString */
        id pb_type_str = objc_msgSend(objc_getClass("NSString"),
                                       sel_registerName("stringWithUTF8String:"), "NSStringPboardType");
        ns_str = objc_msgSend(pasteboard, string_for_type, pb_type_str);
    }
    if (!ns_str) return NULL;
    /* 获取 UTF-8 C 字符串 */
    const char* utf8 = (const char*)objc_msgSend(ns_str, sel_registerName("UTF8String"));
    if (!utf8) return NULL;
    /* 复制一份返回（调用者负责释放） */
    size_t len = strlen(utf8) + 1;
    char* result = (char*)malloc(len);
    if (result) {
        memcpy(result, utf8, len);
    }
    return result;
}

/* 设置剪贴板文本内容（使用 NSPasteboard） */
void leno_gui_platform_set_clipboard_text(const char* text) {
    if (!text) return;
    id pasteboard = objc_msgSend(objc_getClass("NSPasteboard"), sel_registerName("generalPasteboard"));
    if (!pasteboard) return;
    /* 清空剪贴板 */
    objc_msgSend(pasteboard, sel_registerName("clearContents"));
    /* 创建 NSString */
    id ns_str = objc_msgSend(objc_getClass("NSString"),
                              sel_registerName("stringWithUTF8String:"), text);
    if (!ns_str) return;
    /* 声明类型并写入字符串 */
    id ns_string_type = objc_msgSend(objc_getClass("NSString"),
                                      sel_registerName("stringWithUTF8String:"), "public.utf8-plain-text");
    id types_array = objc_msgSend(objc_getClass("NSArray"),
                                   sel_registerName("arrayWithObject:"), ns_string_type);
    objc_msgSend(pasteboard, sel_registerName("declareTypes:owner:"), types_array, nil);
    objc_msgSend(pasteboard, sel_registerName("setString:forType:"), ns_str, ns_string_type);
}

/* ===== 光标控制（参考 SDL3 SDL_ShowCursor / SDL_HideCursor） ===== */

/* 显示或隐藏光标（使用 NSCursor hide/unhide） */
void leno_gui_platform_show_cursor(int show) {
    if (show) {
        objc_msgSend(objc_getClass("NSCursor"), sel_registerName("unhide"));
    } else {
        objc_msgSend(objc_getClass("NSCursor"), sel_registerName("hide"));
    }
}

/* ===== 窗口透明度（参考 SDL3 SDL_SetWindowOpacity） ===== */

/* 设置窗口透明度（0.0 完全透明 ~ 1.0 完全不透明），使用 [NSWindow setAlphaValue:] */
void leno_gui_platform_set_window_opacity(LenoGUIPlatformWindow* win, float opacity) {
    if (!win || !win->ns_window) return;
    if (opacity < 0.0f) opacity = 0.0f;
    if (opacity > 1.0f) opacity = 1.0f;
    objc_msgSend(win->ns_window, sel_registerName("setAlphaValue:"), (CGFloat)opacity);
}

/* ===== 字体操作（Cocoa NSFont 系统字体渲染） ===== */

struct LenoGUIPlatformFont {
    id ns_font;
    int size;
};

LenoGUIPlatformFont* leno_gui_platform_load_font(const char* name, int size) {
    if (size <= 0) size = 16;
    id ns_name = objc_msgSend((id)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), name);
    id ns_font = objc_msgSend((id)objc_getClass("NSFont"), sel_registerName("fontWithName:size:"), ns_name, (CGFloat)size);
    if (!ns_font) {
        ns_font = objc_msgSend((id)objc_getClass("NSFont"), sel_registerName("systemFontOfSize:"), (CGFloat)size);
    }
    if (!ns_font) return NULL;
    LenoGUIPlatformFont* font = (LenoGUIPlatformFont*)calloc(1, sizeof(LenoGUIPlatformFont));
    if (!font) return NULL;
    font->ns_font = ns_font;
    font->size = size;
    return font;
}

void leno_gui_platform_destroy_font(LenoGUIPlatformFont* font) {
    if (!font) return;
    free(font);
}

void leno_gui_platform_draw_text_font(LenoGUIPlatformRenderer* ren, LenoGUIPlatformFont* font, const char* text, int x, int y) {
    if (!ren || !ren->pixels || !font || !font->ns_font || !text) return;
    id ns_text = objc_msgSend((id)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), text);
    id attrs = objc_msgSend((id)objc_getClass("NSMutableDictionary"), sel_registerName("alloc"));
    attrs = objc_msgSend(attrs, sel_registerName("init"));
    objc_msgSend(attrs, sel_registerName("setObject:forKey:"), font->ns_font,
                 objc_msgSend((id)objc_getClass("NSFont"), sel_registerName("alloc")));
    CGFloat r = ren->draw_r / 255.0;
    CGFloat g = ren->draw_g / 255.0;
    CGFloat b = ren->draw_b / 255.0;
    CGFloat a = ren->draw_a / 255.0;
    id color = objc_msgSend((id)objc_getClass("NSColor"), sel_registerName("colorWithCalibratedRed:green:blue:alpha:"), r, g, b, a);
    objc_msgSend(attrs, sel_registerName("setObject:forKey:"), color,
                 objc_msgSend((id)objc_getClass("NSAttributedString"), sel_registerName("alloc")));
    id str = objc_msgSend((id)objc_getClass("NSAttributedString"), sel_registerName("alloc"));
    str = objc_msgSend(str, sel_registerName("initWithString:attributes:"), ns_text, attrs);
    CGRect rect = CGRectMake(x + ren->vp_x, y + ren->vp_y, ren->width, ren->height);
    objc_msgSend(str, sel_registerName("drawInRect:"), rect);
}

void leno_gui_platform_text_size_font(LenoGUIPlatformFont* font, const char* text, int* w, int* h) {
    if (!font || !font->ns_font || !text) { if (w) *w = 0; if (h) *h = 0; return; }
    id ns_text = objc_msgSend((id)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), text);
    id attrs = objc_msgSend((id)objc_getClass("NSMutableDictionary"), sel_registerName("alloc"));
    attrs = objc_msgSend(attrs, sel_registerName("init"));
    objc_msgSend(attrs, sel_registerName("setObject:forKey:"), font->ns_font,
                 objc_msgSend((id)objc_getClass("NSFont"), sel_registerName("alloc")));
    id str = objc_msgSend((id)objc_getClass("NSAttributedString"), sel_registerName("alloc"));
    str = objc_msgSend(str, sel_registerName("initWithString:attributes:"), ns_text, attrs);
    CGRect size = objc_msgSend(str, sel_registerName("boundingRectWithSize:options:"), CGRectMake(0, 0, 10000, 10000), (NSUInteger)1);
    if (w) *w = (int)size.size.width;
    if (h) *h = (int)size.size.height;
}

/* ===== 消息框（参考 SDL3 SDL_cocoamessagebox.m） ===== */

/* 显示系统消息框：type 0=信息 1=警告 2=错误，使用 NSAlert */
int leno_gui_platform_show_message_box(const char* title, const char* message, int type) {
    id pool = objc_msgSend(objc_getClass("NSAutoreleasePool"), sel_registerName("new"));

    id alert = objc_msgSend(objc_getClass("NSAlert"), sel_registerName("new"));
    if (!alert) {
        objc_msgSend(pool, sel_registerName("drain"));
        return 0;
    }

    /* 设置消息框样式 */
    switch (type) {
        case 2:
            objc_msgSend(alert, sel_registerName("setAlertStyle:"), 2);
            break;
        case 1:
            objc_msgSend(alert, sel_registerName("setAlertStyle:"), 1);
            break;
        default:
            objc_msgSend(alert, sel_registerName("setAlertStyle:"), 0);
            break;
    }

    /* 设置标题和消息文本 */
    id title_str = objc_msgSend(objc_getClass("NSString"),
                                 sel_registerName("stringWithUTF8String:"), title);
    id msg_str = objc_msgSend(objc_getClass("NSString"),
                               sel_registerName("stringWithUTF8String:"), message);
    objc_msgSend(alert, sel_registerName("setMessageText:"), title_str);
    objc_msgSend(alert, sel_registerName("setInformativeText:"), msg_str);

    /* 添加 OK 按钮 */
    id ok_str = objc_msgSend(objc_getClass("NSString"),
                              sel_registerName("stringWithUTF8String:"), "OK");
    objc_msgSend(alert, sel_registerName("addButtonWithTitle:"), ok_str);

    /* 运行模态对话框 */
    NSInteger result = ((NSInteger(*)(id, SEL))objc_msgSend)(alert, sel_registerName("runModal"));

    objc_msgSend(pool, sel_registerName("drain"));

    (void)result;
    return 1;
}

/* ===== 高精度计时器（参考 SDL3 SDL_GetTicks / SDL_GetPerformanceCounter） ===== */

/* 用于缓存 mach_timebase 信息 */
static int g_timebase_initialized = 0;
static mach_timebase_info_data_t g_timebase_info;

/* 确保 mach_timebase_info 已初始化 */
static void ensure_timebase_init(void) {
    if (!g_timebase_initialized) {
        mach_timebase_info(&g_timebase_info);
        g_timebase_initialized = 1;
    }
}

/* 获取自系统启动以来的毫秒数（使用 mach_absolute_time） */
uint64_t leno_gui_platform_get_ticks(void) {
    ensure_timebase_init();
    uint64_t now = mach_absolute_time();
    /* 将 mach 绝对时间转换为纳秒 */
    uint64_t nanos = now * g_timebase_info.numer / g_timebase_info.denom;
    return nanos / 1000000ULL;
}

/* 获取高精度性能计数器值 */
uint64_t leno_gui_platform_get_performance_counter(void) {
    return mach_absolute_time();
}

/* 获取高精度性能计数器频率（每秒计数次数） */
uint64_t leno_gui_platform_get_performance_frequency(void) {
    ensure_timebase_init();
    /* mach_absolute_time 的单位由 timebase info 决定 */
    /* 频率 = 1秒 / 每个计数的时间（纳秒） = 10^9 / (numer/denom) */
    return 1000000000ULL * g_timebase_info.denom / g_timebase_info.numer;
}

/* 延迟指定毫秒数 */
void leno_gui_platform_delay(uint32_t ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* ===== 显示器 DPI 查询 ===== */

/* 获取主显示器 DPI 缩放值（使用 NSScreen backingScaleFactor） */
float leno_gui_platform_get_display_dpi(void) {
    id screen = objc_msgSend(objc_getClass("NSScreen"), sel_registerName("mainScreen"));
    if (screen) {
        CGFloat scale = ((CGFloat(*)(id, SEL))objc_msgSend)(screen, sel_registerName("backingScaleFactor"));
        return (float)(scale * 96.0);
    }
    return 96.0f;
}

#endif /* __APPLE__ */
