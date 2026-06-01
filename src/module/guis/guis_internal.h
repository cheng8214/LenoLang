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
ObjGUIWindow* as_window_from_platform(LenoGUIPlatformWindow* pw);

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

/* 文件对话框结果处理（由平台层调用，确保在主线程中执行） */
void process_filedialog_callback(const char* const* files, int nfiles, int filter_index);

/* ===== Style 字段类型系统（供 LSP 使用） ===== */

typedef enum {
    STYLE_TYPE_STRING,
    STYLE_TYPE_INT,
    STYLE_TYPE_FLOAT,
    STYLE_TYPE_BOOL,
    STYLE_TYPE_COLOR,
    STYLE_TYPE_ENUM,
} StyleFieldType;

typedef struct {
    const char* name;
    StyleFieldType type;
    const char* description;
    const char* default_value;
    const char** options;
    int option_count;
} StyleFieldInfo;

// 获取字段名称列表（返回的数组需要 free 释放）
const char** guis_get_style_fields(const char* target, int* count);

// 获取字段详细信息
StyleFieldInfo* guis_get_style_field_info(const char* target, const char* field_name);

// 获取字段类型名称字符串
const char* guis_style_field_type_name(StyleFieldType type);

#endif /* GUIS_INTERNAL_H */
