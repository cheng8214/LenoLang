/* Leno GUI - GLabel 标签控件
 *
 * GLabel 是非交互式的静态文本标签，用于显示不可编辑的文字。
 *
 * GLabel 实例方法 (label.method()):
 *   label.set_text(text)              设置文字
 *   label.set_pos(x, y)               设置位置
 *   label.set_size(w, h)              设置尺寸
 *   label.set_text_color(color)       设置文字颜色
 *   label.set_bg_color(color)         设置背景色
 *   label.set_font_size(size)         设置字体大小
 *   label.set_font_name(name)         设置字体名称
 *   label.set_font_bold(b)            设置粗体
 *   label.set_padding(x, y)           设置内边距
 *   label.set_align(align)            设置对齐 (0=左, 1=中, 2=右)
 *   label.set_anchor(anchor, mx, my)  设置锚点布局
 *   label.set_visible(v)              设置可见性
 *   label.set_enabled(v)              设置启用状态
 *   label.set_letter_spacing(s)       设置字间距
 *   label.set_radius(r)               设置圆角
 *   label.set_border(width, color)    设置边框
 *   label.set_opacity(val)            设置透明度 (0~255)
 *   label.set_shadow(ox, oy, r, color) 设置阴影
 *   label.get_text() -> string        获取文字
 *   label.close()                     移除标签
 *
 * 内部函数（供 win.run 调用）:
 *   gui_label_draw_all()              绘制所有标签
 *   gui_label_free_all()              释放所有标签
 *   gui_label_update_anchors()        更新锚点布局
 */

#include "include/native.h"
#include "include/leno_value.h"
#include "guis_internal.h"
#include <string.h>
#include <stdlib.h>

/* ============================================================================
 * 辅助函数
 * ============================================================================ */

ObjGUILabel* as_label(Value v) {
    if (!val_is_obj(v)) return NULL;
    Object* obj = val_as_obj(v);
    if (obj->type != OBJ_GUI_LABEL) return NULL;
    return (ObjGUILabel*)obj;
}

/* 从 Value 提取颜色分量到 int* */
static void extract_color(Value v, int* r, int* g, int* b, int* a) {
    if (val_is_obj(v) && val_as_obj(v)->type == OBJ_RGB) {
        ObjRgb* rgb = (ObjRgb*)val_as_obj(v);
        *r = rgb->r; *g = rgb->g; *b = rgb->b; *a = rgb->a;
    }
}

/* ============================================================================
 * GLabel 实例方法
 * ============================================================================ */

/* label.set_text(text) */
static Value gl_set_text(int argc, Value* args) {
    (void)argc; ObjGUILabel* lb = as_label(args[0]); if (!lb) return val_null();
    const char* s = "";
    if (val_is_obj(args[1]) && val_as_obj(args[1])->type == OBJ_STRING)
        s = ((ObjString*)val_as_obj(args[1]))->chars;
    if (lb->text) free(lb->text);
    lb->text = s && s[0] ? strdup(s) : NULL;
    return val_null();
}

/* label.set_pos(x, y) */
static Value gl_set_pos(int argc, Value* args) {
    (void)argc; ObjGUILabel* lb = as_label(args[0]); if (!lb) return val_null();
    lb->x = val_as_int(args[1]); lb->y = val_as_int(args[2]);
    return val_null();
}

/* label.set_size(w, h) */
static Value gl_set_size(int argc, Value* args) {
    (void)argc; ObjGUILabel* lb = as_label(args[0]); if (!lb) return val_null();
    lb->width = val_as_int(args[1]); lb->height = val_as_int(args[2]);
    return val_null();
}

/* label.set_text_color(color) */
static Value gl_set_text_color(int argc, Value* args) {
    (void)argc; ObjGUILabel* lb = as_label(args[0]); if (!lb) return val_null();
    extract_color(args[1], &lb->text_r, &lb->text_g, &lb->text_b, &lb->text_a);
    return val_null();
}

/* label.set_bg_color(color) */
static Value gl_set_bg_color(int argc, Value* args) {
    (void)argc; ObjGUILabel* lb = as_label(args[0]); if (!lb) return val_null();
    extract_color(args[1], &lb->bg_r, &lb->bg_g, &lb->bg_b, &lb->bg_a);
    return val_null();
}

