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

/* stb_image 类型定义 */
typedef unsigned short stbi_us;

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
#define LENO_GUI_EVT_WINDOW_EXPOSED    0x208  /* 窗口需要重绘（拖动/遮挡后暴露） */
#define LENO_GUI_EVT_WINDOW_MINIMIZED  0x209  /* 窗口已最小化 */
#define LENO_GUI_EVT_WINDOW_MAXIMIZED  0x20A  /* 窗口已最大化 */
#define LENO_GUI_EVT_WINDOW_RESTORED   0x20B  /* 窗口已恢复（从最小化/最大化） */
#define LENO_GUI_EVT_KEY_DOWN          0x300
#define LENO_GUI_EVT_KEY_UP            0x301
#define LENO_GUI_EVT_TEXT_INPUT        0x302
#define LENO_GUI_EVT_MOUSE_MOVE        0x400
#define LENO_GUI_EVT_MOUSE_DOWN        0x401
#define LENO_GUI_EVT_MOUSE_UP          0x402
#define LENO_GUI_EVT_MOUSE_WHEEL       0x403
#define LENO_GUI_EVT_DROP_FILE         0x500  /* 文件拖放 */
#define LENO_GUI_EVT_DROP_TEXT         0x501  /* 文本拖放 */
#define LENO_GUI_EVT_DROP_BEGIN        0x502  /* 拖放开始 */
#define LENO_GUI_EVT_DROP_COMPLETE     0x503  /* 拖放完成 */
#define LENO_GUI_EVT_FILEDIALOG_RESULT 0x600  /* 文件对话框结果（内部使用） */

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
    /* 拖放数据 */
    char drop_file[512];  /* 拖放的文件路径 */
} LenoGUIEvent;

/* ===== 平台窗口/渲染器/图像（不透明指针） ===== */
typedef struct LenoGUIPlatformWindow LenoGUIPlatformWindow;
typedef struct LenoGUIPlatformRenderer LenoGUIPlatformRenderer;
typedef struct LenoGUIPlatformImage LenoGUIPlatformImage;

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
void   leno_gui_platform_set_window_drag_area(LenoGUIPlatformWindow* win, int x, int y, int w, int h);
void   leno_gui_platform_clear_window_drag_area(LenoGUIPlatformWindow* win);

/* ===== 翻转标志（参考 SDL_FlipMode） ===== */
#define LENO_GUI_FLIP_NONE       0
#define LENO_GUI_FLIP_HORIZONTAL 1
#define LENO_GUI_FLIP_VERTICAL   2

/* ===== 逻辑呈现模式（借鉴 SDL3 SDL_RendererLogicalPresentation）===== */
#define LENO_GUI_LOGICAL_PRESENTATION_DISABLED      0  /* 禁用逻辑大小 */
#define LENO_GUI_LOGICAL_PRESENTATION_STRETCH       1  /* 拉伸填充 */
#define LENO_GUI_LOGICAL_PRESENTATION_LETTERBOX     2  /* 信箱模式（保持比例） */
#define LENO_GUI_LOGICAL_PRESENTATION_OVERSCAN      3  /* 过扫描 */
#define LENO_GUI_LOGICAL_PRESENTATION_INTEGER_SCALE 4  /* 整数倍缩放 */

/* ===== 渲染器操作 ===== */
LenoGUIPlatformRenderer* leno_gui_platform_create_renderer(LenoGUIPlatformWindow* win);
void   leno_gui_platform_destroy_renderer(LenoGUIPlatformRenderer* ren);
void   leno_gui_platform_render_clear(LenoGUIPlatformRenderer* ren);
void   leno_gui_platform_render_present(LenoGUIPlatformRenderer* ren);
/* 当窗口大小改变时，重新调整渲染器大小 */
int    leno_gui_platform_renderer_resize(LenoGUIPlatformRenderer* ren, int w, int h);

/* ===== 逻辑呈现模式（借鉴 SDL3）===== */
void   leno_gui_platform_set_logical_size(LenoGUIPlatformRenderer* ren, int w, int h);
void   leno_gui_platform_get_logical_size(LenoGUIPlatformRenderer* ren, int* w, int* h);
void   leno_gui_platform_set_logical_presentation(LenoGUIPlatformRenderer* ren, int mode);
int    leno_gui_platform_get_logical_presentation(LenoGUIPlatformRenderer* ren);
void   leno_gui_platform_get_logical_viewport(LenoGUIPlatformRenderer* ren, int* x, int* y, int* w, int* h);
void   leno_gui_platform_reset_logical_size(LenoGUIPlatformRenderer* ren);
/* 标记渲染器需要调整大小（参考 SDL3 surface_valid） */
void   leno_gui_platform_renderer_mark_resize(LenoGUIPlatformRenderer* ren);
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

