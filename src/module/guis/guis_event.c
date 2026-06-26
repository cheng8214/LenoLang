/* Leno GUI - Event 事件实例方法
 * 从 guis.c 拆分出来的 Event 方法实现
 *
 * Event 实例方法 (e.method()):
 *   e.type() -> int                          获取事件类型码
 *   e.quit()                                 是否为退出事件
 *   e.window_close()                         是否为窗口关闭事件
 *   e.window_resize()                        是否为窗口大小改变事件
 *   e.window_move()                          是否为窗口移动事件
 *   e.key_down()                             是否为按键按下事件
 *   e.key_up()                               是否为按键释放事件
 *   e.text_input()                           是否为文本输入事件
 *   e.mouse_move()                           是否为鼠标移动事件
 *   e.mouse_down()                           是否为鼠标按下事件
 *   e.mouse_up()                             是否为鼠标释放事件
 *   e.mouse_wheel()                          是否为鼠标滚轮事件
 *   e.key() -> int                           获取按键码
 *   e.mouse_x() / e.mouse_y() -> int         获取鼠标坐标
 *   e.mouse_button() -> int                  获取鼠标按钮（1=左键, 2=中键, 3=右键）
 *   e.width() / e.height() -> int            获取窗口新尺寸（resize 事件）
 *   e.text() -> string                       获取输入的文本字符
 *
 */

#include "include/native.h"
#include "include/leno_value.h"
#include "include/leno_vm.h"
#include "leno_guis.h"
#include "guis_internal.h"
#include <string.h>

/* ObjGUIEvent 已在 guis_internal.h 中定义 */

/* ============================================================================
 * Event 事件实例方法（e.method() 风格）
 * 事件底层是 Dict，方法通过 event_find_method 查找分发
 * ============================================================================ */

/* 辅助：从事件对象中获取内部 dict */
static ObjDict* event_get_dict(Value event_val) {
    if (!val_is_obj(event_val) || val_as_obj(event_val)->type != OBJ_GUI_EVENT) return NULL;
    return ((ObjGUIEvent*)val_as_obj(event_val))->data;
}

/* 辅助：从事件字典中获取 "type" 字段值 */
static int event_get_type(Value event_val) {
    ObjDict* d = event_get_dict(event_val);
    if (!d) return 0;
    Value v = dict_get(d, val_obj((Object*)str_key_type));
    return val_is_int(v) ? val_as_int(v) : 0;
}

/* 辅助：从事件字典中获取整数字段 */
static int event_get_int(Value event_val, ObjString* key) {
    ObjDict* d = event_get_dict(event_val);
    if (!d) return 0;
    Value v = dict_get(d, val_obj((Object*)key));
    if (val_is_int(v)) return val_as_int(v);
    if (val_is_float(v)) return (int)val_as_double(v);
    if (val_is_num(v)) return (int)val_as_num(v);
    return 0;
}

/* 辅助：从事件字典中获取字符串字段 */
static const char* event_get_string(Value event_val, ObjString* key) {
    ObjDict* d = event_get_dict(event_val);
    if (!d) return "";
    Value v = dict_get(d, val_obj((Object*)key));
    if (val_is_obj(v) && val_as_obj(v)->type == OBJ_STRING) {
        return ((ObjString*)val_as_obj(v))->chars;
    }
    return "";
}

/* e.type() - 获取事件类型 */
static Value event_type_func(int argc, Value* args) {
    (void)argc;
    return val_int(event_get_type(args[0]));
}

/* e.quit() - 是否为退出事件 */
static Value event_quit_func(int argc, Value* args) {
    (void)argc;
    return val_bool(event_get_type(args[0]) == LENO_GUI_EVT_QUIT);
}

/* e.window_close() - 是否为窗口关闭事件 */
static Value event_window_close_func(int argc, Value* args) {
    (void)argc;
    return val_bool(event_get_type(args[0]) == LENO_GUI_EVT_WINDOW_CLOSE);
}

/* e.window_resize() - 是否为窗口大小改变事件 */
static Value event_window_resize_func(int argc, Value* args) {
    (void)argc;
    return val_bool(event_get_type(args[0]) == LENO_GUI_EVT_WINDOW_RESIZE);
}

/* e.window_move() - 是否为窗口移动事件 */
static Value event_window_move_func(int argc, Value* args) {
    (void)argc;
    return val_bool(event_get_type(args[0]) == LENO_GUI_EVT_WINDOW_MOVE);
}

/* e.key_down() - 是否为按键按下事件 */
static Value event_key_down_func(int argc, Value* args) {
    (void)argc;
    return val_bool(event_get_type(args[0]) == LENO_GUI_EVT_KEY_DOWN);
}

/* e.key_up() - 是否为按键释放事件 */
static Value event_key_up_func(int argc, Value* args) {
    (void)argc;
    return val_bool(event_get_type(args[0]) == LENO_GUI_EVT_KEY_UP);
}

