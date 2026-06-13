/* Leno GUI - 内部共享定义
 * 供 guis.c、guis_draw.c、guis_event.c 共享使用
 */

#ifndef GUIS_INTERNAL_H
#define GUIS_INTERNAL_H

#include "include/leno_value.h"
#include "leno_guis.h"

/* GUI 对象类型定义 (GWin, GDraw, GFont, GImage, GEvent, GRgb, GButton) */
typedef struct {
    Object header;
    LenoGUIPlatformWindow* platform;
    /* 窗口背景色（创建时从 style 读取，在渲染循环中应用） */
    int bg_r, bg_g, bg_b, bg_a;
    int use_bg_color;       /* 是否使用自定义背景色 */
    int vsync_enabled;      /* 是否启用帧率限制（垂直同步模拟） */
    /* 按钮列表 */
    struct ObjGUIButton* buttons;      /* 按钮链表头 */
    int button_count;
} ObjGUIWindow;

typedef struct {
    Object header;
    LenoGUIPlatformRenderer* platform;
    ObjGUIWindow* window;
} ObjGUIRenderer;

typedef struct {
    Object header;
    LenoGUIPlatformFont* platform;
} ObjGUIFont;

typedef struct {
    Object header;
    LenoGUIPlatformImage* platform;
} ObjGUIImage;

typedef struct {
    Object header;
    ObjDict* data;
} ObjGUIEvent;

/* GButton 按钮对象 - 自绘按钮 */
typedef struct ObjGUIButton {
    Object header;
    struct ObjGUIButton* next;     /* 链表下一个按钮 */
    ObjGUIWindow* window;          /* 所属窗口 */
    /* 位置和尺寸 */
    int x, y, width, height;
    /* 文字 */
    char* text;
    /* 颜色 */
    int bg_r, bg_g, bg_b, bg_a;           /* 背景色 */
    int hover_r, hover_g, hover_b, hover_a; /* 悬停色 */
    int press_r, press_g, press_b, press_a; /* 按下色 */
    int text_r, text_g, text_b, text_a;     /* 文字色 */
    /* 字体 */
    char* font_name;
    int font_size;
    ObjGUIFont* font;              /* 缓存的字体对象 */
    /* 圆角 */
    int radius;
    /* 透明度 (0~255, 255=不透明) */
    int opacity;
    /* 边框 */
    int border_width;              /* 边框宽度 (0=无边框) */
    int border_r, border_g, border_b, border_a; /* 边框颜色 */
    /* 阴影 */
    int shadow_offset_x, shadow_offset_y; /* 阴影偏移 */
    int shadow_radius;             /* 阴影模糊半径 (0=无阴影) */
    int shadow_r, shadow_g, shadow_b, shadow_a; /* 阴影颜色 */
    /* 状态 */
    int visible;
    int enabled;
    int hovered;                   /* 鼠标是否悬停 */
    int pressed;                   /* 鼠标是否按下 */
    /* 回调 */
    Value on_click;                /* 点击回调闭包 */
} ObjGUIButton;

/* 辅助函数 */
ObjGUIWindow* as_window(Value v);
ObjGUIRenderer* as_renderer(Value v);
ObjGUIFont* as_font(Value v);
ObjGUIImage* as_image(Value v);
ObjGUIButton* as_button(Value v);
ObjArray* make_int_array2(int a, int b);
ObjGUIWindow* as_window_from_platform(LenoGUIPlatformWindow* pw);

/* 事件字符串键（由 guis.c 定义） */
extern ObjString* str_key_type;
extern ObjString* str_key_window_id;
extern ObjString* str_key_width;
extern ObjString* str_key_height;
extern ObjString* str_key_x;
extern ObjString* str_key_y;
extern ObjString* str_key_key;
extern ObjString* str_key_scancode;
extern ObjString* str_key_mod;
extern ObjString* str_key_repeat;
extern ObjString* str_key_text;
extern ObjString* str_key_xrel;
extern ObjString* str_key_yrel;
extern ObjString* str_key_button;
extern ObjString* str_key_clicks;
extern ObjString* str_key_wheel_x;
extern ObjString* str_key_wheel_y;

void init_event_string_keys(void);
void dict_add_int_key(ObjDict* d, ObjString* key, int value);
void dict_add_float_key(ObjDict* d, ObjString* key, float value);
void dict_add_string_key(ObjDict* d, ObjString* key, const char* value);

/* 事件转换 */
Value event_to_dict(LenoGUIEvent* ev);

/* 创建对象 */
ObjGUIWindow* create_window_obj(LenoGUIPlatformWindow* pw);
ObjGUIRenderer* create_renderer_obj(LenoGUIPlatformRenderer* pr, ObjGUIWindow* win);

/* 调用 Leno 闭包 */
Value call_leno_closure(Value callee, int arg_count, Value* args);

/* GButton 按钮绘制和事件处理 */
void gui_button_draw_all(ObjGUIWindow* win, ObjGUIRenderer* ren);
int gui_button_handle_event(ObjGUIWindow* win, LenoGUIEvent* ev);
void gui_button_free_all(ObjGUIWindow* win);

/* 文件对话框结果处理（由平台层调用，确保在主线程中执行） */
void process_filedialog_callback(const char* const* files, int nfiles, int filter_index);

/* ===== Style 字段类型系统（供 LSP 使用） ===== */

typedef enum {
    STYLE_TYPE_STRING,
    STYLE_TYPE_INT,
    STYLE_TYPE_FLOAT,
    STYLE_TYPE_BOOL,
    STYLE_TYPE_COLOR,
    STYLE_TYPE_ENUM,
} StyleFieldType;

typedef struct {
    const char* name;
    StyleFieldType type;
    const char* description;
    const char* default_value;
    const char** options;
    int option_count;
} StyleFieldInfo;

// 获取字段名称列表（返回的数组需要 free 释放）
const char** guis_get_style_fields(const char* target, int* count);

// 获取字段详细信息
StyleFieldInfo* guis_get_style_field_info(const char* target, const char* field_name);

// 获取字段类型名称字符串
const char* guis_style_field_type_name(StyleFieldType type);

#endif /* GUIS_INTERNAL_H */
