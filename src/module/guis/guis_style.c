/* Leno GUI - Style 字段定义（供 LSP 补全使用）
 * 从 guis.c 拆分出来的 Style 相关定义
 */
#include <string.h>
#include <stdlib.h>

/* ===== Style 字段类型定义 ===== */

typedef enum {
    STYLE_TYPE_STRING,   // 字符串
    STYLE_TYPE_INT,      // 整数
    STYLE_TYPE_FLOAT,    // 浮点数
    STYLE_TYPE_BOOL,     // 布尔值
    STYLE_TYPE_COLOR,    // 颜色 (RGB)
    STYLE_TYPE_ENUM,     // 枚举值（有固定选项）
} StyleFieldType;

typedef struct {
    const char* name;
    StyleFieldType type;
    const char* description;       // 字段说明
    const char* default_value;     // 默认值（字符串形式）
    const char** options;          // 枚举选项（仅 STYLE_TYPE_ENUM 使用）
    int option_count;              // 选项数量
} StyleFieldInfo;

typedef struct {
    const char* target;
    StyleFieldInfo* fields;
    int field_count;
} StyleDef;

/* ===== Window 样式字段定义 ===== */

static const char* cursor_options[] = {
    "arrow", "hand", "ibeam", "crosshair", "wait",
    "progress", "resize_nwse", "resize_nesw", "resize_ew", "resize_ns",
    "move", "not_allowed", "none"
};

static StyleFieldInfo window_style_fields[] = {
    // ===== 尺寸 =====
    { "width",        STYLE_TYPE_INT,    "窗口宽度",                    "800",       NULL, 0 },
    { "height",       STYLE_TYPE_INT,    "窗口高度",                    "600",       NULL, 0 },
    { "min_width",    STYLE_TYPE_INT,    "最小宽度（0=不限制）",          "0",         NULL, 0 },
    { "min_height",   STYLE_TYPE_INT,    "最小高度（0=不限制）",          "0",         NULL, 0 },
    { "max_width",    STYLE_TYPE_INT,    "最大宽度（0=不限制）",          "0",         NULL, 0 },
    { "max_height",   STYLE_TYPE_INT,    "最大高度（0=不限制）",          "0",         NULL, 0 },
    // ===== 位置 =====
    { "x",            STYLE_TYPE_INT,    "窗口X坐标（屏幕位置）",        "0",         NULL, 0 },
    { "y",            STYLE_TYPE_INT,    "窗口Y坐标（屏幕位置）",        "0",         NULL, 0 },
    { "center",       STYLE_TYPE_BOOL,   "是否居中显示（优先于x/y）",     "false",     NULL, 0 },
    // ===== 外观 =====
    { "title",        STYLE_TYPE_STRING, "窗口标题",                    "\"LenoC\"",  NULL, 0 },
    { "icon",         STYLE_TYPE_STRING, "窗口图标路径（空=不使用）",      "\"\"",      NULL, 0 },
    { "bg_color",     STYLE_TYPE_COLOR,  "窗口背景颜色",                 "_rgb(255,255,255)", NULL, 0 },
    { "fullscreen",   STYLE_TYPE_BOOL,   "是否全屏",                    "false",     NULL, 0 },
    { "borderless",   STYLE_TYPE_BOOL,   "是否无边框",                  "false",     NULL, 0 },
    { "resizable",    STYLE_TYPE_BOOL,   "是否可拖拽边缘调整大小（独立于 maximizable）", "true", NULL, 0 },
    { "opacity",      STYLE_TYPE_FLOAT,  "窗口透明度 (0.0~1.0)",        "1.0",       NULL, 0 },
    // ===== 行为 =====
    { "visible",      STYLE_TYPE_BOOL,   "是否可见",                    "true",      NULL, 0 },
    { "always_on_top",STYLE_TYPE_BOOL,   "是否始终置顶",                 "false",     NULL, 0 },
    { "maximized",    STYLE_TYPE_BOOL,   "是否最大化启动",               "false",     NULL, 0 },
    { "maximizable",  STYLE_TYPE_BOOL,   "是否允许最大化按钮（独立于 resizable）", "true",  NULL, 0 },
    { "vsync",        STYLE_TYPE_BOOL,   "是否垂直同步（帧率限制60fps）", "true",      NULL, 0 },
    { "cursor",       STYLE_TYPE_ENUM,   "鼠标光标样式",                 "\"arrow\"", cursor_options, 13 },
};

