/* Leno GUI - Image 图像实例方法
 * 从 guis.c 拆分出来的 Image 方法实现
 *
 * Image 实例方法 (image.method()):
 *   image.draw(ren, x, y)                  绘制图像到渲染器
 *   image.draw_src(ren, sx, sy, sw, sh, dx, dy, dw, dh)  源矩形绘制
 *   image.draw_scaled(ren, dx, dy, dw, dh) 缩放绘制到目标矩形
 *   image.draw_rotated(ren, x, y, angle, flip)  旋转/翻转绘制（原始尺寸）
 *   image.draw_flipped(ren, x, y, flip)    翻转绘制（原始尺寸）
 *   image.draw_flipped_scaled(ren, x, y, w, h, flip)  翻转+缩放绘制
 *   image.draw_rotated_scaled(ren, x, y, w, h, angle) 旋转+缩放绘制
 *   image.width() -> int                   获取图像宽度
 *   image.height() -> int                  获取图像高度
 *   image.size() -> [w, h]                 获取图像尺寸数组
 *   image.access() -> int                  获取图像访问模式
 */
#include "include/native.h"
#include "include/leno_value.h"
#include "guis_internal.h"
#include <string.h>

/* ============================================================================
 * Image 实例方法（image.method() 风格）
 * ============================================================================ */

/* image.draw(ren, x, y) */
static Value gui_image_draw_func(int argc, Value* args) {
    (void)argc;
    ObjGUIImage* tex = as_image(args[0]);
    ObjGUIRenderer* ren = as_renderer(args[1]);
    if (!ren || !ren->platform || !tex || !tex->platform) return val_null();
    int x = val_as_int(args[2]);
    int y = val_as_int(args[3]);
    leno_gui_platform_render_image(ren->platform, tex->platform, x, y);
    return val_null();
}

/* image.draw_src(ren, sx, sy, sw, sh, dx, dy, dw, dh) */
static Value gui_image_draw_src_func(int argc, Value* args) {
    (void)argc;
    ObjGUIImage* tex = as_image(args[0]);
    ObjGUIRenderer* ren = as_renderer(args[1]);
    if (!ren || !ren->platform || !tex || !tex->platform) return val_null();
    int sx = val_as_int(args[2]);
    int sy = val_as_int(args[3]);
    int sw = val_as_int(args[4]);
    int sh = val_as_int(args[5]);
    int dx = val_as_int(args[6]);
    int dy = val_as_int(args[7]);
    int dw = val_as_int(args[8]);
    int dh = val_as_int(args[9]);
    leno_gui_platform_render_image_src(ren->platform, tex->platform, sx, sy, sw, sh, dx, dy, dw, dh);
    return val_null();
}

/* image.draw_rotated(ren, x, y, angle, flip) */
static Value gui_image_draw_rotated_func(int argc, Value* args) {
    (void)argc;
    ObjGUIImage* tex = as_image(args[0]);
    ObjGUIRenderer* ren = as_renderer(args[1]);
    if (!ren || !ren->platform || !tex || !tex->platform) return val_null();
    int x = val_as_int(args[2]);
    int y = val_as_int(args[3]);
    double angle = val_as_double(args[4]);
    int flip = val_as_int(args[5]);
    leno_gui_platform_render_image_rotated(ren->platform, tex->platform, x, y, angle, flip);
    return val_null();
}

/* image.draw_src_flipped(ren, sx, sy, sw, sh, dx, dy, dw, dh) - 水平翻转 */
static Value gui_image_draw_src_flipped_func(int argc, Value* args) {
    (void)argc;
    ObjGUIImage* tex = as_image(args[0]);
    ObjGUIRenderer* ren = as_renderer(args[1]);
    if (!ren || !ren->platform || !tex || !tex->platform) return val_null();
    int sx = val_as_int(args[2]);
    int sy = val_as_int(args[3]);
    int sw = val_as_int(args[4]);
    int sh = val_as_int(args[5]);
    int dx = val_as_int(args[6]);
    int dy = val_as_int(args[7]);
    int dw = val_as_int(args[8]);
    int dh = val_as_int(args[9]);
    leno_gui_platform_render_image_src_flipped(ren->platform, tex->platform, sx, sy, sw, sh, dx, dy, dw, dh);
    return val_null();
}

/* image.width() -> int */
static Value gui_image_width_func(int argc, Value* args) {
    (void)argc;
    ObjGUIImage* tex = as_image(args[0]);
    if (!tex || !tex->platform) return val_int(0);
    return val_int(leno_gui_platform_image_width(tex->platform));
}

/* image.height() -> int */
static Value gui_image_height_func(int argc, Value* args) {
    (void)argc;
    ObjGUIImage* tex = as_image(args[0]);
    if (!tex || !tex->platform) return val_int(0);
    return val_int(leno_gui_platform_image_height(tex->platform));
}

