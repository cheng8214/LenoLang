#include "include/lenolang.h"
#include "include/native.h"
#include "include/platform_thread.h"
#include "include/leno_vm.h"
#include <stdlib.h>
#include <string.h>

// 子线程使用独立的 VM 数组（多线程模式下）
// 主程序直接使用全局 vm，保持效率第一

// 外部函数声明
extern int vm_run_coroutine(ObjCoroutine* co);

// ============================================================================
// Thread 方法注册表（哈希表实现）
// ============================================================================

#define THREAD_METHOD_TABLE_INITIAL_CAPACITY 16
#define THREAD_METHOD_TABLE_MAX_LOAD 0.75

static uint32_t thread_hash_string(const char* str) {
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= (unsigned char)(*str);
        hash *= 16777619;
        str++;
    }
    return hash;
}

typedef struct ThreadMethodHashEntry {
    char* name;
    ObjNative* method;
    int arity;
    TypeKind return_type;
    TypeKind return_element_type;
    TypeKind param_types[MAX_METHOD_PARAMS];
    struct ThreadMethodHashEntry* next;
} ThreadMethodHashEntry;

typedef struct {
    ThreadMethodHashEntry** entries;
    int capacity;
    int count;
} ThreadMethodTable;

static THREAD_LOCAL ThreadMethodTable threadMethodTable = {NULL, 0, 0};

static void thread_method_table_init(void) {
    threadMethodTable.capacity = THREAD_METHOD_TABLE_INITIAL_CAPACITY;
    threadMethodTable.count = 0;
    threadMethodTable.entries = (ThreadMethodHashEntry**)calloc(threadMethodTable.capacity, sizeof(ThreadMethodHashEntry*));
}

static void thread_method_table_free(void) {
    if (!threadMethodTable.entries) return;
    for (int i = 0; i < threadMethodTable.capacity; i++) {
        ThreadMethodHashEntry* entry = threadMethodTable.entries[i];
        while (entry) {
            ThreadMethodHashEntry* next = entry->next;
            free(entry->name);
            free(entry);
            entry = next;
        }
    }
    free(threadMethodTable.entries);
    threadMethodTable.entries = NULL;
    threadMethodTable.capacity = 0;
    threadMethodTable.count = 0;
}

static void thread_method_table_resize(void) {
    int old_capacity = threadMethodTable.capacity;
    ThreadMethodHashEntry** old_entries = threadMethodTable.entries;
    int new_capacity = old_capacity * 2;
    ThreadMethodHashEntry** new_entries = (ThreadMethodHashEntry**)calloc(new_capacity, sizeof(ThreadMethodHashEntry*));
    if (!new_entries) return;
    for (int i = 0; i < old_capacity; i++) {
        ThreadMethodHashEntry* entry = old_entries[i];
        while (entry) {
            ThreadMethodHashEntry* next = entry->next;
            uint32_t hash = thread_hash_string(entry->name);
            int index = hash & (new_capacity - 1);
            entry->next = new_entries[index];
            new_entries[index] = entry;
            entry = next;
        }
    }
    free(old_entries);
    threadMethodTable.entries = new_entries;
    threadMethodTable.capacity = new_capacity;
}

