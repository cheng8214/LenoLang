/* Leno GUI - LenoC 模块注册
 * 将平台抽象层连接到 LenoC VM
 *
 * 类型关键字:
 *   Win     - 窗口对象 (OBJ_GUI_WINDOW)
 *   Draw    - 渲染器对象 (OBJ_GUI_RENDERER)
 *   Event   - 事件对象 (OBJ_GUI_EVENT )
 *   Image   - 图像对象 (OBJ_GUI_IMAGE)
 *   Font    - 字体对象 (OBJ_GUI_FONT)
 *   Rgb     - 颜色对象 (_rgb(r, g, b, a?))
 *
 * 模块级 API:
 *   guis.create_window(title, style_dict) -> Win      Style 方式创建窗口
 *   guis.create_renderer(win) -> Draw
 *   guis.destroy_renderer(ren)
 *   guis.resize_renderer(ren, w, h) -> bool    窗口大小改变时调整渲染器
 *   ren.draw_text(text, x, y, size)                       内置 8x8 点阵字体
 *   ren.text_size(text, size) -> [w, h]                   计算文字尺寸
 *   guis.load_font(name, size) -> Font                    加载系统字体
 *   guis.destroy_font(font)                               销毁字体
 *   ren.draw_text_ex(font, text, x, y)                    系统字体渲染
 *   ren.font_size(font, text) -> [w, h]                   系统字体文字尺寸
 *   guis.poll() / wait(timeout_ms)
 *   guis.get_key(key) -> bool
 *   guis.get_mouse() -> {x, y, buttons}
 *   guis.get_clipboard() / set_clipboard(text)
 *   guis.show_cursor(bool)
 *   guis.set_cursor(cursor_type)                          设置系统光标样式
 *   guis.file_dialog(type, callback, opts?)               系统原生文件对话框
 *   guis.msg_box(title, message, type) -> int
 *   guis.get_ticks() / get_perf_counter() / get_perf_freq()
 *   guis.delay(ms)
 *   guis.add_timer(interval_ms, callback) -> timer_id    定时器回调
 *   guis.remove_timer(timer_id) -> bool                  取消定时器
 *   guis.get_display() / get_dpi()
 *   guis.run(win, onDraw, onEvent)          回调式事件循环
 *   ren.set_logical_size(w, h)                           设置逻辑渲染尺寸
 *   ren.get_logical_size() -> [w, h]                     获取逻辑渲染尺寸
 *   ren.set_logical_presentation(mode)                   设置逻辑呈现模式
 *   ren.get_logical_presentation() -> int                获取逻辑呈现模式
 *   ren.get_logical_viewport() -> [x, y, w, h]           获取实际视口区域
 *   ren.reset_logical_size()                             重置逻辑尺寸
 *
 * 图片加载 API:
 *   guis.load_image(path) -> Image                        从文件加载图片
 *   guis.load_image_ex(path, options) -> Image            带选项加载（翻转等）
 *   guis.load_image_from_memory(data) -> Image            从内存加载图片
 *   guis.image_info(path) -> {width, height, channels}    获取图片信息
 *   guis.image_info_from_memory(data) -> {...}            从内存获取图片信息
 *   guis.is_16_bit(path) -> bool                          检查是否为16位图片
 *   guis.is_16_bit_from_memory(data) -> bool              内存检查16位
 *   guis.is_hdr(path) -> bool                             检查是否为HDR
 *   guis.is_hdr_from_memory(data) -> bool                 内存检查HDR
 *
 * zlib解压 API:
 *   guis.zlib_decode(data) -> string                      解压zlib数据
 *   guis.zlib_decode_noheader(data) -> string             解压无头部zlib数据
 */

#include "include/native.h"
#include "include/leno_value.h"
#include "include/leno_vm.h"
#include "include/string_table.h"
#include "leno_guis.h"
#include "leno_guis_log.h"
#include "guis_internal.h"
#include "guis_constants.h"
#include <string.h>



ObjGUIWindow* create_window_obj(LenoGUIPlatformWindow* pw) {
    ObjGUIWindow* obj = (ObjGUIWindow*)gc_alloc(sizeof(ObjGUIWindow), OBJ_GUI_WINDOW);
    if (!obj) return NULL;
    obj->platform = pw;
    return obj;
}

ObjGUIRenderer* create_renderer_obj(LenoGUIPlatformRenderer* pr, ObjGUIWindow* win) {
    ObjGUIRenderer* obj = (ObjGUIRenderer*)gc_alloc(sizeof(ObjGUIRenderer), OBJ_GUI_RENDERER);
    if (!obj) return NULL;
    obj->platform = pr;
    obj->window = win;
    gc_write_barrier_obj((Object*)obj, (Object*)win);
    return obj;
}

ObjGUIWindow* as_window(Value v) {
    if (!val_is_obj(v)) return NULL;
    Object* obj = val_as_obj(v);
    if (obj->type != OBJ_GUI_WINDOW) return NULL;
    return (ObjGUIWindow*)obj;
}

ObjGUIRenderer* as_renderer(Value v) {
    if (!val_is_obj(v)) return NULL;
    Object* obj = val_as_obj(v);
    if (obj->type != OBJ_GUI_RENDERER) return NULL;
    return (ObjGUIRenderer*)obj;
}

ObjGUIFont* as_font(Value v) {
    if (!val_is_obj(v)) return NULL;
    Object* obj = val_as_obj(v);
    if (obj->type != OBJ_GUI_FONT) return NULL;
    return (ObjGUIFont*)obj;
}

ObjGUIImage* as_image(Value v) {
    if (!val_is_obj(v)) return NULL;
    Object* obj = val_as_obj(v);
    if (obj->type != OBJ_GUI_IMAGE) return NULL;
    return (ObjGUIImage*)obj;
}

// 静态字符串键，避免每次事件都创建新字符串导致内存泄漏
ObjString* str_key_type = NULL;
ObjString* str_key_window_id = NULL;
ObjString* str_key_width = NULL;
ObjString* str_key_height = NULL;
ObjString* str_key_x = NULL;
ObjString* str_key_y = NULL;
ObjString* str_key_key = NULL;
ObjString* str_key_scancode = NULL;
ObjString* str_key_mod = NULL;
ObjString* str_key_repeat = NULL;
ObjString* str_key_text = NULL;
ObjString* str_key_xrel = NULL;
ObjString* str_key_yrel = NULL;
ObjString* str_key_button = NULL;
ObjString* str_key_clicks = NULL;
ObjString* str_key_wheel_x = NULL;
ObjString* str_key_wheel_y = NULL;