/* image.draw_scaled(ren, dx, dy, dw, dh) - 缩放绘制到目标矩形 */
static Value gui_image_draw_scaled_func(int argc, Value* args) {
    (void)argc;
    ObjGUIImage* tex = as_image(args[0]);
    ObjGUIRenderer* ren = as_renderer(args[1]);
    if (!ren || !ren->platform || !tex || !tex->platform) return val_null();
    int w = leno_gui_platform_image_width(tex->platform);
    int h = leno_gui_platform_image_height(tex->platform);
    int dx = val_as_int(args[2]);
    int dy = val_as_int(args[3]);
    int dw = val_as_int(args[4]);
    int dh = val_as_int(args[5]);
    leno_gui_platform_render_image_src(ren->platform, tex->platform, 0, 0, w, h, dx, dy, dw, dh);
    return val_null();
}

/* image.draw_flipped(ren, x, y, flip) - 翻转绘制 */
static Value gui_image_draw_flipped_func(int argc, Value* args) {
    (void)argc;
    ObjGUIImage* tex = as_image(args[0]);
    ObjGUIRenderer* ren = as_renderer(args[1]);
    if (!ren || !ren->platform || !tex || !tex->platform) return val_null();
    int x = val_as_int(args[2]);
    int y = val_as_int(args[3]);
    int flip = val_as_int(args[4]);
    leno_gui_platform_render_image_rotated(ren->platform, tex->platform, x, y, 0.0, flip);
    return val_null();
}

/* image.size() -> [w, h] */
static Value gui_image_size_func(int argc, Value* args) {
    (void)argc;
    ObjGUIImage* tex = as_image(args[0]);
    if (!tex || !tex->platform) return val_obj((Object*)make_int_array2(0, 0));
    int w = leno_gui_platform_image_width(tex->platform);
    int h = leno_gui_platform_image_height(tex->platform);
    return val_obj((Object*)make_int_array2(w, h));
}

/* image.access() -> int */
static Value gui_image_access_func(int argc, Value* args) {
    (void)argc;
    ObjGUIImage* tex = as_image(args[0]);
    if (!tex || !tex->platform) return val_int(0);
    return val_int(leno_gui_platform_image_access(tex->platform));
}

/* image.draw_flipped_scaled(ren, x, y, w, h, flip) - 翻转+缩放绘制 */
static Value gui_image_draw_flipped_scaled_func(int argc, Value* args) {
    (void)argc;
    ObjGUIImage* tex = as_image(args[0]);
    ObjGUIRenderer* ren = as_renderer(args[1]);
    if (!ren || !ren->platform || !tex || !tex->platform) return val_null();
    int w = leno_gui_platform_image_width(tex->platform);
    int h = leno_gui_platform_image_height(tex->platform);
    int dx = val_as_int(args[2]);
    int dy = val_as_int(args[3]);
    int dw = val_as_int(args[4]);
    int dh = val_as_int(args[5]);
    int flip = val_as_int(args[6]);
    leno_gui_platform_render_image_rotated_src(ren->platform, tex->platform,
        0, 0, w, h, dx, dy, dw, dh, 0.0, flip);
    return val_null();
}

/* image.draw_rotated_scaled(ren, x, y, w, h, angle) - 旋转+缩放绘制 */
static Value gui_image_draw_rotated_scaled_func(int argc, Value* args) {
    (void)argc;
    ObjGUIImage* tex = as_image(args[0]);
    ObjGUIRenderer* ren = as_renderer(args[1]);
    if (!ren || !ren->platform || !tex || !tex->platform) return val_null();
    int w = leno_gui_platform_image_width(tex->platform);
    int h = leno_gui_platform_image_height(tex->platform);
    int dx = val_as_int(args[2]);
    int dy = val_as_int(args[3]);
    int dw = val_as_int(args[4]);
    int dh = val_as_int(args[5]);
    double angle = val_as_double(args[6]);
    leno_gui_platform_render_image_rotated_src(ren->platform, tex->platform,
        0, 0, w, h, dx, dy, dw, dh, angle, 0);
    return val_null();
}

/* image.close() - 关闭并释放图像资源 */
static Value gui_image_close_func(int argc, Value* args) {
    (void)argc;
    ObjGUIImage* tex = as_image(args[0]);
    if (tex && tex->platform) {
        leno_gui_platform_destroy_image(tex->platform);
        tex->platform = NULL;
    }
    return val_null();
}

/* ============================================================================
 * 注册 Image 实例方法
 * ============================================================================ */

/* 前向声明 */
extern void image_register_method_with_params(const char* name, ObjNative* method, int arity,
                                              int min_arity, int max_arity,
                                              TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);
extern ObjNative* make_native(NativeFn fn, int arity, const char* name);
extern void image_init_methods(void);

