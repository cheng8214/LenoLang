#include "../../include/lenolang.h"
#include "../../include/leno_vm.h"
#include "../../include/leno_value.h"
#include "../../include/native.h"
#include "../../include/leno_types.h"
#include <string.h>

// ============================================================================
// async 模块 - 协程支持
// ============================================================================

static inline VM* get_current_vm(void) {
    extern THREAD_LOCAL VM* current_exec_vm;
    extern VM vm;
    return current_exec_vm ? current_exec_vm : &vm;
}

// async.sleep(ms) - 暂停指定毫秒，返回 Future
static Value native_async_sleep(int arg_count, Value* args) {
    if (arg_count < 1 || (!val_is_int(args[0]) && !val_is_float(args[0]))) {
        native_throw_error("async.sleep 需要一个数字参数（毫秒）");
        return val_null();
    }
    
    int ms = val_is_int(args[0]) ? val_as_int(args[0]) : (int)val_as_num(args[0]);
    
    ObjCoroutine* current = vm_current_coroutine();
    if (!current) {
        native_throw_error("async.sleep 只能在 async 函数中调用");
        return val_null();
    }
    
    ObjFuture* future = future_new();
    
    uint64_t wake_time = current_time_ms() + ms;
    VM* target_vm = get_current_vm();
    event_loop_add_timer(target_vm->event_loop, wake_time, current);
    
    future->waiter = current;
    current->waiting_for = future;
    
    return val_obj((Object*)future);
}

// async.run() - 启动事件循环，运行所有协程直到全部完成
static Value native_async_run(int arg_count, Value* args) {
    (void)arg_count;
    (void)args;

    // 子线程中 current_exec_vm 为 NULL，回退到主线程 &vm 会导致并发访问崩溃
    // 必须在主线程中调用 asyncs.run()
    extern THREAD_LOCAL VM* current_exec_vm;
    if (!current_exec_vm) {
        native_throw_error("asyncs.run() 只能在主线程调用，子线程不支持事件循环");
        return val_null();
    }

    VM* target_vm = current_exec_vm;
    if (!target_vm->event_loop) {
        native_throw_error("async.run 需要在初始化事件循环后调用");
        return val_null();
    }
    
    event_loop_run(target_vm->event_loop);
    
    // 检查是否有失败的协程
    EventLoop* loop = target_vm->event_loop;
    if (loop->error_count > 0) {
        if (loop->error_count == 1) {
            // 单个错误：直接设置异常，让 try-catch 捕获
            Value err = loop->errors[0];
            loop->error_count = 0;
            target_vm->exception = err;
            target_vm->has_exception = 1;
            target_vm->exception_line = native_get_current_line();
            return val_null();
        } else {
            // 多个错误：汇总为字符串异常
            char buf[512];
            int offset = snprintf(buf, sizeof(buf), "%d 个协程失败: ", loop->error_count);
            for (int i = 0; i < loop->error_count && offset < (int)sizeof(buf) - 64; i++) {
                if (i > 0) {
                    offset += snprintf(buf + offset, sizeof(buf) - offset, "; ");
                }
                char* err_str = value_to_string(loop->errors[i]);
                offset += snprintf(buf + offset, sizeof(buf) - offset, "%s", err_str);
                free(err_str);
            }
            loop->error_count = 0;
            ObjString* err_obj = str_copy(buf, strlen(buf));
            target_vm->exception = val_obj((Object*)err_obj);
            target_vm->has_exception = 1;
            target_vm->exception_line = native_get_current_line();
            return val_null();
        }
    }
    
    return val_null();
}

// async.yield() - 主动让出执行权
static Value native_async_yield(int arg_count, Value* args) {
    (void)arg_count;
    (void)args;
    
    ObjCoroutine* current = vm_current_coroutine();
    if (!current) {
        native_throw_error("async.yield 只能在 async 函数中调用");
        return val_null();
    }
    
    ObjFuture* future = future_new();
    future->waiter = current;
    
    VM* target_vm = get_current_vm();
    event_loop_add_ready(target_vm->event_loop, current);
    
    return val_obj((Object*)future);
}

