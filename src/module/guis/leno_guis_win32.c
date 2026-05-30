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

/* ===== 事件队列 ===== */

#define MAX_GUI_EVENTS 512

static LenoGUIEvent g_event_queue[MAX_GUI_EVENTS];
static int g_event_head = 0;
static int g_event_tail = 0;
static int g_event_count = 0;

static CRITICAL_SECTION g_event_cs;
static int g_cs_initialized = 0;

static void event_queue_init(void) {
    if (!g_cs_initialized) {
        InitializeCriticalSection(&g_event_cs);
        g_cs_initialized = 1;
    }
    g_event_head = 0;
    g_event_tail = 0;
    g_event_count = 0;
}

static void event_queue_push(const LenoGUIEvent* ev) {
    EnterCriticalSection(&g_event_cs);
    if (g_event_count < MAX_GUI_EVENTS) {
        g_event_queue[g_event_tail] = *ev;
        g_event_tail = (g_event_tail + 1) % MAX_GUI_EVENTS;
        g_event_count++;
    }
    LeaveCriticalSection(&g_event_cs);
}

static int event_queue_pop(LenoGUIEvent* ev) {
    EnterCriticalSection(&g_event_cs);
    if (g_event_count == 0) {
        LeaveCriticalSection(&g_event_cs);
        return 0;
    }
    *ev = g_event_queue[g_event_head];
    g_event_head = (g_event_head + 1) % MAX_GUI_EVENTS;
    g_event_count--;
    LeaveCriticalSection(&g_event_cs);
    return 1;
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
};

/* ===== 平台纹理结构 ===== */

struct LenoGUIPlatformTexture {
    uint32_t* pixels;
    int width;
    int height;
    int pitch;
};

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

/* ===== 窗口过程 ===== */

