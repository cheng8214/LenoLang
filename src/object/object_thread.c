#include "include/lenolang.h"
#include "include/native.h"
#include "include/platform_thread.h"
#include "include/leno_vm.h"
#include "include/method_table.h"
#include <stdlib.h>
#include <string.h>

// 子线程使用独立的 VM 数组（多线程模式下）
// 主程序直接使用全局 vm，保持效率第一

// 外部函数声明
extern int vm_run_coroutine(ObjCoroutine* co);

// ============================================================================
// Thread 方法注册表（使用通用 MethodTable）
// ============================================================================

#define THREAD_METHOD_TABLE_INITIAL_CAPACITY 16

static THREAD_LOCAL MethodTable threadMethodTable = {NULL, 0, 0};

void thread_register_method_with_params(const char* name, ObjNative* method, int arity,
                                         int min_arity, int max_arity,
                                         TypeKind return_type, TypeKind return_element_type, TypeKind* param_types) {
    method_table_register_with_params(&threadMethodTable, "Thread", name, method, arity, min_arity, max_arity, return_type, return_element_type, param_types);
}

ObjNative* thread_find_method(const char* name) {
    return method_table_find(&threadMethodTable, name);
}

void thread_init_methods(void) {
    method_table_init_methods(&threadMethodTable, THREAD_METHOD_TABLE_INITIAL_CAPACITY);
}

void thread_mark_methods(void) {
    method_table_mark(&threadMethodTable);
}

// ============================================================================
// Channel 方法注册表（使用通用 MethodTable）
// ============================================================================

#define CHANNEL_METHOD_TABLE_INITIAL_CAPACITY 16

static THREAD_LOCAL MethodTable channelMethodTable = {NULL, 0, 0};

void channel_register_method_with_params(const char* name, ObjNative* method, int arity,
                                          int min_arity, int max_arity,
                                          TypeKind return_type, TypeKind return_element_type, TypeKind* param_types) {
    method_table_register_with_params(&channelMethodTable, "Channel", name, method, arity, min_arity, max_arity, return_type, return_element_type, param_types);
}

ObjNative* channel_find_method(const char* name) {
    return method_table_find(&channelMethodTable, name);
}

void channel_init_methods(void) {
    method_table_init_methods(&channelMethodTable, CHANNEL_METHOD_TABLE_INITIAL_CAPACITY);
}