/* ===== Button 样式字段定义 ===== */

static const char* text_align_options[] = { "left", "center", "right" };
static const char* border_style_options[] = { "solid", "dashed", "dotted" };
static const char* text_decoration_options[] = { "none", "underline", "strikethrough", "overline" };

static StyleFieldInfo button_style_fields[] = {
    /* ===== 位置和尺寸 ===== */
    { "x",              STYLE_TYPE_INT,    "X坐标",                       "0",                 NULL, 0 },
    { "y",              STYLE_TYPE_INT,    "Y坐标",                       "0",                 NULL, 0 },
    { "width",          STYLE_TYPE_INT,    "按钮宽度",                    "100",               NULL, 0 },
    { "height",         STYLE_TYPE_INT,    "按钮高度",                    "40",                NULL, 0 },
    /* ===== 文本 ===== */
    { "text",           STYLE_TYPE_STRING, "按钮文本",                    "\"Button\"",        NULL, 0 },
    { "text_color",     STYLE_TYPE_COLOR,  "文本颜色",                    "_rgb(0,0,0)",       NULL, 0 },
    { "font",           STYLE_TYPE_STRING, "字体名称（空=使用默认字体）",  "\"\"",              NULL, 0 },
    { "font_size",      STYLE_TYPE_INT,    "字体大小",                    "16",                NULL, 0 },
    { "font_bold",      STYLE_TYPE_BOOL,   "是否粗体",                    "false",             NULL, 0 },
    { "text_align",     STYLE_TYPE_ENUM,   "文本对齐方式",                "\"center\"",        text_align_options, 3 },
    { "letter_spacing", STYLE_TYPE_INT,    "字间距（像素）",              "0",                 NULL, 0 },
    { "text_decoration",STYLE_TYPE_ENUM,   "文字装饰线",                  "\"none\"",          text_decoration_options, 4 },
    /* ===== 颜色 ===== */
    { "bg_color",       STYLE_TYPE_COLOR,  "背景颜色",                    "_rgb(200,200,200)", NULL, 0 },
    { "hover_color",    STYLE_TYPE_COLOR,  "悬停颜色（未设置则默认变淡）", "_rgb(220,220,220)", NULL, 0 },
    { "press_color",    STYLE_TYPE_COLOR,  "按下颜色",                    "_rgb(180,180,180)", NULL, 0 },
    { "opacity",        STYLE_TYPE_INT,    "整体不透明度 (0~255)",        "255",               NULL, 0 },
    /* ===== 圆角 ===== */
    { "radius",         STYLE_TYPE_INT,    "圆角半径（统一）",            "4",                 NULL, 0 },
    { "radius_tl",      STYLE_TYPE_INT,    "左上圆角半径",                "4",                 NULL, 0 },
    { "radius_tr",      STYLE_TYPE_INT,    "右上圆角半径",                "4",                 NULL, 0 },
    { "radius_bl",      STYLE_TYPE_INT,    "左下圆角半径",                "4",                 NULL, 0 },
    { "radius_br",      STYLE_TYPE_INT,    "右下圆角半径",                "4",                 NULL, 0 },
    /* ===== 渐变 ===== */
    { "gradient",       STYLE_TYPE_COLOR,  "渐变颜色数组 [c1, c2, ...]",  "[]",                NULL, 0 },
    { "gradient_radial",STYLE_TYPE_BOOL,   "是否径向渐变",                "false",             NULL, 0 },
    /* ===== 边框 ===== */
    { "border_width",   STYLE_TYPE_INT,    "边框宽度",                    "0",                 NULL, 0 },
    { "border_color",   STYLE_TYPE_COLOR,  "边框颜色",                    "_rgb(0,0,0)",       NULL, 0 },
    { "border_style",   STYLE_TYPE_ENUM,   "边框样式",                    "\"solid\"",         border_style_options, 3 },
    /* ===== 阴影 ===== */
    { "shadow_offset_x",STYLE_TYPE_INT,    "阴影水平偏移",                "0",                 NULL, 0 },
    { "shadow_offset_y",STYLE_TYPE_INT,    "阴影垂直偏移",                "0",                 NULL, 0 },
    { "shadow_radius",  STYLE_TYPE_INT,    "阴影模糊半径",                "0",                 NULL, 0 },
    { "shadow_color",   STYLE_TYPE_COLOR,  "阴影颜色",                    "_rgb(0,0,0,0)",     NULL, 0 },
    /* ===== 内边距 ===== */
    { "padding_x",      STYLE_TYPE_INT,    "水平内边距",                  "0",                 NULL, 0 },
    { "padding_y",      STYLE_TYPE_INT,    "垂直内边距",                  "0",                 NULL, 0 },
    /* ===== 焦点 ===== */
    { "focus_width",    STYLE_TYPE_INT,    "焦点边框宽度",                "0",                 NULL, 0 },
    { "focus_color",    STYLE_TYPE_COLOR,  "焦点边框颜色",                "_rgb(100,180,255)", NULL, 0 },
    /* ===== 其他 ===== */
    { "hover_scale",    STYLE_TYPE_FLOAT,  "悬停缩放比例 (1.0=不变)",      "1.0",               NULL, 0 },
    { "loading",        STYLE_TYPE_BOOL,   "是否显示加载状态",            "false",             NULL, 0 },
    { "cursor",         STYLE_TYPE_ENUM,   "悬停时光标样式",              "\"arrow\"",         cursor_options, 13 },
    { "press_effect",   STYLE_TYPE_BOOL,   "是否启用默认按下效果",        "true",              NULL, 0 },
    { "press_offset",   STYLE_TYPE_INT,    "按下时偏移像素数",            "1",                 NULL, 0 },
};