/* ===== 图像访问模式（借鉴 SDL3）===== */
#define LENO_GUI_IMAGEACCESS_STATIC    0  /* 变化少，不可锁定 */
#define LENO_GUI_IMAGEACCESS_STREAMING 1  /* 变化频繁，可锁定 */
#define LENO_GUI_IMAGEACCESS_TARGET    2  /* 可作为渲染目标 */

/* ===== 图像操作 ===== */
LenoGUIPlatformImage* leno_gui_platform_create_image(LenoGUIPlatformRenderer* ren, int w, int h);
LenoGUIPlatformImage* leno_gui_platform_create_image_with_access(LenoGUIPlatformRenderer* ren, int w, int h, int access);
void   leno_gui_platform_destroy_image(LenoGUIPlatformImage* tex);
void   leno_gui_platform_render_image(LenoGUIPlatformRenderer* ren, LenoGUIPlatformImage* tex, int x, int y);
void   leno_gui_platform_render_image_src(LenoGUIPlatformRenderer* ren, LenoGUIPlatformImage* tex,
                                             int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh);
void   leno_gui_platform_render_image_rotated(LenoGUIPlatformRenderer* ren, LenoGUIPlatformImage* tex,
                                                  int x, int y, double angle, int flip);
void   leno_gui_platform_render_image_rotated_src(LenoGUIPlatformRenderer* ren, LenoGUIPlatformImage* tex,
                                                      int sx, int sy, int sw, int sh,
                                                      int dx, int dy, int dw, int dh,
                                                      double angle, int flip);
void   leno_gui_platform_update_image(LenoGUIPlatformImage* tex, const void* data, int pitch);
int    leno_gui_platform_image_width(LenoGUIPlatformImage* tex);
int    leno_gui_platform_image_height(LenoGUIPlatformImage* tex);
int    leno_gui_platform_image_access(LenoGUIPlatformImage* tex);

/* ===== 渲染目标（离屏渲染，借鉴 SDL3）===== */
int    leno_gui_platform_set_render_target(LenoGUIPlatformRenderer* ren, LenoGUIPlatformImage* tex);
LenoGUIPlatformImage* leno_gui_platform_get_render_target(LenoGUIPlatformRenderer* ren);
void   leno_gui_platform_reset_render_target(LenoGUIPlatformRenderer* ren);
void   leno_gui_platform_render_target_to_window(LenoGUIPlatformRenderer* ren, LenoGUIPlatformImage* tex,
                                                  int x, int y, int w, int h);
