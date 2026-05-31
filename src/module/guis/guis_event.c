/* Leno GUI - Event 事件实例方法
 * 从 guis.c 拆分出来的 Event 方法实现
 */

#include "include/native.h"
#include "include/leno_value.h"
#include "include/leno_vm.h"
#include "leno_guis.h"
#include <string.h>

/* Event 对象：内部持有 ObjDict 存储事件数据，独立类型避免与普通 Dict 冲突 */
typedef struct {
    Object header;
    ObjDict* data;
} ObjGUIEvent;

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
    Value v = dict_get(d, str_copy("type", 4));
    return val_is_int(v) ? val_as_int(v) : 0;
}

/* 辅助：从事件字典中获取整数字段 */
static int event_get_int(Value event_val, const char* key) {
    ObjDict* d = event_get_dict(event_val);
    if (!d) return 0;
    Value v = dict_get(d, str_copy(key, (int)strlen(key)));
    if (val_is_int(v)) return val_as_int(v);
    if (val_is_float(v)) return (int)val_as_double(v);
    if (val_is_num(v)) return (int)val_as_num(v);
    return 0;
}

/* 辅助：从事件字典中获取字符串字段 */
static const char* event_get_string(Value event_val, const char* key) {
    ObjDict* d = event_get_dict(event_val);
    if (!d) return "";
    Value v = dict_get(d, str_copy(key, (int)strlen(key)));
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

/* e.is_quit() - 是否为退出事件 */
static Value event_is_quit_func(int argc, Value* args) {
    (void)argc;
    return val_bool(event_get_type(args[0]) == LENO_GUI_EVT_QUIT);
}

/* e.is_window_close() - 是否为窗口关闭事件 */
static Value event_is_window_close_func(int argc, Value* args) {
    (void)argc;
    return val_bool(event_get_type(args[0]) == LENO_GUI_EVT_WINDOW_CLOSE);
}

/* e.is_window_resize() - 是否为窗口大小改变事件 */
static Value event_is_window_resize_func(int argc, Value* args) {
    (void)argc;
    return val_bool(event_get_type(args[0]) == LENO_GUI_EVT_WINDOW_RESIZE);
}

/* e.is_window_move() - 是否为窗口移动事件 */
static Value event_is_window_move_func(int argc, Value* args) {
    (void)argc;
    return val_bool(event_get_type(args[0]) == LENO_GUI_EVT_WINDOW_MOVE);
}

/* e.is_key_down() - 是否为按键按下事件 */
static Value event_is_key_down_func(int argc, Value* args) {
    (void)argc;
    return val_bool(event_get_type(args[0]) == LENO_GUI_EVT_KEY_DOWN);
}

/* e.is_key_up() - 是否为按键释放事件 */
static Value event_is_key_up_func(int argc, Value* args) {
    (void)argc;
    return val_bool(event_get_type(args[0]) == LENO_GUI_EVT_KEY_UP);
}

/* e.is_text_input() - 是否为文本输入事件 */
static Value event_is_text_input_func(int argc, Value* args) {
    (void)argc;
    return val_bool(event_get_type(args[0]) == LENO_GUI_EVT_TEXT_INPUT);
}

/* e.is_mouse_move() - 是否为鼠标移动事件 */
static Value event_is_mouse_move_func(int argc, Value* args) {
    (void)argc;
    return val_bool(event_get_type(args[0]) == LENO_GUI_EVT_MOUSE_MOVE);
}

/* e.is_mouse_down() - 是否为鼠标按下事件 */
static Value event_is_mouse_down_func(int argc, Value* args) {
    (void)argc;
    return val_bool(event_get_type(args[0]) == LENO_GUI_EVT_MOUSE_DOWN);
}

/* e.is_mouse_up() - 是否为鼠标释放事件 */
static Value event_is_mouse_up_func(int argc, Value* args) {
    (void)argc;
    return val_bool(event_get_type(args[0]) == LENO_GUI_EVT_MOUSE_UP);
}

/* e.is_mouse_wheel() - 是否为鼠标滚轮事件 */
static Value event_is_mouse_wheel_func(int argc, Value* args) {
    (void)argc;
    return val_bool(event_get_type(args[0]) == LENO_GUI_EVT_MOUSE_WHEEL);
}

/* e.key() - 获取按键码 */
static Value event_key_func(int argc, Value* args) {
    (void)argc;
    return val_int(event_get_int(args[0], "key"));
}

/* e.mouse_x() - 获取鼠标 X 坐标 */
static Value event_mouse_x_func(int argc, Value* args) {
    (void)argc;
    return val_int(event_get_int(args[0], "x"));
}

/* e.mouse_y() - 获取鼠标 Y 坐标 */
static Value event_mouse_y_func(int argc, Value* args) {
    (void)argc;
    return val_int(event_get_int(args[0], "y"));
}

/* e.mouse_button() - 获取鼠标按钮 */
static Value event_mouse_button_func(int argc, Value* args) {
    (void)argc;
    return val_int(event_get_int(args[0], "button"));
}

/* e.width() - 获取窗口宽度（resize 事件） */
static Value event_width_func(int argc, Value* args) {
    (void)argc;
    return val_int(event_get_int(args[0], "width"));
}

/* e.height() - 获取窗口高度（resize 事件） */
static Value event_height_func(int argc, Value* args) {
    (void)argc;
    return val_int(event_get_int(args[0], "height"));
}

/* e.text() - 获取输入文本（text_input 事件） */
static Value event_text_func(int argc, Value* args) {
    (void)argc;
    const char* text = event_get_string(args[0], "text");
    return val_obj((Object*)str_copy(text, (int)strlen(text)));
}

/* 注册 Event 实例方法 */
void guis_init_event_methods(void) {
    event_init_methods();

    TypeKind no_params[] = {};

    event_register_method_with_params("type", make_native(event_type_func, 1, "type"), 0, -1, -1, TYPE_INT, no_params);
    event_register_method_with_params("is_quit", make_native(event_is_quit_func, 1, "is_quit"), 0, -1, -1, TYPE_BOOL, no_params);
    event_register_method_with_params("is_window_close", make_native(event_is_window_close_func, 1, "is_window_close"), 0, -1, -1, TYPE_BOOL, no_params);
    event_register_method_with_params("is_window_resize", make_native(event_is_window_resize_func, 1, "is_window_resize"), 0, -1, -1, TYPE_BOOL, no_params);
    event_register_method_with_params("is_window_move", make_native(event_is_window_move_func, 1, "is_window_move"), 0, -1, -1, TYPE_BOOL, no_params);
    event_register_method_with_params("is_key_down", make_native(event_is_key_down_func, 1, "is_key_down"), 0, -1, -1, TYPE_BOOL, no_params);
    event_register_method_with_params("is_key_up", make_native(event_is_key_up_func, 1, "is_key_up"), 0, -1, -1, TYPE_BOOL, no_params);
    event_register_method_with_params("is_text_input", make_native(event_is_text_input_func, 1, "is_text_input"), 0, -1, -1, TYPE_BOOL, no_params);
    event_register_method_with_params("is_mouse_move", make_native(event_is_mouse_move_func, 1, "is_mouse_move"), 0, -1, -1, TYPE_BOOL, no_params);
    event_register_method_with_params("is_mouse_down", make_native(event_is_mouse_down_func, 1, "is_mouse_down"), 0, -1, -1, TYPE_BOOL, no_params);
    event_register_method_with_params("is_mouse_up", make_native(event_is_mouse_up_func, 1, "is_mouse_up"), 0, -1, -1, TYPE_BOOL, no_params);
    event_register_method_with_params("is_mouse_wheel", make_native(event_is_mouse_wheel_func, 1, "is_mouse_wheel"), 0, -1, -1, TYPE_BOOL, no_params);
    event_register_method_with_params("key", make_native(event_key_func, 1, "key"), 0, -1, -1, TYPE_INT, no_params);
    event_register_method_with_params("mouse_x", make_native(event_mouse_x_func, 1, "mouse_x"), 0, -1, -1, TYPE_INT, no_params);
    event_register_method_with_params("mouse_y", make_native(event_mouse_y_func, 1, "mouse_y"), 0, -1, -1, TYPE_INT, no_params);
    event_register_method_with_params("mouse_button", make_native(event_mouse_button_func, 1, "mouse_button"), 0, -1, -1, TYPE_INT, no_params);
    event_register_method_with_params("width", make_native(event_width_func, 1, "width"), 0, -1, -1, TYPE_INT, no_params);
    event_register_method_with_params("height", make_native(event_height_func, 1, "height"), 0, -1, -1, TYPE_INT, no_params);
    event_register_method_with_params("text", make_native(event_text_func, 1, "text"), 0, -1, -1, TYPE_STRING, no_params);
}
