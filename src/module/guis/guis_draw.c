/* Leno GUI - Draw 渲染器实例方法
 * 从 guis.c 拆分出来的 Draw 方法实现

 * Draw 实例方法 (ren.method()):
 *   ren.set_color(r, g, b, a)
 *   ren.clear() / ren.present()
 *   ren.draw_point(x, y) / ren.draw_line(x1, y1, x2, y2)
 *   ren.draw_rect(x, y, w, h) / ren.fill_rect(x, y, w, h)
 *   ren.draw_circle(cx, cy, r) / ren.fill_circle(cx, cy, r)
 *   ren.draw_rounded_rect(x, y, w, h, r) / ren.fill_rounded_rect(x, y, w, h, r)
 *   ren.set_viewport(x, y, w, h) / ren.get_viewport() -> [x, y, w, h]
 *   ren.set_clip_rect(x, y, w, h) / ren.get_clip_rect() / ren.disable_clip_rect()
 *   ren.draw_text(text, x, y, size)                       内置 8x8 点阵字体
 *   ren.text_size(text, size) -> [w, h]                   计算文字尺寸
 *   ren.get_size() -> [w, h]
   */
#include "include/native.h"
#include "include/leno_value.h"
#include "guis_internal.h"
#include <string.h>

/* ============================================================================
 * Draw 渲染器实例方法（ren.method() 风格）
 * ============================================================================ */

/* ren.set_color(r, g, b, a) */
static Value gui_set_color_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    uint8_t r = (uint8_t)val_as_int(args[1]);
    uint8_t g = (uint8_t)val_as_int(args[2]);
    uint8_t b = (uint8_t)val_as_int(args[3]);
    uint8_t a = (uint8_t)val_as_int(args[4]);
    if (ren && ren->platform) leno_gui_platform_set_draw_color(ren->platform, r, g, b, a);
    return val_null();
}

/* ren.clear() */
static Value gui_render_clear_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    if (ren && ren->platform) leno_gui_platform_render_clear(ren->platform);
    return val_null();
}

/* ren.present() */
static Value gui_render_present_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    if (ren && ren->platform) leno_gui_platform_render_present(ren->platform);
    return val_null();
}

/* ren.draw_point(x, y) */
static Value gui_render_draw_point_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    int x = val_as_int(args[1]);
    int y = val_as_int(args[2]);
    if (ren && ren->platform) leno_gui_platform_render_draw_point(ren->platform, x, y);
    return val_null();
}

/* ren.draw_line(x1, y1, x2, y2) */
static Value gui_render_draw_line_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    int x1 = val_as_int(args[1]);
    int y1 = val_as_int(args[2]);
    int x2 = val_as_int(args[3]);
    int y2 = val_as_int(args[4]);
    if (ren && ren->platform) leno_gui_platform_render_draw_line(ren->platform, x1, y1, x2, y2);
    return val_null();
}

/* ren.draw_rect(x, y, w, h) */
static Value gui_render_draw_rect_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    int x = val_as_int(args[1]);
    int y = val_as_int(args[2]);
    int w = val_as_int(args[3]);
    int h = val_as_int(args[4]);
    if (ren && ren->platform) leno_gui_platform_render_draw_rect(ren->platform, x, y, w, h);
    return val_null();
}

/* ren.fill_rect(x, y, w, h) */
static Value gui_render_fill_rect_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    int x = val_as_int(args[1]);
    int y = val_as_int(args[2]);
    int w = val_as_int(args[3]);
    int h = val_as_int(args[4]);
    if (ren && ren->platform) leno_gui_platform_render_fill_rect(ren->platform, x, y, w, h);
    return val_null();
}

/* ren.get_size() -> [w, h] */
static Value gui_get_renderer_size_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    int w = 0, h = 0;
    if (ren && ren->platform) leno_gui_platform_get_renderer_size(ren->platform, &w, &h);
    return val_obj((Object*)make_int_array2(w, h));
}

/* ren.draw_text(text, x, y, size) */
static Value gui_draw_text_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    if (!ren || !ren->platform) return val_null();

    const char* text = "";
    if (val_is_obj(args[1])) {
        Object* obj = val_as_obj(args[1]);
        if (obj->type == OBJ_STRING) text = ((ObjString*)obj)->chars;
    }
    int x = val_as_int(args[2]);
    int y = val_as_int(args[3]);
    int size = val_as_int(args[4]);

    leno_gui_platform_draw_text(ren->platform, text, x, y, size);
    return val_null();
}

/* ren.text_size(text, size) -> [w, h] */
static Value gui_text_size_func(int argc, Value* args) {
    (void)argc;
    const char* text = "";
    if (val_is_obj(args[0])) {
        Object* obj = val_as_obj(args[0]);
        if (obj->type == OBJ_STRING) text = ((ObjString*)obj)->chars;
    }
    int size = val_as_int(args[1]);
    int w = 0, h = 0;
    leno_gui_platform_text_size(text, size, &w, &h);
    return val_obj((Object*)make_int_array2(w, h));
}