void init_event_string_keys(void) {
    if (str_key_type) return;  // 已初始化
    str_key_type = str_copy("type", 4);
    str_key_window_id = str_copy("window_id", 9);
    str_key_width = str_copy("width", 5);
    str_key_height = str_copy("height", 6);
    str_key_x = str_copy("x", 1);
    str_key_y = str_copy("y", 1);
    str_key_key = str_copy("key", 3);
    str_key_scancode = str_copy("scancode", 8);
    str_key_mod = str_copy("mod", 3);
    str_key_repeat = str_copy("repeat", 6);
    str_key_text = str_copy("text", 4);
    str_key_xrel = str_copy("xrel", 4);
    str_key_yrel = str_copy("yrel", 4);
    str_key_button = str_copy("button", 6);
    str_key_clicks = str_copy("clicks", 6);
    str_key_wheel_x = str_copy("wheel_x", 7);
    str_key_wheel_y = str_copy("wheel_y", 7);
}

void dict_add_int_key(ObjDict* d, ObjString* key, int value) {
    dict_set(d, key, val_int(value));
}

void dict_add_float_key(ObjDict* d, ObjString* key, float value) {
    dict_set(d, key, val_float((double)value));
}

void dict_add_string_key(ObjDict* d, ObjString* key, const char* value) {
    ObjString* v = str_copy(value, (int)strlen(value));
    dict_set(d, key, val_obj((Object*)v));
}

ObjArray* make_int_array2(int a, int b) {
    ObjArray* arr = arr_new(2);
    arr->count = 2;
    arr_write(arr, 0, val_int(a));
    arr_write(arr, 1, val_int(b));
    return arr;
}

Value call_leno_closure(Value callee, int arg_count, Value* args) {
    VM* vm_ptr = current_exec_vm ? current_exec_vm : &vm;
    int saved_sp = vm_ptr->sp;
    int saved_frame_cnt = vm_ptr->frame_cnt;

    for (int i = 0; i < arg_count; i++) {
        vm_stack_push(vm_ptr, args[i]);
    }
    vm_stack_push(vm_ptr, callee);

    int call_result = vm_call_value(callee, arg_count, 0);
    Value ret_val = vm_ptr->last_return_value;

    if (call_result != 1) {
        vm_ptr->has_exception = 0;
        vm_ptr->exception = val_null();
        vm_ptr->frame_cnt = saved_frame_cnt;
        ret_val = val_null();
    }
    vm_ptr->sp = saved_sp;
    return ret_val;
}

Value event_to_dict(LenoGUIEvent* ev) {
    init_event_string_keys();  // 确保静态字符串键已初始化
    ObjDict* d = dict_new(16);
    ObjGUIEvent* event_obj = (ObjGUIEvent*)gc_alloc(sizeof(ObjGUIEvent), OBJ_GUI_EVENT);
    event_obj->data = d;
    dict_add_int_key(d, str_key_type, ev->type);
    dict_add_int_key(d, str_key_window_id, ev->window_id);

    if (ev->type == LENO_GUI_EVT_WINDOW_RESIZE || ev->type == LENO_GUI_EVT_WINDOW_MINIMIZED || ev->type == LENO_GUI_EVT_WINDOW_MAXIMIZED || ev->type == LENO_GUI_EVT_WINDOW_RESTORED) {
        dict_add_int_key(d, str_key_width, ev->data1);
        dict_add_int_key(d, str_key_height, ev->data2);
    } else if (ev->type == LENO_GUI_EVT_WINDOW_MOVE) {
        dict_add_int_key(d, str_key_x, ev->data1);
        dict_add_int_key(d, str_key_y, ev->data2);
    } else if (ev->type == LENO_GUI_EVT_KEY_DOWN || ev->type == LENO_GUI_EVT_KEY_UP) {
        dict_add_int_key(d, str_key_key, ev->key);
        dict_add_int_key(d, str_key_scancode, ev->scancode);
        dict_add_int_key(d, str_key_mod, ev->mod_flags);
        dict_add_int_key(d, str_key_repeat, ev->repeat);
    } else if (ev->type == LENO_GUI_EVT_TEXT_INPUT) {
        dict_add_string_key(d, str_key_text, ev->text);
    } else if (ev->type == LENO_GUI_EVT_MOUSE_MOVE) {
        dict_add_float_key(d, str_key_x, ev->mouse_x);
        dict_add_float_key(d, str_key_y, ev->mouse_y);
        dict_add_float_key(d, str_key_xrel, ev->mouse_xrel);
        dict_add_float_key(d, str_key_yrel, ev->mouse_yrel);
    } else if (ev->type == LENO_GUI_EVT_MOUSE_DOWN || ev->type == LENO_GUI_EVT_MOUSE_UP) {
        dict_add_float_key(d, str_key_x, ev->mouse_x);
        dict_add_float_key(d, str_key_y, ev->mouse_y);
        dict_add_int_key(d, str_key_button, ev->mouse_button);
        dict_add_int_key(d, str_key_clicks, ev->mouse_clicks);
    } else if (ev->type == LENO_GUI_EVT_MOUSE_WHEEL) {
        dict_add_int_key(d, str_key_x, (int)ev->mouse_x);
        dict_add_int_key(d, str_key_y, (int)ev->mouse_y);
        dict_add_int_key(d, str_key_wheel_x, (int)ev->wheel_x);
        dict_add_int_key(d, str_key_wheel_y, (int)ev->wheel_y);
    } else if (ev->type == LENO_GUI_EVT_DROP_FILE) {
        dict_add_string_key(d, str_key_text, ev->drop_file);
    } else if (ev->type == LENO_GUI_EVT_DROP_TEXT) {
        dict_add_string_key(d, str_key_text, ev->drop_file);
    }

    return val_obj((Object*)event_obj);
}

static Value gui_create_window_func(int argc, Value* args) {
    if (argc != 2) return val_null();

    ObjString* title = (ObjString*)val_as_obj(args[0]);
    int w = 800, h = 600, flags = 0;

    if (val_is_obj(args[1]) && val_as_obj(args[1])->type == OBJ_DICT) {
        ObjDict* style = (ObjDict*)val_as_obj(args[1]);
        ObjString* key_w = str_copy("width", 5);
        ObjString* key_h = str_copy("height", 6);
        ObjString* key_full = str_copy("fullscreen", 10);
        ObjString* key_borderless = str_copy("borderless", 10);
        ObjString* key_resizable = str_copy("resizable", 9);
        ObjString* key_hidden = str_copy("visible", 7);
        ObjString* key_always_on_top = str_copy("always_on_top", 13);

        Value vw = dict_get(style, key_w);
        if (!val_is_null(vw)) w = val_as_int(vw);

        Value vh = dict_get(style, key_h);
        if (!val_is_null(vh)) h = val_as_int(vh);

        Value vfull = dict_get(style, key_full);
        if (!val_is_null(vfull) && val_as_bool(vfull)) {
            flags |= LENO_GUI_WIN_FULLSCREEN;
        }

        Value vborderless = dict_get(style, key_borderless);
        if (!val_is_null(vborderless) && val_as_bool(vborderless)) {
            flags |= LENO_GUI_WIN_BORDERLESS;
        }

        Value vresizable = dict_get(style, key_resizable);
        if (!val_is_null(vresizable) && val_as_bool(vresizable)) {
            flags |= LENO_GUI_WIN_RESIZABLE;
        }

        Value vhidden = dict_get(style, key_hidden);
        if (!val_is_null(vhidden) && !val_as_bool(vhidden)) {
            flags |= LENO_GUI_WIN_HIDDEN;
        }

        Value vtop = dict_get(style, key_always_on_top);
        if (!val_is_null(vtop) && val_as_bool(vtop)) {
            flags |= LENO_GUI_WIN_ALWAYS_ON_TOP;
        }
    }

    if (!leno_gui_platform_init()) return val_null();

    LenoGUIPlatformWindow* pw = leno_gui_platform_create_window(title->chars, w, h, flags);
    if (!pw) return val_null();

    ObjGUIWindow* obj = create_window_obj(pw);
    if (!obj) {
        leno_gui_platform_destroy_window(pw);
        return val_null();
    }
    return val_obj((Object*)obj);
}