void thread_register_method_with_params(const char* name, ObjNative* method, int arity,
                                         int min_arity, int max_arity,
                                         TypeKind return_type, TypeKind return_element_type, TypeKind* param_types) {
    if (!threadMethodTable.entries) {
        thread_method_table_init();
    }
    if (threadMethodTable.count >= threadMethodTable.capacity * THREAD_METHOD_TABLE_MAX_LOAD) {
        thread_method_table_resize();
    }
    uint32_t hash = thread_hash_string(name);
    int index = hash & (threadMethodTable.capacity - 1);
    ThreadMethodHashEntry* entry = threadMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            entry->method = method;
            entry->arity = arity;
            entry->return_type = return_type;
            entry->return_element_type = return_element_type;
            if (param_types && arity > 0) {
                int count = arity < MAX_METHOD_PARAMS ? arity : MAX_METHOD_PARAMS;
                for (int i = 0; i < count; i++) entry->param_types[i] = param_types[i];
                for (int i = count; i < MAX_METHOD_PARAMS; i++) entry->param_types[i] = TYPE_ANY;
            } else {
                for (int i = 0; i < MAX_METHOD_PARAMS; i++) entry->param_types[i] = TYPE_ANY;
            }
            return;
        }
        entry = entry->next;
    }
    ThreadMethodHashEntry* new_entry = (ThreadMethodHashEntry*)malloc(sizeof(ThreadMethodHashEntry));
    if (!new_entry) return;
    new_entry->name = strdup(name);
    new_entry->method = method;
    new_entry->arity = arity;
    new_entry->return_type = return_type;
    new_entry->return_element_type = return_element_type;
    if (param_types && arity > 0) {
        int count = arity < MAX_METHOD_PARAMS ? arity : MAX_METHOD_PARAMS;
        for (int i = 0; i < count; i++) new_entry->param_types[i] = param_types[i];
        for (int i = count; i < MAX_METHOD_PARAMS; i++) new_entry->param_types[i] = TYPE_ANY;
    } else {
        for (int i = 0; i < MAX_METHOD_PARAMS; i++) new_entry->param_types[i] = TYPE_ANY;
    }
    new_entry->next = threadMethodTable.entries[index];
    threadMethodTable.entries[index] = new_entry;
    threadMethodTable.count++;
    native_register_instance_method_meta_with_params("thread", name, arity, min_arity, max_arity, return_type, return_element_type, param_types);
}

ObjNative* thread_find_method(const char* name) {
    if (!threadMethodTable.entries || threadMethodTable.count == 0) return NULL;
    uint32_t hash = thread_hash_string(name);
    int index = hash & (threadMethodTable.capacity - 1);
    ThreadMethodHashEntry* entry = threadMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) return entry->method;
        entry = entry->next;
    }
    return NULL;
}

void thread_init_methods(void) {
    thread_method_table_free();
    thread_method_table_init();
}

void thread_mark_methods(void) {
    if (!threadMethodTable.entries) return;
    for (int i = 0; i < threadMethodTable.capacity; i++) {
        ThreadMethodHashEntry* entry = threadMethodTable.entries[i];
        while (entry) {
            if (entry->method) gc_mark_object((Object*)entry->method);
            entry = entry->next;
        }
    }
}

// ============================================================================
// Channel 方法注册表（哈希表实现）
// ============================================================================

#define CHANNEL_METHOD_TABLE_INITIAL_CAPACITY 16
#define CHANNEL_METHOD_TABLE_MAX_LOAD 0.75

typedef struct ChannelMethodHashEntry {
    char* name;
    ObjNative* method;
    int arity;
    TypeKind return_type;
    TypeKind return_element_type;
    TypeKind param_types[MAX_METHOD_PARAMS];
    struct ChannelMethodHashEntry* next;
} ChannelMethodHashEntry;

typedef struct {
    ChannelMethodHashEntry** entries;
    int capacity;
    int count;
} ChannelMethodTable;

static THREAD_LOCAL ChannelMethodTable channelMethodTable = {NULL, 0, 0};

static void channel_method_table_init(void) {
    channelMethodTable.capacity = CHANNEL_METHOD_TABLE_INITIAL_CAPACITY;
    channelMethodTable.count = 0;
    channelMethodTable.entries = (ChannelMethodHashEntry**)calloc(channelMethodTable.capacity, sizeof(ChannelMethodHashEntry*));
}

static void channel_method_table_free(void) {
    if (!channelMethodTable.entries) return;
    for (int i = 0; i < channelMethodTable.capacity; i++) {
        ChannelMethodHashEntry* entry = channelMethodTable.entries[i];
        while (entry) {
            ChannelMethodHashEntry* next = entry->next;
            free(entry->name);
            free(entry);
            entry = next;
        }
    }
    free(channelMethodTable.entries);
    channelMethodTable.entries = NULL;
    channelMethodTable.capacity = 0;
    channelMethodTable.count = 0;
}