/* label.set_font_size(size) */
static Value gl_set_font_size(int argc, Value* args) {
    (void)argc; ObjGUILabel* lb = as_label(args[0]); if (!lb) return val_null();
    int fs = val_as_int(args[1]);
    if (fs <= 0) return val_null();
    lb->font_size = fs;
    if (lb->font && lb->font->platform) { leno_gui_platform_destroy_font(lb->font->platform); lb->font->platform = NULL; }
    LenoGUIPlatformFont* pf = leno_gui_platform_load_font(lb->font_name ? lb->font_name : "Microsoft YaHei", fs);
    if (pf) {
        if (!lb->font) {
            lb->font = (ObjGUIFont*)gc_alloc(sizeof(ObjGUIFont), OBJ_GUI_FONT);
            if (!lb->font) { leno_gui_platform_destroy_font(pf); return val_null(); }
            gc_write_barrier_obj((Object*)lb, (Object*)lb->font);
        }
        lb->font->platform = pf;
    }
    return val_null();
}

/* label.set_font_name(name) */
static Value gl_set_font_name(int argc, Value* args) {
    (void)argc; ObjGUILabel* lb = as_label(args[0]); if (!lb) return val_null();
    const char* s = "";
    if (val_is_obj(args[1]) && val_as_obj(args[1])->type == OBJ_STRING)
        s = ((ObjString*)val_as_obj(args[1]))->chars;
    if (lb->font_name) free(lb->font_name);
    lb->font_name = strdup(s);
    /* 重新加载字体 */
    if (lb->font && lb->font->platform) { leno_gui_platform_destroy_font(lb->font->platform); lb->font->platform = NULL; }
    LenoGUIPlatformFont* pf = leno_gui_platform_load_font(s, lb->font_size);
    if (pf) {
        if (!lb->font) {
            lb->font = (ObjGUIFont*)gc_alloc(sizeof(ObjGUIFont), OBJ_GUI_FONT);
            if (!lb->font) { leno_gui_platform_destroy_font(pf); return val_null(); }
            gc_write_barrier_obj((Object*)lb, (Object*)lb->font);
        }
        lb->font->platform = pf;
    }
    return val_null();
}

/* label.set_font_bold(b) */
static Value gl_set_font_bold(int argc, Value* args) {
    (void)argc; ObjGUILabel* lb = as_label(args[0]); if (!lb) return val_null();
    lb->font_bold = val_as_bool(args[1]) ? 1 : 0;
    return val_null();
}

/* label.set_padding(x, y) */
static Value gl_set_padding(int argc, Value* args) {
    (void)argc; ObjGUILabel* lb = as_label(args[0]); if (!lb) return val_null();
    lb->padding_x = val_as_int(args[1]); lb->padding_y = val_as_int(args[2]);
    return val_null();
}

/* label.set_align(align)  0=左, 1=中, 2=右 */
static Value gl_set_align(int argc, Value* args) {
    (void)argc; ObjGUILabel* lb = as_label(args[0]); if (!lb) return val_null();
    lb->text_align = val_as_int(args[1]);
    return val_null();
}

/* label.set_anchor(anchor, margin_x, margin_y) */
static Value gl_set_anchor(int argc, Value* args) {
    (void)argc; ObjGUILabel* lb = as_label(args[0]); if (!lb) return val_null();
    lb->anchor = val_as_int(args[1]);
    lb->anchor_margin_x = val_as_int(args[2]);
    lb->anchor_margin_y = val_as_int(args[3]);
    return val_null();
}

/* label.set_visible(v) */
static Value gl_set_visible(int argc, Value* args) {
    (void)argc; ObjGUILabel* lb = as_label(args[0]); if (!lb) return val_null();
    lb->visible = val_as_bool(args[1]) ? 1 : 0;
    return val_null();
}

/* label.set_enabled(v) */
static Value gl_set_enabled(int argc, Value* args) {
    (void)argc; ObjGUILabel* lb = as_label(args[0]); if (!lb) return val_null();
    lb->enabled = val_as_bool(args[1]) ? 1 : 0;
    return val_null();
}

/* label.set_letter_spacing(s) */
static Value gl_set_letter_spacing(int argc, Value* args) {
    (void)argc; ObjGUILabel* lb = as_label(args[0]); if (!lb) return val_null();
    lb->letter_spacing = val_as_int(args[1]);
    return val_null();
}

