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

/* ===== 平台窗口结构 ===== */

struct LenoGUIPlatformWindow {
    id ns_window;
    id ns_view;
    int window_id;
    int width;
    int height;
    int should_close;
    int is_fullscreen;
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
};

/* ===== 平台纹理结构 ===== */

struct LenoGUIPlatformTexture {
    uint32_t* pixels;
    int width;
    int height;
    int pitch;
};

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

/* ===== 软件渲染辅助函数（接受渲染器指针，自动处理视口偏移和裁剪） ===== */

/* 绘制单个像素点，应用视口偏移和裁剪矩形 */
static void sw_draw_point(LenoGUIPlatformRenderer* ren, int x, int y, uint32_t color) {
    int px = x + ren->vp_x;
    int py = y + ren->vp_y;
    if (px < ren->vp_x || px >= ren->vp_x + ren->vp_w) return;
    if (py < ren->vp_y || py >= ren->vp_y + ren->vp_h) return;
    if (ren->clip_enabled) {
        if (px < ren->clip_x || px >= ren->clip_x + ren->clip_w) return;
        if (py < ren->clip_y || py >= ren->clip_y + ren->clip_h) return;
    }
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
    int px = x + ren->vp_x;
    int py = y + ren->vp_y;
    int x1 = px > ren->vp_x ? px : ren->vp_x;
    int y1 = py > ren->vp_y ? py : ren->vp_y;
    int x2 = (px + w) < (ren->vp_x + ren->vp_w) ? (px + w) : (ren->vp_x + ren->vp_w);
    int y2 = (py + h) < (ren->vp_y + ren->vp_h) ? (py + h) : (ren->vp_y + ren->vp_h);
    if (ren->clip_enabled) {
        if (x1 < ren->clip_x) x1 = ren->clip_x;
        if (y1 < ren->clip_y) y1 = ren->clip_y;
        if (x2 > ren->clip_x + ren->clip_w) x2 = ren->clip_x + ren->clip_w;
        if (y2 > ren->clip_y + ren->clip_h) y2 = ren->clip_y + ren->clip_h;
    }
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > ren->width) x2 = ren->width;
    if (y2 > ren->height) y2 = ren->height;
    if (x1 >= x2 || y1 >= y2) return;
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
        if (py < ren->vp_y || py >= ren->vp_y + ren->vp_h) continue;
        if (ren->clip_enabled && (py < ren->clip_y || py >= ren->clip_y + ren->clip_h)) continue;
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

    id ns_string = objc_msgSend(objc_getClass("NSString"),
                                 sel_registerName("stringWithUTF8String:"), title);

    unsigned int style_mask = 1 | 2 | 4 | 8;
    if (flags & LENO_GUI_WIN_BORDERLESS) style_mask = 0;
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

    return win;
}

