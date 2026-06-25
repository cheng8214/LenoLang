/* Leno GUI - Draw 渲染器实例方法
 * 从 guis.c 拆分出来的 Draw 方法实现
 *
 * Draw 实例方法 (ren.method()):
 *   ren.set_color(_Rgb)              设置绘制颜色（影响后续所有绘制）
 *   ren.clear()                            用当前颜色清空画布
 *   ren.point(x, y)                        绘制单个像素点
 *   ren.line(x1, y1, x2, y2)               绘制直线
 *   ren.rect(x, y, w, h)                   绘制矩形边框
 *   ren.fill_rect(x, y, w, h)              填充矩形
 *   ren.circle(cx, cy, r)                  绘制圆形边框
 *   ren.fill_circle(cx, cy, r)             填充圆形
 *   ren.round_rect(x, y, w, h, r)          绘制圆角矩形边框
 *   ren.fill_round(x, y, w, h, r)          填充圆角矩形
 *   ren.draw_text(font, text, x, y)        使用指定字体绘制文字
 *   ren.text_size(font, text) -> [w, h]    计算指定字体文字尺寸
 *   ren.get_size() -> [w, h]               获取渲染器缓冲区大小
 *   ren.set_viewport(x, y, w, h)           设置渲染视口
 *   ren.get_viewport() -> [x, y, w, h]     获取当前视口
 *   ren.set_clip_rect(x, y, w, h)          设置裁剪矩形
 *   ren.get_clip_rect() -> [x, y, w, h]    获取当前裁剪矩形
 *   ren.no_clip()                          禁用裁剪矩形
   */
#include "include/native.h"
#include "include/leno_value.h"
#include "guis_internal.h"
#include <string.h>

/* ============================================================================
 * Draw 渲染器实例方法（ren.method() 风格）
 * ============================================================================ */

/* ren.set_color(Rgb) */
static Value gui_set_color_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    if (!ren || !ren->platform) return val_null();

    if (!val_is_obj(args[1]) || val_as_obj(args[1])->type != OBJ_RGB) {
        return val_null();
    }

    ObjRgb* rgb = (ObjRgb*)val_as_obj(args[1]);
    leno_gui_platform_set_draw_color(ren->platform, rgb->r, rgb->g, rgb->b, rgb->a);
    return val_null();
}

/* ren.clear() */
static Value gui_render_clear_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    if (ren && ren->platform) leno_gui_platform_render_clear(ren->platform);
    return val_null();
}

/* ren.point(x, y) */
static Value gui_render_draw_point_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    int x = val_as_int(args[1]);
    int y = val_as_int(args[2]);
    if (ren && ren->platform) leno_gui_platform_render_draw_point(ren->platform, x, y);
    return val_null();
}

/* ren.line(x1, y1, x2, y2) */
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

/* ren.rect(x, y, w, h) */
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

/* ren.draw_text(font, text, x, y) */
static Value gui_draw_text_func(int argc, Value* args) {
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

/* ren.text_size(font, text) -> [w, h] */
static Value gui_text_size_func(int argc, Value* args) {
    (void)argc;
    ObjGUIFont* font = as_font(args[0]);
    if (!font || !font->platform) return val_obj((Object*)make_int_array2(0, 0));
    const char* text = "";
    if (val_is_obj(args[1])) {
        Object* obj = val_as_obj(args[1]);
        if (obj->type == OBJ_STRING) text = ((ObjString*)obj)->chars;
    }
    int w = 0, h = 0;
    leno_gui_platform_text_size_font(NULL, font->platform, text, &w, &h);
    return val_obj((Object*)make_int_array2(w, h));
}

/* ren.circle(cx, cy, radius) */
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

/* ren.round_rect(x, y, w, h, radius) */
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

/* ren.fill_round(x, y, w, h, radius) */
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

/* ren.set_logical_size(w, h) */
static Value gui_set_logical_size_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    int w = val_as_int(args[1]);
    int h = val_as_int(args[2]);
    if (ren && ren->platform) leno_gui_platform_set_logical_size(ren->platform, w, h);
    return val_null();
}

/* ren.get_logical_size() -> [w, h] */
static Value gui_get_logical_size_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    int w = 0, h = 0;
    if (ren && ren->platform) leno_gui_platform_get_logical_size(ren->platform, &w, &h);
    ObjArray* arr = arr_new(2);
    arr->count = 2;
    arr_write(arr, 0, val_int(w));
    arr_write(arr, 1, val_int(h));
    return val_obj((Object*)arr);
}

