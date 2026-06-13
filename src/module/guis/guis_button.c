/* Leno GUI - GButton 按钮实例方法 + 绘制 + 事件处理
 *
 * GButton 实例方法 (btn.method()):
 *   btn.set_text(text)                  设置按钮文字
 *   btn.set_pos(x, y)                   设置按钮位置
 *   btn.set_size(w, h)                  设置按钮尺寸
 *   btn.set_visible(bool)               设置可见性
 *   btn.set_enabled(bool)               设置启用状态
 *   btn.set_opacity(val)                设置透明度 (0~255)
 *   btn.on_click(callback)              设置点击回调
 *   btn.close()                         移除按钮
 *
 * 样式字段 (win.add_button(style)):
 *   opacity                             透明度 (0~255, 默认255)
 *   border_width                        边框宽度 (0=无边框)
 *   border_color                        边框颜色 (GRgb)
 *   shadow_offset_x, shadow_offset_y    阴影偏移
 *   shadow_radius                       阴影模糊半径 (0=无阴影)
 *   shadow_color                        阴影颜色 (GRgb)
 *
 * 内部函数（供 win.run 调用）:
 *   gui_button_draw_all()               绘制所有按钮
 *   gui_button_handle_event()           处理按钮事件
 *   gui_button_free_all()               释放所有按钮
 */
#include "include/native.h"
#include "include/leno_value.h"
#include "guis_internal.h"
#include <string.h>

/* ============================================================================
 * GButton 实例方法
 * ============================================================================ */

/* btn.set_text(text) */
static Value btn_set_text_func(int argc, Value* args) {
    (void)argc;
    ObjGUIButton* btn = as_button(args[0]);
    if (!btn) return val_null();

    if (val_is_obj(args[1]) && val_as_obj(args[1])->type == OBJ_STRING) {
        ObjString* str = (ObjString*)val_as_obj(args[1]);
        if (btn->text) free(btn->text);
        btn->text = strdup(str->chars);
    }
    return val_null();
}

/* btn.set_pos(x, y) */
static Value btn_set_pos_func(int argc, Value* args) {
    (void)argc;
    ObjGUIButton* btn = as_button(args[0]);
    if (!btn) return val_null();
    btn->x = val_as_int(args[1]);
    btn->y = val_as_int(args[2]);
    return val_null();
}

/* btn.set_size(w, h) */
static Value btn_set_size_func(int argc, Value* args) {
    (void)argc;
    ObjGUIButton* btn = as_button(args[0]);
    if (!btn) return val_null();
    btn->width = val_as_int(args[1]);
    btn->height = val_as_int(args[2]);
    return val_null();
}

/* btn.set_visible(bool) */
static Value btn_set_visible_func(int argc, Value* args) {
    (void)argc;
    ObjGUIButton* btn = as_button(args[0]);
    if (!btn) return val_null();
    btn->visible = val_as_bool(args[1]) ? 1 : 0;
    return val_null();
}

/* btn.set_enabled(bool) */
static Value btn_set_enabled_func(int argc, Value* args) {
    (void)argc;
    ObjGUIButton* btn = as_button(args[0]);
    if (!btn) return val_null();
    btn->enabled = val_as_bool(args[1]) ? 1 : 0;
    return val_null();
}

/* btn.set_opacity(val) - 设置透明度 (0~255) */
static Value btn_set_opacity_func(int argc, Value* args) {
    (void)argc;
    ObjGUIButton* btn = as_button(args[0]);
    if (!btn) return val_null();
    btn->opacity = val_as_int(args[1]);
    if (btn->opacity < 0) btn->opacity = 0;
    if (btn->opacity > 255) btn->opacity = 255;
    return val_null();
}

/* btn.on_click(callback) - 设置点击回调 */
static Value btn_on_click_func(int argc, Value* args) {
    (void)argc;
    ObjGUIButton* btn = as_button(args[0]);
    if (!btn) return val_null();
    btn->on_click = args[1];
    return val_null();
}