/* ren.draw_text_font(font, text, x, y) */
static Value gui_draw_text_font_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    ObjGUIFont* font = as_font(args[1]);
    if (!ren || !ren->platform || !font || !font->platform) return val_null();
    const char* text = "";
    if (val_is_obj(args[2])) {
        Object* obj = val_as_obj(args[2]);
        if (obj->type == OBJ_STRING) text = ((ObjString*)obj)->chars;
    }
    int x = val_as_int(args[3]);
    int y = val_as_int(args[4]);
    leno_gui_platform_draw_text_font(ren->platform, font->platform, text, x, y);
    return val_null();
}

/* ren.text_size_font(font, text) -> [w, h] */
static Value gui_text_size_font_func(int argc, Value* args) {
    (void)argc;
    ObjGUIFont* font = as_font(args[0]);
    if (!font || !font->platform) return val_obj((Object*)make_int_array2(0, 0));
    const char* text = "";
    if (val_is_obj(args[1])) {
        Object* obj = val_as_obj(args[1]);
        if (obj->type == OBJ_STRING) text = ((ObjString*)obj)->chars;
    }
    int w = 0, h = 0;
    leno_gui_platform_text_size_font(font->platform, text, &w, &h);
    return val_obj((Object*)make_int_array2(w, h));
}

/* ren.draw_circle(cx, cy, radius) */
static Value gui_render_draw_circle_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    int cx = val_as_int(args[1]);
    int cy = val_as_int(args[2]);
    int radius = val_as_int(args[3]);
    if (ren && ren->platform) leno_gui_platform_render_draw_circle(ren->platform, cx, cy, radius);
    return val_null();
}

/* ren.fill_circle(cx, cy, radius) */
static Value gui_render_fill_circle_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    int cx = val_as_int(args[1]);
    int cy = val_as_int(args[2]);
    int radius = val_as_int(args[3]);
    if (ren && ren->platform) leno_gui_platform_render_fill_circle(ren->platform, cx, cy, radius);
    return val_null();
}

/* ren.draw_rounded_rect(x, y, w, h, radius) */
static Value gui_render_draw_rounded_rect_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    int x = val_as_int(args[1]);
    int y = val_as_int(args[2]);
    int w = val_as_int(args[3]);
    int h = val_as_int(args[4]);
    int radius = val_as_int(args[5]);
    if (ren && ren->platform) leno_gui_platform_render_draw_rounded_rect(ren->platform, x, y, w, h, radius);
    return val_null();
}

/* ren.fill_rounded_rect(x, y, w, h, radius) */
static Value gui_render_fill_rounded_rect_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    int x = val_as_int(args[1]);
    int y = val_as_int(args[2]);
    int w = val_as_int(args[3]);
    int h = val_as_int(args[4]);
    int radius = val_as_int(args[5]);
    if (ren && ren->platform) leno_gui_platform_render_fill_rounded_rect(ren->platform, x, y, w, h, radius);
    return val_null();
}

/* ren.set_viewport(x, y, w, h) */
static Value gui_set_viewport_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    int x = val_as_int(args[1]);
    int y = val_as_int(args[2]);
    int w = val_as_int(args[3]);
    int h = val_as_int(args[4]);
    if (ren && ren->platform) leno_gui_platform_set_viewport(ren->platform, x, y, w, h);
    return val_null();
}

/* ren.get_viewport() -> [x, y, w, h] */
static Value gui_get_viewport_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    int x = 0, y = 0, w = 0, h = 0;
    if (ren && ren->platform) leno_gui_platform_get_viewport(ren->platform, &x, &y, &w, &h);
    ObjArray* arr = arr_new(4);
    arr->count = 4;
    arr_write(arr, 0, val_int(x));
    arr_write(arr, 1, val_int(y));
    arr_write(arr, 2, val_int(w));
    arr_write(arr, 3, val_int(h));
    return val_obj((Object*)arr);
}

/* ren.set_clip_rect(x, y, w, h) */
static Value gui_set_clip_rect_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    int x = val_as_int(args[1]);
    int y = val_as_int(args[2]);
    int w = val_as_int(args[3]);
    int h = val_as_int(args[4]);
    if (ren && ren->platform) leno_gui_platform_set_clip_rect(ren->platform, x, y, w, h);
    return val_null();
}

/* ren.get_clip_rect() -> [x, y, w, h] */
static Value gui_get_clip_rect_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    int x = 0, y = 0, w = 0, h = 0;
    if (ren && ren->platform) leno_gui_platform_get_clip_rect(ren->platform, &x, &y, &w, &h);
    ObjArray* arr = arr_new(4);
    arr->count = 4;
    arr_write(arr, 0, val_int(x));
    arr_write(arr, 1, val_int(y));
    arr_write(arr, 2, val_int(w));
    arr_write(arr, 3, val_int(h));
    return val_obj((Object*)arr);
}

/* ren.disable_clip_rect() */
static Value gui_disable_clip_rect_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    if (ren && ren->platform) leno_gui_platform_disable_clip_rect(ren->platform);
    return val_null();
}

/* ============================================================================
 * 注册 Draw 实例方法
 * ============================================================================ */

