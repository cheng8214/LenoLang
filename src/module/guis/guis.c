/* Leno GUI - LenoC 模块注册
 * 将平台抽象层连接到 LenoC VM
 *
 * 类型关键字:
 *   Win   - 窗口对象 (OBJ_GUI_WINDOW)
 *   Draw  - 渲染器对象 (OBJ_GUI_RENDERER)
 *   Event - 事件对象 (OBJ_DICT 别名)
 *
 * 模块级 API:
 *   guis.init() -> bool
 *   guis.cleanup(win) -> null              销毁窗口 + 退出 GUI
 *   guis.create_window(title, w, h, flags) -> Win
 *   guis.show_window(win) / hide_window(win)
 *   guis.set_window_title(win, title)
 *   guis.set_window_size(win, w, h) / get_window_size(win) -> [w, h]
 *   guis.set_window_position(win, x, y) / get_window_position(win) -> [x, y]
 *   guis.set_window_fullscreen(win, bool)
 *   guis.window_should_close(win) -> bool
 *   guis.set_window_should_close(win, bool)
 *   guis.set_window_opacity(win, opacity)
 *   guis.create_renderer(win) -> Draw
 *   guis.destroy_renderer(ren)
 *   guis.resize_renderer(ren, w, h) -> bool    窗口大小改变时调整渲染器
 *   ren.draw_text(text, x, y, size)                       内置 8x8 点阵字体
 *   ren.text_size(text, size) -> [w, h]                   计算文字尺寸
 *   guis.load_font(name, size) -> Font                    加载系统字体
 *   guis.destroy_font(font)                               销毁字体
 *   ren.draw_text_font(font, text, x, y)                  系统字体渲染
 *   ren.text_size_font(font, text) -> [w, h]              系统字体文字尺寸
 *   guis.poll_event() / wait_event(timeout_ms)
 *   guis.get_key_state(key) -> bool
 *   guis.get_mouse_state() -> {x, y, buttons}
 *   guis.get_clipboard_text() / set_clipboard_text(text)
 *   guis.show_cursor(bool)
 *   guis.msg_box(title, message, type) -> int
 *   guis.get_ticks() / get_performance_counter() / get_performance_frequency()
 *   guis.delay(ms)
 *   guis.add_timer(interval_ms, callback) -> timer_id    定时器回调
 *   guis.remove_timer(timer_id) -> bool                  取消定时器
 *   guis.get_display_size() / get_display_dpi()
 *   guis.run(win, onDraw, onEvent)          回调式事件循环
 *
 * Draw 实例方法 (ren.method()):
 *   ren.set_draw_color(r, g, b, a)
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
 *
 * Event 实例方法 (e.method()):
 *   e.type() -> int
 *   e.is_quit() / e.is_window_close() / e.is_window_resize() / e.is_window_move()
 *   e.is_key_down() / e.is_key_up() / e.is_text_input()
 *   e.is_mouse_move() / e.is_mouse_down() / e.is_mouse_up() / e.is_mouse_wheel()
 *   e.key() -> int
 *   e.mouse_x() / e.mouse_y() / e.mouse_button() -> int
 *   e.width() / e.height() -> int
 *   e.text() -> string
 *
 * 使用示例:
 *   import guis
 *   main() {
 *       guis.init()
 *       Win win = guis.create_window("Hello", 800, 600, 1)
 *       guis.run(win,
 *           func(Draw ren) {
 *               ren.set_draw_color(30, 30, 46, 255)
 *               ren.clear()
 *               ren.set_draw_color(255, 0, 0, 255)
 *               ren.fill_rect(100, 100, 200, 150)
 *               ren.present()
 *           },
 *           func(Event e) {
 *               if e.is_quit() or e.is_window_close() {
 *                   guis.set_window_should_close(win, true)
 *               }
 *               if e.is_key_down() and e.key() == 0x1B {
 *                   guis.set_window_should_close(win, true)
 *               }
 *           }
 *       )
 *       guis.cleanup(win)
 *   }
 */

#include "include/native.h"
#include "include/leno_value.h"
#include "include/leno_vm.h"
#include "leno_guis.h"
#include "guis_internal.h"
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

    if (ev->type == LENO_GUI_EVT_WINDOW_RESIZE) {
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
        dict_add_float_key(d, str_key_x, ev->mouse_x);
        dict_add_float_key(d, str_key_y, ev->mouse_y);
        dict_add_float_key(d, str_key_wheel_x, ev->wheel_x);
        dict_add_float_key(d, str_key_wheel_y, ev->wheel_y);
    }

    return val_obj((Object*)event_obj);
}

static Value gui_create_window_func(int argc, Value* args) {
    (void)argc;
    ObjString* title = (ObjString*)val_as_obj(args[0]);
    int w = val_as_int(args[1]);
    int h = val_as_int(args[2]);
    int flags = val_as_int(args[3]);

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

static Value gui_show_window_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    if (win && win->platform) leno_gui_platform_show_window(win->platform);
    return val_null();
}