/* ===== Edit/Textbox 样式字段定义 ===== */

static StyleFieldInfo edit_style_fields[] = {
    /* ===== 位置和尺寸 ===== */
    { "x",              STYLE_TYPE_INT,    "X坐标",                       "0",                 NULL, 0 },
    { "y",              STYLE_TYPE_INT,    "Y坐标",                       "0",                 NULL, 0 },
    { "width",          STYLE_TYPE_INT,    "文本框宽度",                  "200",               NULL, 0 },
    { "height",         STYLE_TYPE_INT,    "文本框高度",                  "36",                NULL, 0 },
    /* ===== 锚点布局 ===== */
    { "anchor",         STYLE_TYPE_INT,    "锚点:0=无,1=左上,2=右上,3=左下,4=右下,5=居中,6=上中,7=下中", "0", NULL, 0 },
    { "anchor_margin_x",STYLE_TYPE_INT,    "锚点水平边距",                "0",                 NULL, 0 },
    { "anchor_margin_y",STYLE_TYPE_INT,    "锚点垂直边距",                "0",                 NULL, 0 },
    /* ===== 颜色 ===== */
    { "bg_color",       STYLE_TYPE_COLOR,  "背景色",                      "_rgb(255,255,255)", NULL, 0 },
    { "border_color",   STYLE_TYPE_COLOR,  "边框颜色",                    "_rgb(180,180,180)", NULL, 0 },
    { "focus_color",    STYLE_TYPE_COLOR,  "聚焦时边框颜色",               "_rgb(70,130,220)",  NULL, 0 },
    { "text_color",     STYLE_TYPE_COLOR,  "文本颜色",                    "_rgb(30,30,30)",    NULL, 0 },
    { "placeholder_color",STYLE_TYPE_COLOR,"占位提示文字颜色",             "_rgb(160,160,160)", NULL, 0 },
    { "cursor_color",   STYLE_TYPE_COLOR,  "光标颜色",                    "_rgb(0,0,0)",       NULL, 0 },
    { "selection_color",STYLE_TYPE_COLOR,  "选区高亮颜色（别名sel_color）","_rgb(70,130,220,100)",NULL,0 },
    /* ===== 边框 ===== */
    { "border_width",   STYLE_TYPE_INT,    "边框宽度（像素）",            "1",                 NULL, 0 },
    { "radius",         STYLE_TYPE_INT,    "圆角半径（像素）",            "4",                 NULL, 0 },
    /* ===== 字体 ===== */
    { "font",           STYLE_TYPE_STRING, "字体名称（空=Microsoft YaHei）","\"\"",              NULL, 0 },
    { "font_size",      STYLE_TYPE_INT,    "字体大小（像素）",            "16",                NULL, 0 },
    { "letter_spacing", STYLE_TYPE_INT,    "字符间距（像素，0=默认）",    "0",                 NULL, 0 },
    { "placeholder_font_size",STYLE_TYPE_INT,"placeholder字体大小（0=跟随）","0",               NULL, 0 },
    /* ===== 滚动条 ===== */
    { "sb_track_color",       STYLE_TYPE_COLOR, "滚动条轨道颜色",         "_rgb(220,220,220)", NULL, 0 },
    { "sb_thumb_color",       STYLE_TYPE_COLOR, "滚动条滑块颜色",         "_rgb(150,150,150)", NULL, 0 },
    { "sb_thumb_hover_color", STYLE_TYPE_COLOR, "滚动条滑块悬停颜色",     "_rgb(110,110,110)", NULL, 0 },
    { "sb_thumb_press_color", STYLE_TYPE_COLOR, "滚动条滑块按下颜色",     "_rgb(80,80,80)",    NULL, 0 },
    /* ===== 内边距 ===== */
    { "padding_x",      STYLE_TYPE_INT,    "水平内边距",                  "8",                 NULL, 0 },
    { "padding_y",      STYLE_TYPE_INT,    "垂直内边距",                  "4",                 NULL, 0 },
    /* ===== 文本 ===== */
    { "placeholder",    STYLE_TYPE_STRING, "占位提示文字",                "\"\"",              NULL, 0 },
    { "max_length",     STYLE_TYPE_INT,    "最大字符数（0=无限制）",      "0",                 NULL, 0 },
    { "password",       STYLE_TYPE_BOOL,   "密码模式（显示为*）",         "false",             NULL, 0 },
    { "multiline",      STYLE_TYPE_BOOL,   "多行模式",                    "false",             NULL, 0 },
    /* ===== 状态 ===== */
    { "visible",        STYLE_TYPE_BOOL,   "是否可见",                    "true",              NULL, 0 },
    { "enabled",        STYLE_TYPE_BOOL,   "是否启用",                    "true",              NULL, 0 },
};

