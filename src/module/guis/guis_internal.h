/* Leno GUI - 内部共享定义
 * 供 guis.c、guis_draw.c、guis_event.c 共享使用
 */

#ifndef GUIS_INTERNAL_H
#define GUIS_INTERNAL_H

#include "include/leno_value.h"
#include "leno_guis.h"

/* GUI 对象类型定义 */
typedef struct {
    Object header;
    LenoGUIPlatformWindow* platform;
} ObjGUIWindow;

typedef struct {
    Object header;
    LenoGUIPlatformRenderer* platform;
    ObjGUIWindow* window;
} ObjGUIRenderer;

typedef struct {
    Object header;
    LenoGUIPlatformFont* platform;
} ObjGUIFont;

typedef struct {
    Object header;
    ObjDict* data;
} ObjGUIEvent;

/* 辅助函数 */
ObjGUIWindow* as_window(Value v);
ObjGUIRenderer* as_renderer(Value v);
ObjGUIFont* as_font(Value v);
ObjArray* make_int_array2(int a, int b);

/* 事件字符串键（由 guis.c 定义） */
extern ObjString* str_key_type;
extern ObjString* str_key_window_id;
extern ObjString* str_key_width;
extern ObjString* str_key_height;
extern ObjString* str_key_x;
extern ObjString* str_key_y;
extern ObjString* str_key_key;
extern ObjString* str_key_scancode;
extern ObjString* str_key_mod;
extern ObjString* str_key_repeat;
extern ObjString* str_key_text;
extern ObjString* str_key_xrel;
extern ObjString* str_key_yrel;
extern ObjString* str_key_button;
extern ObjString* str_key_clicks;
extern ObjString* str_key_wheel_x;
extern ObjString* str_key_wheel_y;

void init_event_string_keys(void);
void dict_add_int_key(ObjDict* d, ObjString* key, int value);
void dict_add_float_key(ObjDict* d, ObjString* key, float value);
void dict_add_string_key(ObjDict* d, ObjString* key, const char* value);

/* 事件转换 */
Value event_to_dict(LenoGUIEvent* ev);

/* 创建对象 */
ObjGUIWindow* create_window_obj(LenoGUIPlatformWindow* pw);
ObjGUIRenderer* create_renderer_obj(LenoGUIPlatformRenderer* pr, ObjGUIWindow* win);

/* 调用 Leno 闭包 */
Value call_leno_closure(Value callee, int arg_count, Value* args);

#endif /* GUIS_INTERNAL_H */
