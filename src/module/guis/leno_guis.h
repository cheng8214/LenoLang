/* Leno GUI - 跨平台图形界面模块
 * 灵感来源于 SDL3，无外部依赖
 * 支持: Windows (Win32+GDI), Linux (X11), macOS (Cocoa)
 *
 * 架构设计:
 *   leno_guis.h          - 平台抽象层（类型定义 + 函数声明）
 *   leno_guis_win32.c    - Windows 实现 (Win32 API + GDI 双缓冲)
 *   leno_guis_linux.c    - Linux 实现 (X11)
 *   leno_guis_macos.c    - macOS 实现 (Cocoa)
 *   guis.c               - LenoC 模块注册
 *
 * 渲染策略:
 *   使用软件渲染（像素缓冲区 + 平台 blit），类似 SDL 软件渲染器
 *   像素格式: 32 位 ARGB (内存中为 BGRA，小端序)
 *   双缓冲: 后备缓冲区渲染完成后 blit 到窗口
 *
 * 事件系统:
 *   参考 SDL3 的事件设计，使用类型编码区分事件类别
 *   事件通过队列传递，支持 poll 和 wait 两种模式
 */

#ifndef LENO_GUIS_H
#define LENO_GUIS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 窗口标志 ===== */
#define LENO_GUI_WIN_RESIZABLE     0x0001
#define LENO_GUI_WIN_FULLSCREEN    0x0002
#define LENO_GUI_WIN_BORDERLESS    0x0004
#define LENO_GUI_WIN_HIDDEN        0x0008
#define LENO_GUI_WIN_ALWAYS_ON_TOP 0x0010
#define LENO_GUI_WIN_MINIMIZED     0x0020
#define LENO_GUI_WIN_MAXIMIZED     0x0040

/* ===== 事件类型（参考 SDL3 分段编码） ===== */
#define LENO_GUI_EVT_QUIT              0x100
#define LENO_GUI_EVT_WINDOW_CLOSE      0x201
#define LENO_GUI_EVT_WINDOW_RESIZE     0x202
#define LENO_GUI_EVT_WINDOW_MOVE       0x203
#define LENO_GUI_EVT_WINDOW_FOCUS      0x204
#define LENO_GUI_EVT_WINDOW_UNFOCUS    0x205
#define LENO_GUI_EVT_WINDOW_SHOW       0x206
#define LENO_GUI_EVT_WINDOW_HIDE       0x207
#define LENO_GUI_EVT_KEY_DOWN          0x300
#define LENO_GUI_EVT_KEY_UP            0x301
#define LENO_GUI_EVT_TEXT_INPUT        0x302
#define LENO_GUI_EVT_MOUSE_MOVE        0x400
#define LENO_GUI_EVT_MOUSE_DOWN        0x401
#define LENO_GUI_EVT_MOUSE_UP          0x402
#define LENO_GUI_EVT_MOUSE_WHEEL       0x403

/* ===== 鼠标按钮 ===== */
#define LENO_GUI_MOUSE_LEFT     1
#define LENO_GUI_MOUSE_MIDDLE   2
#define LENO_GUI_MOUSE_RIGHT    3

/* ===== 键码（可打印字符使用 Unicode 码点，特殊键从 0x1000 开始） ===== */
#define LENO_GUI_KEY_UNKNOWN    0
#define LENO_GUI_KEY_RETURN     0x0D
#define LENO_GUI_KEY_ESCAPE     0x1B
#define LENO_GUI_KEY_BACKSPACE  0x08
#define LENO_GUI_KEY_TAB        0x09
#define LENO_GUI_KEY_SPACE      0x20
#define LENO_GUI_KEY_DELETE     0x7F

#define LENO_GUI_KEY_LEFT       0x1001
#define LENO_GUI_KEY_RIGHT      0x1002
#define LENO_GUI_KEY_UP         0x1003
#define LENO_GUI_KEY_DOWN       0x1004
#define LENO_GUI_KEY_INSERT     0x1005
#define LENO_GUI_KEY_HOME       0x1006
#define LENO_GUI_KEY_END        0x1007
#define LENO_GUI_KEY_PAGEUP     0x1008
#define LENO_GUI_KEY_PAGEDOWN   0x1009
#define LENO_GUI_KEY_F1         0x1011
#define LENO_GUI_KEY_F2         0x1012
#define LENO_GUI_KEY_F3         0x1013
#define LENO_GUI_KEY_F4         0x1014
#define LENO_GUI_KEY_F5         0x1015
#define LENO_GUI_KEY_F6         0x1016
#define LENO_GUI_KEY_F7         0x1017
#define LENO_GUI_KEY_F8         0x1018
#define LENO_GUI_KEY_F9         0x1019
#define LENO_GUI_KEY_F10        0x101A
#define LENO_GUI_KEY_F11        0x101B
#define LENO_GUI_KEY_F12        0x101C
#define LENO_GUI_KEY_LSHIFT     0x1021
#define LENO_GUI_KEY_RSHIFT     0x1022
#define LENO_GUI_KEY_LCTRL      0x1023
#define LENO_GUI_KEY_RCTRL      0x1024
#define LENO_GUI_KEY_LALT       0x1025
#define LENO_GUI_KEY_RALT       0x1026
#define LENO_GUI_KEY_CAPSLOCK   0x1027
#define LENO_GUI_KEY_NUMLOCK    0x1028