static void channel_method_table_resize(void) {
    int old_capacity = channelMethodTable.capacity;
    ChannelMethodHashEntry** old_entries = channelMethodTable.entries;
    int new_capacity = old_capacity * 2;
    ChannelMethodHashEntry** new_entries = (ChannelMethodHashEntry**)calloc(new_capacity, sizeof(ChannelMethodHashEntry*));
    if (!new_entries) return;
    for (int i = 0; i < old_capacity; i++) {
        ChannelMethodHashEntry* entry = old_entries[i];
        while (entry) {
            ChannelMethodHashEntry* next = entry->next;
            uint32_t hash = thread_hash_string(entry->name);
            int index = hash & (new_capacity - 1);
            entry->next = new_entries[index];
            new_entries[index] = entry;
            entry = next;
        }
    }
    free(old_entries);
    channelMethodTable.entries = new_entries;
    channelMethodTable.capacity = new_capacity;
}

void channel_register_method_with_params(const char* name, ObjNative* method, int arity,
                                          int min_arity, int max_arity,
                                          TypeKind return_type, TypeKind return_element_type, TypeKind* param_types) {
    if (!channelMethodTable.entries) {
        channel_method_table_init();
    }
    if (channelMethodTable.count >= channelMethodTable.capacity * CHANNEL_METHOD_TABLE_MAX_LOAD) {
        channel_method_table_resize();
    }
    uint32_t hash = thread_hash_string(name);
    int index = hash & (channelMethodTable.capacity - 1);
    ChannelMethodHashEntry* entry = channelMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            entry->method = method;
            entry->arity = arity;
            entry->return_type = return_type;
            entry->return_element_type = return_element_type;
            if (param_types && arity > 0) {
                int count = arity < MAX_METHOD_PARAMS ? arity : MAX_METHOD_PARAMS;
                for (int i = 0; i < count; i++) entry->param_types[i] = param_types[i];
                for (int i = count; i < MAX_METHOD_PARAMS; i++) entry->param_types[i] = TYPE_ANY;
            } else {
                for (int i = 0; i < MAX_METHOD_PARAMS; i++) entry->param_types[i] = TYPE_ANY;
            }
            return;
        }
        entry = entry->next;
    }
    ChannelMethodHashEntry* new_entry = (ChannelMethodHashEntry*)malloc(sizeof(ChannelMethodHashEntry));
    if (!new_entry) return;
    new_entry->name = strdup(name);
    new_entry->method = method;
    new_entry->arity = arity;
    new_entry->return_type = return_type;
    new_entry->return_element_type = return_element_type;
    if (param_types && arity > 0) {
        int count = arity < MAX_METHOD_PARAMS ? arity : MAX_METHOD_PARAMS;
        for (int i = 0; i < count; i++) new_entry->param_types[i] = param_types[i];
        for (int i = count; i < MAX_METHOD_PARAMS; i++) new_entry->param_types[i] = TYPE_ANY;
    } else {
        for (int i = 0; i < MAX_METHOD_PARAMS; i++) new_entry->param_types[i] = TYPE_ANY;
    }
    new_entry->next = channelMethodTable.entries[index];
    channelMethodTable.entries[index] = new_entry;
    channelMethodTable.count++;
    native_register_instance_method_meta_with_params("channel", name, arity, min_arity, max_arity, return_type, return_element_type, param_types);
}

ObjNative* channel_find_method(const char* name) {
    if (!channelMethodTable.entries || channelMethodTable.count == 0) return NULL;
    uint32_t hash = thread_hash_string(name);
    int index = hash & (channelMethodTable.capacity - 1);
    ChannelMethodHashEntry* entry = channelMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) return entry->method;
        entry = entry->next;
    }
    return NULL;
}

void channel_init_methods(void) {
    channel_method_table_free();
    channel_method_table_init();
}

void channel_mark_methods(void) {
    if (!channelMethodTable.entries) return;
    for (int i = 0; i < channelMethodTable.capacity; i++) {
        ChannelMethodHashEntry* entry = channelMethodTable.entries[i];
        while (entry) {
            if (entry->method) gc_mark_object((Object*)entry->method);
            entry = entry->next;
        }
    }
}

// ============================================================================
// 值深拷贝（跨 VM 安全）
// ============================================================================

void channel_ref(ObjChannel* channel);
void channel_unref(ObjChannel* channel);