static Value gui_hide_window_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    if (win && win->platform) leno_gui_platform_hide_window(win->platform);
    return val_null();
}

static Value gui_set_window_title_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    ObjString* title = (ObjString*)val_as_obj(args[1]);
    if (win && win->platform) leno_gui_platform_set_window_title(win->platform, title->chars);
    return val_null();
}

static Value gui_set_window_size_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    int w = val_as_int(args[1]);
    int h = val_as_int(args[2]);
    if (win && win->platform) leno_gui_platform_set_window_size(win->platform, w, h);
    return val_null();
}

static Value gui_get_window_size_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    int w = 0, h = 0;
    if (win && win->platform) leno_gui_platform_get_window_size(win->platform, &w, &h);
    return val_obj((Object*)make_int_array2(w, h));
}

static Value gui_set_window_position_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    int x = val_as_int(args[1]);
    int y = val_as_int(args[2]);
    if (win && win->platform) leno_gui_platform_set_window_position(win->platform, x, y);
    return val_null();
}

static Value gui_get_window_position_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    int x = 0, y = 0;
    if (win && win->platform) leno_gui_platform_get_window_position(win->platform, &x, &y);
    return val_obj((Object*)make_int_array2(x, y));
}

static Value gui_set_window_fullscreen_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    int fullscreen = val_as_bool(args[1]) ? 1 : 0;
    if (win && win->platform) leno_gui_platform_set_window_fullscreen(win->platform, fullscreen);
    return val_null();
}

static Value gui_window_should_close_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    if (win && win->platform) return val_bool(leno_gui_platform_window_should_close(win->platform) != 0);
    return val_bool(true);
}

static Value gui_set_window_should_close_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    int val = val_as_bool(args[1]) ? 1 : 0;
    if (win && win->platform) leno_gui_platform_set_window_should_close(win->platform, val);
    return val_null();
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