static Value gui_create_renderer_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    if (!win || !win->platform) return val_null();

    LenoGUIPlatformRenderer* pr = leno_gui_platform_create_renderer(win->platform);
    if (!pr) return val_null();

    ObjGUIRenderer* obj = create_renderer_obj(pr, win);
    if (!obj) {
        leno_gui_platform_destroy_renderer(pr);
        return val_null();
    }
    return val_obj((Object*)obj);
}

static Value gui_destroy_renderer_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    if (ren && ren->platform) {
        leno_gui_platform_destroy_renderer(ren->platform);
        ren->platform = NULL;
    }
    return val_null();
}

static Value gui_resize_renderer_func(int argc, Value* args) {
    (void)argc;
    ObjGUIRenderer* ren = as_renderer(args[0]);
    int w = val_as_int(args[1]);
    int h = val_as_int(args[2]);
    if (ren && ren->platform) {
        int result = leno_gui_platform_renderer_resize(ren->platform, w, h);
        return val_bool(result != 0);
    }
    return val_bool(false);
}

static Value gui_load_font_func(int argc, Value* args) {
    (void)argc;
    const char* name = "Arial";
    if (val_is_obj(args[0])) {
        Object* obj = val_as_obj(args[0]);
        if (obj->type == OBJ_STRING) name = ((ObjString*)obj)->chars;
    }
    int size = val_as_int(args[1]);
    LenoGUIPlatformFont* pf = leno_gui_platform_load_font(name, size);
    if (!pf) return val_null();
    ObjGUIFont* font = (ObjGUIFont*)gc_alloc(sizeof(ObjGUIFont), OBJ_GUI_FONT);
    if (!font) { leno_gui_platform_destroy_font(pf); return val_null(); }
    font->platform = pf;
    return val_obj((Object*)font);
}

/* ===== 图片加载 ===== */

/* guis.load_image(path) -> Image */
static Value gui_load_image_func(int argc, Value* args) {
    (void)argc;
    const char* path = NULL;
    if (val_is_obj(args[0])) {
        Object* obj = val_as_obj(args[0]);
        if (obj->type == OBJ_STRING) path = ((ObjString*)obj)->chars;
    }
    if (!path) return val_null();

    if (!leno_gui_platform_init()) return val_null();

    LenoGUIPlatformImage* pt = leno_gui_platform_load_image(path);
    if (!pt) {
        /* 加载失败，打印错误信息 */
        const char* err = leno_gui_platform_get_image_error();
        if (err && err[0]) {
            leno_gui_log_error("load_image 失败: %s", err);
        }
        return val_null();
    }

    ObjGUIImage* tex = (ObjGUIImage*)gc_alloc(sizeof(ObjGUIImage), OBJ_GUI_IMAGE);
    if (!tex) {
        leno_gui_platform_destroy_image(pt);
        return val_null();
    }
    tex->platform = pt;
    return val_obj((Object*)tex);
}

/* guis.image_info(path) -> {width: int, height: int, channels: int} | null */
static Value gui_image_info_func(int argc, Value* args) {
    (void)argc;
    const char* path = NULL;
    if (val_is_obj(args[0])) {
        Object* obj = val_as_obj(args[0]);
        if (obj->type == OBJ_STRING) path = ((ObjString*)obj)->chars;
    }
    if (!path) return val_null();

    int w = 0, h = 0, channels = 0;
    if (!leno_gui_platform_get_image_info(path, &w, &h, &channels)) {
        return val_null();
    }

    /* 返回字典 {width: w, height: h, channels: channels} */
    ObjDict* dict = dict_new(8);
    if (!dict) return val_null();

    dict_add_int_key(dict, intern_string("width", 5), w);
    dict_add_int_key(dict, intern_string("height", 6), h);
    dict_add_int_key(dict, intern_string("channels", 8), channels);

    return val_obj((Object*)dict);
}

/* guis.load_image_ex(path, options) -> Image */
/* options: {flip_vertical: bool} */
static Value gui_load_image_ex_func(int argc, Value* args) {
    (void)argc;
    const char* path = NULL;
    if (val_is_obj(args[0])) {
        Object* obj = val_as_obj(args[0]);
        if (obj->type == OBJ_STRING) path = ((ObjString*)obj)->chars;
    }
    if (!path) return val_null();

    /* 解析 options 字典 */
    int flip_vertical = 0;
    if (val_is_obj(args[1])) {
        Object* obj = val_as_obj(args[1]);
        if (obj->type == OBJ_DICT) {
            ObjDict* opts = (ObjDict*)obj;
            ObjString* key = intern_string("flip_vertical", 13);
            Value flip_val = dict_get(opts, key);
            flip_vertical = val_is_truthy(flip_val);
        }
    }

    if (!leno_gui_platform_init()) return val_null();

    /* 设置翻转选项 */
    leno_gui_platform_set_flip_vertically_on_load(flip_vertical);

    LenoGUIPlatformImage* pt = leno_gui_platform_load_image(path);

    /* 重置为默认值 */
    leno_gui_platform_set_flip_vertically_on_load(0);

    if (!pt) {
        const char* err = leno_gui_platform_get_image_error();
        if (err && err[0]) {
            leno_gui_log_error("load_image_ex 失败: %s", err);
        }
        return val_null();
    }

    ObjGUIImage* tex = (ObjGUIImage*)gc_alloc(sizeof(ObjGUIImage), OBJ_GUI_IMAGE);
    if (!tex) {
        leno_gui_platform_destroy_image(pt);
        return val_null();
    }
    tex->platform = pt;
    return val_obj((Object*)tex);
}

/* guis.load_image_from_memory(data) -> Image */
/* data: 字符串（二进制字节数据） */
static Value gui_load_image_from_memory_func(int argc, Value* args) {
    (void)argc;
    const char* data = NULL;
    int len = 0;
    if (val_is_obj(args[0])) {
        Object* obj = val_as_obj(args[0]);
        if (obj->type == OBJ_STRING) {
            data = ((ObjString*)obj)->chars;
            len = ((ObjString*)obj)->len;
        }
    }
    if (!data || len <= 0) return val_null();

    if (!leno_gui_platform_init()) return val_null();

    LenoGUIPlatformImage* pt = leno_gui_platform_load_image_mem((const unsigned char*)data, len);
    if (!pt) {
        const char* err = leno_gui_platform_get_image_error();
        if (err && err[0]) {
            leno_gui_log_error("load_image_from_memory 失败: %s", err);
        }
        return val_null();
    }

    ObjGUIImage* tex = (ObjGUIImage*)gc_alloc(sizeof(ObjGUIImage), OBJ_GUI_IMAGE);
    if (!tex) {
        leno_gui_platform_destroy_image(pt);
        return val_null();
    }
    tex->platform = pt;
    return val_obj((Object*)tex);
}