Value value_clone_for_channel(Value val) {
    switch (val_get_type(val)) {
        case VAL_NULL:
        case VAL_INT:
        case VAL_FLOAT:
        case VAL_BOOL:
            return val;
        case VAL_OBJ: {
            Object* obj = val_as_obj(val);
            switch (obj->type) {
                case OBJ_STRING: {
                    ObjString* str = (ObjString*)obj;
                    return val_obj((Object*)str_copy(str->chars, str->len));
                }
                case OBJ_ARRAY: {
                    ObjArray* arr = (ObjArray*)obj;
                    ObjArray* copy = (ObjArray*)gc_alloc(sizeof(ObjArray), OBJ_ARRAY);
                    if (!copy) return val_null();
                    copy->count = arr->count;
                    copy->capacity = arr->count > 0 ? arr->count : 1;
                    copy->elements = NULL;
                    copy->type_info = arr->type_info ? type_copy(arr->type_info) : NULL;
                    if (arr->count > 0) {
                        copy->elements = (Value*)malloc(copy->capacity * sizeof(Value));
                        if (!copy->elements) return val_null();
                        for (int i = 0; i < arr->count; i++) {
                            copy->elements[i] = value_clone_for_channel(arr->elements[i]);
                        }
                    }
                    return val_obj((Object*)copy);
                }
                case OBJ_DICT: {
                    ObjDict* dict = (ObjDict*)obj;
                    ObjDict* copy = dict_new(dict->capacity > 0 ? dict->capacity : 8);
                    if (!copy) return val_null();
                    for (int i = 0; i < dict->asize; i++) {
                        if (dict->array && i < dict->asize) {
                            while (copy->asize <= i) {
                                if (copy->asize >= copy->capacity) break;
                                copy->array[copy->asize] = value_clone_for_channel(dict->array[i]);
                                copy->asize++;
                                copy->acount++;
                            }
                        }
                    }
                    for (int i = 0; i < dict->capacity; i++) {
                        Value entry_key = dict->entries[i].key;
                        if (!val_is_null(entry_key) && entry_key != DICT_TOMBSTONE_VAL) {
                            dict_set(copy, entry_key,
                                    value_clone_for_channel(dict->entries[i].value));
                        }
                    }
                    return val_obj((Object*)copy);
                }
                case OBJ_CHANNEL: {
                    ObjChannel* ch = (ObjChannel*)obj;
                    channel_ref(ch);
                    return val_obj((Object*)ch);
                }
                default:
                    native_throw_error("不能跨线程传递此类型");
                    return val_null();
            }
        }
        default:
            native_throw_error("不能跨线程传递此类型");
            return val_null();
    }
}

// ============================================================================
// 子线程执行入口
// ============================================================================

typedef struct {
    ObjClosure* closure;
    ObjThread* thread_obj;
    Value* globals;
    int global_count;
    int global_capacity;
    Value* global_funcs;
    int global_func_count;
    int global_func_capacity;
    Value* initial_args;
    int initial_arg_count;
} ThreadStartArgs;