void   leno_gui_platform_clear_render_target(LenoGUIPlatformImage* tex, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
const uint32_t* leno_gui_platform_get_render_target_pixels(LenoGUIPlatformImage* tex);
int    leno_gui_platform_get_render_target_pitch(LenoGUIPlatformImage* tex);
int    leno_gui_platform_copy_render_target(LenoGUIPlatformImage* dst, LenoGUIPlatformImage* src);
void   leno_gui_platform_blend_render_targets(LenoGUIPlatformImage* dst, LenoGUIPlatformImage* src,
                                               int x, int y, uint8_t alpha);
int    leno_gui_platform_resize_render_target(LenoGUIPlatformImage* tex, int w, int h);

/* ===== 文字渲染（内置 8x8 点阵字体，参考 SDL3 SDL_RenderDebugText） ===== */
void   leno_gui_platform_draw_text(LenoGUIPlatformRenderer* ren, const char* text, int x, int y, int size);
void   leno_gui_platform_text_size(const char* text, int size, int* w, int* h);

/* ===== 字体操作（系统字体渲染） ===== */
typedef struct LenoGUIPlatformFont LenoGUIPlatformFont;
LenoGUIPlatformFont* leno_gui_platform_load_font(const char* name, int size);
void   leno_gui_platform_destroy_font(LenoGUIPlatformFont* font);
void   leno_gui_platform_draw_text_font(LenoGUIPlatformRenderer* ren, LenoGUIPlatformFont* font, const char* text, int x, int y);
void   leno_gui_platform_text_size_font(LenoGUIPlatformFont* font, const char* text, int* w, int* h);

/* ===== 事件过滤器（借鉴 SDL3）===== */
typedef int (*LenoGUIEventFilterFunc)(void* userdata, LenoGUIEvent* event);
void   leno_gui_platform_add_event_filter(void* userdata, LenoGUIEventFilterFunc filter);
void   leno_gui_platform_remove_event_filter(void* userdata, LenoGUIEventFilterFunc filter);

/* ===== 全局渲染状态（参考 SDL3 主回调机制） ===== */
typedef void (*LenoGUIRenderCallback)(void* user_data);
typedef void (*LenoGUIEventCallback)(void* user_data, LenoGUIEvent* event);

void   leno_gui_platform_set_main_callbacks(LenoGUIPlatformWindow* win, 
                                             LenoGUIPlatformRenderer* ren,
                                             LenoGUIRenderCallback render_cb,
                                             LenoGUIEventCallback event_cb,
                                             void* user_data);
int    leno_gui_platform_iterate_main_callbacks(void);
void   leno_gui_platform_request_redraw(void);

/* ===== 事件操作 ===== */
int    leno_gui_platform_poll_event(LenoGUIEvent* event);
int    leno_gui_platform_wait_event(LenoGUIEvent* event, int timeout_ms);

/* ===== 输入状态查询（参考 SDL_GetKeyboardState / SDL_GetMouseState） ===== */
int    leno_gui_platform_get_key_state(int key);
int    leno_gui_platform_get_mouse_state(int* x, int* y, int* buttons);

/* ===== 键盘状态跟踪（参考 SDL3 prev/curr 按键状态数组） ===== */
/* 更新键盘状态（每次 poll 前由平台层调用，比较 prev/curr 产生 is_pressed/is_released） */
void   leno_gui_platform_update_key_states(void);
/* 检查按键是否刚被按下（从上一帧的 up 变为当前帧的 down） */
int    leno_gui_platform_is_key_pressed(int key);
/* 检查按键是否刚被释放（从上一帧的 down 变为当前帧的 up） */
int    leno_gui_platform_is_key_released(int key);

/* ===== 文本输入控制（参考 SDL_StartTextInput / SDL_StopTextInput） ===== */
void   leno_gui_platform_start_text_input(void);
void   leno_gui_platform_stop_text_input(void);
int    leno_gui_platform_is_text_input_active(void);

/* ===== 剪贴板（参考 SDL_GetClipboardText / SDL_SetClipboardText） ===== */
char*  leno_gui_platform_get_clipboard_text(void);
void   leno_gui_platform_set_clipboard_text(const char* text);

/* ===== 系统光标类型（参考 SDL3 SDL_SystemCursor） ===== */
#define LENO_GUI_CURSOR_DEFAULT        0   /* 默认箭头 */
#define LENO_GUI_CURSOR_TEXT           1   /* 文本选择 I 型 */
#define LENO_GUI_CURSOR_WAIT           2   /* 等待（沙漏） */
#define LENO_GUI_CURSOR_CROSSHAIR      3   /* 十字准星 */
#define LENO_GUI_CURSOR_PROGRESS       4   /* 后台忙（带箭头沙漏） */
#define LENO_GUI_CURSOR_RESIZE_NWSE    5   /* 西北-东南双箭头 */
#define LENO_GUI_CURSOR_RESIZE_NESW    6   /* 东北-西南双箭头 */
#define LENO_GUI_CURSOR_RESIZE_EW      7   /* 东西双箭头 */
#define LENO_GUI_CURSOR_RESIZE_NS      8   /* 南北双箭头 */
#define LENO_GUI_CURSOR_MOVE           9   /* 四向箭头（移动） */
#define LENO_GUI_CURSOR_NOT_ALLOWED    10  /* 禁止 */
#define LENO_GUI_CURSOR_POINTER        11  /* 链接手型 */
#define LENO_GUI_CURSOR_COUNT          12

/* ===== 光标控制（参考 SDL_ShowCursor / SDL_HideCursor） ===== */
void   leno_gui_platform_show_cursor(int show);
/* 设置系统光标 */
void   leno_gui_platform_set_system_cursor(int cursor_type);
/* 设置自定义光标（从 ARGB 像素数据创建） */
int    leno_gui_platform_create_custom_cursor(const uint32_t* pixels, int w, int h, int hot_x, int hot_y);
void   leno_gui_platform_destroy_custom_cursor(void);
void   leno_gui_platform_set_cursor(int cursor_id);

/* ===== 窗口透明度（参考 SDL_SetWindowOpacity） ===== */
void   leno_gui_platform_set_window_opacity(LenoGUIPlatformWindow* win, float opacity);

/* ===== 拖放支持（参考 SDL3 Drop Events） ===== */
void   leno_gui_platform_accept_drag_and_drop(LenoGUIPlatformWindow* win, int accept);

/* ===== 文件对话框（参考 SDL3 SDL_ShowFileDialogWithProperties） ===== */
#define LENO_GUI_FILEDIALOG_OPENFILE     0
#define LENO_GUI_FILEDIALOG_SAVEFILE     1
#define LENO_GUI_FILEDIALOG_OPENFOLDER   2

/* 文件对话框回调 */
typedef void (*LenoGUIFileDialogCallback)(void* userdata, const char* const* files, int filter_index);

/* 文件过滤器 */
typedef struct {
    const char* name;   /* 显示名称，如 "Text Files" */
    const char* pattern; /* 匹配模式，如 "*.txt" */
} LenoGUIFileFilter;

void   leno_gui_platform_show_file_dialog(int type, LenoGUIFileDialogCallback callback,
                                           void* userdata, LenoGUIPlatformWindow* win,
                                           const LenoGUIFileFilter* filters, int nfilters,
                                           const char* default_path, int allow_many,
                                           const char* title);

/* ===== 窗口图标（参考 SDL_SetWindowIcon） ===== */
void   leno_gui_platform_set_window_icon(LenoGUIPlatformWindow* win, const uint32_t* pixels, int w, int h);

/* ===== 窗口最小/最大尺寸限制（参考 SDL_SetWindowMinimumSize / SDL_SetWindowMaximumSize） ===== */
void   leno_gui_platform_set_window_minimum_size(LenoGUIPlatformWindow* win, int min_w, int min_h);
void   leno_gui_platform_set_window_maximum_size(LenoGUIPlatformWindow* win, int max_w, int max_h);

/* ===== 消息框（参考 SDL_ShowSimpleMessageBox） ===== */
int    leno_gui_platform_show_message_box(const char* title, const char* message, int type);

/* ===== 文件对话框结果处理（由主循环调用，线程安全） ===== */
int    leno_gui_platform_process_filedialog_result(void);

/* ===== 高精度计时器（参考 SDL_GetTicks / SDL_GetPerformanceCounter） ===== */
uint64_t leno_gui_platform_get_ticks(void);
uint64_t leno_gui_platform_get_performance_counter(void);
uint64_t leno_gui_platform_get_performance_frequency(void);
void     leno_gui_platform_delay(uint32_t ms);

/* ===== 显示器信息 ===== */
void   leno_gui_platform_get_display_size(int* w, int* h);
float  leno_gui_platform_get_display_dpi(void);

/* ===== 图片加载（参考 SDL3 SDL_LoadSurface / SDL_LoadPNG） ===== */
LenoGUIPlatformImage* leno_gui_platform_load_image(const char* filepath);
LenoGUIPlatformImage* leno_gui_platform_load_image_mem(const unsigned char* data, int len);
const char* leno_gui_platform_get_image_error(void);

/* 图片信息查询（不加载像素数据） */
int leno_gui_platform_get_image_info(const char* filepath, int* w, int* h, int* channels);
int leno_gui_platform_get_image_info_mem(const unsigned char* data, int len, int* w, int* h, int* channels);

/* 设置图片加载选项 */
void leno_gui_platform_set_flip_vertically_on_load(int flag);

/* 高级图片加载选项 */
void leno_gui_platform_set_unpremultiply_on_load(int flag);
void leno_gui_platform_convert_iphone_png_to_rgb(int flag);

/* 16位/HDR图片检测 */
int leno_gui_platform_is_16_bit(const char* filepath);
int leno_gui_platform_is_16_bit_from_memory(const unsigned char* data, int len);
int leno_gui_platform_is_hdr(const char* filepath);
int leno_gui_platform_is_hdr_from_memory(const unsigned char* data, int len);

/* 16位深度图片加载 - 返回原始像素数据 */
stbi_us* leno_gui_platform_load_image_16_raw(const char* filepath, int* w, int* h, int* channels);
stbi_us* leno_gui_platform_load_image_16_raw_mem(const unsigned char* data, int len, int* w, int* h, int* channels);
void leno_gui_platform_free_16_pixels(stbi_us* pixels);

/* zlib解压支持 */
char* leno_gui_platform_zlib_decode_malloc(const char* buffer, int len, int* outlen);
int leno_gui_platform_zlib_decode_buffer(char* obuffer, int olen, const char* ibuffer, int ilen);
char* leno_gui_platform_zlib_decode_noheader_malloc(const char* buffer, int len, int* outlen);
int leno_gui_platform_zlib_decode_noheader_buffer(char* obuffer, int olen, const char* ibuffer, int ilen);

#ifdef __cplusplus
}
#endif

#endif /* LENO_GUIS_H */