/* guis.image_info_from_memory(data) -> {width: int, height: int, channels: int} | null */
/* data: 字符串（二进制字节数据） */
static Value gui_image_info_from_memory_func(int argc, Value* args) {
    (void)argc;
    const char* data = NULL;
    int len = 0;
    if (val_is_obj(args[0])) {
        Object* obj = val_as_obj(args[0]);
        if (obj->type == OBJ_STRING) {
            data = ((ObjString*)obj)->chars;
            len = ((ObjString*)obj)->len;
        }
    }
    if (!data || len <= 0) return val_null();

    int w = 0, h = 0, channels = 0;
    if (!leno_gui_platform_get_image_info_mem((const unsigned char*)data, len, &w, &h, &channels)) {
        return val_null();
    }

    /* 返回字典 {width: w, height: h, channels: channels} */
    ObjDict* dict = dict_new(8);
    if (!dict) return val_null();

    dict_add_int_key(dict, intern_string("width", 5), w);
    dict_add_int_key(dict, intern_string("height", 6), h);
    dict_add_int_key(dict, intern_string("channels", 8), channels);

    return val_obj((Object*)dict);
}

/* guis.is_16_bit(path) -> bool */
static Value gui_is_16_bit_func(int argc, Value* args) {
    (void)argc;
    const char* path = NULL;
    if (val_is_obj(args[0])) {
        Object* obj = val_as_obj(args[0]);
        if (obj->type == OBJ_STRING) path = ((ObjString*)obj)->chars;
    }
    if (!path) return val_bool(0);
    return val_bool(leno_gui_platform_is_16_bit(path));
}

/* guis.is_16_bit_from_memory(data) -> bool */
static Value gui_is_16_bit_from_memory_func(int argc, Value* args) {
    (void)argc;
    const char* data = NULL;
    int len = 0;
    if (val_is_obj(args[0])) {
        Object* obj = val_as_obj(args[0]);
        if (obj->type == OBJ_STRING) {
            data = ((ObjString*)obj)->chars;
            len = ((ObjString*)obj)->len;
        }
    }
    if (!data || len <= 0) return val_bool(0);
    return val_bool(leno_gui_platform_is_16_bit_from_memory((const unsigned char*)data, len));
}

/* guis.is_hdr(path) -> bool */
static Value gui_is_hdr_func(int argc, Value* args) {
    (void)argc;
    const char* path = NULL;
    if (val_is_obj(args[0])) {
        Object* obj = val_as_obj(args[0]);
        if (obj->type == OBJ_STRING) path = ((ObjString*)obj)->chars;
    }
    if (!path) return val_bool(0);
    return val_bool(leno_gui_platform_is_hdr(path));
}

/* guis.is_hdr_from_memory(data) -> bool */
static Value gui_is_hdr_from_memory_func(int argc, Value* args) {
    (void)argc;
    const char* data = NULL;
    int len = 0;
    if (val_is_obj(args[0])) {
        Object* obj = val_as_obj(args[0]);
        if (obj->type == OBJ_STRING) {
            data = ((ObjString*)obj)->chars;
            len = ((ObjString*)obj)->len;
        }
    }
    if (!data || len <= 0) return val_bool(0);
    return val_bool(leno_gui_platform_is_hdr_from_memory((const unsigned char*)data, len));
}

/* guis.zlib_decode(data) -> string | null */
/* 解压 zlib 压缩的数据 */
static Value gui_zlib_decode_func(int argc, Value* args) {
    (void)argc;
    const char* data = NULL;
    int len = 0;
    if (val_is_obj(args[0])) {
        Object* obj = val_as_obj(args[0]);
        if (obj->type == OBJ_STRING) {
            data = ((ObjString*)obj)->chars;
            len = ((ObjString*)obj)->len;
        }
    }
    if (!data || len <= 0) return val_null();

    int outlen = 0;
    char* result = leno_gui_platform_zlib_decode_malloc(data, len, &outlen);
    if (!result) return val_null();

    ObjString* str = str_copy(result, outlen);
    free(result);
    if (!str) return val_null();
    return val_obj((Object*)str);
}

/* guis.zlib_decode_noheader(data) -> string | null */
/* 解压无头部的 zlib 压缩数据 */
static Value gui_zlib_decode_noheader_func(int argc, Value* args) {
    (void)argc;
    const char* data = NULL;
    int len = 0;
    if (val_is_obj(args[0])) {
        Object* obj = val_as_obj(args[0]);
        if (obj->type == OBJ_STRING) {
            data = ((ObjString*)obj)->chars;
            len = ((ObjString*)obj)->len;
        }
    }
    if (!data || len <= 0) return val_null();

    int outlen = 0;
    char* result = leno_gui_platform_zlib_decode_noheader_malloc(data, len, &outlen);
    if (!result) return val_null();

    ObjString* str = str_copy(result, outlen);
    free(result);
    if (!str) return val_null();
    return val_obj((Object*)str);
}

static Value gui_poll_event_func(int argc, Value* args) {
    (void)argc; (void)args;
    LenoGUIEvent ev;
    if (!leno_gui_platform_poll_event(&ev)) return val_null();
    return event_to_dict(&ev);
}

static Value gui_wait_event_func(int argc, Value* args) {
    (void)argc;
    int timeout_ms = val_as_int(args[0]);
    LenoGUIEvent ev;
    if (!leno_gui_platform_wait_event(&ev, timeout_ms)) return val_null();
    return event_to_dict(&ev);
}

static Value gui_get_display_size_func(int argc, Value* args) {
    (void)argc; (void)args;
    int w = 0, h = 0;
    leno_gui_platform_get_display_size(&w, &h);
    return val_obj((Object*)make_int_array2(w, h));
}

/* ===== 输入状态查询 ===== */

/* 查询指定按键是否按下 */
static Value gui_get_key_state_func(int argc, Value* args) {
    (void)argc;
    int key = val_as_int(args[0]);
    return val_bool(leno_gui_platform_get_key_state(key) != 0);
}

/* 检查按键是否刚被按下（本帧按下的瞬间） */
static Value gui_is_key_pressed_func(int argc, Value* args) {
    (void)argc;
    int key = val_as_int(args[0]);
    return val_bool(leno_gui_platform_is_key_pressed(key) != 0);
}

/* 检查按键是否刚被释放（本帧释放的瞬间） */
static Value gui_is_key_released_func(int argc, Value* args) {
    (void)argc;
    int key = val_as_int(args[0]);
    return val_bool(leno_gui_platform_is_key_released(key) != 0);
}