static void* thread_entry_point(void* arg) {
    ThreadStartArgs* args = (ThreadStartArgs*)arg;
    ObjThread* thread_obj = args->thread_obj;
    ObjClosure* closure = args->closure;

    Value* saved_globals = args->globals;
    int saved_global_count = args->global_count;
    int saved_global_capacity = args->global_capacity;
    Value* saved_global_funcs = args->global_funcs;
    int saved_global_func_count = args->global_func_count;
    int saved_global_func_capacity = args->global_func_capacity;
    Value* thread_initial_args = args->initial_args;
    int thread_initial_arg_count = args->initial_arg_count;
    free(args);

    // 创建子线程自己的 VM（独立的局部变量，不共享主程序数据）
    VM child_vm;
    memset(&child_vm, 0, sizeof(VM));

    // 初始化子线程 VM
    child_vm.chunk = NULL;
    child_vm.ip = NULL;
    child_vm.sp = 0;
    child_vm.stack_capacity = 256;
    child_vm.stack = (Value*)malloc(child_vm.stack_capacity * sizeof(Value));
    memset(child_vm.stack, 0, child_vm.stack_capacity * sizeof(Value));

    child_vm.frame_cnt = 0;
    child_vm.frame_capacity = 16;
    child_vm.frames = (CallFrame*)malloc(child_vm.frame_capacity * sizeof(CallFrame));
    memset(child_vm.frames, 0, child_vm.frame_capacity * sizeof(CallFrame));

    child_vm.exception = val_null();
    child_vm.has_exception = 0;
    child_vm.catch_ip = NULL;
    child_vm.catch_frame = -1;
    child_vm.pending_exception = 0;
    child_vm.exception_line = 0;

    for (int i = 0; i < IC_CACHE_SIZE; i++) {
        child_vm.ic_cache[i].valid = 0;
        child_vm.ic_cache[i].receiver_type = 0;
        child_vm.ic_cache[i].method = NULL;
    }

    child_vm.global_scope = NULL;
    child_vm.open_upvalues = NULL;
    child_vm.current_module_frame = NULL;
    child_vm.event_loop = NULL;
    child_vm.current_coroutine = NULL;
    child_vm.all_coroutines = NULL;

    // 复制全局变量（深拷贝，独立数据）
    // 注意：cstruct 定义是只读类型元数据，直接共享
    if (saved_globals && saved_global_count > 0) {
        child_vm.globals = (Value*)malloc(saved_global_capacity * sizeof(Value));
        if (child_vm.globals) {
            // 逐个复制全局变量，对 cstruct 定义进行特殊处理
            for (int i = 0; i < saved_global_count; i++) {
                Value val = saved_globals[i];
                if (val_is_obj(val)) {
                    Object* obj = val_as_obj(val);
                    if (obj->type == OBJ_CSTRUCT_DEF) {
                        // cstruct 定义是只读类型元数据，直接共享
                        child_vm.globals[i] = val;
                    } else {
                        // 其他对象类型，浅拷贝 Value（对象本身共享）
                        child_vm.globals[i] = val;
                    }
                } else {
                    // 基本类型，直接复制
                    child_vm.globals[i] = val;
                }
            }
            child_vm.global_count = saved_global_count;
            child_vm.global_capacity = saved_global_capacity;
        }
    } else {
        child_vm.global_capacity = 8;
        child_vm.globals = (Value*)malloc(child_vm.global_capacity * sizeof(Value));
        for (int i = 0; i < child_vm.global_capacity; i++) child_vm.globals[i] = val_null();
        child_vm.global_count = 0;
    }

    if (saved_global_funcs && saved_global_func_count > 0) {
        child_vm.global_funcs = (Value*)malloc(saved_global_func_capacity * sizeof(Value));
        if (child_vm.global_funcs) {
            memcpy(child_vm.global_funcs, saved_global_funcs, saved_global_func_count * sizeof(Value));
            child_vm.global_func_count = saved_global_func_count;
            child_vm.global_func_capacity = saved_global_func_capacity;
        }
    } else {
        child_vm.global_func_capacity = 8;
        child_vm.global_funcs = (Value*)malloc(child_vm.global_func_capacity * sizeof(Value));
        child_vm.global_func_count = 0;
    }

    // 初始化子线程的 GC（线程局部）
    gc_init();
    gc.vm = &child_vm;

    // 初始化子线程的字符串表（线程局部）
    extern void intern_table_init(void);
    intern_table_init();

    // 注册内置 Native 全局函数（与主 VM 一致）
    extern void native_register_globals(void);
    native_register_globals();

    // 初始化所有模块方法（与主 VM 一致）
    extern void native_init_module(const char* module_name);
    native_init_module("io");
    native_init_module("times");
    native_init_module("arrays");
    native_init_module("strings");
    native_init_module("maths");
    native_init_module("rands");
    native_init_module("files");
    native_init_module("asyncs");
    native_init_module("dirs");
    native_init_module("jsons");
    native_init_module("sockets");
    native_init_module("ffi");
    native_init_module("threads");

    // 初始化所有实例方法（与主 VM 一致）
    extern void arrays_init_instance_methods(void);
    arrays_init_instance_methods();
    extern void strings_init_instance_methods(void);
    strings_init_instance_methods();
    extern void maths_init_instance_methods(void);
    maths_init_instance_methods();
    extern void dicts_init_instance_methods(void);
    dicts_init_instance_methods();
    extern void files_init_instance_methods(void);
    files_init_instance_methods();
    extern void guis_init_instance_methods(void);
    guis_init_instance_methods();
    extern void guis_init_event_methods(void);
    guis_init_event_methods();
    extern void cstructs_init_methods(void);
    cstructs_init_methods();
    extern void threads_init_instance_methods(void);
    threads_init_instance_methods();

    // 创建协程对象来执行函数
    ObjCoroutine* co = coroutine_new(closure);
    if (!co) {
        platform_mutex_lock(&thread_obj->mutex);
        thread_obj->state = THREAD_ERROR;
        thread_obj->error_msg = strdup("Failed to create coroutine");
        thread_obj->has_result = 0;
        platform_cond_signal(&thread_obj->done_cond);
        platform_mutex_unlock(&thread_obj->mutex);

        if (saved_globals) free(saved_globals);
        if (saved_global_funcs) free(saved_global_funcs);
        if (thread_initial_args) free(thread_initial_args);

        if (child_vm.frames) free(child_vm.frames);
        if (child_vm.stack) free(child_vm.stack);
        if (child_vm.globals) free(child_vm.globals);
        if (child_vm.global_funcs) free(child_vm.global_funcs);

        return NULL;
    }

    if (thread_initial_args && thread_initial_arg_count > 0) {
        co->initial_args = thread_initial_args;
        co->initial_arg_count = thread_initial_arg_count;
    }

    int result = vm_run_coroutine_with_vm(co, &child_vm);

    if (result != 0 || child_vm.has_exception) {
        platform_mutex_lock(&thread_obj->mutex);
        thread_obj->state = THREAD_ERROR;
        if (child_vm.has_exception && val_is_obj(child_vm.exception) && val_as_obj(child_vm.exception)->type == OBJ_STRING) {
            ObjString* err = (ObjString*)val_as_obj(child_vm.exception);
            thread_obj->error_msg = strdup(err->chars);
        } else {
            thread_obj->error_msg = strdup("unknown error");
        }
        thread_obj->has_result = 0;
        platform_cond_signal(&thread_obj->done_cond);
        platform_mutex_unlock(&thread_obj->mutex);
    } else {
        platform_mutex_lock(&thread_obj->mutex);
        thread_obj->state = THREAD_DONE;
        // 从协程对象中获取结果
        thread_obj->result = value_clone_for_channel(co->result);
        gc_write_barrier((Object*)thread_obj, thread_obj->result);
        thread_obj->has_result = 1;
        platform_cond_signal(&thread_obj->done_cond);
        platform_mutex_unlock(&thread_obj->mutex);
    }

    if (saved_globals) free(saved_globals);
    if (saved_global_funcs) free(saved_global_funcs);

    // 释放子线程 VM 资源
    for (int i = 0; i < child_vm.frame_cnt; i++) {
        CallFrame* f = &child_vm.frames[i];
        if (f->locals != NULL && f->locals_is_dynamic) {
            free(f->locals);
            f->locals = NULL;
        }
    }
    if (child_vm.frames) free(child_vm.frames);
    if (child_vm.stack) free(child_vm.stack);
    if (child_vm.globals) free(child_vm.globals);
    if (child_vm.global_funcs) free(child_vm.global_funcs);

    return NULL;
}