void channel_mark_methods(void) {
    method_table_mark(&channelMethodTable);
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
                    // 拷贝数组部分
                    if (dict->array && dict->asize > 0) {
                        for (int i = 0; i < dict->asize && i < copy->capacity; i++) {
                            copy->array[i] = value_clone_for_channel(dict->array[i]);
                            copy->asize++;
                            copy->acount++;
                        }
                    }
                    // 拷贝哈希表部分
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
    ObjStructDef** struct_defs;   // 主线程结构体定义快照（供子线程注册）
    int struct_def_count;
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
    ObjStructDef** saved_struct_defs = args->struct_defs;
    int saved_struct_def_count = args->struct_def_count;
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
    for (int i = 0; i < IC_NATIVE_CACHE_SIZE; i++) {
        child_vm.ic_native_cache[i].valid = 0;
        child_vm.ic_native_cache[i].fn = NULL;
    }
    for (int i = 0; i < IC_MODULE_CACHE_SIZE; i++) {
        child_vm.ic_module_cache[i].valid = 0;
        child_vm.ic_module_cache[i].combined_hash = 0;
        child_vm.ic_module_cache[i].meta = NULL;
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
    native_init_module("regexs");
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
    extern void cstructs_init_methods(void);
    cstructs_init_methods();
    extern void threads_init_instance_methods(void);
    threads_init_instance_methods();

    // ★ 将主线程已定义的 struct 类型导入子线程的定义表。
    // 普通 struct 定义表是线程局部，子线程默认为空；
    // 不导入则子线程 new StructName() 会报"未定义的结构体"。
    // struct 定义是只读类型元数据，与 cstruct 一样可跨线程共享。
    if (saved_struct_defs && saved_struct_def_count > 0) {
        struct_def_import_from_thread(saved_struct_defs, saved_struct_def_count);
    }

    // ★ 始终在子线程 GC 堆上创建新的闭包副本。
    // 原始闭包在主线程 GC 堆上，主线程 GC 可能在闭包不再被主线程引用时回收它，
    // 导致子线程访问已释放的 ObjClosure → use-after-free。
    // function 对象是只读的且通常作为全局函数常驻，可以安全共享。
    ObjClosure* child_closure = (ObjClosure*)gc_alloc(sizeof(ObjClosure), OBJ_CLOSURE);
    if (!child_closure) {
        platform_mutex_lock(&thread_obj->mutex);
        thread_obj->state = THREAD_ERROR;
        thread_obj->error_msg = strdup("Failed to allocate closure for thread");
        thread_obj->has_result = 0;
        platform_cond_signal(&thread_obj->done_cond);
        platform_mutex_unlock(&thread_obj->mutex);
        if (saved_globals) free(saved_globals);
        if (saved_global_funcs) free(saved_global_funcs);
        if (saved_struct_defs) free(saved_struct_defs);
        if (thread_initial_args) free(thread_initial_args);
        if (child_vm.frames) free(child_vm.frames);
        if (child_vm.stack) free(child_vm.stack);
        if (child_vm.globals) free(child_vm.globals);
        if (child_vm.global_funcs) free(child_vm.global_funcs);
        return NULL;
    }
    child_closure->function = closure->function;
    child_closure->upvalue_count = closure->upvalue_count;
    child_closure->type_param_count = closure->type_param_count;
    child_closure->type_param_args = NULL;  // 不深拷贝类型参数（泛型场景由调用方处理）
    if (closure->upvalue_count > 0) {
        for (int i = 0; i < closure->upvalue_count; i++) {
            // 读取原始 upvalue 的当前值，创建 closed upvalue
            Value upval = *closure->upvalues[i]->location;
            Value cloned = value_clone_for_channel(upval);
            Upvalue* uv = (Upvalue*)gc_alloc(sizeof(Upvalue), OBJ_UPVALUE);
            if (!uv) {
                child_closure->upvalue_count = i;  // 部分初始化
                break;
            }
            uv->location = &uv->closed;
            uv->closed = cloned;
            uv->next = NULL;
            child_closure->upvalues[i] = uv;
        }
    }

    // 创建协程对象来执行函数
    ObjCoroutine* co = coroutine_new(child_closure);
    if (!co) {
        platform_mutex_lock(&thread_obj->mutex);
        thread_obj->state = THREAD_ERROR;
        thread_obj->error_msg = strdup("Failed to create coroutine");
        thread_obj->has_result = 0;
        platform_cond_signal(&thread_obj->done_cond);
        platform_mutex_unlock(&thread_obj->mutex);

        if (saved_globals) free(saved_globals);
        if (saved_global_funcs) free(saved_global_funcs);
        if (saved_struct_defs) free(saved_struct_defs);
        if (thread_initial_args) free(thread_initial_args);

        if (child_vm.frames) free(child_vm.frames);
        if (child_vm.stack) free(child_vm.stack);
        if (child_vm.globals) free(child_vm.globals);
        if (child_vm.global_funcs) free(child_vm.global_funcs);

        return NULL;
    }

    // ★ 从 thread->pending_args 读取参数（已在主线程 GC 堆上深拷贝），
    // re-clone 到子线程 GC 堆。读取和清 NULL 都在 mutex 保护下进行，
    // 与主线程 GC 的 gc_scan_children(OBJ_THREAD) 同步。
    // thread_initial_args 已在函数开头声明（从 args->initial_args 初始化为 NULL）
    platform_mutex_lock(&thread_obj->mutex);
    if (thread_obj->pending_args && thread_obj->pending_arg_count > 0) {
        thread_initial_arg_count = thread_obj->pending_arg_count;
        thread_initial_args = (Value*)malloc(thread_initial_arg_count * sizeof(Value));
        if (thread_initial_args) {
            // ★ 禁用同步 GC：防止 value_clone_for_channel → gc_alloc → gc_major_collect
            // → gc_scan_children(OBJ_THREAD) → lock thread->mutex → 自死锁
            int saved_gc_enabled = gc_get_enabled();
            gc_set_enabled(0);
            for (int i = 0; i < thread_initial_arg_count; i++) {
                thread_initial_args[i] = value_clone_for_channel(thread_obj->pending_args[i]);
            }
            gc_set_enabled(saved_gc_enabled);
        }
        // 清 NULL，主线程 GC 不再扫描这些值
        free(thread_obj->pending_args);
        thread_obj->pending_args = NULL;
        thread_obj->pending_arg_count = 0;
    }
    platform_mutex_unlock(&thread_obj->mutex);

    // 设置协程的初始参数（已 clone 到子线程 GC 堆）
    if (thread_initial_args && thread_initial_arg_count > 0) {
        co->initial_args = thread_initial_args;
        co->initial_arg_count = thread_initial_arg_count;
    }

    int result = vm_run_coroutine_with_vm(co, &child_vm);

    // ★ 统一在 mutex 内完成状态设置 + 最终 GC + 信号通知
    // 参考 Go goroutine 退出时的处理：Go 在 goroutine 退出时会清理栈上资源，
    // 但堆对象由共享 GC 管理。Leno 使用 per-thread GC heap，因此需要在退出前
    // 主动运行一次 GC 回收不可达对象（临时字符串、局部变量等），减少内存泄漏。
    //
    // 关键：gc_collect 必须在持有 thread mutex 时执行，因为：
    // 1. 主线程 join() 在 cond_wait 上等待，signal 后尝试获取 mutex
    // 2. GC 期间 thread_obj->result 可能被标记/扫描，必须确保主线程不会并发访问
    // 3. GC 期间 channel buffer 条目通过 child_vm.globals → channel → buffer 标记为存活
    // 4. thread_obj->result 通过 gc_push_root 标记为存活，不会被误回收
    platform_mutex_lock(&thread_obj->mutex);

    if (result != 0 || child_vm.has_exception) {
        thread_obj->state = THREAD_ERROR;
        if (child_vm.has_exception && val_is_obj(child_vm.exception) && val_as_obj(child_vm.exception)->type == OBJ_STRING) {
            ObjString* err = (ObjString*)val_as_obj(child_vm.exception);
            thread_obj->error_msg = strdup(err->chars);
        } else {
            thread_obj->error_msg = strdup("unknown error");
        }
        thread_obj->has_result = 0;
    } else {
        thread_obj->state = THREAD_DONE;
        // 从协程对象中获取结果
        thread_obj->result = value_clone_for_channel(co->result);
        gc_write_barrier((Object*)thread_obj, thread_obj->result);
        thread_obj->has_result = 1;
    }

    platform_cond_signal(&thread_obj->done_cond);

    // 最终 GC：回收子线程 GC 堆上的不可达对象
    // ★ 关键：不能在此处执行 GC 回收 channel buffer 中的对象。
    // value_clone_for_channel 在 channel_send 中通过 gc_alloc 在子线程 GC 堆上分配对象，
    // 这些对象存储在 channel buffer 中，但子线程退出时 VM 状态已清空，
    // GC 无法通过根集合→栈→channel→buffer 追踪到它们，会误回收 → use-after-free。
    // 修复：禁用 GC，让 gc_collect 成为空操作。子线程 GC 堆上的对象会在进程退出时
    // 由 gc_free_all 统一释放，不会永久泄漏。
    gc_push_root(&thread_obj->result);
    child_vm.frame_cnt = 0;
    child_vm.sp = 0;
    child_vm.current_coroutine = NULL;
    child_vm.all_coroutines = NULL;
    child_vm.open_upvalues = NULL;
    child_vm.exception = val_null();
    child_vm.has_exception = 0;
    int saved_gc_enabled = gc_get_enabled();
    gc_set_enabled(0);
    gc_collect();
    gc_set_enabled(saved_gc_enabled);
    gc_pop_root();

    platform_mutex_unlock(&thread_obj->mutex);

    if (saved_globals) free(saved_globals);
    if (saved_global_funcs) free(saved_global_funcs);
    if (saved_struct_defs) free(saved_struct_defs);

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
    thread->joined = 0;
    thread->pending_args = NULL;
    thread->pending_arg_count = 0;

    platform_mutex_init(&thread->mutex);
    platform_cond_init(&thread->done_cond);

    // ★ 深拷贝线程参数到主线程 GC 堆，并存入 thread->pending_args。
    // 这样主线程 GC 可以通过 gc_scan_children(OBJ_THREAD) 扫描到这些值，
    // 防止子线程读取前被 GC 回收（多线程崩溃的主要根因）。
    // 子线程在 thread_entry_point 中 re-clone 到自己的 GC 堆后清 NULL。
    if (call_args && call_arg_count > 0) {
        platform_mutex_lock(&thread->mutex);
        thread->pending_args = (Value*)malloc(call_arg_count * sizeof(Value));
        if (thread->pending_args) {
            // ★ 禁用同步 GC：value_clone_for_channel → gc_alloc 在 malloc 失败时
            // 会同步调用 gc_major_collect()，此时 GC 的 gc_scan_children(OBJ_THREAD)
            // 会尝试 lock thread->mutex，但当前线程已持有 → 自死锁。
            int saved_gc_enabled = gc_get_enabled();
            gc_set_enabled(0);
            for (int i = 0; i < call_arg_count; i++) {
                thread->pending_args[i] = value_clone_for_channel(call_args[i]);
            }
            gc_set_enabled(saved_gc_enabled);
            thread->pending_arg_count = call_arg_count;
        }
        platform_mutex_unlock(&thread->mutex);
    }

    ThreadStartArgs* args = (ThreadStartArgs*)malloc(sizeof(ThreadStartArgs));
    if (!args) {
        native_throw_error("Failed to allocate thread args");
        return NULL;
    }
    memset(args, 0, sizeof(ThreadStartArgs));
    args->closure = closure;
    args->thread_obj = thread;
    // initial_args 不再存储在 args 中，改为存储在 thread->pending_args（GC 可见）
    args->initial_args = NULL;
    args->initial_arg_count = 0;
    
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

    // ★ 抓取主线程的结构体定义快照，供子线程注册。
    // 普通 struct 的定义表是线程局部（THREAD_LOCAL），子线程默认是空表，
    // 若不导入，子线程里 new HtmlNode() 会报"未定义的结构体"。
    // struct 定义是只读类型元数据，跨线程共享（与 cstruct 一致）。
    args->struct_def_count = struct_def_get_count();
    args->struct_defs = NULL;
    if (args->struct_def_count > 0) {
        args->struct_defs = (ObjStructDef**)malloc(args->struct_def_count * sizeof(ObjStructDef*));
        if (args->struct_defs) {
            for (int i = 0; i < args->struct_def_count; i++) {
                args->struct_defs[i] = struct_def_get(i);
            }
        }
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

    // ★ 在 mutex 内克隆 result 到主线程 GC 堆。
    // thread->result 是子线程在 thread_entry_point 中通过 value_clone_for_channel
    // 分配在子线程 GC 堆上的。子线程退出后其 GC 堆被遗弃，如果主线程 GC
    // 试图扫描该对象（通过 gc_scan_children(OBJ_THREAD)），会访问子线程堆内存——
    // 虽然 malloc 不会回收，但 marked 标志会 stale，导致 GC 跳过扫描其子对象。
    // 克隆到主线程堆后，GC 可正确管理。
    //
    // ★ 禁用同步 GC：value_clone_for_channel → gc_alloc → gc_major_collect
    // → gc_scan_children(OBJ_THREAD) → lock thread->mutex → 自死锁
    Value result_copy = val_null();
    if (thread->has_result) {
        int saved_gc_enabled = gc_get_enabled();
        gc_set_enabled(0);
        result_copy = value_clone_for_channel(thread->result);
        gc_set_enabled(saved_gc_enabled);
        thread->result = result_copy;
        gc_write_barrier((Object*)thread, result_copy);
    }

    platform_mutex_unlock(&thread->mutex);

    if (!thread->joined) {
        platform_thread_join(thread->os_thread, NULL);
        thread->joined = 1;
    }

    // 从 active_threads 中移除
    extern THREAD_LOCAL VM* current_exec_vm;
    extern VM vm;
    VM* target_vm = current_exec_vm ? current_exec_vm : &vm;
    if (target_vm->active_threads) {
        for (int i = 0; i < target_vm->active_thread_count; i++) {
            if (target_vm->active_threads[i] == thread) {
                target_vm->active_threads[i] = target_vm->active_thread_count > 1
                    ? target_vm->active_threads[target_vm->active_thread_count - 1]
                    : NULL;
                target_vm->active_thread_count--;
                break;
            }
        }
    }

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
    platform_mutex_lock(&thread->mutex);
    ThreadState state = thread->state;
    platform_mutex_unlock(&thread->mutex);
    return state;
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

    // ★ 先克隆再移除（关键修复）
    // 与 channel_try_receive 相同：必须先克隆再从 buffer 移除，
    // 否则在 unlock 到 clone 之间发送线程 GC 可能释放该对象。
    //
    // ★ 同步 GC 死锁防护：
    // value_clone_for_channel → gc_alloc 在 malloc 失败时会同步调用
    // gc_major_collect()，此时 GC 的 gc_scan_children 会尝试 lock channel
    // mutex，但当前线程已持有该 mutex → 死锁。
    // 解决：临时禁用 GC，gc_alloc 失败时仅返回 NULL 而不触发 gc_major_collect。
    int saved_gc_enabled = gc_get_enabled();
    gc_set_enabled(0);
    Value re_cloned = value_clone_for_channel(value);
    gc_set_enabled(saved_gc_enabled);

    channel->buffer[channel->head] = val_null();
    channel->head = (channel->head + 1) % channel->capacity;
    channel->count--;

    platform_cond_signal(&channel->not_full);
    platform_mutex_unlock(&channel->mutex);

    return re_cloned;
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

    // ★ 先克隆再移除（关键修复）
    // channel buffer 中的对象是在发送线程的 GC 上分配的。
    // 如果先从 buffer 移除再克隆，在 unlock mutex 到 value_clone_for_channel 之间
    // 存在一个危险窗口：发送线程的 GC 可能在 OP_RETURN 安全点触发，
    // 此时对象已不在 buffer 中（不可达），会被 sweep 释放，
    // 导致接收线程 value_clone_for_channel 读取已释放内存 → use-after-free 崩溃。
    //
    // 先克隆确保克隆期间对象仍在 buffer 中，被发送线程的 mark_roots
    // 通过 channel（在发送线程栈上）标记为存活，不会被 GC 回收。
    //
    // ★ 同步 GC 死锁防护（与 channel_receive 相同）：
    // value_clone_for_channel → gc_alloc 在 malloc 失败时同步调用 gc_major_collect()
    // → gc_scan_children 尝试 lock channel mutex → 死锁（当前线程已持有 mutex）。
    // 解决：临时禁用 GC，gc_alloc 失败时仅返回 NULL 而不触发同步 GC。
    int saved_gc_enabled = gc_get_enabled();
    gc_set_enabled(0);
    Value re_cloned = value_clone_for_channel(value);
    gc_set_enabled(saved_gc_enabled);

    channel->buffer[channel->head] = val_null();
    channel->head = (channel->head + 1) % channel->capacity;
    channel->count--;

    platform_cond_signal(&channel->not_full);
    platform_mutex_unlock(&channel->mutex);

    return re_cloned;
}

// 注意：Thread 和 Channel 的实例方法已移到 threads.c 模块中