static LRESULT CALLBACK leno_gui_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    LenoGUIPlatformWindow* win = (LenoGUIPlatformWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_CLOSE: {
            if (win) win->should_close = 1;
            LenoGUIEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = LENO_GUI_EVT_WINDOW_CLOSE;
            ev.timestamp = GetTickCount64();
            ev.window_id = win ? win->window_id : 0;
            event_queue_push(&ev);
            return 0;
        }
        case WM_DESTROY: {
            if (win) win->should_close = 1;
            LenoGUIEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = LENO_GUI_EVT_QUIT;
            ev.timestamp = GetTickCount64();
            event_queue_push(&ev);
            return 0;
        }
        case WM_SIZE: {
            if (win) {
                win->width = LOWORD(lparam);
                win->height = HIWORD(lparam);
            }
            LenoGUIEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = LENO_GUI_EVT_WINDOW_RESIZE;
            ev.timestamp = GetTickCount64();
            ev.window_id = win ? win->window_id : 0;
            ev.data1 = (int)LOWORD(lparam);
            ev.data2 = (int)HIWORD(lparam);
            event_queue_push(&ev);
            return 0;
        }
        case WM_MOVE: {
            LenoGUIEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = LENO_GUI_EVT_WINDOW_MOVE;
            ev.timestamp = GetTickCount64();
            ev.window_id = win ? win->window_id : 0;
            ev.data1 = (int)(short)LOWORD(lparam);
            ev.data2 = (int)(short)HIWORD(lparam);
            event_queue_push(&ev);
            return 0;
        }
        case WM_SETFOCUS: {
            LenoGUIEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = LENO_GUI_EVT_WINDOW_FOCUS;
            ev.timestamp = GetTickCount64();
            ev.window_id = win ? win->window_id : 0;
            event_queue_push(&ev);
            return 0;
        }
        case WM_KILLFOCUS: {
            LenoGUIEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = LENO_GUI_EVT_WINDOW_UNFOCUS;
            ev.timestamp = GetTickCount64();
            ev.window_id = win ? win->window_id : 0;
            event_queue_push(&ev);
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            LenoGUIEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = LENO_GUI_EVT_KEY_DOWN;
            ev.timestamp = GetTickCount64();
            ev.window_id = win ? win->window_id : 0;
            ev.key = vk_to_leno_key(wparam);
            ev.scancode = (int)((lparam >> 16) & 0xFF);
            ev.mod_flags = get_mod_flags();
            ev.repeat = (lparam >> 30) & 1;
            event_queue_push(&ev);
            return 0;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            LenoGUIEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = LENO_GUI_EVT_KEY_UP;
            ev.timestamp = GetTickCount64();
            ev.window_id = win ? win->window_id : 0;
            ev.key = vk_to_leno_key(wparam);
            ev.scancode = (int)((lparam >> 16) & 0xFF);
            ev.mod_flags = get_mod_flags();
            event_queue_push(&ev);
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
            LenoGUIEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = LENO_GUI_EVT_MOUSE_MOVE;
            ev.timestamp = GetTickCount64();
            ev.window_id = win ? win->window_id : 0;
            ev.mouse_x = (float)(short)LOWORD(lparam);
            ev.mouse_y = (float)(short)HIWORD(lparam);
            {
                static int last_x = 0, last_y = 0;
                ev.mouse_xrel = ev.mouse_x - (float)last_x;
                ev.mouse_yrel = ev.mouse_y - (float)last_y;
                last_x = (int)ev.mouse_x;
                last_y = (int)ev.mouse_y;
            }
            event_queue_push(&ev);
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_RBUTTONDOWN: {
            LenoGUIEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = LENO_GUI_EVT_MOUSE_DOWN;
            ev.timestamp = GetTickCount64();
            ev.window_id = win ? win->window_id : 0;
            ev.mouse_x = (float)(short)LOWORD(lparam);
            ev.mouse_y = (float)(short)HIWORD(lparam);
            if (msg == WM_LBUTTONDOWN)      ev.mouse_button = LENO_GUI_MOUSE_LEFT;
            else if (msg == WM_MBUTTONDOWN) ev.mouse_button = LENO_GUI_MOUSE_MIDDLE;
            else                            ev.mouse_button = LENO_GUI_MOUSE_RIGHT;
            ev.mouse_clicks = 1;
            event_queue_push(&ev);
            return 0;
        }
        case WM_LBUTTONUP:
        case WM_MBUTTONUP:
        case WM_RBUTTONUP: {
            LenoGUIEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = LENO_GUI_EVT_MOUSE_UP;
            ev.timestamp = GetTickCount64();
            ev.window_id = win ? win->window_id : 0;
            ev.mouse_x = (float)(short)LOWORD(lparam);
            ev.mouse_y = (float)(short)HIWORD(lparam);
            if (msg == WM_LBUTTONUP)        ev.mouse_button = LENO_GUI_MOUSE_LEFT;
            else if (msg == WM_MBUTTONUP)   ev.mouse_button = LENO_GUI_MOUSE_MIDDLE;
            else                            ev.mouse_button = LENO_GUI_MOUSE_RIGHT;
            event_queue_push(&ev);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            LenoGUIEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = LENO_GUI_EVT_MOUSE_WHEEL;
            ev.timestamp = GetTickCount64();
            ev.window_id = win ? win->window_id : 0;
            POINT pt;
            pt.x = (short)LOWORD(lparam);
            pt.y = (short)HIWORD(lparam);
            ScreenToClient(hwnd, &pt);
            ev.mouse_x = (float)pt.x;
            ev.mouse_y = (float)pt.y;
            short delta = GET_WHEEL_DELTA_WPARAM(wparam);
            ev.wheel_x = 0.0f;
            ev.wheel_y = (float)delta / (float)WHEEL_DELTA;
            event_queue_push(&ev);
            return 0;
        }
        case WM_MOUSEHWHEEL: {
            LenoGUIEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = LENO_GUI_EVT_MOUSE_WHEEL;
            ev.timestamp = GetTickCount64();
            ev.window_id = win ? win->window_id : 0;
            POINT pt;
            pt.x = (short)LOWORD(lparam);
            pt.y = (short)HIWORD(lparam);
            ScreenToClient(hwnd, &pt);
            ev.mouse_x = (float)pt.x;
            ev.mouse_y = (float)pt.y;
            short delta = GET_WHEEL_DELTA_WPARAM(wparam);
            ev.wheel_x = (float)delta / (float)WHEEL_DELTA;
            ev.wheel_y = 0.0f;
            event_queue_push(&ev);
            return 0;
        }
        case WM_SHOWWINDOW: {
            LenoGUIEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = wparam ? LENO_GUI_EVT_WINDOW_SHOW : LENO_GUI_EVT_WINDOW_HIDE;
            ev.timestamp = GetTickCount64();
            ev.window_id = win ? win->window_id : 0;
            event_queue_push(&ev);
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

/* ===== 软件渲染辅助函数 ===== */

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

    event_queue_init();

    if (!g_class_registered) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = leno_gui_wndproc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
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

    DWORD style = WS_OVERLAPPEDWINDOW;
    if (flags & LENO_GUI_WIN_BORDERLESS) style = WS_POPUP;
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

void leno_gui_platform_get_window_size(LenoGUIPlatformWindow* win, int* w, int* h) {
    if (!win) { if (w) *w = 0; if (h) *h = 0; return; }
    if (w) *w = win->width;
    if (h) *h = win->height;
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

int leno_gui_platform_window_should_close(LenoGUIPlatformWindow* win) {
    return win ? win->should_close : 1;
}

void leno_gui_platform_set_window_should_close(LenoGUIPlatformWindow* win, int val) {
    if (win) win->should_close = val;
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

    return ren;
}

void leno_gui_platform_destroy_renderer(LenoGUIPlatformRenderer* ren) {
    if (!ren) return;
    destroy_dib_section(&ren->back_dc, &ren->back_bitmap, &ren->old_bitmap);
    ren->pixels = NULL;
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
    if (!ren || !ren->window || !ren->window->hwnd || !ren->back_dc) return;

    HWND hwnd = ren->window->hwnd;
    HDC hdc = GetDC(hwnd);
    if (hdc) {
        BitBlt(hdc, 0, 0, ren->width, ren->height, ren->back_dc, 0, 0, SRCCOPY);
        ReleaseDC(hwnd, hdc);
    }
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

/* ===== 画圆（Bresenham 中点圆算法） ===== */

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

/* 填充圆（水平扫描线填充） */
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

/* 绘制圆角矩形边框 */
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

/* 填充圆角矩形 */
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
                                          int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh) {
    if (!ren || !ren->pixels || !tex || !tex->pixels) return;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    /* 计算缩放比例 */
    float x_scale = (float)sw / (float)dw;
    float y_scale = (float)sh / (float)dh;
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