// ============================================================================
// Thread 对象 API
// ============================================================================

ObjThread* thread_new_with_args(ObjClosure* closure, Value* call_args, int call_arg_count) {
    ObjThread* thread = (ObjThread*)gc_alloc(sizeof(ObjThread), OBJ_THREAD);
    if (!thread) return NULL;

    thread->vm = NULL;
    thread->os_thread = 0;
    thread->state = THREAD_RUNNING;
    thread->result = val_null();
    thread->error_msg = NULL;
    thread->has_result = 0;

    platform_mutex_init(&thread->mutex);
    platform_cond_init(&thread->done_cond);

    ThreadStartArgs* args = (ThreadStartArgs*)malloc(sizeof(ThreadStartArgs));
    if (!args) {
        native_throw_error("Failed to allocate thread args");
        return NULL;
    }
    memset(args, 0, sizeof(ThreadStartArgs));
    args->closure = closure;
    args->thread_obj = thread;

    if (call_args && call_arg_count > 0) {
        args->initial_args = (Value*)malloc(call_arg_count * sizeof(Value));
        if (args->initial_args) {
            memcpy(args->initial_args, call_args, call_arg_count * sizeof(Value));
            args->initial_arg_count = call_arg_count;
        }
    }
    
    extern THREAD_LOCAL VM* current_exec_vm;
    extern VM vm;
    VM* target_vm = current_exec_vm ? current_exec_vm : &vm;

    args->global_count = target_vm->global_count;
    args->global_capacity = target_vm->global_capacity;
    if (target_vm->global_count > 0 && target_vm->globals) {
        args->globals = (Value*)malloc(target_vm->global_capacity * sizeof(Value));
        if (args->globals) {
            memcpy(args->globals, target_vm->globals, target_vm->global_count * sizeof(Value));
        }
    } else {
        args->globals = NULL;
    }

    args->global_func_count = target_vm->global_func_count;
    args->global_func_capacity = target_vm->global_func_capacity;
    if (target_vm->global_func_count > 0 && target_vm->global_funcs) {
        args->global_funcs = (Value*)malloc(target_vm->global_func_capacity * sizeof(Value));
        if (args->global_funcs) {
            memcpy(args->global_funcs, target_vm->global_funcs, target_vm->global_func_count * sizeof(Value));
        }
    } else {
        args->global_funcs = NULL;
    }

    if (platform_thread_create(&thread->os_thread, thread_entry_point, args) != 0) {
        free(args);
        native_throw_error("Failed to create OS thread");
        return NULL;
    }

    if (target_vm->active_thread_count >= target_vm->active_thread_capacity) {
        int new_capacity = target_vm->active_thread_capacity == 0 ? 8 : target_vm->active_thread_capacity * 2;
        ObjThread** new_threads = (ObjThread**)realloc(target_vm->active_threads, new_capacity * sizeof(ObjThread*));
        if (new_threads) {
            target_vm->active_threads = new_threads;
            target_vm->active_thread_capacity = new_capacity;
        }
    }
    if (target_vm->active_threads && target_vm->active_thread_count < target_vm->active_thread_capacity) {
        target_vm->active_threads[target_vm->active_thread_count++] = thread;
    }

    return thread;
}

