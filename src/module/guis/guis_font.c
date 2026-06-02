/* Leno GUI - Font 字体实例方法
 * 从 guis.c 拆分出来的 Font 方法实现
 *
 * Font 实例方法 (font.method()):
 *   font.close()                           关闭并释放字体资源
 */
#include "include/native.h"
#include "include/leno_value.h"
#include "guis_internal.h"
#include <string.h>

/* ============================================================================
 * Font 实例方法（font.method() 风格）
 * ============================================================================ */

/* font.close() - 关闭并释放字体资源 */
static Value gui_font_close_func(int argc, Value* args) {
    (void)argc;
    ObjGUIFont* font = as_font(args[0]);
    if (font && font->platform) {
        leno_gui_platform_destroy_font(font->platform);
        font->platform = NULL;
    }
    return val_null();
}

/* ============================================================================
 * 注册 Font 实例方法
 * ============================================================================ */

/* 前向声明 */
extern void font_register_method_with_params(const char* name, ObjNative* method, int arity,
                                              int min_arity, int max_arity,
                                              TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);
extern ObjNative* make_native(NativeFn fn, int arity, const char* name);
extern void font_init_methods(void);

void guis_init_font_instance_methods(void) {
    font_init_methods();

    TypeKind no_params[] = {};

    /* font.close() */
    font_register_method_with_params("close", make_native(gui_font_close_func, 1, "close"), 0, -1, -1, TYPE_NULL, TYPE_UNKNOWN, no_params);
}