static Value gui_destroy_font_func(int argc, Value* args) {
    (void)argc;
    ObjGUIFont* font = as_font(args[0]);
    if (font && font->platform) {
        leno_gui_platform_destroy_font(font->platform);
        font->platform = NULL;
    }
    return val_null();
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

/* ===== 窗口透明度 ===== */

/* 设置窗口透明度 */
static Value gui_set_window_opacity_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    float opacity = (float)val_as_double(args[1]);
    if (win && win->platform) leno_gui_platform_set_window_opacity(win->platform, opacity);
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
    if (!g_timers_initialized) return;
    for (int i = 0; i < LENO_GUI_MAX_TIMERS; i++) {
        if (g_timers[i].active) {
            gc_mark_value(g_timers[i].callback);
        }
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

/* 一键清理：销毁窗口 + 退出 GUI 子系统 */
static Value gui_cleanup_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    if (win && win->platform) {
        leno_gui_platform_destroy_window(win->platform);
        win->platform = NULL;
    }
    leno_gui_platform_quit();
    return val_null();
}

/* 前向声明：Draw 实例方法注册 */
void guis_init_instance_methods(void);

void guis_init_module(void) {
    TypeKind no_params[] = {};
    TypeKind str_4int[] = {TYPE_STRING, TYPE_INT, TYPE_INT, TYPE_INT};
    TypeKind obj_1int[] = {TYPE_ANY, TYPE_INT};
    TypeKind obj_2int[] = {TYPE_ANY, TYPE_INT, TYPE_INT};
    TypeKind obj_str[] = {TYPE_ANY, TYPE_STRING};
    TypeKind obj_bool[] = {TYPE_ANY, TYPE_BOOL};
    TypeKind obj_2func[] = {TYPE_ANY, TYPE_ANY, TYPE_ANY};
    TypeKind int_params[] = {TYPE_INT};
    TypeKind str_2int[] = {TYPE_STRING, TYPE_STRING, TYPE_INT};
    TypeKind str_int[] = {TYPE_STRING, TYPE_INT};
    TypeKind obj_float[] = {TYPE_ANY, TYPE_FLOAT};
    TypeKind bool_params[] = {TYPE_BOOL};
    TypeKind str_params[] = {TYPE_STRING};

    /* ===== 初始化/关闭 ===== */
    native_register_module_method("guis", "cleanup", gui_cleanup_func, 1, -1, -1, TYPE_NULL, obj_1int);

    /* ===== 窗口操作 ===== */
    native_register_module_method("guis", "create_window", gui_create_window_func, 4, -1, -1, TYPE_WIN, str_4int);
    native_register_module_method("guis", "show_window", gui_show_window_func, 1, -1, -1, TYPE_NULL, obj_1int);
    native_register_module_method("guis", "hide_window", gui_hide_window_func, 1, -1, -1, TYPE_NULL, obj_1int);
    native_register_module_method("guis", "set_window_title", gui_set_window_title_func, 2, -1, -1, TYPE_NULL, obj_str);
    native_register_module_method("guis", "set_window_size", gui_set_window_size_func, 3, -1, -1, TYPE_NULL, obj_2int);
    native_register_module_method("guis", "get_window_size", gui_get_window_size_func, 1, -1, -1, TYPE_ANY, obj_1int);
    native_register_module_method("guis", "set_window_position", gui_set_window_position_func, 3, -1, -1, TYPE_NULL, obj_2int);
    native_register_module_method("guis", "get_window_position", gui_get_window_position_func, 1, -1, -1, TYPE_ANY, obj_1int);
    native_register_module_method("guis", "set_window_fullscreen", gui_set_window_fullscreen_func, 2, -1, -1, TYPE_NULL, obj_bool);
    native_register_module_method("guis", "window_should_close", gui_window_should_close_func, 1, -1, -1, TYPE_BOOL, obj_1int);
    native_register_module_method("guis", "set_window_should_close", gui_set_window_should_close_func, 2, -1, -1, TYPE_NULL, obj_bool);
    native_register_module_method("guis", "set_window_opacity", gui_set_window_opacity_func, 2, -1, -1, TYPE_NULL, obj_float);

    /* ===== 渲染器操作（工厂/析构） ===== */
    native_register_module_method("guis", "create_renderer", gui_create_renderer_func, 1, -1, -1, TYPE_DRAW, obj_1int);
    native_register_module_method("guis", "destroy_renderer", gui_destroy_renderer_func, 1, -1, -1, TYPE_NULL, obj_1int);
    native_register_module_method("guis", "resize_renderer", gui_resize_renderer_func, 3, -1, -1, TYPE_BOOL, obj_2int);

    /* ===== 事件操作 ===== */
    native_register_module_method("guis", "poll_event", gui_poll_event_func, 0, -1, -1, TYPE_ANY, no_params);
    native_register_module_method("guis", "wait_event", gui_wait_event_func, 1, -1, -1, TYPE_ANY, int_params);

    /* ===== 输入状态查询 ===== */
    native_register_module_method("guis", "get_key_state", gui_get_key_state_func, 1, -1, -1, TYPE_BOOL, int_params);
    native_register_module_method("guis", "get_mouse_state", gui_get_mouse_state_func, 0, -1, -1, TYPE_ANY, no_params);

    /* ===== 剪贴板 ===== */
    native_register_module_method("guis", "get_clipboard_text", gui_get_clipboard_text_func, 0, -1, -1, TYPE_ANY, no_params);
    native_register_module_method("guis", "set_clipboard_text", gui_set_clipboard_text_func, 1, -1, -1, TYPE_NULL, str_params);

    /* ===== 光标控制 ===== */
    native_register_module_method("guis", "show_cursor", gui_show_cursor_func, 1, -1, -1, TYPE_NULL, bool_params);

    /* ===== 消息框 ===== */
    native_register_module_method("guis", "msg_box", gui_msg_box_func, 3, -1, -1, TYPE_INT, str_2int);

    /* ===== 高精度计时器 ===== */
    native_register_module_method("guis", "get_ticks", gui_get_ticks_func, 0, -1, -1, TYPE_INT, no_params);
    native_register_module_method("guis", "get_performance_counter", gui_get_performance_counter_func, 0, -1, -1, TYPE_INT, no_params);
    native_register_module_method("guis", "get_performance_frequency", gui_get_performance_frequency_func, 0, -1, -1, TYPE_INT, no_params);
    native_register_module_method("guis", "delay", gui_delay_func, 1, -1, -1, TYPE_NULL, int_params);

    /* ===== 定时器回调 ===== */
    TypeKind int_func[] = {TYPE_INT, TYPE_ANY};
    native_register_module_method("guis", "add_timer", gui_add_timer_func, 2, -1, -1, TYPE_INT, int_func);
    native_register_module_method("guis", "remove_timer", gui_remove_timer_func, 1, -1, -1, TYPE_BOOL, int_params);

    /* ===== 字体操作 ===== */
    native_register_module_method("guis", "load_font", gui_load_font_func, 2, -1, -1, TYPE_ANY, str_int);
    native_register_module_method("guis", "destroy_font", gui_destroy_font_func, 1, -1, -1, TYPE_NULL, obj_1int);

    /* ===== 显示器信息 ===== */
    native_register_module_method("guis", "get_display_size", gui_get_display_size_func, 0, -1, -1, TYPE_ANY, no_params);
    native_register_module_method("guis", "get_display_dpi", gui_get_display_dpi_func, 0, -1, -1, TYPE_FLOAT, no_params);

    /* ===== 回调式事件循环 ===== */
    native_register_module_method("guis", "run", gui_run_func, 3, -1, -1, TYPE_NULL, obj_2func);

    /* 注册 Draw 实例方法 */
    guis_init_instance_methods();
}