/* 销毁平台窗口 */
void leno_gui_platform_destroy_window(LenoGUIPlatformWindow* win) {
    if (!win) return;
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

/* 获取窗口客户区大小 */
void leno_gui_platform_get_window_size(LenoGUIPlatformWindow* win, int* w, int* h) {
    if (!win) { if (w) *w = 0; if (h) *h = 0; return; }
    if (w) *w = win->width;
    if (h) *h = win->height;
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

/* 查询窗口是否应该关闭 */
int leno_gui_platform_window_should_close(LenoGUIPlatformWindow* win) {
    return win ? win->should_close : 1;
}

/* 设置窗口关闭标志 */
void leno_gui_platform_set_window_should_close(LenoGUIPlatformWindow* win, int val) {
    if (win) win->should_close = val;
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

/* 用当前绘制颜色清除整个渲染缓冲区 */
void leno_gui_platform_render_clear(LenoGUIPlatformRenderer* ren) {
    if (!ren || !ren->pixels) return;
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    int total = ren->width * ren->height;
    for (int i = 0; i < total; i++) {
        ren->pixels[i] = color;
    }
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

/* 设置绘制颜色 */
void leno_gui_platform_set_draw_color(LenoGUIPlatformRenderer* ren, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!ren) return;
    ren->draw_r = r;
    ren->draw_g = g;
    ren->draw_b = b;
    ren->draw_a = a;
}

/* 绘制单个像素点 */
void leno_gui_platform_render_draw_point(LenoGUIPlatformRenderer* ren, int x, int y) {
    if (!ren || !ren->pixels) return;
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    sw_draw_point(ren, x, y, color);
}

/* 绘制直线 */
void leno_gui_platform_render_draw_line(LenoGUIPlatformRenderer* ren, int x1, int y1, int x2, int y2) {
    if (!ren || !ren->pixels) return;
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    sw_draw_line(ren, x1, y1, x2, y2, color);
}

/* 绘制矩形边框 */
void leno_gui_platform_render_draw_rect(LenoGUIPlatformRenderer* ren, int x, int y, int w, int h) {
    if (!ren || !ren->pixels) return;
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    sw_draw_rect(ren, x, y, w, h, color);
}

/* 填充矩形 */
void leno_gui_platform_render_fill_rect(LenoGUIPlatformRenderer* ren, int x, int y, int w, int h) {
    if (!ren || !ren->pixels) return;
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    sw_fill_rect(ren, x, y, w, h, color);
}

/* 获取渲染器大小 */
void leno_gui_platform_get_renderer_size(LenoGUIPlatformRenderer* ren, int* w, int* h) {
    if (!ren) { if (w) *w = 0; if (h) *h = 0; return; }
    if (w) *w = ren->width;
    if (h) *h = ren->height;
}

/* ===== 画圆（Bresenham 中点圆算法） ===== */

/* 绘制圆形边框：利用八分对称性只计算 1/8 圆弧 */
void leno_gui_platform_render_draw_circle(LenoGUIPlatformRenderer* ren, int cx, int cy, int radius) {
    if (!ren || !ren->pixels || radius <= 0) return;
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    int x = 0, y = radius;
    int d = 3 - 2 * radius;
    while (x <= y) {
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

/* 填充圆：使用 Bresenham 算法计算圆边界，然后水平扫描线填充 */
void leno_gui_platform_render_fill_circle(LenoGUIPlatformRenderer* ren, int cx, int cy, int radius) {
    if (!ren || !ren->pixels || radius <= 0) return;
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    int x = 0, y = radius;
    int d = 3 - 2 * radius;
    while (x <= y) {
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

/* 绘制圆角矩形边框 */
void leno_gui_platform_render_draw_rounded_rect(LenoGUIPlatformRenderer* ren, int x, int y, int w, int h, int radius) {
    if (!ren || !ren->pixels || w <= 0 || h <= 0) return;
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    if (radius <= 0) {
        leno_gui_platform_render_draw_rect(ren, x, y, w, h);
        return;
    }
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    sw_draw_line(ren, x + radius, y, x + w - 1 - radius, y, color);
    sw_draw_line(ren, x + radius, y + h - 1, x + w - 1 - radius, y + h - 1, color);
    sw_draw_line(ren, x, y + radius, x, y + h - 1 - radius, color);
    sw_draw_line(ren, x + w - 1, y + radius, x + w - 1, y + h - 1 - radius, color);
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

/* 填充圆角矩形 */
void leno_gui_platform_render_fill_rounded_rect(LenoGUIPlatformRenderer* ren, int x, int y, int w, int h, int radius) {
    if (!ren || !ren->pixels || w <= 0 || h <= 0) return;
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    if (radius <= 0) {
        leno_gui_platform_render_fill_rect(ren, x, y, w, h);
        return;
    }
    leno_gui_platform_render_fill_rect(ren, x, y + radius, w, h - 2 * radius);
    leno_gui_platform_render_fill_rect(ren, x + radius, y, w - 2 * radius, radius);
    leno_gui_platform_render_fill_rect(ren, x + radius, y + h - radius, w - 2 * radius, radius);
    uint32_t color = LENO_GUI_PIXEL(ren->draw_r, ren->draw_g, ren->draw_b, ren->draw_a);
    int ax = 0, ay = radius;
    int dd = 3 - 2 * radius;
    while (ax <= ay) {
        sw_draw_line(ren, x + radius - ay, y + radius - ax, x + radius - 1, y + radius - ax, color);
        sw_draw_line(ren, x + radius - ax, y + radius - ay, x + radius - 1, y + radius - ay, color);
        sw_draw_line(ren, x + w - radius, y + radius - ax, x + w - radius + ay - 1, y + radius - ax, color);
        sw_draw_line(ren, x + w - radius, y + radius - ay, x + w - radius + ax - 1, y + radius - ay, color);
        sw_draw_line(ren, x + radius - ay, y + h - radius + ax, x + radius - 1, y + h - radius + ax, color);
        sw_draw_line(ren, x + radius - ax, y + h - radius + ay, x + radius - 1, y + h - radius + ay, color);
        sw_draw_line(ren, x + w - radius, y + h - radius + ax, x + w - radius + ay - 1, y + h - radius + ax, color);
        sw_draw_line(ren, x + w - radius, y + h - radius + ay, x + w - radius + ax - 1, y + h - radius + ay, color);
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

/* ===== 纹理 ===== */

/* 创建纹理（像素缓冲区） */
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

/* 销毁纹理 */
void leno_gui_platform_destroy_texture(LenoGUIPlatformTexture* tex) {
    if (!tex) return;
    if (tex->pixels) free(tex->pixels);
    free(tex);
}

/* 将纹理整体绘制到渲染器目标位置 */
void leno_gui_platform_render_texture(LenoGUIPlatformRenderer* ren, LenoGUIPlatformTexture* tex, int x, int y) {
    if (!ren || !ren->pixels || !tex || !tex->pixels) return;
    sw_blit_texture(ren, tex->pixels, tex->width, tex->height, tex->pitch, x, y);
}

/* 更新纹理像素数据 */
void leno_gui_platform_update_texture(LenoGUIPlatformTexture* tex, const void* data, int pitch) {
    if (!tex || !tex->pixels || !data) return;
    const uint32_t* src = (const uint32_t*)data;
    for (int y = 0; y < tex->height; y++) {
        memcpy(tex->pixels + y * tex->width, (const uint8_t*)src + y * pitch, tex->width * 4);
    }
}

/* 获取纹理宽度 */
int leno_gui_platform_texture_width(LenoGUIPlatformTexture* tex) {
    return tex ? tex->width : 0;
}

/* 获取纹理高度 */
int leno_gui_platform_texture_height(LenoGUIPlatformTexture* tex) {
    return tex ? tex->height : 0;
}

/* ===== 纹理高级渲染 ===== */

/* 纹理源矩形渲染：从纹理中取子区域绘制到目标位置（可缩放） */
void leno_gui_platform_render_texture_src(LenoGUIPlatformRenderer* ren, LenoGUIPlatformTexture* tex,
                                          int sx, int sy, int sw_val, int sh_val, int dx, int dy, int dw, int dh) {
    if (!ren || !ren->pixels || !tex || !tex->pixels) return;
    if (sw_val <= 0 || sh_val <= 0 || dw <= 0 || dh <= 0) return;
    float x_scale = (float)sw_val / (float)dw;
    float y_scale = (float)sh_val / (float)dh;
    int src_pitch_int = tex->pitch / 4;
    for (int row = 0; row < dh; row++) {
        int src_y = sy + (int)(row * y_scale);
        if (src_y < 0 || src_y >= tex->height) continue;
        for (int col = 0; col < dw; col++) {
            int src_x = sx + (int)(col * x_scale);
            if (src_x < 0 || src_x >= tex->width) continue;
            uint32_t src_pixel = tex->pixels[src_y * src_pitch_int + src_x];
            uint8_t sa = (src_pixel >> 24) & 0xFF;
            if (sa == 0) continue;
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
    double rad = angle * 3.14159265358979323846 / 180.0;
    double cos_a = cos(rad);
    double sin_a = sin(rad);
    int tw = tex->width;
    int th = tex->height;
    double cx = tw / 2.0;
    double cy = th / 2.0;
    int src_pitch_int = tex->pitch / 4;
    for (int dy = -th; dy <= th; dy++) {
        for (int dx = -tw; dx <= tw; dx++) {
            double fx = dx, fy = dy;
            if (flip & LENO_GUI_FLIP_HORIZONTAL) fx = -fx;
            if (flip & LENO_GUI_FLIP_VERTICAL) fy = -fy;
            double src_x = cos_a * fx + sin_a * fy + cx;
            double src_y = -sin_a * fx + cos_a * fy + cy;
            int isx = (int)(src_x + 0.5);
            int isy = (int)(src_y + 0.5);
            if (isx < 0 || isx >= tw || isy < 0 || isy >= th) continue;
            uint32_t src_pixel = tex->pixels[isy * src_pitch_int + isx];
            uint8_t sa = (src_pixel >> 24) & 0xFF;
            if (sa == 0) continue;
            sw_draw_point(ren, x + dx, y + dy, src_pixel);
        }
    }
}

/* ===== 事件 ===== */

/* 轮询事件队列 */
int leno_gui_platform_poll_event(LenoGUIEvent* event) {
    return event_queue_pop(event);
}

/* 等待事件（带超时） */
int leno_gui_platform_wait_event(LenoGUIEvent* event, int timeout_ms) {
    (void)timeout_ms;
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