/* btn.close() - 从窗口移除按钮 */
static Value btn_close_func(int argc, Value* args) {
    (void)argc;
    ObjGUIButton* btn = as_button(args[0]);
    if (!btn || !btn->window) return val_null();

    /* 从窗口按钮链表中移除 */
    ObjGUIWindow* win = btn->window;
    ObjGUIButton** pp = (ObjGUIButton**)&win->buttons;
    while (*pp) {
        if (*pp == btn) {
            *pp = btn->next;
            win->button_count--;
            break;
        }
        pp = (ObjGUIButton**)&(*pp)->next;
    }
    btn->next = NULL;
    btn->window = NULL;
    return val_null();
}

/* ============================================================================
 * 按钮绘制（自绘圆角矩形 + 文字）
 * ============================================================================ */

/* 绘制单个按钮 */
static void gui_button_draw_one(ObjGUIButton* btn, ObjGUIRenderer* ren) {
    if (!btn->visible || !ren || !ren->platform) return;

    /* 选择颜色：按下 > 悬停 > 默认 */
    int r, g, b, a;
    if (!btn->enabled) {
        /* 禁用状态：灰化 */
        r = 128; g = 128; b = 128; a = 200;
    } else if (btn->pressed) {
        r = btn->press_r; g = btn->press_g; b = btn->press_b; a = btn->press_a;
    } else if (btn->hovered) {
        r = btn->hover_r; g = btn->hover_g; b = btn->hover_b; a = btn->hover_a;
    } else {
        r = btn->bg_r; g = btn->bg_g; b = btn->bg_b; a = btn->bg_a;
    }

    /* 应用整体透明度 */
    int opacity = btn->opacity;
    if (opacity < 255) {
        a = (a * opacity) / 255;
    }

    /* 1. 绘制阴影（在按钮下方） */
    if (btn->shadow_radius > 0 && btn->shadow_a > 0) {
        leno_gui_platform_render_draw_shadow(ren->platform,
            btn->x, btn->y, btn->width, btn->height, btn->radius,
            btn->shadow_offset_x, btn->shadow_offset_y, btn->shadow_radius,
            (uint8_t)btn->shadow_r, (uint8_t)btn->shadow_g,
            (uint8_t)btn->shadow_b, (uint8_t)btn->shadow_a);
    }

    /* 2. 绘制圆角矩形背景 */
    leno_gui_platform_set_draw_color(ren->platform, r, g, b, a);
    leno_gui_platform_render_fill_rounded_rect(ren->platform, btn->x, btn->y, btn->width, btn->height, btn->radius);

    /* 3. 绘制边框 */
    if (btn->border_width > 0) {
        int ba = btn->border_a;
        if (opacity < 255) ba = (ba * opacity) / 255;
        leno_gui_platform_set_draw_color(ren->platform, btn->border_r, btn->border_g, btn->border_b, ba);
        for (int i = 0; i < btn->border_width; i++) {
            leno_gui_platform_render_draw_rounded_rect(ren->platform,
                btn->x + i, btn->y + i,
                btn->width - 2 * i, btn->height - 2 * i,
                btn->radius > i ? btn->radius - i : 0);
        }
    }

    /* 4. 绘制文字 */
    if (btn->text && btn->font && btn->font->platform) {
        int ta = btn->text_a;
        if (opacity < 255) ta = (ta * opacity) / 255;
        leno_gui_platform_set_draw_color(ren->platform, btn->text_r, btn->text_g, btn->text_b, ta);
        /* 居中绘制文字 */
        int tw = 0, th = 0;
        leno_gui_platform_text_size_font(btn->font->platform, btn->text, &tw, &th);
        int tx = btn->x + (btn->width - tw) / 2;
        int ty = btn->y + (btn->height - th) / 2;
        leno_gui_platform_draw_text_font(ren->platform, btn->font->platform, btn->text, tx, ty);
    }
}

/* 绘制窗口上所有按钮 */
void gui_button_draw_all(ObjGUIWindow* win, ObjGUIRenderer* ren) {
    if (!win || !ren) return;
    ObjGUIButton* btn = (ObjGUIButton*)win->buttons;
    while (btn) {
        gui_button_draw_one(btn, ren);
        btn = btn->next;
    }
}

/* ============================================================================
 * 按钮事件处理
 * ============================================================================ */

/* 检测点是否在按钮区域内 */
static int point_in_button(ObjGUIButton* btn, float mx, float my) {
    return mx >= btn->x && mx <= btn->x + btn->width &&
           my >= btn->y && my <= btn->y + btn->height;
}

