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
    /* 内边距 */
    int padding_x;                 /* 水平内边距 */
    int padding_y;                 /* 垂直内边距 */
    /* 文字对齐: 0=左, 1=中, 2=右 */
    int text_align;
    /* 字体粗细 */
    int font_bold;                 /* 0=正常, 1=粗体 */
    /* 字间距 */
    int letter_spacing;
    /* 圆角 */
    int radius;
    int radius_tl, radius_tr, radius_bl, radius_br; /* 独立圆角 (0=使用 radius) */
    /* 透明度 (0~255, 255=不透明) */
    int opacity;
    /* 渐变背景 */
    int gradient_count;            /* 渐变颜色数量 (0=无渐变) */
    int gradient_r[4], gradient_g[4], gradient_b[4], gradient_a[4]; /* 最多4个渐变停止点 */
    int gradient_radial;           /* 0=线性, 1=径向 */
    /* 边框 */
    int border_width;              /* 边框宽度 (0=无边框) */
    int border_r, border_g, border_b, border_a; /* 边框颜色 */
    int border_style;              /* 0=solid, 1=dashed, 2=dotted */
    /* 阴影 */
    int shadow_offset_x, shadow_offset_y; /* 阴影偏移 */
    int shadow_radius;             /* 阴影模糊半径 (0=无阴影) */
    int shadow_r, shadow_g, shadow_b, shadow_a; /* 阴影颜色 */
    /* 文字装饰: 0=none, 1=underline, 2=strikethrough, 3=overline */
    int text_decoration;
    /* 焦点样式 */
    int focus_width;
    int focus_r, focus_g, focus_b, focus_a;
    /* 悬停缩放 */
    float hover_scale;
    /* 加载状态 */
    int loading;
    /* 悬停光标 */
    char* cursor;
    /* 按下效果: 0=关闭, 1=开启默认效果(偏移+变暗) */
    int press_effect;
    /* 按下偏移像素数 (默认1) */
    int press_offset;
    /* 状态 */
    int visible;
    int enabled;
    int hovered;                   /* 鼠标是否悬停 */
    int pressed;                   /* 鼠标是否按下 */
    int focused;                   /* 是否获得焦点 */
    /* 回调 */
    Value on_click;                /* 点击回调闭包 */
    /* 锚点布局: 0=无, 1=左上, 2=右上, 3=左下, 4=右下, 5=中, 6=上中, 7=下中 */
    int anchor;
    int anchor_margin_x;           /* 锚点水平边距 */
    int anchor_margin_y;           /* 锚点垂直边距 */
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
void gui_button_update_anchors(ObjGUIWindow* win, int win_w, int win_h);

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