/* 前向声明 */
extern void draw_register_method_with_params(const char* name, ObjNative* method, int arity,
                                              int min_arity, int max_arity,
                                              TypeKind return_type, TypeKind* param_types);
extern ObjNative* make_native(NativeFn fn, int arity, const char* name);
extern void draw_init_methods(void);

void guis_init_instance_methods(void) {
    draw_init_methods();

    TypeKind no_params[] = {};
    TypeKind int_4[] = {TYPE_INT, TYPE_INT, TYPE_INT, TYPE_INT};
    TypeKind int_2[] = {TYPE_INT, TYPE_INT};
    TypeKind int_4_rect[] = {TYPE_INT, TYPE_INT, TYPE_INT, TYPE_INT};
    TypeKind int_3_circle[] = {TYPE_INT, TYPE_INT, TYPE_INT};
    TypeKind int_5_rounded[] = {TYPE_INT, TYPE_INT, TYPE_INT, TYPE_INT, TYPE_INT};
    TypeKind int_4_vp[] = {TYPE_INT, TYPE_INT, TYPE_INT, TYPE_INT};
    TypeKind any_3int[] = {TYPE_ANY, TYPE_INT, TYPE_INT, TYPE_INT};
    TypeKind str_int[] = {TYPE_STRING, TYPE_INT};

    draw_register_method_with_params("set_color", make_native(gui_set_color_func, 5, "set_color"), 4, -1, -1, TYPE_NULL, int_4);
    draw_register_method_with_params("clear", make_native(gui_render_clear_func, 1, "clear"), 0, -1, -1, TYPE_NULL, no_params);
    draw_register_method_with_params("present", make_native(gui_render_present_func, 1, "present"), 0, -1, -1, TYPE_NULL, no_params);
    draw_register_method_with_params("draw_point", make_native(gui_render_draw_point_func, 3, "draw_point"), 2, -1, -1, TYPE_NULL, int_2);
    draw_register_method_with_params("draw_line", make_native(gui_render_draw_line_func, 5, "draw_line"), 4, -1, -1, TYPE_NULL, int_4);
    draw_register_method_with_params("draw_rect", make_native(gui_render_draw_rect_func, 5, "draw_rect"), 4, -1, -1, TYPE_NULL, int_4_rect);
    draw_register_method_with_params("fill_rect", make_native(gui_render_fill_rect_func, 5, "fill_rect"), 4, -1, -1, TYPE_NULL, int_4_rect);
    draw_register_method_with_params("draw_circle", make_native(gui_render_draw_circle_func, 4, "draw_circle"), 3, -1, -1, TYPE_NULL, int_3_circle);
    draw_register_method_with_params("fill_circle", make_native(gui_render_fill_circle_func, 4, "fill_circle"), 3, -1, -1, TYPE_NULL, int_3_circle);
    draw_register_method_with_params("draw_rounded_rect", make_native(gui_render_draw_rounded_rect_func, 6, "draw_rounded_rect"), 5, -1, -1, TYPE_NULL, int_5_rounded);
    draw_register_method_with_params("fill_rounded_rect", make_native(gui_render_fill_rounded_rect_func, 6, "fill_rounded_rect"), 5, -1, -1, TYPE_NULL, int_5_rounded);
    draw_register_method_with_params("get_size", make_native(gui_get_renderer_size_func, 1, "get_size"), 0, -1, -1, TYPE_ANY, no_params);
    draw_register_method_with_params("set_viewport", make_native(gui_set_viewport_func, 5, "set_viewport"), 4, -1, -1, TYPE_NULL, int_4_vp);
    draw_register_method_with_params("get_viewport", make_native(gui_get_viewport_func, 1, "get_viewport"), 0, -1, -1, TYPE_ANY, no_params);
    draw_register_method_with_params("set_clip_rect", make_native(gui_set_clip_rect_func, 5, "set_clip_rect"), 4, -1, -1, TYPE_NULL, int_4_vp);
    draw_register_method_with_params("get_clip_rect", make_native(gui_get_clip_rect_func, 1, "get_clip_rect"), 0, -1, -1, TYPE_ANY, no_params);
    draw_register_method_with_params("disable_clip_rect", make_native(gui_disable_clip_rect_func, 1, "disable_clip_rect"), 0, -1, -1, TYPE_NULL, no_params);
    draw_register_method_with_params("draw_text", make_native(gui_draw_text_func, 5, "draw_text"), 4, -1, -1, TYPE_NULL, any_3int);
    draw_register_method_with_params("text_size", make_native(gui_text_size_func, 2, "text_size"), 2, -1, -1, TYPE_ANY, str_int);

    TypeKind font_str_2int[] = {TYPE_ANY, TYPE_ANY, TYPE_INT, TYPE_INT};
    draw_register_method_with_params("draw_text_font", make_native(gui_draw_text_font_func, 5, "draw_text_font"), 4, -1, -1, TYPE_NULL, font_str_2int);

    TypeKind font_str[] = {TYPE_ANY, TYPE_STRING};
    draw_register_method_with_params("text_size_font", make_native(gui_text_size_font_func, 2, "text_size_font"), 2, -1, -1, TYPE_ANY, font_str);
}