/* label.set_radius(r) */
static Value gl_set_radius(int argc, Value* args) {
    (void)argc; ObjGUILabel* lb = as_label(args[0]); if (!lb) return val_null();
    lb->radius = val_as_int(args[1]);
    return val_null();
}

/* label.set_border(width, color) */
static Value gl_set_border(int argc, Value* args) {
    (void)argc; ObjGUILabel* lb = as_label(args[0]); if (!lb) return val_null();
    lb->border_width = val_as_int(args[1]);
    extract_color(args[2], &lb->border_r, &lb->border_g, &lb->border_b, &lb->border_a);
    return val_null();
}

/* label.set_opacity(val) - 设置透明度 (0~255) */
static Value gl_set_opacity(int argc, Value* args) {
    (void)argc; ObjGUILabel* lb = as_label(args[0]); if (!lb) return val_null();
    lb->opacity = val_as_int(args[1]);
    if (lb->opacity < 0) lb->opacity = 0;
    if (lb->opacity > 255) lb->opacity = 255;
    return val_null();
}

/* label.set_shadow(offset_x, offset_y, radius, color) */
static Value gl_set_shadow(int argc, Value* args) {
    (void)argc; ObjGUILabel* lb = as_label(args[0]); if (!lb) return val_null();
    lb->shadow_offset_x = val_as_int(args[1]);
    lb->shadow_offset_y = val_as_int(args[2]);
    lb->shadow_radius = val_as_int(args[3]);
    extract_color(args[4], &lb->shadow_r, &lb->shadow_g, &lb->shadow_b, &lb->shadow_a);
    return val_null();
}

/* label.get_text() -> string */
static Value gl_get_text(int argc, Value* args) {
    (void)argc; ObjGUILabel* lb = as_label(args[0]); if (!lb) return val_null();
    if (!lb->text) return val_obj((Object*)str_copy("", 0));
    return val_obj((Object*)str_copy(lb->text, (int)strlen(lb->text)));
}

/* label.close() - 从窗口中移除 */
static Value gl_close_func(int argc, Value* args) {
    (void)argc; ObjGUILabel* lb = as_label(args[0]); if (!lb || !lb->window) return val_null();
    ObjGUILabel** p = &lb->window->labels;
    while (*p) {
        if (*p == lb) { *p = lb->next; lb->window->label_count--; break; }
        p = &(*p)->next;
    }
    if (lb->text) { free(lb->text); lb->text = NULL; }
    if (lb->font_name) { free(lb->font_name); lb->font_name = NULL; }
    /* lb->font 是 GC 对象，由 GC 回收 */
    lb->window = NULL;
    /* lb 本身由 GC 回收，不再手动 free */
    return val_null();
}

/* ============================================================================
 * 绘制
 * ============================================================================ */

/* 计算文本像素宽度（考虑字间距） */
static int gl_text_width(ObjGUILabel* lb, const char* text) {
    if (!text || !lb->font || !lb->font->platform) return 0;
    int total = 0;
    int spacing = lb->letter_spacing;
    int len = (int)strlen(text);
    for (int i = 0; i < len; ) {
        unsigned char c = (unsigned char)text[i];
        int clen = 1;
        if ((c & 0xE0) == 0xC0) clen = 2;
        else if ((c & 0xF0) == 0xE0) clen = 3;
        else if ((c & 0xF8) == 0xF0) clen = 4;
        total += leno_gui_platform_text_width_utf8(lb->font->platform, text + i, clen);
        if (i + clen < len) total += spacing;
        i += clen;
    }
    return total;
}

