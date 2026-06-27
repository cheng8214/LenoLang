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
    /* 文本框列表 */
    struct ObjGUITextBox* textboxes;   /* 文本框链表头 */
    int textbox_count;
    struct ObjGUITextBox* focused_textbox; /* 当前焦点文本框 */
    /* 标签列表 */
    struct ObjGUILabel* labels;        /* 标签链表头 */
    int label_count;
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

/* 间隙缓冲区 —— 仿 Scintilla 文档模型，纯 C */
typedef struct {
    char* buf;      /* 底层缓冲区（包含间隙） */
    int cap;        /* 总容量 */
    int gap_start;  /* 间隙起始索引 */
    int gap_end;    /* 间隙结束索引（exclusive） */
} GapBuffer;

/* Scintilla 风格行布局缓存 —— 缓存每行字符的像素偏移，避免每帧重测 */
typedef struct {
    int line;           /* 逻辑行号 */
    int start;          /* 行起始字节偏移 */
    int len;            /* 行长度（字节） */
    int char_count;     /* UTF-8 字符数 */
    int total_width;    /* 整行像素宽度 */
    int* offsets;       /* 每个字符结束位置的 x 偏移（相对于行首），长度 = char_count */
    int valid;          /* 是否有效 */
} LineLayout;

/* Undo/Redo 动作类型 */
typedef enum {
    TB_UNDO_INSERT,     /* 在 pos 处插入 text */
    TB_UNDO_DELETE,     /* 在 pos 处删除 text */
    TB_UNDO_REPLACE     /* 在 pos 处用 text 替换原 old_text */
} TBUndoType;

/* 单个撤销动作 */
typedef struct TBUndoAction {
    TBUndoType type;
    int pos;
    char* text;         /* 插入/删除/替换后的文本 */
    char* old_text;     /* 仅 REPLACE 时原文本 */
    int cursor_before;  /* 撤销后恢复的光标位置 */
    int cursor_after;   /* 重做后恢复的光标位置 */
    int sel_start;
    int sel_len;
    int group;          /* 所属分组，同组一起撤销 */
    struct TBUndoAction* next;
} TBUndoAction;

/* Undo/Redo 栈 */
typedef struct {
    TBUndoAction* top;
    int count;
    size_t total_size;             /* 栈中 action 占用的总字节数 */
} TBUndoStack;

/* GTextBox 文本框对象 */
typedef struct ObjGUITextBox {
    Object header;
    struct ObjGUITextBox* next;    /* 链表下一个 */
    ObjGUIWindow* window;          /* 所属窗口 */
    /* 位置和尺寸 */
    int x, y, width, height;
    /* 文字缓冲区 —— 间隙缓冲区 */
    GapBuffer gb;                  /* 当前文本内容 */
    char* placeholder;             /* 占位提示文字 */
    int cursor_pos;                /* 光标字节位置 */
    int sel_start;                 /* 选区起始（-1=无选区） */
    int sel_len;                   /* 选区长度 */
    int scroll_x;                  /* 水平滚动偏移（像素） */
    int scroll_y;                  /* 垂直滚动偏移（像素） */
    int dragging;                  /* 鼠标拖拽选择中 */
    int drag_start_cp;             /* 拖拽起始光标字节位置 */
    int multiline;                 /* 是否多行 */
    /* 滚动条拖拽 */
    int sb_h_dragging;             /* 正在拖拽水平滚动条 */
    int sb_v_dragging;             /* 正在拖拽垂直滚动条 */
    int sb_drag_start_mx;          /* 拖拽起始鼠标 X */
    int sb_drag_start_my;          /* 拖拽起始鼠标 Y */
    int sb_drag_start_sx;          /* 拖拽起始 scroll_x */
    int sb_drag_start_sy;          /* 拖拽起始 scroll_y */
    /* 颜色 */
    int bg_r, bg_g, bg_b, bg_a;
    int border_r, border_g, border_b, border_a;
    int focus_r, focus_g, focus_b, focus_a;
    int text_r, text_g, text_b, text_a;
    int placeholder_r, placeholder_g, placeholder_b, placeholder_a;
    int cursor_r, cursor_g, cursor_b, cursor_a;
    int sel_r, sel_g, sel_b, sel_a;
    int border_width;
    int radius;
    /* 字体 */
    char* font_name;
    int font_size;
    ObjGUIFont* font;
    int padding_x, padding_y;
    /* placeholder 独立字体（0=跟随主字体大小） */
    int placeholder_font_size;
    char* placeholder_font_name;
    ObjGUIFont* placeholder_font;
    /* 字间距 */
    int letter_spacing;            /* 字符间距（像素，0=默认） */
    /* 行间距 */
    int line_spacing;              /* 行间额外间距（像素，默认 4） */
    /* 滚动条颜色 */
    int sb_track_r, sb_track_g, sb_track_b, sb_track_a;           /* 轨道 */
    int sb_thumb_r, sb_thumb_g, sb_thumb_b, sb_thumb_a;           /* 滑块 */
    int sb_thumb_hover_r, sb_thumb_hover_g, sb_thumb_hover_b, sb_thumb_hover_a;   /* 悬停 */
    int sb_thumb_press_r, sb_thumb_press_g, sb_thumb_press_b, sb_thumb_press_a;   /* 按下 */
    int sb_h_hovered;              /* 水平滚动条被悬停 */
    int sb_v_hovered;              /* 垂直滚动条被悬停 */
    /* 状态 */
    int visible;
    int enabled;
    int focused;                   /* 键盘焦点 */
    int hovered;                   /* 鼠标悬停 */
    int password;                  /* 密码模式 */
    int max_length;                /* 最大字符数 (0=无限制) */
    /* 缓存：避免每帧遍历测量 */
    int text_is_dirty;             /* 文本已修改，需重测宽度 */
    int cached_max_text_width;     /* 最宽行像素宽度 */
    int cached_max_text_width_line;/* 最宽行所在行号（-1=未知） */
    int cached_cursor_x;           /* 缓存光标 X（仅移动时重算） */
    int cached_cursor_pos;         /* 缓存时的 cursor_pos */
    /* Scintilla 风格行索引：避免每次 O(n) 扫描全文找换行 */
    int* line_starts;              /* 动态数组，每行的字节偏移 */
    int line_count;                /* 当前行数 */
    int line_cap;                  /* line_starts 容量 */
    /* Scintilla View 层：行布局缓存 */
    LineLayout* layouts;           /* 每行布局缓存，长度 = line_count */
    int layout_cap;                /* layouts 容量 */
    /* 光标闪烁 */
    int blink_visible;
    uint64_t last_blink;
    /* 锚点布局 */
    int anchor;
    int anchor_margin_x;
    int anchor_margin_y;
    /* 颜色范围（语法高亮等） */
    #define TB_MAX_COLOR_RANGES 2048
    struct { int start; int len; int r, g, b, a; } color_ranges[2048];
    int color_range_count;
    /* Undo/Redo */
    TBUndoStack undo_stack;
    TBUndoStack redo_stack;
    int undo_group;                /* 当前分组编号 */
    int undo_grouping;             /* 是否处于分组中 */
    int undo_enabled;              /* 是否启用 undo */
    uint64_t undo_last_ticks;      /* 上一次 undo 操作时间戳 */
    int undo_last_pos;             /* 上一次 undo 操作结束光标位置 */
    TBUndoType undo_last_type;     /* 上一次 undo 操作类型 */
    int undo_last_group;           /* 上一次 undo 操作分组 */
    /* 回调 */
    Value on_change;               /* 文本改变回调 */
    Value on_submit;               /* 回车提交回调 */
} ObjGUITextBox;

