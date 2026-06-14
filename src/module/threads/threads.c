#include "../../include/native.h"
#include "../../include/leno_value.h"
#include "../../include/platform_thread.h"
#include "../../include/platform.h"
#include <string.h>

extern ObjThread* thread_new_with_args(ObjClosure* closure, Value* call_args, int call_arg_count);
extern Value thread_join(ObjThread* thread);
extern ThreadState thread_get_state(ObjThread* thread);
extern ObjChannel* channel_new(int capacity);
extern void channel_close(ObjChannel* channel);
extern int channel_send(ObjChannel* channel, Value value);
extern Value channel_receive(ObjChannel* channel);
extern int channel_try_send(ObjChannel* channel, Value value);
extern Value channel_try_receive(ObjChannel* channel);

// 外部声明：线程方法注册函数
extern void thread_register_method_with_params(const char* name, ObjNative* method, int arity,
                                              int min_arity, int max_arity,
                                              TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);
extern void channel_register_method_with_params(const char* name, ObjNative* method, int arity,
                                               int min_arity, int max_arity,
                                               TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);
// 外部声明：创建原生函数对象的辅助函数
extern ObjNative* make_native(NativeFn fn, int arity, const char* name);
// 外部声明：初始化方法表
extern void thread_init_methods(void);
extern void channel_init_methods(void);

// ==================== Thread 实例方法 ====================

static Value thread_method_join(int argc, Value* args) {
    (void)argc;
    ObjThread* thread = (ObjThread*)val_as_obj(args[0]);
    return thread_join(thread);
}

static Value thread_method_state(int argc, Value* args) {
    (void)argc;
    ObjThread* thread = (ObjThread*)val_as_obj(args[0]);
    ThreadState state = thread_get_state(thread);
    const char* state_str;
    switch (state) {
        case THREAD_RUNNING: state_str = "running"; break;
        case THREAD_DONE:    state_str = "done";    break;
        case THREAD_ERROR:   state_str = "error";   break;
        default:             state_str = "unknown"; break;
    }
    return val_obj((Object*)str_copy(state_str, (int)strlen(state_str)));
}

// ==================== Channel 实例方法 ====================

static Value channel_method_send(int argc, Value* args) {
    (void)argc;
    ObjChannel* channel = (ObjChannel*)val_as_obj(args[0]);
    Value value = args[1];
    int result = channel_send(channel, value);
    if (result != 0) {
        native_throw_error("Cannot send to closed channel");
        return val_null();
    }
    return val_null();
}

static Value channel_method_receive(int argc, Value* args) {
    (void)argc;
    ObjChannel* channel = (ObjChannel*)val_as_obj(args[0]);
    return channel_receive(channel);
}

static Value channel_method_close(int argc, Value* args) {
    (void)argc;
    ObjChannel* channel = (ObjChannel*)val_as_obj(args[0]);
    channel_close(channel);
    return val_null();
}

static Value channel_method_try_send(int argc, Value* args) {
    (void)argc;
    ObjChannel* channel = (ObjChannel*)val_as_obj(args[0]);
    Value value = args[1];
    int result = channel_try_send(channel, value);
    return val_bool(result == 0);
}

static Value channel_method_try_receive(int argc, Value* args) {
    (void)argc;
    ObjChannel* channel = (ObjChannel*)val_as_obj(args[0]);
    return channel_try_receive(channel);
}

static Value channel_method_is_closed(int argc, Value* args) {
    (void)argc;
    ObjChannel* channel = (ObjChannel*)val_as_obj(args[0]);
    return val_bool(channel->closed);
}

static Value channel_method_len(int argc, Value* args) {
    (void)argc;
    ObjChannel* channel = (ObjChannel*)val_as_obj(args[0]);
    platform_mutex_lock(&channel->mutex);
    int count = channel->count;
    platform_mutex_unlock(&channel->mutex);
    return val_num(count);
}

// ==================== 模块静态方法 ====================