/* 处理窗口上所有按钮的事件，返回 1 表示事件被按钮消费 */
int gui_button_handle_event(ObjGUIWindow* win, LenoGUIEvent* ev) {
    if (!win || !ev) return 0;

    int consumed = 0;
    ObjGUIButton* btn = (ObjGUIButton*)win->buttons;

    while (btn) {
        if (!btn->visible || !btn->enabled) {
            btn = btn->next;
            continue;
        }

        if (ev->type == LENO_GUI_EVT_MOUSE_MOVE) {
            int was_hovered = btn->hovered;
            btn->hovered = point_in_button(btn, ev->mouse_x, ev->mouse_y);
            if (btn->hovered && !was_hovered) consumed = 1;
        } else if (ev->type == LENO_GUI_EVT_MOUSE_DOWN && ev->mouse_button == LENO_GUI_MOUSE_LEFT) {
            if (point_in_button(btn, ev->mouse_x, ev->mouse_y)) {
                btn->pressed = 1;
                consumed = 1;
            }
        } else if (ev->type == LENO_GUI_EVT_MOUSE_UP && ev->mouse_button == LENO_GUI_MOUSE_LEFT) {
            if (btn->pressed && point_in_button(btn, ev->mouse_x, ev->mouse_y)) {
                /* 触发点击回调 */
                if (!val_is_null(btn->on_click)) {
                    call_leno_closure(btn->on_click, 0, NULL);
                }
                consumed = 1;
            }
            btn->pressed = 0;
        }

        btn = btn->next;
    }
    return consumed;
}

/* 释放窗口上所有按钮资源 */
void gui_button_free_all(ObjGUIWindow* win) {
    if (!win) return;
    ObjGUIButton* btn = (ObjGUIButton*)win->buttons;
    while (btn) {
        ObjGUIButton* next = btn->next;
        if (btn->text) { free(btn->text); btn->text = NULL; }
        if (btn->font_name) { free(btn->font_name); btn->font_name = NULL; }
        btn = next;
    }
    win->buttons = NULL;
    win->button_count = 0;
}

/* ============================================================================
 * as_button 辅助函数
 * ============================================================================ */

ObjGUIButton* as_button(Value v) {
    if (!val_is_obj(v)) return NULL;
    Object* obj = val_as_obj(v);
    if (obj->type != OBJ_GUI_BUTTON) return NULL;
    return (ObjGUIButton*)obj;
}

/* ============================================================================
 * 注册 GButton 实例方法
 * ============================================================================ */

extern void button_register_method_with_params(const char* name, ObjNative* method, int arity,
                                                int min_arity, int max_arity,
                                                TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);
extern ObjNative* make_native(NativeFn fn, int arity, const char* name);
extern void button_init_methods(void);

void guis_init_button_instance_methods(void) {
    button_init_methods();

    TypeKind no_params[] = {};
    TypeKind str_1[] = {TYPE_STRING};
    TypeKind int_1[] = {TYPE_INT};
    TypeKind int_2[] = {TYPE_INT, TYPE_INT};
    TypeKind bool_1[] = {TYPE_BOOL};
    TypeKind any_1[] = {TYPE_ANY};

    button_register_method_with_params("set_text", make_native(btn_set_text_func, 2, "set_text"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, str_1);
    button_register_method_with_params("set_pos", make_native(btn_set_pos_func, 3, "set_pos"), 2, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_2);
    button_register_method_with_params("set_size", make_native(btn_set_size_func, 3, "set_size"), 2, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_2);
    button_register_method_with_params("set_visible", make_native(btn_set_visible_func, 2, "set_visible"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, bool_1);
    button_register_method_with_params("set_enabled", make_native(btn_set_enabled_func, 2, "set_enabled"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, bool_1);
    button_register_method_with_params("set_opacity", make_native(btn_set_opacity_func, 2, "set_opacity"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_1);
    button_register_method_with_params("on_click", make_native(btn_on_click_func, 2, "on_click"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, any_1);
    button_register_method_with_params("close", make_native(btn_close_func, 1, "close"), 0, -1, -1, TYPE_NULL, TYPE_UNKNOWN, no_params);
}