/* e.text_input() - 是否为文本输入事件 */
static Value event_text_input_func(int argc, Value* args) {
    (void)argc;
    return val_bool(event_get_type(args[0]) == LENO_GUI_EVT_TEXT_INPUT);
}

/* e.mouse_move() - 是否为鼠标移动事件 */
static Value event_mouse_move_func(int argc, Value* args) {
    (void)argc;
    return val_bool(event_get_type(args[0]) == LENO_GUI_EVT_MOUSE_MOVE);
}

/* e.mouse_down() - 是否为鼠标按下事件 */
static Value event_mouse_down_func(int argc, Value* args) {
    (void)argc;
    return val_bool(event_get_type(args[0]) == LENO_GUI_EVT_MOUSE_DOWN);
}

/* e.mouse_up() - 是否为鼠标释放事件 */
static Value event_mouse_up_func(int argc, Value* args) {
    (void)argc;
    return val_bool(event_get_type(args[0]) == LENO_GUI_EVT_MOUSE_UP);
}

/* e.mouse_wheel() - 是否为鼠标滚轮事件 */
static Value event_mouse_wheel_func(int argc, Value* args) {
    (void)argc;
    return val_bool(event_get_type(args[0]) == LENO_GUI_EVT_MOUSE_WHEEL);
}

/* e.window_focus() - 是否为窗口获得焦点事件 */
static Value event_window_focus_func(int argc, Value* args) {
    (void)argc;
    return val_bool(event_get_type(args[0]) == LENO_GUI_EVT_WINDOW_FOCUS);
}

/* e.window_unfocus() - 是否为窗口失去焦点事件 */
static Value event_window_unfocus_func(int argc, Value* args) {
    (void)argc;
    return val_bool(event_get_type(args[0]) == LENO_GUI_EVT_WINDOW_UNFOCUS);
}

/* e.window_show() - 是否为窗口显示事件 */
static Value event_window_show_func(int argc, Value* args) {
    (void)argc;
    return val_bool(event_get_type(args[0]) == LENO_GUI_EVT_WINDOW_SHOW);
}

/* e.window_hide() - 是否为窗口隐藏事件 */
static Value event_window_hide_func(int argc, Value* args) {
    (void)argc;
    return val_bool(event_get_type(args[0]) == LENO_GUI_EVT_WINDOW_HIDE);
}

/* e.window_exposed() - 是否为窗口暴露事件 */
static Value event_window_exposed_func(int argc, Value* args) {
    (void)argc;
    return val_bool(event_get_type(args[0]) == LENO_GUI_EVT_WINDOW_EXPOSED);
}

/* e.window_minimized() - 是否为窗口最小化事件 */
static Value event_window_minimized_func(int argc, Value* args) {
    (void)argc;
    return val_bool(event_get_type(args[0]) == LENO_GUI_EVT_WINDOW_MINIMIZED);
}

/* e.window_maximized() - 是否为窗口最大化事件 */
static Value event_window_maximized_func(int argc, Value* args) {
    (void)argc;
    return val_bool(event_get_type(args[0]) == LENO_GUI_EVT_WINDOW_MAXIMIZED);
}

/* e.window_restored() - 是否为窗口恢复事件 */
static Value event_window_restored_func(int argc, Value* args) {
    (void)argc;
    return val_bool(event_get_type(args[0]) == LENO_GUI_EVT_WINDOW_RESTORED);
}

/* e.key() - 获取按键码 */
static Value event_key_func(int argc, Value* args) {
    (void)argc;
    init_event_string_keys();
    return val_int(event_get_int(args[0], str_key_key));
}

/* e.mouse_x() - 获取鼠标 X 坐标 */
static Value event_mouse_x_func(int argc, Value* args) {
    (void)argc;
    init_event_string_keys();
    return val_int(event_get_int(args[0], str_key_x));
}

/* e.mouse_y() - 获取鼠标 Y 坐标 */
static Value event_mouse_y_func(int argc, Value* args) {
    (void)argc;
    init_event_string_keys();
    return val_int(event_get_int(args[0], str_key_y));
}

/* e.mouse_button() - 获取鼠标按钮 */
static Value event_mouse_button_func(int argc, Value* args) {
    (void)argc;
    init_event_string_keys();
    return val_int(event_get_int(args[0], str_key_button));
}

/* e.width() - 获取窗口宽度（resize 事件） */
static Value event_width_func(int argc, Value* args) {
    (void)argc;
    init_event_string_keys();
    return val_int(event_get_int(args[0], str_key_width));
}

/* e.height() - 获取窗口高度（resize 事件） */
static Value event_height_func(int argc, Value* args) {
    (void)argc;
    init_event_string_keys();
    return val_int(event_get_int(args[0], str_key_height));
}

/* e.text() - 获取输入文本（text_input 事件） */
static Value event_text_func(int argc, Value* args) {
    (void)argc;
    init_event_string_keys();
    const char* text = event_get_string(args[0], str_key_text);
    return val_obj((Object*)str_copy(text, (int)strlen(text)));
}