static void gl_draw_one(ObjGUILabel* lb, ObjGUIRenderer* ren) {
    if (!lb->visible) return;
    LenoGUIPlatformRenderer* r = ren->platform;
    int dx = lb->x, dy = lb->y, dw = lb->width, dh = lb->height;
    int rad = lb->radius;
    int opacity = lb->opacity;

    /* 阴影 */
    if (lb->shadow_radius > 0 && lb->shadow_a > 0) {
        leno_gui_platform_render_draw_shadow(r,
            dx, dy, dw, dh, rad,
            lb->shadow_offset_x, lb->shadow_offset_y, lb->shadow_radius,
            (uint8_t)lb->shadow_r, (uint8_t)lb->shadow_g,
            (uint8_t)lb->shadow_b, (uint8_t)lb->shadow_a);
    }

    /* 背景 */
    if (lb->bg_a > 0 && dw > 0 && dh > 0) {
        int a = lb->bg_a;
        if (opacity < 255) a = (a * opacity) / 255;
        leno_gui_platform_set_draw_color(r, (uint8_t)lb->bg_r, (uint8_t)lb->bg_g,
                                         (uint8_t)lb->bg_b, (uint8_t)a);
        if (rad > 0)
            leno_gui_platform_render_fill_rounded_rect(r, dx, dy, dw, dh, rad);
        else
            leno_gui_platform_render_fill_rect(r, dx, dy, dw, dh);
    }

    /* 边框 */
    if (lb->border_width > 0 && dw > 0 && dh > 0) {
        int ba = lb->border_a;
        if (opacity < 255) ba = (ba * opacity) / 255;
        leno_gui_platform_set_draw_color(r, (uint8_t)lb->border_r, (uint8_t)lb->border_g,
                                         (uint8_t)lb->border_b, (uint8_t)ba);
        if (rad > 0)
            leno_gui_platform_render_draw_rounded_rect(r, dx, dy, dw, dh, rad);
        else
            leno_gui_platform_render_draw_rect(r, dx, dy, dw, dh);
    }

    /* 文字 */
    if (lb->text && lb->font && lb->font->platform) {
        const char* txt = lb->text;
        int tw = gl_text_width(lb, txt);
        int tx = dx + lb->padding_x;
        int ty = dy + lb->padding_y;

        /* 水平对齐 */
        if (lb->text_align == 1 && dw > 0) { /* 居中 */
            tx = dx + (dw - tw) / 2;
            if (tx < dx + lb->padding_x) tx = dx + lb->padding_x;
        } else if (lb->text_align == 2 && dw > 0) { /* 右对齐 */
            tx = dx + dw - tw - lb->padding_x;
            if (tx < dx + lb->padding_x) tx = dx + lb->padding_x;
        }

        /* 垂直居中 */
        if (dh > 0) {
            int lead = leno_gui_platform_font_internal_leading(lb->font->platform);
            if (lead > 0) lead = lead - 2;
            ty = dy + (dh - lb->font_size) / 2 - lead / 2;
        }

        int ta = lb->text_a;
        if (opacity < 255) ta = (ta * opacity) / 255;
        leno_gui_platform_set_draw_color(r, (uint8_t)lb->text_r, (uint8_t)lb->text_g,
                                         (uint8_t)lb->text_b, (uint8_t)ta);
        leno_gui_platform_draw_text_font(r, lb->font->platform, txt, tx, ty);
    }
}

void gui_label_draw_all(ObjGUIWindow* win, ObjGUIRenderer* ren) {
    if (!win || !ren) return;
    ObjGUILabel* lb = win->labels;
    while (lb) {
        gl_draw_one(lb, ren);
        lb = lb->next;
    }
}

/* ============================================================================
 * 锚点布局
 * ============================================================================ */

void gui_label_update_anchors(ObjGUIWindow* win, int win_w, int win_h) {
    if (!win) return;
    ObjGUILabel* lb = win->labels;
    while (lb) {
        if (lb->anchor > 0) {
            int mx = lb->anchor_margin_x, my = lb->anchor_margin_y;
            switch (lb->anchor) {
                case 1: /* 左上 */ break;
                case 2: /* 右上 */ lb->x = win_w - lb->width - mx; break;
                case 3: /* 左下 */ lb->y = win_h - lb->height - my; break;
                case 4: /* 右下 */
                    lb->x = win_w - lb->width - mx;
                    lb->y = win_h - lb->height - my;
                    break;
                case 5: /* 居中 */
                    lb->x = (win_w - lb->width) / 2 + mx;
                    lb->y = (win_h - lb->height) / 2 + my;
                    break;
                case 6: /* 上中 */
                    lb->x = (win_w - lb->width) / 2 + mx;
                    break;
                case 7: /* 下中 */
                    lb->x = (win_w - lb->width) / 2 + mx;
                    lb->y = win_h - lb->height - my;
                    break;
                case 8: /* 左中 */
                    lb->y = (win_h - lb->height) / 2 + my;
                    break;
                case 9: /* 右中 */
                    lb->x = win_w - lb->width - mx;
                    lb->y = (win_h - lb->height) / 2 + my;
                    break;
            }
        }
        lb = lb->next;
    }
}