/* ===== Label 样式字段定义 ===== */
static StyleFieldInfo label_style_fields[] = {
    { "x",              STYLE_TYPE_INT,    "X坐标",                       "0",                 NULL, 0 },
    { "y",              STYLE_TYPE_INT,    "Y坐标",                       "0",                 NULL, 0 },
    { "width",          STYLE_TYPE_INT,    "宽度（0=自动）",              "0",                 NULL, 0 },
    { "height",         STYLE_TYPE_INT,    "高度（0=自动）",              "0",                 NULL, 0 },
    { "text",           STYLE_TYPE_STRING, "显示文本",                    "\"\"",              NULL, 0 },
    { "text_color",     STYLE_TYPE_COLOR,  "文字颜色",                    "_rgb(255,255,255)", NULL, 0 },
    { "bg_color",       STYLE_TYPE_COLOR,  "背景色(a=0则透明)",           "_rgb(0,0,0,0)",     NULL, 0 },
    { "font_name",      STYLE_TYPE_STRING, "字体名称",                    "\"Microsoft YaHei\"", NULL, 0 },
    { "font_size",      STYLE_TYPE_INT,    "字体大小",                    "16",                NULL, 0 },
    { "font_bold",      STYLE_TYPE_BOOL,   "是否粗体",                    "false",             NULL, 0 },
    { "padding_x",      STYLE_TYPE_INT,    "水平内边距",                  "4",                 NULL, 0 },
    { "padding_y",      STYLE_TYPE_INT,    "垂直内边距",                  "2",                 NULL, 0 },
    { "align",          STYLE_TYPE_ENUM,   "对齐:0=左,1=中,2=右",         "\"left\"",          text_align_options, 3 },
    { "letter_spacing", STYLE_TYPE_INT,    "字间距",                      "0",                 NULL, 0 },
    { "radius",         STYLE_TYPE_INT,    "圆角半径",                    "0",                 NULL, 0 },
    { "border_width",   STYLE_TYPE_INT,    "边框宽度",                    "0",                 NULL, 0 },
    { "border_color",   STYLE_TYPE_COLOR,  "边框颜色",                    "_rgb(0,0,0)",       NULL, 0 },
    { "opacity",        STYLE_TYPE_INT,    "整体不透明度 (0~255)",        "255",               NULL, 0 },
    /* ===== 阴影 ===== */
    { "shadow_offset_x",STYLE_TYPE_INT,    "阴影水平偏移",                "0",                 NULL, 0 },
    { "shadow_offset_y",STYLE_TYPE_INT,    "阴影垂直偏移",                "0",                 NULL, 0 },
    { "shadow_radius",  STYLE_TYPE_INT,    "阴影模糊半径",                "0",                 NULL, 0 },
    { "shadow_color",   STYLE_TYPE_COLOR,  "阴影颜色",                    "_rgb(0,0,0,0)",     NULL, 0 },
    /* ===== 状态 ===== */
    { "visible",        STYLE_TYPE_BOOL,   "是否可见",                    "true",              NULL, 0 },
    { "enabled",        STYLE_TYPE_BOOL,   "是否启用",                    "true",              NULL, 0 },
    { "anchor",         STYLE_TYPE_INT,    "锚点位置(1~9)",               "0",                 NULL, 0 },
    { "anchor_margin_x",STYLE_TYPE_INT,    "锚点水平边距",                "0",                 NULL, 0 },
    { "anchor_margin_y",STYLE_TYPE_INT,    "锚点垂直边距",                "0",                 NULL, 0 },
};