/* ===== 修饰键标志 ===== */
#define LENO_GUI_MOD_SHIFT      0x01
#define LENO_GUI_MOD_CTRL       0x02
#define LENO_GUI_MOD_ALT        0x04
#define LENO_GUI_MOD_SUPER      0x08

/* ===== 像素格式 ===== */
/* 32 位 ARGB: 内存布局 (小端序) B G R A */
#define LENO_GUI_PIXEL(r, g, b, a) \
    ((uint32_t)((uint8_t)(a) << 24) | ((uint8_t)(r) << 16) | ((uint8_t)(g) << 8) | (uint8_t)(b))

/* ===== GUI 事件结构 ===== */
typedef struct {
    int type;
    uint64_t timestamp;
    int window_id;
    int data1;
    int data2;
    int key;
    int scancode;
    int mod_flags;
    int repeat;
    char text[32];
    float mouse_x;
    float mouse_y;
    float mouse_xrel;
    float mouse_yrel;
    int mouse_button;
    int mouse_clicks;
    float wheel_x;
    float wheel_y;
} LenoGUIEvent;

/* ===== 平台窗口/渲染器/纹理（不透明指针） ===== */
typedef struct LenoGUIPlatformWindow LenoGUIPlatformWindow;
typedef struct LenoGUIPlatformRenderer LenoGUIPlatformRenderer;
typedef struct LenoGUIPlatformTexture LenoGUIPlatformTexture;

/* ===== 平台初始化/关闭 ===== */
int  leno_gui_platform_init(void);
void leno_gui_platform_quit(void);

/* ===== 窗口操作 ===== */
LenoGUIPlatformWindow* leno_gui_platform_create_window(const char* title, int w, int h, int flags);
void   leno_gui_platform_destroy_window(LenoGUIPlatformWindow* win);
void   leno_gui_platform_show_window(LenoGUIPlatformWindow* win);
void   leno_gui_platform_hide_window(LenoGUIPlatformWindow* win);
void   leno_gui_platform_set_window_title(LenoGUIPlatformWindow* win, const char* title);
void   leno_gui_platform_set_window_size(LenoGUIPlatformWindow* win, int w, int h);
void   leno_gui_platform_get_window_size(LenoGUIPlatformWindow* win, int* w, int* h);
void   leno_gui_platform_set_window_position(LenoGUIPlatformWindow* win, int x, int y);
void   leno_gui_platform_get_window_position(LenoGUIPlatformWindow* win, int* x, int* y);
void   leno_gui_platform_set_window_fullscreen(LenoGUIPlatformWindow* win, int fullscreen);
int    leno_gui_platform_window_should_close(LenoGUIPlatformWindow* win);
void   leno_gui_platform_set_window_should_close(LenoGUIPlatformWindow* win, int val);

/* ===== 翻转标志（参考 SDL_FlipMode） ===== */
#define LENO_GUI_FLIP_NONE       0
#define LENO_GUI_FLIP_HORIZONTAL 1
#define LENO_GUI_FLIP_VERTICAL   2