/* ren.set_logical_presentation(mode) */
static Value gui_set_logical_presentation_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    int mode = val_as_int(args[1]);
    if (ren && ren->platform) leno_gui_platform_set_logical_presentation(ren->platform, mode);
    return val_null();
}

/* ren.get_logical_presentation() -> int */
static Value gui_get_logical_presentation_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    int mode = LENO_GUI_LOGICAL_PRESENTATION_DISABLED;
    if (ren && ren->platform) mode = leno_gui_platform_get_logical_presentation(ren->platform);
    return val_int(mode);
}

/* ren.get_logical_viewport() -> [x, y, w, h] */
static Value gui_get_logical_viewport_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    int x = 0, y = 0, w = 0, h = 0;
    if (ren && ren->platform) leno_gui_platform_get_logical_viewport(ren->platform, &x, &y, &w, &h);
    ObjArray* arr = arr_new(4);
    arr->count = 4;
    arr_write(arr, 0, val_int(x));
    arr_write(arr, 1, val_int(y));
    arr_write(arr, 2, val_int(w));
    arr_write(arr, 3, val_int(h));
    return val_obj((Object*)arr);
}

/* ren.reset_logical_size() */
static Value gui_reset_logical_size_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    if (ren && ren->platform) leno_gui_platform_reset_logical_size(ren->platform);
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

/* ren.no_clip() */
static Value gui_disable_clip_rect_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    if (ren && ren->platform) leno_gui_platform_disable_clip_rect(ren->platform);
    return val_null();
}

/* ============================================================================
 * 几何图形扩展（椭圆、三角形、多边形、圆弧）
 * ============================================================================ */

/* ren.ellipse(cx, cy, rx, ry) */
static Value gui_render_draw_ellipse_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    int cx = val_as_int(args[1]);
    int cy = val_as_int(args[2]);
    int rx = val_as_int(args[3]);
    int ry = val_as_int(args[4]);
    if (ren && ren->platform) leno_gui_platform_render_draw_ellipse(ren->platform, cx, cy, rx, ry);
    return val_null();
}

/* ren.fill_ellipse(cx, cy, rx, ry) */
static Value gui_render_fill_ellipse_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    int cx = val_as_int(args[1]);
    int cy = val_as_int(args[2]);
    int rx = val_as_int(args[3]);
    int ry = val_as_int(args[4]);
    if (ren && ren->platform) leno_gui_platform_render_fill_ellipse(ren->platform, cx, cy, rx, ry);
    return val_null();
}

/* ren.arc(cx, cy, r, start_angle, end_angle) */
static Value gui_render_draw_arc_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    int cx = val_as_int(args[1]);
    int cy = val_as_int(args[2]);
    int r = val_as_int(args[3]);
    double start_angle = val_as_double(args[4]);
    double end_angle = val_as_double(args[5]);
    if (ren && ren->platform) leno_gui_platform_render_draw_arc(ren->platform, cx, cy, r, start_angle, end_angle);
    return val_null();
}

/* ren.triangle(x1, y1, x2, y2, x3, y3) */
static Value gui_render_draw_triangle_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    int x1 = val_as_int(args[1]);
    int y1 = val_as_int(args[2]);
    int x2 = val_as_int(args[3]);
    int y2 = val_as_int(args[4]);
    int x3 = val_as_int(args[5]);
    int y3 = val_as_int(args[6]);
    if (ren && ren->platform) leno_gui_platform_render_draw_triangle(ren->platform, x1, y1, x2, y2, x3, y3);
    return val_null();
}

/* ren.fill_triangle(x1, y1, x2, y2, x3, y3) */
static Value gui_render_fill_triangle_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    int x1 = val_as_int(args[1]);
    int y1 = val_as_int(args[2]);
    int x2 = val_as_int(args[3]);
    int y2 = val_as_int(args[4]);
    int x3 = val_as_int(args[5]);
    int y3 = val_as_int(args[6]);
    if (ren && ren->platform) leno_gui_platform_render_fill_triangle(ren->platform, x1, y1, x2, y2, x3, y3);
    return val_null();
}

