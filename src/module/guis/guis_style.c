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
    "arrow", "hand", "ibeam", "crosshair", "wait", "none"
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
    { "cursor",       STYLE_TYPE_ENUM,   "鼠标光标样式",                 "\"arrow\"", cursor_options, 6 },
};

/* ===== Button 样式字段定义 ===== */

static StyleFieldInfo button_style_fields[] = {
    { "bk_color",      STYLE_TYPE_COLOR,  "背景颜色",                    "_rgb(200,200,200)", NULL, 0 },
    { "text",          STYLE_TYPE_STRING, "按钮文本",                    "\"Button\"",        NULL, 0 },
    { "text_color",    STYLE_TYPE_COLOR,  "文本颜色",                    "_rgb(0,0,0)",       NULL, 0 },
    { "text_size",     STYLE_TYPE_INT,    "文本大小",                    "16",                NULL, 0 },
    { "width",         STYLE_TYPE_INT,    "按钮宽度",                    "100",               NULL, 0 },
    { "height",        STYLE_TYPE_INT,    "按钮高度",                    "40",                NULL, 0 },
    { "x",             STYLE_TYPE_INT,    "X坐标",                       "0",                 NULL, 0 },
    { "y",             STYLE_TYPE_INT,    "Y坐标",                       "0",                 NULL, 0 },
    { "border_radius", STYLE_TYPE_INT,    "圆角半径",                    "4",                 NULL, 0 },
    { "hover_color",   STYLE_TYPE_COLOR,  "悬停颜色",                    "_rgb(220,220,220)", NULL, 0 },
    { "active_color",  STYLE_TYPE_COLOR,  "按下颜色",                    "_rgb(180,180,180)", NULL, 0 },
};

static StyleDef style_defs[] = {
    { "window", window_style_fields, sizeof(window_style_fields) / sizeof(window_style_fields[0]) },
    { "button", button_style_fields, sizeof(button_style_fields) / sizeof(button_style_fields[0]) },
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