/* ===== 渲染器操作 ===== */
LenoGUIPlatformRenderer* leno_gui_platform_create_renderer(LenoGUIPlatformWindow* win);
void   leno_gui_platform_destroy_renderer(LenoGUIPlatformRenderer* ren);
void   leno_gui_platform_render_clear(LenoGUIPlatformRenderer* ren);
void   leno_gui_platform_render_present(LenoGUIPlatformRenderer* ren);
void   leno_gui_platform_set_draw_color(LenoGUIPlatformRenderer* ren, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void   leno_gui_platform_render_draw_point(LenoGUIPlatformRenderer* ren, int x, int y);
void   leno_gui_platform_render_draw_line(LenoGUIPlatformRenderer* ren, int x1, int y1, int x2, int y2);
void   leno_gui_platform_render_draw_rect(LenoGUIPlatformRenderer* ren, int x, int y, int w, int h);
void   leno_gui_platform_render_fill_rect(LenoGUIPlatformRenderer* ren, int x, int y, int w, int h);
void   leno_gui_platform_render_draw_circle(LenoGUIPlatformRenderer* ren, int cx, int cy, int radius);
void   leno_gui_platform_render_fill_circle(LenoGUIPlatformRenderer* ren, int cx, int cy, int radius);
void   leno_gui_platform_render_draw_rounded_rect(LenoGUIPlatformRenderer* ren, int x, int y, int w, int h, int radius);
void   leno_gui_platform_render_fill_rounded_rect(LenoGUIPlatformRenderer* ren, int x, int y, int w, int h, int radius);
void   leno_gui_platform_get_renderer_size(LenoGUIPlatformRenderer* ren, int* w, int* h);

/* ===== 视口和裁剪（参考 SDL_SetRenderViewport / SDL_SetRenderClipRect） ===== */
void   leno_gui_platform_set_viewport(LenoGUIPlatformRenderer* ren, int x, int y, int w, int h);
void   leno_gui_platform_get_viewport(LenoGUIPlatformRenderer* ren, int* x, int* y, int* w, int* h);
void   leno_gui_platform_set_clip_rect(LenoGUIPlatformRenderer* ren, int x, int y, int w, int h);
void   leno_gui_platform_get_clip_rect(LenoGUIPlatformRenderer* ren, int* x, int* y, int* w, int* h);
void   leno_gui_platform_disable_clip_rect(LenoGUIPlatformRenderer* ren);

/* ===== 纹理操作 ===== */
LenoGUIPlatformTexture* leno_gui_platform_create_texture(LenoGUIPlatformRenderer* ren, int w, int h);
void   leno_gui_platform_destroy_texture(LenoGUIPlatformTexture* tex);
void   leno_gui_platform_render_texture(LenoGUIPlatformRenderer* ren, LenoGUIPlatformTexture* tex, int x, int y);
void   leno_gui_platform_render_texture_src(LenoGUIPlatformRenderer* ren, LenoGUIPlatformTexture* tex,
                                             int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh);
void   leno_gui_platform_render_texture_rotated(LenoGUIPlatformRenderer* ren, LenoGUIPlatformTexture* tex,
                                                  int x, int y, double angle, int flip);
void   leno_gui_platform_update_texture(LenoGUIPlatformTexture* tex, const void* data, int pitch);
int    leno_gui_platform_texture_width(LenoGUIPlatformTexture* tex);
int    leno_gui_platform_texture_height(LenoGUIPlatformTexture* tex);

/* ===== 事件操作 ===== */
int    leno_gui_platform_poll_event(LenoGUIEvent* event);
int    leno_gui_platform_wait_event(LenoGUIEvent* event, int timeout_ms);

/* ===== 输入状态查询（参考 SDL_GetKeyboardState / SDL_GetMouseState） ===== */
int    leno_gui_platform_get_key_state(int key);
int    leno_gui_platform_get_mouse_state(int* x, int* y, int* buttons);

/* ===== 剪贴板（参考 SDL_GetClipboardText / SDL_SetClipboardText） ===== */
char*  leno_gui_platform_get_clipboard_text(void);
void   leno_gui_platform_set_clipboard_text(const char* text);

/* ===== 光标控制（参考 SDL_ShowCursor / SDL_HideCursor） ===== */
void   leno_gui_platform_show_cursor(int show);

/* ===== 窗口透明度（参考 SDL_SetWindowOpacity） ===== */
void   leno_gui_platform_set_window_opacity(LenoGUIPlatformWindow* win, float opacity);

/* ===== 消息框（参考 SDL_ShowSimpleMessageBox） ===== */
int    leno_gui_platform_show_message_box(const char* title, const char* message, int type);

/* ===== 高精度计时器（参考 SDL_GetTicks / SDL_GetPerformanceCounter） ===== */
uint64_t leno_gui_platform_get_ticks(void);
uint64_t leno_gui_platform_get_performance_counter(void);
uint64_t leno_gui_platform_get_performance_frequency(void);
void     leno_gui_platform_delay(uint32_t ms);

/* ===== 显示器信息 ===== */
void   leno_gui_platform_get_display_size(int* w, int* h);
float  leno_gui_platform_get_display_dpi(void);

#ifdef __cplusplus
}
#endif

#endif /* LENO_GUIS_H */