Value thread_join(ObjThread* thread) {
    platform_mutex_lock(&thread->mutex);
    while (thread->state == THREAD_RUNNING) {
        platform_cond_wait(&thread->done_cond, &thread->mutex);
    }
    platform_mutex_unlock(&thread->mutex);

    platform_thread_join(thread->os_thread, NULL);

    if (thread->state == THREAD_ERROR) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Thread error: %s",
                 thread->error_msg ? thread->error_msg : "unknown");
        native_throw_error(msg);
        return val_null();
    }

    return thread->has_result ? thread->result : val_null();
}

ThreadState thread_get_state(ObjThread* thread) {
    return thread->state;
}

// ============================================================================
// Channel 对象 API
// ============================================================================

ObjChannel* channel_new(int capacity) {
    ObjChannel* channel = (ObjChannel*)malloc(sizeof(ObjChannel));
    if (!channel) return NULL;
    memset(channel, 0, sizeof(ObjChannel));
    channel->header.type = OBJ_CHANNEL;
    channel->ref_count = 1;

    if (capacity <= 0) {
        channel->capacity = 1;
    } else {
        channel->capacity = capacity;
    }

    channel->buffer = (Value*)calloc(channel->capacity, sizeof(Value));
    if (!channel->buffer) {
        free(channel);
        return NULL;
    }
    for (int i = 0; i < channel->capacity; i++) {
        channel->buffer[i] = val_null();
    }

    channel->head = 0;
    channel->tail = 0;
    channel->count = 0;
    channel->closed = 0;

    platform_mutex_init(&channel->mutex);
    platform_cond_init(&channel->not_empty);
    platform_cond_init(&channel->not_full);

    return channel;
}