void guis_init_image_instance_methods(void) {
    image_init_methods();

    TypeKind no_params[] = {};

    /* image.draw(ren, x, y) */
    TypeKind draw_params[] = {TYPE_ANY, TYPE_INT, TYPE_INT};
    image_register_method_with_params("draw", make_native(gui_image_draw_func, 4, "draw"), 3, -1, -1, TYPE_NULL, TYPE_UNKNOWN, draw_params);

    /* image.draw_src(ren, sx, sy, sw, sh, dx, dy, dw, dh) */
    TypeKind draw_src_params[] = {TYPE_ANY, TYPE_INT, TYPE_INT, TYPE_INT, TYPE_INT, TYPE_INT, TYPE_INT, TYPE_INT, TYPE_INT};
    image_register_method_with_params("draw_src", make_native(gui_image_draw_src_func, 10, "draw_src"), 9, -1, -1, TYPE_NULL, TYPE_UNKNOWN, draw_src_params);

    /* image.draw_rotated(ren, x, y, angle, flip) */
    TypeKind draw_rotated_params[] = {TYPE_ANY, TYPE_INT, TYPE_INT, TYPE_ANY, TYPE_INT};
    image_register_method_with_params("draw_rotated", make_native(gui_image_draw_rotated_func, 6, "draw_rotated"), 5, -1, -1, TYPE_NULL, TYPE_UNKNOWN, draw_rotated_params);

    /* image.width() -> int */
    image_register_method_with_params("width", make_native(gui_image_width_func, 1, "width"), 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, no_params);

    /* image.height() -> int */
    image_register_method_with_params("height", make_native(gui_image_height_func, 1, "height"), 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, no_params);

    /* image.draw_scaled(ren, dx, dy, dw, dh) */
    TypeKind draw_scaled_params[] = {TYPE_ANY, TYPE_INT, TYPE_INT, TYPE_INT, TYPE_INT};
    image_register_method_with_params("draw_scaled", make_native(gui_image_draw_scaled_func, 6, "draw_scaled"), 5, -1, -1, TYPE_NULL, TYPE_UNKNOWN, draw_scaled_params);

    /* image.draw_flipped(ren, x, y, flip) */
    TypeKind draw_flipped_params[] = {TYPE_ANY, TYPE_INT, TYPE_INT, TYPE_INT};
    image_register_method_with_params("draw_flipped", make_native(gui_image_draw_flipped_func, 5, "draw_flipped"), 4, -1, -1, TYPE_NULL, TYPE_UNKNOWN, draw_flipped_params);

    /* image.size() -> [w, h] */
    image_register_method_with_params("size", make_native(gui_image_size_func, 1, "size"), 0, -1, -1, TYPE_ANY, TYPE_UNKNOWN, no_params);

    /* image.access() -> int */
    image_register_method_with_params("access", make_native(gui_image_access_func, 1, "access"), 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, no_params);

    /* image.draw_flipped_scaled(ren, x, y, w, h, flip) */
    TypeKind draw_flipped_scaled_params[] = {TYPE_ANY, TYPE_INT, TYPE_INT, TYPE_INT, TYPE_INT, TYPE_INT};
    image_register_method_with_params("draw_flipped_scaled", make_native(gui_image_draw_flipped_scaled_func, 7, "draw_flipped_scaled"), 6, -1, -1, TYPE_NULL, TYPE_UNKNOWN, draw_flipped_scaled_params);

    /* image.draw_rotated_scaled(ren, x, y, w, h, angle) */
    TypeKind draw_rotated_scaled_params[] = {TYPE_ANY, TYPE_INT, TYPE_INT, TYPE_INT, TYPE_INT, TYPE_ANY};
    image_register_method_with_params("draw_rotated_scaled", make_native(gui_image_draw_rotated_scaled_func, 7, "draw_rotated_scaled"), 6, -1, -1, TYPE_NULL, TYPE_UNKNOWN, draw_rotated_scaled_params);

    /* image.draw_src_flipped(ren, sx, sy, sw, sh, dx, dy, dw, dh) - 水平翻转 */
    TypeKind draw_src_flipped_params[] = {TYPE_ANY, TYPE_INT, TYPE_INT, TYPE_INT, TYPE_INT, TYPE_INT, TYPE_INT, TYPE_INT, TYPE_INT};
    image_register_method_with_params("draw_src_flipped", make_native(gui_image_draw_src_flipped_func, 10, "draw_src_flipped"), 9, -1, -1, TYPE_NULL, TYPE_UNKNOWN, draw_src_flipped_params);

    /* image.close() */
    image_register_method_with_params("close", make_native(gui_image_close_func, 1, "close"), 0, -1, -1, TYPE_NULL, TYPE_UNKNOWN, no_params);
}