/* e.mod() - 获取修饰键标志 */
static Value event_mod_func(int argc, Value* args) {
    (void)argc;
    init_event_string_keys();
    return val_int(event_get_int(args[0], str_key_mod));
}

/* e.scancode() - 获取按键扫描码 */
static Value event_scancode_func(int argc, Value* args) {
    (void)argc;
    init_event_string_keys();
    return val_int(event_get_int(args[0], str_key_scancode));
}

/* e.repeat() - 获取按键重复标志 */
static Value event_repeat_func(int argc, Value* args) {
    (void)argc;
    init_event_string_keys();
    return val_bool(event_get_int(args[0], str_key_repeat) != 0);
}

/* e.wheel_x() - 获取水平滚轮增量 */
static Value event_wheel_x_func(int argc, Value* args) {
    (void)argc;
    init_event_string_keys();
    return val_int(event_get_int(args[0], str_key_wheel_x));
}

/* e.wheel_y() - 获取垂直滚轮增量 */
static Value event_wheel_y_func(int argc, Value* args) {
    (void)argc;
    init_event_string_keys();
    return val_int(event_get_int(args[0], str_key_wheel_y));
}

/* e.xrel() - 获取鼠标相对 X 移动 */
static Value event_xrel_func(int argc, Value* args) {
    (void)argc;
    init_event_string_keys();
    return val_int(event_get_int(args[0], str_key_xrel));
}

/* e.yrel() - 获取鼠标相对 Y 移动 */
static Value event_yrel_func(int argc, Value* args) {
    (void)argc;
    init_event_string_keys();
    return val_int(event_get_int(args[0], str_key_yrel));
}

/* e.clicks() - 获取鼠标点击次数 */
static Value event_clicks_func(int argc, Value* args) {
    (void)argc;
    init_event_string_keys();
    return val_int(event_get_int(args[0], str_key_clicks));
}

/* e.window_id() - 获取窗口 ID */
static Value event_window_id_func(int argc, Value* args) {
    (void)argc;
    init_event_string_keys();
    return val_int(event_get_int(args[0], str_key_window_id));
}

/* 注册 Event 实例方法 */
void guis_init_event_methods(void) {
    event_init_methods();

    TypeKind no_params[] = {};

    event_register_method_with_params("type", make_native(event_type_func, 1, "type"), 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("quit", make_native(event_quit_func, 1, "quit"), 0, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("window_close", make_native(event_window_close_func, 1, "window_close"), 0, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("window_resize", make_native(event_window_resize_func, 1, "window_resize"), 0, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("window_move", make_native(event_window_move_func, 1, "window_move"), 0, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("window_focus", make_native(event_window_focus_func, 1, "window_focus"), 0, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("window_unfocus", make_native(event_window_unfocus_func, 1, "window_unfocus"), 0, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("window_show", make_native(event_window_show_func, 1, "window_show"), 0, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("window_hide", make_native(event_window_hide_func, 1, "window_hide"), 0, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("window_exposed", make_native(event_window_exposed_func, 1, "window_exposed"), 0, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("window_minimized", make_native(event_window_minimized_func, 1, "window_minimized"), 0, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("window_maximized", make_native(event_window_maximized_func, 1, "window_maximized"), 0, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("window_restored", make_native(event_window_restored_func, 1, "window_restored"), 0, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("key_down", make_native(event_key_down_func, 1, "key_down"), 0, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("key_up", make_native(event_key_up_func, 1, "key_up"), 0, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("text_input", make_native(event_text_input_func, 1, "text_input"), 0, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("mouse_move", make_native(event_mouse_move_func, 1, "mouse_move"), 0, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("mouse_down", make_native(event_mouse_down_func, 1, "mouse_down"), 0, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("mouse_up", make_native(event_mouse_up_func, 1, "mouse_up"), 0, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("mouse_wheel", make_native(event_mouse_wheel_func, 1, "mouse_wheel"), 0, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("key", make_native(event_key_func, 1, "key"), 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("mouse_x", make_native(event_mouse_x_func, 1, "mouse_x"), 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("mouse_y", make_native(event_mouse_y_func, 1, "mouse_y"), 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("mouse_button", make_native(event_mouse_button_func, 1, "mouse_button"), 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("width", make_native(event_width_func, 1, "width"), 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("height", make_native(event_height_func, 1, "height"), 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("text", make_native(event_text_func, 1, "text"), 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("mod", make_native(event_mod_func, 1, "mod"), 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("scancode", make_native(event_scancode_func, 1, "scancode"), 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("repeat", make_native(event_repeat_func, 1, "repeat"), 0, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("wheel_x", make_native(event_wheel_x_func, 1, "wheel_x"), 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("wheel_y", make_native(event_wheel_y_func, 1, "wheel_y"), 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("xrel", make_native(event_xrel_func, 1, "xrel"), 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("yrel", make_native(event_yrel_func, 1, "yrel"), 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("clicks", make_native(event_clicks_func, 1, "clicks"), 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, no_params);
    event_register_method_with_params("window_id", make_native(event_window_id_func, 1, "window_id"), 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, no_params);
}