/* 开始接收文本输入事件 */
static Value gui_start_text_input_func(int argc, Value* args) {
    (void)argc; (void)args;
    leno_gui_platform_start_text_input();
    return val_null();
}

/* 停止接收文本输入事件 */
static Value gui_stop_text_input_func(int argc, Value* args) {
    (void)argc; (void)args;
    leno_gui_platform_stop_text_input();
    return val_null();
}

/* 检查是否正在接收文本输入 */
static Value gui_is_text_input_active_func(int argc, Value* args) {
    (void)argc; (void)args;
    return val_bool(leno_gui_platform_is_text_input_active() != 0);
}

/* 查询鼠标状态 */
static Value gui_get_mouse_state_func(int argc, Value* args) {
    (void)argc; (void)args;
    int x = 0, y = 0, buttons = 0;
    leno_gui_platform_get_mouse_state(&x, &y, &buttons);
    ObjDict* d = dict_new(8);
    init_event_string_keys();
    dict_add_int_key(d, str_key_x, x);
    dict_add_int_key(d, str_key_y, y);
    dict_add_int_key(d, str_key_button, buttons);
    return val_obj((Object*)d);
}

/* ===== 剪贴板 ===== */

/* 获取剪贴板文本 */
static Value gui_get_clipboard_text_func(int argc, Value* args) {
    (void)argc; (void)args;
    char* text = leno_gui_platform_get_clipboard_text();
    if (!text) return val_null();
    ObjString* str = str_copy(text, (int)strlen(text));
    free(text);
    return val_obj((Object*)str);
}

/* 设置剪贴板文本 */
static Value gui_set_clipboard_text_func(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    if (str) leno_gui_platform_set_clipboard_text(str->chars);
    return val_null();
}

/* ===== 光标控制 ===== */

/* 显示或隐藏光标 */
static Value gui_show_cursor_func(int argc, Value* args) {
    (void)argc;
    int show = val_as_bool(args[0]) ? 1 : 0;
    leno_gui_platform_show_cursor(show);
    return val_null();
}

/* 设置系统光标 */
static Value gui_set_cursor_func(int argc, Value* args) {
    (void)argc;
    int cursor_type = val_as_int(args[0]);
    leno_gui_platform_set_system_cursor(cursor_type);
    return val_null();
}

/* ===== 消息框 ===== */

/* 显示系统消息框 */
static Value gui_msg_box_func(int argc, Value* args) {
    (void)argc;
    ObjString* title = (ObjString*)val_as_obj(args[0]);
    ObjString* message = (ObjString*)val_as_obj(args[1]);
    int type = val_as_int(args[2]);
    int result = leno_gui_platform_show_message_box(title->chars, message->chars, type);
    return val_int(result);
}

/* ===== 文件对话框 ===== */

/* 文件对话框回调状态 */
typedef struct {
    Value callback;     /* Leno 回调函数 */
    int is_active;
} FileDialogCbState;

static FileDialogCbState g_filedlg_cb = { 0, 0 };  /* 零初始化，is_active=0 保证不被使用 */

/* 文件对话框回调处理（必须在主线程中调用） */
void process_filedialog_callback(const char* const* files, int nfiles, int filter_index) {
    if (!g_filedlg_cb.is_active || val_is_null(g_filedlg_cb.callback)) return;

    /* 构建文件列表数组 */
    ObjArray* arr = arr_new(8);
    if (files && arr) {
        for (int i = 0; i < nfiles; i++) {
            ObjString* s = str_copy(files[i], (int)strlen(files[i]));
            while (arr->count >= arr->capacity) {
                if (!arr_grow(arr)) break;
            }
            arr_write(arr, arr->count, val_obj((Object*)s));
        }
    }

    Value args[2];
    args[0] = val_obj((Object*)arr);
    args[1] = val_int(filter_index);
    call_leno_closure(g_filedlg_cb.callback, 2, args);

    g_filedlg_cb.is_active = 0;
    g_filedlg_cb.callback = val_null();
}

/* 显示文件对话框 */
static Value gui_file_dialog_func(int argc, Value* args) {
    (void)argc;
    int type = val_as_int(args[0]);      /* 对话框类型 */
    Value callback_val = args[1];        /* 回调函数 */

    /* 可选参数：过滤器、默认路径等 */
    LenoGUIFileFilter* filters = NULL;
    int nfilters = 0;
    const char* default_path = NULL;
    int allow_many = 0;
    const char* title = NULL;
    LenoGUIPlatformWindow* win = NULL;

    if (argc >= 3 && val_is_obj(args[2])) {
        ObjDict* opts = (ObjDict*)val_as_obj(args[2]);
        if (opts && opts->header.type == OBJ_DICT) {
            /* 读取 title */
            ObjString* key_title = str_copy("title", 5);
            Value vtitle = dict_get(opts, key_title);
            if (!val_is_null(vtitle) && val_is_obj(vtitle)) {
                Object* obj = val_as_obj(vtitle);
                if (obj->type == OBJ_STRING) title = ((ObjString*)obj)->chars;
            }

            /* 读取 default_path */
            ObjString* key_path = str_copy("path", 4);
            Value vpath = dict_get(opts, key_path);
            if (!val_is_null(vpath) && val_is_obj(vpath)) {
                Object* obj = val_as_obj(vpath);
                if (obj->type == OBJ_STRING) default_path = ((ObjString*)obj)->chars;
            }

            /* 读取 allow_many */
            ObjString* key_many = str_copy("multiple", 8);
            Value vmany = dict_get(opts, key_many);
            if (!val_is_null(vmany) && val_as_bool(vmany)) allow_many = 1;

            /* 读取 filters */
            ObjString* key_filters = str_copy("filters", 7);
            Value vfilters = dict_get(opts, key_filters);
            if (!val_is_null(vfilters) && val_is_obj(vfilters)) {
                Object* obj = val_as_obj(vfilters);
                if (obj->type == OBJ_ARRAY) {
                    ObjArray* fa = (ObjArray*)obj;
                    nfilters = fa->count;
                    filters = (LenoGUIFileFilter*)malloc(nfilters * sizeof(LenoGUIFileFilter));
                    if (filters) {
                        memset(filters, 0, nfilters * sizeof(LenoGUIFileFilter));
                        for (int i = 0; i < nfilters; i++) {
                            Value vf = fa->elements[i];
                            if (val_is_obj(vf) && val_as_obj(vf)->type == OBJ_DICT) {
                                ObjDict* fd = (ObjDict*)val_as_obj(vf);
                                ObjString* kn = str_copy("name", 4);
                                ObjString* kp = str_copy("pattern", 7);
                                Value vn = dict_get(fd, kn);
                                Value vp = dict_get(fd, kp);
                                if (!val_is_null(vn) && val_is_obj(vn)) {
                                    filters[i].name = ((ObjString*)val_as_obj(vn))->chars;
                                }
                                if (!val_is_null(vp) && val_is_obj(vp)) {
                                    filters[i].pattern = ((ObjString*)val_as_obj(vp))->chars;
                                }
                            }
                        }
                    }
                }
            }

            /* 读取 window（可选） */
            ObjString* key_win = str_copy("window", 6);
            Value vwin = dict_get(opts, key_win);
            if (!val_is_null(vwin)) {
                win = as_window(vwin)->platform;
            }
        }
    }

    /* 保存回调 */
    g_filedlg_cb.callback = callback_val;
    g_filedlg_cb.is_active = 1;

    /* 显示对话框（回调通过事件队列投递到主线程） */
    leno_gui_platform_show_file_dialog(type, NULL, NULL,
                                        win, filters, nfilters,
                                        default_path, allow_many, title);

    if (filters) free(filters);
    return val_null();
}