// async.all(futures) - 等待所有 Future 完成，返回结果数组
static Value native_async_all(int arg_count, Value* args) {
    if (arg_count < 1 || !val_is_obj(args[0]) || val_as_obj(args[0])->type != OBJ_ARRAY) {
        native_throw_error("async.all 需要一个数组参数");
        return val_null();
    }
    
    ObjArray* futures_arr = (ObjArray*)val_as_obj(args[0]);
    int count = futures_arr->count;
    
    if (count == 0) {
        ObjArray* result = arr_new(0);
        return val_obj((Object*)result);
    }
    
    ObjArray* results = arr_new(count);
    for (int i = 0; i < count; i++) {
        results->elements[i] = val_null();
    }
    results->count = count;
    
    for (int i = 0; i < count; i++) {
        Value fv = futures_arr->elements[i];
        if (!val_is_obj(fv) || val_as_obj(fv)->type != OBJ_FUTURE) {
            results->elements[i] = fv;
            continue;
        }
        
        ObjFuture* future = (ObjFuture*)val_as_obj(fv);
        if (future->completed) {
            results->elements[i] = future->result;
        } else {
            results->elements[i] = val_null();
        }
    }
    
    return val_obj((Object*)results);
}

// async.timeout(future, ms) - 等待 Future，超时返回 null
static Value native_async_timeout(int arg_count, Value* args) {
    if (arg_count < 2) {
        native_throw_error("async.timeout 需要两个参数: future 和 timeout_ms");
        return val_null();
    }
    
    if (!val_is_obj(args[0]) || val_as_obj(args[0])->type != OBJ_FUTURE) {
        native_throw_error("async.timeout 第一个参数必须是 Future");
        return val_null();
    }
    
    if (!val_is_int(args[1]) && !val_is_float(args[1])) {
        native_throw_error("async.timeout 第二个参数必须是数字（毫秒）");
        return val_null();
    }
    
    ObjCoroutine* current = vm_current_coroutine();
    if (!current) {
        native_throw_error("async.timeout 只能在 async 函数中调用");
        return val_null();
    }
    
    ObjFuture* target = (ObjFuture*)val_as_obj(args[0]);
    int timeout_ms = val_is_int(args[1]) ? val_as_int(args[1]) : (int)val_as_num(args[1]);
    
    if (target->completed) {
        return target->result;
    }
    
    target->waiter = current;
    
    uint64_t wake_time = current_time_ms() + timeout_ms;
    VM* target_vm = get_current_vm();
    event_loop_add_timer(target_vm->event_loop, wake_time, current);
    
    current->waiting_for = target;
    
    return val_obj((Object*)target);
}

// 注册 asyncs 模块
void asyncs_init_module(void) {
    TypeKind sleep_params[] = {TYPE_INT};
    native_register_module_method("asyncs", "sleep", native_async_sleep, 1, -1, -1, TYPE_FUTURE, TYPE_UNKNOWN, sleep_params);

    native_register_module_method("asyncs", "run", native_async_run, 0, -1, -1, TYPE_NULL, TYPE_UNKNOWN, NULL);

    native_register_module_method("asyncs", "yield", native_async_yield, 0, -1, -1, TYPE_FUTURE, TYPE_UNKNOWN, NULL);

    TypeKind all_params[] = {TYPE_ARRAY};
    native_register_module_method("asyncs", "all", native_async_all, 1, -1, -1, TYPE_ARRAY, TYPE_ANY, all_params);

    TypeKind timeout_params[] = {TYPE_ANY, TYPE_INT};
    native_register_module_method("asyncs", "timeout", native_async_timeout, 2, -1, -1, TYPE_FUTURE, TYPE_UNKNOWN, timeout_params);
}

// 初始化 asyncs 模块全局变量和事件循环
void asyncs_init_globals(void) {
    VM* target_vm = get_current_vm();
    if (!target_vm->event_loop) {
        target_vm->event_loop = (EventLoop*)malloc(sizeof(EventLoop));
        event_loop_init(target_vm->event_loop);
        g_event_loop = target_vm->event_loop;
    }
}