/* ren.polygon(points_array) - points_array = [x1, y1, x2, y2, ...] */
static Value gui_render_draw_polygon_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    if (!ren || !ren->platform) return val_null();
    
    if (!val_is_obj(args[1]) || val_as_obj(args[1])->type != OBJ_ARRAY) {
        return val_null();
    }
    ObjArray* arr = (ObjArray*)val_as_obj(args[1]);
    int num_points = arr->count / 2;
    if (num_points < 3) return val_null();
    
    int points[32];  /* 最多支持16个点 (32个坐标) */
    int max_points = num_points > 16 ? 16 : num_points;
    for (int i = 0; i < max_points * 2 && i < arr->count; i++) {
        points[i] = val_as_int(arr->elements[i]);
    }
    leno_gui_platform_render_draw_polygon(ren->platform, points, max_points);
    return val_null();
}

/* ren.fill_polygon(points_array) */
static Value gui_render_fill_polygon_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    if (!ren || !ren->platform) return val_null();
    
    if (!val_is_obj(args[1]) || val_as_obj(args[1])->type != OBJ_ARRAY) {
        return val_null();
    }
    ObjArray* arr = (ObjArray*)val_as_obj(args[1]);
    int num_points = arr->count / 2;
    if (num_points < 3) return val_null();
    
    int points[32];
    int max_points = num_points > 16 ? 16 : num_points;
    for (int i = 0; i < max_points * 2 && i < arr->count; i++) {
        points[i] = val_as_int(arr->elements[i]);
    }
    leno_gui_platform_render_fill_polygon(ren->platform, points, max_points);
    return val_null();
}

/* ren.bezier(points_array, steps) */
static Value gui_render_draw_bezier_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    if (!ren || !ren->platform) return val_null();
    
    if (!val_is_obj(args[1]) || val_as_obj(args[1])->type != OBJ_ARRAY) {
        return val_null();
    }
    ObjArray* arr = (ObjArray*)val_as_obj(args[1]);
    int num_points = arr->count / 2;
    if (num_points < 3) return val_null();
    
    int steps = val_as_int(args[2]);
    if (steps < 10) steps = 10;
    
    int points[32];
    int max_points = num_points > 16 ? 16 : num_points;
    for (int i = 0; i < max_points * 2 && i < arr->count; i++) {
        points[i] = val_as_int(arr->elements[i]);
    }
    leno_gui_platform_render_draw_bezier(ren->platform, points, max_points, steps);
    return val_null();
}

/* ============================================================================
 * 注册 Draw 实例方法
 * ============================================================================ */