/* GLabel 标签对象 - 静态文本显示 */
typedef struct ObjGUILabel {
    Object header;
    struct ObjGUILabel* next;      /* 链表下一个 */
    ObjGUIWindow* window;          /* 所属窗口 */
    /* 位置和尺寸 */
    int x, y, width, height;
    /* 文字 */
    char* text;
    /* 颜色 */
    int text_r, text_g, text_b, text_a;     /* 文字色 */
    int bg_r, bg_g, bg_b, bg_a;             /* 背景色 (a=0 则透明) */
    /* 字体 */
    char* font_name;
    int font_size;
    ObjGUIFont* font;
    int font_bold;                          /* 0=正常, 1=粗体 */
    /* 布局 */
    int padding_x, padding_y;
    int text_align;               /* 0=左, 1=中, 2=右 */
    int letter_spacing;
    int radius;
    /* 边框 */
    int border_width;
    int border_r, border_g, border_b, border_a;
    /* 透明度 */
    int opacity;                  /* 整体不透明度 (0~255, 255=不透明) */
    /* 阴影 */
    int shadow_offset_x, shadow_offset_y;   /* 阴影偏移 */
    int shadow_radius;                      /* 阴影模糊半径 (0=无阴影) */
    int shadow_r, shadow_g, shadow_b, shadow_a; /* 阴影颜色 */
    /* 状态 */
    int visible;
    int enabled;
    /* 锚点布局 */
    int anchor;
    int anchor_margin_x;
    int anchor_margin_y;
} ObjGUILabel;

/* 辅助函数 */
ObjGUIWindow* as_window(Value v);
ObjGUIRenderer* as_renderer(Value v);
ObjGUIFont* as_font(Value v);
ObjGUIImage* as_image(Value v);
ObjGUIButton* as_button(Value v);
ObjGUITextBox* as_textbox(Value v);
ObjGUILabel* as_label(Value v);
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

/* GTextBox 文本框绘制和事件处理 */
void gui_textbox_draw_all(ObjGUIWindow* win, ObjGUIRenderer* ren);
int  gui_textbox_handle_event(ObjGUIWindow* win, LenoGUIEvent* ev);
void gui_textbox_free_all(ObjGUIWindow* win);
void gui_textbox_update_anchors(ObjGUIWindow* win, int win_w, int win_h);
void gui_textbox_update_placeholder_font(ObjGUITextBox* tb);
void gui_textbox_register_methods(void);
extern void guis_init_textbox_instance_methods(void);

/* TextBox 内部资源释放（供 GC free_object_resources 调用） */
extern void tb_free_layouts(ObjGUITextBox* tb);
extern void tb_undo_free_stack(TBUndoStack* stack);

/* GLabel 标签绘制和锚点 */
void gui_label_draw_all(ObjGUIWindow* win, ObjGUIRenderer* ren);
void gui_label_free_all(ObjGUIWindow* win);
void gui_label_update_anchors(ObjGUIWindow* win, int win_w, int win_h);
extern void guis_init_label_instance_methods(void);

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