/* 标记文件对话框回调 */
void guis_mark_filedlg_callback(void) {
    if (g_filedlg_cb.is_active) {
        gc_mark_value(g_filedlg_cb.callback);
    }
}

/* ===== 定时器回调 ===== */

#define LENO_GUI_MAX_TIMERS 64

typedef struct {
    int timer_id;
    uint32_t interval_ms;
    uint64_t next_fire;
    Value callback;
    int active;
} LenoGUITimer;

static LenoGUITimer g_timers[LENO_GUI_MAX_TIMERS];
static int g_next_timer_id = 1;
static int g_timers_initialized = 0;

static void init_timers(void) {
    if (!g_timers_initialized) {
        memset(g_timers, 0, sizeof(g_timers));
        g_timers_initialized = 1;
    }
}

static int process_timers(void) {
    if (!g_timers_initialized) return 0;
    int any_fired = 0;
    uint64_t now = leno_gui_platform_get_ticks();
    for (int i = 0; i < LENO_GUI_MAX_TIMERS; i++) {
        LenoGUITimer* t = &g_timers[i];
        if (!t->active || val_is_null(t->callback)) continue;
        if (now < t->next_fire) continue;

        any_fired = 1;
        Value args[2];
        args[0] = val_int(t->timer_id);
        args[1] = val_int(t->interval_ms);
        Value ret = call_leno_closure(t->callback, 2, args);

        int64_t next_interval = val_is_int(ret) ? val_as_int(ret) : 0;
        if (next_interval <= 0) {
            t->active = 0;
            t->callback = val_null();
        } else {
            t->interval_ms = (uint32_t)next_interval;
            t->next_fire = now + t->interval_ms;
        }
    }
    return any_fired;
}

static void clear_all_timers(void) {
    if (!g_timers_initialized) return;
    for (int i = 0; i < LENO_GUI_MAX_TIMERS; i++) {
        g_timers[i].active = 0;
        g_timers[i].callback = val_null();
    }
    g_next_timer_id = 1;
}

void guis_mark_extra_roots(void) {
    if (g_timers_initialized) {
        for (int i = 0; i < LENO_GUI_MAX_TIMERS; i++) {
            if (g_timers[i].active) {
                gc_mark_value(g_timers[i].callback);
            }
        }
    }
    if (g_filedlg_cb.is_active) {
        gc_mark_value(g_filedlg_cb.callback);
    }
}

void guis_mark_renderer_refs(Object* obj) {
    ObjGUIRenderer* ren = (ObjGUIRenderer*)obj;
    if (ren->window) gc_mark_object((Object*)ren->window);
}