static Value threads_start(int argc, Value* args) {
    if (argc < 1 || !val_is_obj(args[0]) || val_as_obj(args[0])->type != OBJ_CLOSURE) {
        native_throw_error("threads.start() requires a function argument");
        return val_null();
    }

    ObjClosure* closure = (ObjClosure*)val_as_obj(args[0]);

    Value* call_args = NULL;
    int call_arg_count = argc - 1;
    if (call_arg_count > 0) {
        call_args = (Value*)malloc(call_arg_count * sizeof(Value));
        if (call_args) {
            memcpy(call_args, &args[1], call_arg_count * sizeof(Value));
        }
    }

    ObjThread* thread = thread_new_with_args(closure, call_args, call_arg_count);
    if (call_args) free(call_args);
    if (!thread) {
        native_throw_error("Failed to create thread");
        return val_null();
    }

    return val_obj((Object*)thread);
}

static Value threads_channel(int argc, Value* args) {
    int capacity = 0;

    if (argc > 0 && val_is_int(args[0])) {
        capacity = (int)val_as_num(args[0]);
        if (capacity < 0) capacity = 0;
    }

    ObjChannel* channel = channel_new(capacity);
    if (!channel) {
        native_throw_error("Failed to create channel");
        return val_null();
    }

    return val_obj((Object*)channel);
}

static Value threads_sleep(int argc, Value* args) {
    if (argc < 1 || !val_is_int(args[0])) {
        native_throw_error("threads.sleep() requires an integer milliseconds argument");
        return val_null();
    }
    int ms = (int)val_as_num(args[0]);
    if (ms < 0) ms = 0;
    platform_sleep_ms((uint64_t)ms);
    return val_null();
}

// ==================== 实例方法初始化 ====================

void threads_init_instance_methods(void) {
    thread_init_methods();
    channel_init_methods();

    TypeKind no_params[] = {};
    TypeKind any_params[] = {TYPE_ANY};

    // Thread 实例方法
    thread_register_method_with_params("join", make_native(thread_method_join, 1, "join"), 0, -1, -1, TYPE_ANY, TYPE_UNKNOWN, no_params);
    thread_register_method_with_params("state", make_native(thread_method_state, 1, "state"), 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, no_params);

    // Channel 实例方法
    channel_register_method_with_params("send", make_native(channel_method_send, 2, "send"), 1, -1, -1, TYPE_ANY, TYPE_UNKNOWN, any_params);
    channel_register_method_with_params("receive", make_native(channel_method_receive, 1, "receive"), 0, -1, -1, TYPE_ANY, TYPE_UNKNOWN, no_params);
    channel_register_method_with_params("close", make_native(channel_method_close, 1, "close"), 0, -1, -1, TYPE_ANY, TYPE_UNKNOWN, no_params);
    channel_register_method_with_params("try_send", make_native(channel_method_try_send, 2, "try_send"), 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, any_params);
    channel_register_method_with_params("try_receive", make_native(channel_method_try_receive, 1, "try_receive"), 0, -1, -1, TYPE_ANY, TYPE_UNKNOWN, no_params);
    channel_register_method_with_params("is_closed", make_native(channel_method_is_closed, 1, "is_closed"), 0, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, no_params);
    channel_register_method_with_params("len", make_native(channel_method_len, 1, "len"), 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, no_params);
}

// ==================== 模块初始化 ====================

void threads_init_module(void) {
    TypeKind start_params[] = {TYPE_ANY};
    native_register_module_method("threads", "start", threads_start, -1, 1, -1, TYPE_THREAD, TYPE_UNKNOWN, start_params);

    TypeKind channel_params[] = {TYPE_INT};
    native_register_module_method("threads", "channel", threads_channel, 1, -1, -1, TYPE_CHANNEL, TYPE_UNKNOWN, channel_params);

    TypeKind sleep_params[] = {TYPE_INT};
    native_register_module_method("threads", "sleep", threads_sleep, 1, -1, -1, TYPE_ANY, TYPE_UNKNOWN, sleep_params);

    // 调用 threads_init_instance_methods 注册线程和通道实例方法
    threads_init_instance_methods();
}

void threads_init_globals(void) {
}