void channel_ref(ObjChannel* channel) {
    if (!channel) return;
    platform_mutex_lock(&channel->mutex);
    channel->ref_count++;
    platform_mutex_unlock(&channel->mutex);
}

void channel_unref(ObjChannel* channel) {
    if (!channel) return;
    platform_mutex_lock(&channel->mutex);
    channel->ref_count--;
    if (channel->ref_count <= 0) {
        platform_mutex_unlock(&channel->mutex);
        if (channel->buffer) {
            free(channel->buffer);
            channel->buffer = NULL;
        }
        platform_mutex_destroy(&channel->mutex);
        platform_cond_destroy(&channel->not_empty);
        platform_cond_destroy(&channel->not_full);
        free(channel);
        return;
    }
    platform_mutex_unlock(&channel->mutex);
}

void channel_close(ObjChannel* channel) {
    platform_mutex_lock(&channel->mutex);
    channel->closed = 1;
    platform_cond_broadcast(&channel->not_empty);
    platform_cond_broadcast(&channel->not_full);
    platform_mutex_unlock(&channel->mutex);
}

int channel_send(ObjChannel* channel, Value value) {
    Value cloned = value_clone_for_channel(value);

    platform_mutex_lock(&channel->mutex);

    if (channel->closed) {
        platform_mutex_unlock(&channel->mutex);
        return -1;
    }

    if (channel->capacity <= 1 && channel->count == 0) {
        while (channel->count >= channel->capacity && !channel->closed) {
            platform_cond_wait(&channel->not_full, &channel->mutex);
        }
        if (channel->closed) {
            platform_mutex_unlock(&channel->mutex);
            return -1;
        }
    }

    while (channel->count >= channel->capacity && !channel->closed) {
        platform_cond_wait(&channel->not_full, &channel->mutex);
    }

    if (channel->closed) {
        platform_mutex_unlock(&channel->mutex);
        return -1;
    }

    channel->buffer[channel->tail] = cloned;
    gc_write_barrier((Object*)channel, cloned);
    channel->tail = (channel->tail + 1) % channel->capacity;
    channel->count++;

    platform_cond_signal(&channel->not_empty);
    platform_mutex_unlock(&channel->mutex);

    return 0;
}

Value channel_receive(ObjChannel* channel) {
    platform_mutex_lock(&channel->mutex);

    while (channel->count == 0 && !channel->closed) {
        platform_cond_wait(&channel->not_empty, &channel->mutex);
    }

    if (channel->count == 0 && channel->closed) {
        platform_mutex_unlock(&channel->mutex);
        return val_null();
    }

    Value value = channel->buffer[channel->head];
    channel->buffer[channel->head] = val_null();
    channel->head = (channel->head + 1) % channel->capacity;
    channel->count--;

    platform_cond_signal(&channel->not_full);
    platform_mutex_unlock(&channel->mutex);

    return value;
}

int channel_try_send(ObjChannel* channel, Value value) {
    Value cloned = value_clone_for_channel(value);

    platform_mutex_lock(&channel->mutex);

    if (channel->closed || channel->count >= channel->capacity) {
        platform_mutex_unlock(&channel->mutex);
        return -1;
    }

    channel->buffer[channel->tail] = cloned;
    gc_write_barrier((Object*)channel, cloned);
    channel->tail = (channel->tail + 1) % channel->capacity;
    channel->count++;

    platform_cond_signal(&channel->not_empty);
    platform_mutex_unlock(&channel->mutex);

    return 0;
}

Value channel_try_receive(ObjChannel* channel) {
    platform_mutex_lock(&channel->mutex);

    if (channel->count == 0) {
        platform_mutex_unlock(&channel->mutex);
        return val_null();
    }

    Value value = channel->buffer[channel->head];
    channel->buffer[channel->head] = val_null();
    channel->head = (channel->head + 1) % channel->capacity;
    channel->count--;

    platform_cond_signal(&channel->not_full);
    platform_mutex_unlock(&channel->mutex);

    return value;
}

// 注意：Thread 和 Channel 的实例方法已移到 threads.c 模块中
