/* Leno GUI - Style 字段定义（供 LSP 补全使用）
 * 从 guis.c 拆分出来的 Style 相关定义
 */
#include <string.h>

/* ===== Style 字段定义 ===== */

typedef struct {
    const char* target;
    const char** fields;
    int field_count;
} StyleDef;

static const char* window_style_fields[] = {
    "width", "height", "x", "y",
    "title", "fullscreen", "borderless", "resizable",
    "opacity", "visible", "always_on_top"
};

static const char* button_style_fields[] = {
    "bk_color", "text", "text_color", "text_size",
    "width", "height", "x", "y",
    "border_radius", "hover_color", "active_color"
};

static StyleDef style_defs[] = {
    { "window", window_style_fields, sizeof(window_style_fields) / sizeof(window_style_fields[0]) },
    { "button", button_style_fields, sizeof(button_style_fields) / sizeof(button_style_fields[0]) },
};

// 获取 Style 目标控件的字段列表（供 LSP 使用）
const char** guis_get_style_fields(const char* target, int* count) {
    if (!target || !count) return NULL;
    int def_count = sizeof(style_defs) / sizeof(style_defs[0]);
    for (int i = 0; i < def_count; i++) {
        if (strcmp(style_defs[i].target, target) == 0) {
            *count = style_defs[i].field_count;
            return style_defs[i].fields;
        }
    }
    *count = 0;
    return NULL;
}