/* ============================================================================
 * 释放
 * ============================================================================ */

void gui_label_free_all(ObjGUIWindow* win) {
    if (!win) return;
    ObjGUILabel* lb = win->labels;
    while (lb) {
        ObjGUILabel* next = lb->next;
        if (lb->text) { free(lb->text); lb->text = NULL; }
        if (lb->font_name) { free(lb->font_name); lb->font_name = NULL; }
        /* lb->font 是 GC 对象，由 GC 回收 */
        lb = next;
    }
    win->labels = NULL;
    win->label_count = 0;
}

/* ============================================================================
 * 注册 GLabel 实例方法
 * ============================================================================ */

extern void label_register_method_with_params(const char* name, ObjNative* method, int arity,
                                               int min_arity, int max_arity,
                                               TypeKind return_type, TypeKind return_element_type,
                                               TypeKind* param_types);
extern ObjNative* make_native(NativeFn fn, int arity, const char* name);
extern void label_init_methods(void);

void guis_init_label_instance_methods(void) {
    label_init_methods();

    TypeKind no_params[] = {};
    TypeKind str_1[] = {TYPE_STRING};
    TypeKind int_1[] = {TYPE_INT};
    TypeKind int_2[] = {TYPE_INT, TYPE_INT};
    TypeKind bool_1[] = {TYPE_BOOL};
    TypeKind any_1[] = {TYPE_ANY};

    label_register_method_with_params("set_text", make_native(gl_set_text, 2, "set_text"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, str_1);
    label_register_method_with_params("set_pos", make_native(gl_set_pos, 3, "set_pos"), 2, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_2);
    label_register_method_with_params("set_size", make_native(gl_set_size, 3, "set_size"), 2, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_2);
    label_register_method_with_params("set_text_color", make_native(gl_set_text_color, 2, "set_text_color"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, any_1);
    label_register_method_with_params("set_bg_color", make_native(gl_set_bg_color, 2, "set_bg_color"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, any_1);
    label_register_method_with_params("set_font_size", make_native(gl_set_font_size, 2, "set_font_size"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_1);
    label_register_method_with_params("set_font_name", make_native(gl_set_font_name, 2, "set_font_name"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, str_1);
    label_register_method_with_params("set_font_bold", make_native(gl_set_font_bold, 2, "set_font_bold"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, bool_1);
    label_register_method_with_params("set_padding", make_native(gl_set_padding, 3, "set_padding"), 2, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_2);
    label_register_method_with_params("set_align", make_native(gl_set_align, 2, "set_align"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_1);
    label_register_method_with_params("set_visible", make_native(gl_set_visible, 2, "set_visible"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, bool_1);
    label_register_method_with_params("set_enabled", make_native(gl_set_enabled, 2, "set_enabled"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, bool_1);
    label_register_method_with_params("set_letter_spacing", make_native(gl_set_letter_spacing, 2, "set_letter_spacing"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_1);
    label_register_method_with_params("set_radius", make_native(gl_set_radius, 2, "set_radius"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_1);
    TypeKind border_params[] = {TYPE_INT, TYPE_ANY};
    label_register_method_with_params("set_border", make_native(gl_set_border, 3, "set_border"), 2, -1, -1, TYPE_NULL, TYPE_UNKNOWN, border_params);
    label_register_method_with_params("set_opacity", make_native(gl_set_opacity, 2, "set_opacity"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_1);
    TypeKind shadow_params[] = {TYPE_INT, TYPE_INT, TYPE_INT, TYPE_ANY};
    label_register_method_with_params("set_shadow", make_native(gl_set_shadow, 5, "set_shadow"), 4, -1, -1, TYPE_NULL, TYPE_UNKNOWN, shadow_params);
    TypeKind anchor_params[] = {TYPE_INT, TYPE_INT, TYPE_INT};
    label_register_method_with_params("set_anchor", make_native(gl_set_anchor, 4, "set_anchor"), 3, -1, -1, TYPE_NULL, TYPE_UNKNOWN, anchor_params);
    label_register_method_with_params("get_text", make_native(gl_get_text, 1, "get_text"), 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, no_params);
    label_register_method_with_params("close", make_native(gl_close_func, 1, "close"), 0, -1, -1, TYPE_NULL, TYPE_UNKNOWN, no_params);
}