static StyleDef style_defs[] = {
    { "window", window_style_fields, sizeof(window_style_fields) / sizeof(window_style_fields[0]) },
    { "button", button_style_fields, sizeof(button_style_fields) / sizeof(button_style_fields[0]) },
    { "edit",   edit_style_fields,   sizeof(edit_style_fields)   / sizeof(edit_style_fields[0])   },
    { "label",  label_style_fields,  sizeof(label_style_fields)  / sizeof(label_style_fields[0])  },
};

/* ===== 查询接口 ===== */

// 获取 Style 目标控件的字段名称列表
const char** guis_get_style_fields(const char* target, int* count) {
    if (!target || !count) return NULL;
    int def_count = sizeof(style_defs) / sizeof(style_defs[0]);
    for (int i = 0; i < def_count; i++) {
        if (strcmp(style_defs[i].target, target) == 0) {
            *count = style_defs[i].field_count;
            // 返回字段名称数组（需要上层释放）
            const char** names = (const char**)malloc(sizeof(const char*) * (*count));
            for (int j = 0; j < *count; j++) {
                names[j] = style_defs[i].fields[j].name;
            }
            return names;
        }
    }
    *count = 0;
    return NULL;
}

// 获取 Style 字段的详细信息
StyleFieldInfo* guis_get_style_field_info(const char* target, const char* field_name) {
    if (!target || !field_name) return NULL;
    int def_count = sizeof(style_defs) / sizeof(style_defs[0]);
    for (int i = 0; i < def_count; i++) {
        if (strcmp(style_defs[i].target, target) == 0) {
            for (int j = 0; j < style_defs[i].field_count; j++) {
                if (strcmp(style_defs[i].fields[j].name, field_name) == 0) {
                    return &style_defs[i].fields[j];
                }
            }
        }
    }
    return NULL;
}

// 获取字段类型的字符串表示
const char* guis_style_field_type_name(StyleFieldType type) {
    switch (type) {
        case STYLE_TYPE_STRING: return "string";
        case STYLE_TYPE_INT:    return "int";
        case STYLE_TYPE_FLOAT:  return "float";
        case STYLE_TYPE_BOOL:   return "bool";
        case STYLE_TYPE_COLOR:  return "RGB";
        case STYLE_TYPE_ENUM:   return "enum";
        default:                return "unknown";
    }
}