static Value gui_add_timer_func(int argc, Value* args) {
    (void)argc;
    init_timers();
    uint32_t interval_ms = (uint32_t)val_as_int(args[0]);
    Value callback = args[1];

    int slot = -1;
    for (int i = 0; i < LENO_GUI_MAX_TIMERS; i++) {
        if (!g_timers[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return val_int(0);

    int id = g_next_timer_id++;
    g_timers[slot].timer_id = id;
    g_timers[slot].interval_ms = interval_ms;
    g_timers[slot].next_fire = leno_gui_platform_get_ticks() + interval_ms;
    g_timers[slot].callback = callback;
    g_timers[slot].active = 1;

    return val_int(id);
}

static Value gui_remove_timer_func(int argc, Value* args) {
    (void)argc;
    int id = val_as_int(args[0]);
    if (!g_timers_initialized) return val_bool(false);
    for (int i = 0; i < LENO_GUI_MAX_TIMERS; i++) {
        if (g_timers[i].active && g_timers[i].timer_id == id) {
            g_timers[i].active = 0;
            g_timers[i].callback = val_null();
            return val_bool(true);
        }
    }
    return val_bool(false);
}

/* ===== 高精度计时器 ===== */

/* 获取毫秒数 */
static Value gui_get_ticks_func(int argc, Value* args) {
    (void)argc; (void)args;
    return val_int((int64_t)leno_gui_platform_get_ticks());
}

/* 获取性能计数器值 */
static Value gui_get_performance_counter_func(int argc, Value* args) {
    (void)argc; (void)args;
    return val_int((int64_t)leno_gui_platform_get_performance_counter());
}

/* 获取性能计数器频率 */
static Value gui_get_performance_frequency_func(int argc, Value* args) {
    (void)argc; (void)args;
    return val_int((int64_t)leno_gui_platform_get_performance_frequency());
}

/* 延迟指定毫秒 */
static Value gui_delay_func(int argc, Value* args) {
    (void)argc;
    uint32_t ms = (uint32_t)val_as_int(args[0]);
    leno_gui_platform_delay(ms);
    return val_null();
}

/* ===== 显示器 DPI ===== */

/* 获取显示器 DPI */
static Value gui_get_display_dpi_func(int argc, Value* args) {
    (void)argc; (void)args;
    return val_float((double)leno_gui_platform_get_display_dpi());
}

/* 全局运行状态，用于模态循环回调 */
typedef struct {
    Value on_draw;
    Value on_event;
    Value ren_val;
    Value win_val;
    ObjGUIRenderer* ren_obj;
} LenoGUIRunState;

/* C 语言渲染回调包装器 */
static void leno_gui_render_callback(void* user_data) {
    LenoGUIRunState* state = (LenoGUIRunState*)user_data;
    if (!val_is_null(state->on_draw)) {
        call_leno_closure(state->on_draw, 1, &state->ren_val);
    }
}

/* C 语言事件回调包装器 */
static void leno_gui_event_callback(void* user_data, LenoGUIEvent* ev) {
    LenoGUIRunState* state = (LenoGUIRunState*)user_data;
    if (!val_is_null(state->on_event)) {
        Value event_dict = event_to_dict(ev);
        call_leno_closure(state->on_event, 1, &event_dict);
    }
}

static Value gui_run_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    if (!win || !win->platform) return val_null();

    Value on_draw = args[1];
    Value on_event = args[2];

    LenoGUIPlatformRenderer* pr = leno_gui_platform_create_renderer(win->platform);
    if (!pr) return val_null();

    ObjGUIRenderer* ren_obj = create_renderer_obj(pr, win);
    if (!ren_obj) {
        leno_gui_platform_destroy_renderer(pr);
        return val_null();
    }

    Value ren_val = val_obj((Object*)ren_obj);
    Value win_val = args[0];

    /* 设置全局回调状态（用于模态循环） */
    LenoGUIRunState run_state = {
        .on_draw = on_draw,
        .on_event = on_event,
        .ren_val = ren_val,
        .win_val = win_val,
        .ren_obj = ren_obj
    };
    leno_gui_platform_set_main_callbacks(win->platform, pr,
                                          leno_gui_render_callback,
                                          leno_gui_event_callback,
                                          &run_state);

    gc_push_root(&run_state.on_draw);
    gc_push_root(&run_state.on_event);
    gc_push_root(&run_state.ren_val);
    gc_push_root(&run_state.win_val);

    /* 使用迭代回调机制（参考 SDL3） */
    int frame_count = 0;
    while (1) {
        if (process_timers()) {
            leno_gui_platform_request_redraw();
        }

        /* 每帧请求重绘，确保流畅渲染 */
        leno_gui_platform_request_redraw();

        if (!leno_gui_platform_iterate_main_callbacks()) break;

        /* 每 60 帧触发一次 GC，防止事件循环中内存泄漏 */
        frame_count++;
        if (frame_count >= 60) {
            frame_count = 0;
            gc_collect();
        }

#ifdef _WIN32
        Sleep(16);
#else
        struct timespec ts = {0, 16000000};
        nanosleep(&ts, NULL);
#endif
    }

    gc_pop_root();
    gc_pop_root();
    gc_pop_root();
    gc_pop_root();

    clear_all_timers();

    leno_gui_platform_destroy_renderer(pr);
    ren_obj->platform = NULL;

    return val_null();
}

/* _rgb(r, g, b, a?) -> Rgb 颜色对象 */
static Value gui_rgb_func(int argc, Value* args) {
    uint8_t r = (uint8_t)val_as_int(args[0]);
    uint8_t g = (uint8_t)val_as_int(args[1]);
    uint8_t b = (uint8_t)val_as_int(args[2]);
    uint8_t a = (argc >= 4) ? (uint8_t)val_as_int(args[3]) : 255;
    ObjRgb* rgb = (ObjRgb*)gc_alloc(sizeof(ObjRgb), OBJ_RGB);
    if (!rgb) return val_null();
    rgb->r = r;
    rgb->g = g;
    rgb->b = b;
    rgb->a = a;
    return val_obj((Object*)rgb);
}

/* 辅助：从 Value 获取 C 字符串 */
static const char* value_to_cstr(Value v) {
    if (val_is_obj(v) && val_as_obj(v)->type == OBJ_STRING) {
        return ((ObjString*)val_as_obj(v))->chars;
    }
    return "";
}

/* ===== 日志系统 Native Wrappers ===== */

/* guis.log_set_priority(category, priority) */
static Value gui_log_set_priority_func(int argc, Value* args) {
    (void)argc;
    int category = (int)val_as_int(args[0]);
    int priority = (int)val_as_int(args[1]);
    leno_gui_log_set_priority(category, priority);
    return val_null();
}

/* guis.log_set_all_priority(priority) */
static Value gui_log_set_all_priority_func(int argc, Value* args) {
    (void)argc;
    int priority = (int)val_as_int(args[0]);
    leno_gui_log_set_all_priority(priority);
    return val_null();
}

/* guis.log_get_priority(category) -> int */
static Value gui_log_get_priority_func(int argc, Value* args) {
    (void)argc;
    int category = (int)val_as_int(args[0]);
    return val_int(leno_gui_log_get_priority(category));
}

/* guis.log_trace(msg) */
static Value gui_log_trace_func(int argc, Value* args) {
    (void)argc;
    leno_gui_log_trace("%s", value_to_cstr(args[0]));
    return val_null();
}

/* guis.log_debug(msg) */
static Value gui_log_debug_func(int argc, Value* args) {
    (void)argc;
    leno_gui_log_debug("%s", value_to_cstr(args[0]));
    return val_null();
}

/* guis.log_info(msg) */
static Value gui_log_info_func(int argc, Value* args) {
    (void)argc;
    leno_gui_log_info("%s", value_to_cstr(args[0]));
    return val_null();
}

/* guis.log_warn(msg) */
static Value gui_log_warn_func(int argc, Value* args) {
    (void)argc;
    leno_gui_log_warn("%s", value_to_cstr(args[0]));
    return val_null();
}

/* guis.log_error(msg) */
static Value gui_log_error_func(int argc, Value* args) {
    (void)argc;
    leno_gui_log_error("%s", value_to_cstr(args[0]));
    return val_null();
}

/* guis.log_critical(msg) */
static Value gui_log_critical_func(int argc, Value* args) {
    (void)argc;
    leno_gui_log_critical("%s", value_to_cstr(args[0]));
    return val_null();
}

/* 前向声明：Draw / Win / Image 实例方法注册 */
void guis_init_instance_methods(void);

/* 外部声明：Win / Image / Font 实例方法注册 */
extern void guis_init_window_instance_methods(void);
extern void guis_init_image_instance_methods(void);
extern void guis_init_font_instance_methods(void);

void guis_init_module(void) {
    TypeKind no_params[] = {};
    TypeKind obj_1int[] = {TYPE_ANY, TYPE_INT};
    TypeKind obj_2int[] = {TYPE_ANY, TYPE_INT, TYPE_INT};
    TypeKind obj_2func[] = {TYPE_ANY, TYPE_ANY, TYPE_ANY};
    TypeKind int_params[] = {TYPE_INT};
    TypeKind int_2int[] = {TYPE_INT, TYPE_INT};
    TypeKind str_2int[] = {TYPE_STRING, TYPE_STRING, TYPE_INT};
    TypeKind str_int[] = {TYPE_STRING, TYPE_INT};
    TypeKind bool_params[] = {TYPE_BOOL};
    TypeKind str_params[] = {TYPE_STRING};

    /* ===== 窗口操作 ===== */
    native_register_module_method("guis", "create_window", gui_create_window_func, -1, 2, 4, TYPE_WIN, TYPE_UNKNOWN, NULL);

    /* ===== 渲染器操作（工厂/析构） ===== */
    native_register_module_method("guis", "create_renderer", gui_create_renderer_func, 1, -1, -1, TYPE_DRAW, TYPE_UNKNOWN, obj_1int);
    native_register_module_method("guis", "destroy_renderer", gui_destroy_renderer_func, 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, obj_1int);
    native_register_module_method("guis", "resize_renderer", gui_resize_renderer_func, 3, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, obj_2int);

    /* ===== 事件操作 ===== */
    native_register_module_method("guis", "poll", gui_poll_event_func, 0, -1, -1, TYPE_ANY, TYPE_UNKNOWN, no_params);
    native_register_module_method("guis", "wait", gui_wait_event_func, 1, -1, -1, TYPE_ANY, TYPE_UNKNOWN, int_params);

    /* ===== 输入状态查询 ===== */
    native_register_module_method("guis", "get_key", gui_get_key_state_func, 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, int_params);
    native_register_module_method("guis", "is_key_pressed", gui_is_key_pressed_func, 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, int_params);
    native_register_module_method("guis", "is_key_released", gui_is_key_released_func, 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, int_params);
    native_register_module_method("guis", "get_mouse", gui_get_mouse_state_func, 0, -1, -1, TYPE_ANY, TYPE_UNKNOWN, no_params);

    /* ===== 文本输入控制 ===== */
    native_register_module_method("guis", "start_text_input", gui_start_text_input_func, 0, -1, -1, TYPE_NULL, TYPE_UNKNOWN, no_params);
    native_register_module_method("guis", "stop_text_input", gui_stop_text_input_func, 0, -1, -1, TYPE_NULL, TYPE_UNKNOWN, no_params);
    native_register_module_method("guis", "is_text_input_active", gui_is_text_input_active_func, 0, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, no_params);

    /* ===== 剪贴板 ===== */
    native_register_module_method("guis", "get_clipboard", gui_get_clipboard_text_func, 0, -1, -1, TYPE_ANY, TYPE_UNKNOWN, no_params);
    native_register_module_method("guis", "set_clipboard", gui_set_clipboard_text_func, 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, str_params);

    /* ===== 光标控制 ===== */
    native_register_module_method("guis", "show_cursor", gui_show_cursor_func, 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, bool_params);
    native_register_module_method("guis", "set_cursor", gui_set_cursor_func, 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_params);

    /* ===== 文件对话框 ===== */
    TypeKind int_func_dict[] = {TYPE_INT, TYPE_ANY, TYPE_ANY};
    native_register_module_method("guis", "file_dialog", gui_file_dialog_func, -1, 2, 3, TYPE_NULL, TYPE_UNKNOWN, int_func_dict);

    /* ===== 消息框 ===== */
    native_register_module_method("guis", "msg_box", gui_msg_box_func, 3, -1, -1, TYPE_INT, TYPE_UNKNOWN, str_2int);

    /* ===== 高精度计时器 ===== */
    native_register_module_method("guis", "get_ticks", gui_get_ticks_func, 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, no_params);
    native_register_module_method("guis", "get_perf_counter", gui_get_performance_counter_func, 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, no_params);
    native_register_module_method("guis", "get_perf_freq", gui_get_performance_frequency_func, 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, no_params);
    native_register_module_method("guis", "delay", gui_delay_func, 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_params);

    /* ===== 定时器回调 ===== */
    TypeKind int_func[] = {TYPE_INT, TYPE_ANY};
    native_register_module_method("guis", "add_timer", gui_add_timer_func, 2, -1, -1, TYPE_INT, TYPE_UNKNOWN, int_func);
    native_register_module_method("guis", "remove_timer", gui_remove_timer_func, 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, int_params);

    /* ===== 字体操作 ===== */
    native_register_module_method("guis", "load_font", gui_load_font_func, 2, -1, -1, TYPE_FONT, TYPE_UNKNOWN, str_int);

    /* ===== 图片加载 ===== */
    native_register_module_method("guis", "load_image", gui_load_image_func, 1, -1, -1, TYPE_IMAGE, TYPE_UNKNOWN, str_params);

    /* guis.image_info(path) -> {width: int, height: int, channels: int} */
    native_register_module_method("guis", "image_info", gui_image_info_func, 1, -1, -1, TYPE_ANY, TYPE_UNKNOWN, str_params);

    /* guis.load_image_ex(path, options) -> Image */
    TypeKind str_dict_params[] = {TYPE_STRING, TYPE_ANY};
    native_register_module_method("guis", "load_image_ex", gui_load_image_ex_func, 2, -1, -1, TYPE_IMAGE, TYPE_UNKNOWN, str_dict_params);

    /* guis.load_image_from_memory(data) -> Image */
    native_register_module_method("guis", "load_image_from_memory", gui_load_image_from_memory_func, 1, -1, -1, TYPE_IMAGE, TYPE_UNKNOWN, str_params);

    /* guis.image_info_from_memory(data) -> {width, height, channels} */
    native_register_module_method("guis", "image_info_from_memory", gui_image_info_from_memory_func, 1, -1, -1, TYPE_ANY, TYPE_UNKNOWN, str_params);

    /* ===== 图片格式检测 ===== */
    native_register_module_method("guis", "is_16_bit", gui_is_16_bit_func, 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, str_params);
    native_register_module_method("guis", "is_16_bit_from_memory", gui_is_16_bit_from_memory_func, 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, str_params);
    native_register_module_method("guis", "is_hdr", gui_is_hdr_func, 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, str_params);
    native_register_module_method("guis", "is_hdr_from_memory", gui_is_hdr_from_memory_func, 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, str_params);

    /* ===== zlib解压 ===== */
    native_register_module_method("guis", "zlib_decode", gui_zlib_decode_func, 1, -1, -1, TYPE_STRING, TYPE_UNKNOWN, str_params);
    native_register_module_method("guis", "zlib_decode_noheader", gui_zlib_decode_noheader_func, 1, -1, -1, TYPE_STRING, TYPE_UNKNOWN, str_params);

    /* ===== 显示器信息 ===== */
    native_register_module_method("guis", "get_display", gui_get_display_size_func, 0, -1, -1, TYPE_ANY, TYPE_UNKNOWN, no_params);
    native_register_module_method("guis", "get_dpi", gui_get_display_dpi_func, 0, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, no_params);

    /* ===== 回调式事件循环 ===== */
    native_register_module_method("guis", "run", gui_run_func, 3, -1, -1, TYPE_NULL, TYPE_UNKNOWN, obj_2func);

    /* ===== 日志系统函数 ===== */
    native_register_module_method("guis", "log_set_priority", gui_log_set_priority_func, 2, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_2int);
    native_register_module_method("guis", "log_set_all_priority", gui_log_set_all_priority_func, 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_params);
    native_register_module_method("guis", "log_get_priority", gui_log_get_priority_func, 1, -1, -1, TYPE_INT, TYPE_UNKNOWN, int_params);
    native_register_module_method("guis", "log_trace", gui_log_trace_func, 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, str_params);
    native_register_module_method("guis", "log_debug", gui_log_debug_func, 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, str_params);
    native_register_module_method("guis", "log_info", gui_log_info_func, 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, str_params);
    native_register_module_method("guis", "log_warn", gui_log_warn_func, 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, str_params);
    native_register_module_method("guis", "log_error", gui_log_error_func, 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, str_params);
    native_register_module_method("guis", "log_critical", gui_log_critical_func, 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, str_params);

    /* ===== 注册所有模块常量 ===== */
    guis_register_constants();

    /* 注册 Draw 实例方法 */
    guis_init_instance_methods();

    /* 注册 Win 实例方法 */
    guis_init_window_instance_methods();

    /* 注册 Image 实例方法 */
    guis_init_image_instance_methods();

    /* 注册 Font 实例方法 */
    guis_init_font_instance_methods();
}

/* 注册全局函数（不需要 import guis） */
void guis_init_globals(void) {
    vm_register_native("_rgb", gui_rgb_func, -1, 3, 4, TYPE_RGB, NULL);
}