/* 前向声明 */
extern void draw_register_method_with_params(const char* name, ObjNative* method, int arity,
                                              int min_arity, int max_arity,
                                              TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);
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

    TypeKind rgb_1[] = {TYPE_RGB};
    draw_register_method_with_params("set_color", make_native(gui_set_color_func, 2, "set_color"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, rgb_1);
    draw_register_method_with_params("clear", make_native(gui_render_clear_func, 1, "clear"), 0, -1, -1, TYPE_NULL, TYPE_UNKNOWN, no_params);
    draw_register_method_with_params("point", make_native(gui_render_draw_point_func, 3, "point"), 2, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_2);
    draw_register_method_with_params("line", make_native(gui_render_draw_line_func, 5, "line"), 4, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_4);
    draw_register_method_with_params("rect", make_native(gui_render_draw_rect_func, 5, "rect"), 4, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_4_rect);
    draw_register_method_with_params("fill_rect", make_native(gui_render_fill_rect_func, 5, "fill_rect"), 4, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_4_rect);
    draw_register_method_with_params("circle", make_native(gui_render_draw_circle_func, 4, "circle"), 3, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_3_circle);
    draw_register_method_with_params("fill_circle", make_native(gui_render_fill_circle_func, 4, "fill_circle"), 3, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_3_circle);
    draw_register_method_with_params("round_rect", make_native(gui_render_draw_rounded_rect_func, 6, "round_rect"), 5, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_5_rounded);
    draw_register_method_with_params("fill_round", make_native(gui_render_fill_rounded_rect_func, 6, "fill_round"), 5, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_5_rounded);
    draw_register_method_with_params("get_size", make_native(gui_get_renderer_size_func, 1, "get_size"), 0, -1, -1, TYPE_ANY, TYPE_UNKNOWN, no_params);
    
    /* 逻辑呈现模式（借鉴 SDL3） */
    TypeKind int_2_size[] = {TYPE_INT, TYPE_INT};
    draw_register_method_with_params("set_logical_size", make_native(gui_set_logical_size_func, 3, "set_logical_size"), 2, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_2_size);
    draw_register_method_with_params("get_logical_size", make_native(gui_get_logical_size_func, 1, "get_logical_size"), 0, -1, -1, TYPE_ANY, TYPE_UNKNOWN, no_params);
    TypeKind int_1[] = {TYPE_INT};
    draw_register_method_with_params("set_logical_presentation", make_native(gui_set_logical_presentation_func, 2, "set_logical_presentation"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_1);
    draw_register_method_with_params("get_logical_presentation", make_native(gui_get_logical_presentation_func, 1, "get_logical_presentation"), 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, no_params);
    draw_register_method_with_params("get_logical_viewport", make_native(gui_get_logical_viewport_func, 1, "get_logical_viewport"), 0, -1, -1, TYPE_ANY, TYPE_UNKNOWN, no_params);
    draw_register_method_with_params("reset_logical_size", make_native(gui_reset_logical_size_func, 1, "reset_logical_size"), 0, -1, -1, TYPE_NULL, TYPE_UNKNOWN, no_params);
    
    draw_register_method_with_params("set_viewport", make_native(gui_set_viewport_func, 5, "set_viewport"), 4, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_4_vp);
    draw_register_method_with_params("get_viewport", make_native(gui_get_viewport_func, 1, "get_viewport"), 0, -1, -1, TYPE_ANY, TYPE_UNKNOWN, no_params);
    draw_register_method_with_params("set_clip_rect", make_native(gui_set_clip_rect_func, 5, "set_clip_rect"), 4, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_4_vp);
    draw_register_method_with_params("get_clip_rect", make_native(gui_get_clip_rect_func, 1, "get_clip_rect"), 0, -1, -1, TYPE_ANY, TYPE_UNKNOWN, no_params);
    draw_register_method_with_params("no_clip", make_native(gui_disable_clip_rect_func, 1, "no_clip"), 0, -1, -1, TYPE_NULL, TYPE_UNKNOWN, no_params);
    TypeKind font_str_2int[] = {TYPE_ANY, TYPE_STRING, TYPE_INT, TYPE_INT};
    draw_register_method_with_params("draw_text", make_native(gui_draw_text_func, 5, "draw_text"), 4, -1, -1, TYPE_NULL, TYPE_UNKNOWN, font_str_2int);

    TypeKind font_str[] = {TYPE_ANY, TYPE_STRING};
    draw_register_method_with_params("text_size", make_native(gui_text_size_func, 2, "text_size"), 2, -1, -1, TYPE_ANY, TYPE_UNKNOWN, font_str);
    
    /* 几何图形扩展 */
    TypeKind int_4_ellipse[] = {TYPE_INT, TYPE_INT, TYPE_INT, TYPE_INT};
    draw_register_method_with_params("ellipse", make_native(gui_render_draw_ellipse_func, 5, "ellipse"), 4, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_4_ellipse);
    draw_register_method_with_params("fill_ellipse", make_native(gui_render_fill_ellipse_func, 5, "fill_ellipse"), 4, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_4_ellipse);
    
    TypeKind int_3_float2[] = {TYPE_INT, TYPE_INT, TYPE_INT, TYPE_FLOAT, TYPE_FLOAT};
    draw_register_method_with_params("arc", make_native(gui_render_draw_arc_func, 6, "arc"), 5, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_3_float2);
    
    TypeKind int_6_triangle[] = {TYPE_INT, TYPE_INT, TYPE_INT, TYPE_INT, TYPE_INT, TYPE_INT};
    draw_register_method_with_params("triangle", make_native(gui_render_draw_triangle_func, 7, "triangle"), 6, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_6_triangle);
    draw_register_method_with_params("fill_triangle", make_native(gui_render_fill_triangle_func, 7, "fill_triangle"), 6, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_6_triangle);
    
    TypeKind arr_1[] = {TYPE_ARRAY};
    draw_register_method_with_params("polygon", make_native(gui_render_draw_polygon_func, 2, "polygon"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, arr_1);
    draw_register_method_with_params("fill_polygon", make_native(gui_render_fill_polygon_func, 2, "fill_polygon"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, arr_1);
    
    TypeKind arr_int[] = {TYPE_ARRAY, TYPE_INT};
    draw_register_method_with_params("bezier", make_native(gui_render_draw_bezier_func, 3, "bezier"), 2, -1, -1, TYPE_NULL, TYPE_UNKNOWN, arr_int);
}
