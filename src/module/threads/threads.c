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
extern void thread_register_method(const char* name, ObjNative* method,
                                  TypeKind return_type, TypeKind return_element_type,
                                  NativeParamSpec params);
extern void channel_register_method(const char* name, ObjNative* method,
                                  TypeKind return_type, TypeKind return_element_type,
                                  NativeParamSpec params);
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
    platform_mutex_lock(&channel->mutex);
    int closed = channel->closed;
    platform_mutex_unlock(&channel->mutex);
    return val_bool(closed);
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

    // ★ 模块函数作线程入口：**2026-10-12 起支持**（TH2，见 docs/线程并发能力修复追踪.md）✓
    //   原先这里硬拦"入口必须是入口脚本（顶层文件）里的具名全局函数" ✗（判据
    //   `closure->function->module != NULL`，见 docs/语言摩擦与隐式契约清单.md 的 M1 ✓）。
    //   根因其实只有一处：协程首帧漏设 `frame->module`（`src/vm/vm.c` 的
    //   `vm_run_coroutine_with_vm`），而模块字节码全走 `frame->module` 寻址
    //   ⇒ 模块函数体里第一条模块符号指令就报「不在模块上下文中」✗（症状是 join() 只给
    //   "Thread error: unknown error"，连函数体都进不去 ✓）。补上那一行之后，
    //   子线程直接跑模块函数是可行的 ✓（模块对象由 `loaded_modules` 进程级缓存共享，
    //   init_chunk 主线程已跑过 ⇒ globals / 函数槽都在 ✓）。
    //   ⚠ 仍要拦的**一种**情况：该模块此刻**还没初始化**（globals 全空）⇒ 进去只会拿到
    //     一堆 null、报错毫无线索 ⇒ 这里把话说清楚（正常流程下不可能发生：入口脚本 import
    //     的模块在其 chunk 开头就跑过 OP_INIT_LENOMODULE ✓）。
    if (closure->function && closure->function->module) {
        ObjModule* mod = (ObjModule*)closure->function->module;
        if (!mod->initialized) {
            char msg[512];
            snprintf(msg, sizeof(msg),
                "threads.start(): 入口函数 '%s' 所属的模块 '%s' 尚未初始化 ✗ —— "
                "模块的 init_chunk 还没跑过（globals 是空的）⇒ 进去只会拿到 null。"
                "常见原因：该模块没有出现在入口脚本的 import 链上（或用了非常规的加载方式）。"
                "解决办法：在入口脚本里 `import` 该模块（正常用法）✓",
                closure->function->name ? closure->function->name : "?",
                mod->name ? mod->name : "?");
            native_throw_error(msg);
            return val_null();
        }
    }

    // ★ 实参个数校验（B13，2026-10-07）：在**主线程**把话说清楚。
    //   此前少传实参的症状是 `join()` 抛
    //   "Thread error: 非字符串异常（对象类型 2）—— 常见于**进入函数体之前**就失败
    //   （如入口函数所属模块未加载）"，把人往"模块没加载"上引（实录 B13 副发现 1）。
    //   ⚠ 判据用**严格相等**：Leno 的默认参数是**调用点**补齐的（codegen 的 fill_default_args），
    //     而线程入口是 VM 直接调用 ⇒ 默认值不会被补，少传就是真错位。
    if (closure->function) {
        int want = closure->function->arity;
        int got = argc - 1;
        if (got != want) {
            char amsg[256];
            snprintf(amsg, sizeof(amsg),
                "threads.start(): 入口函数期望 %d 个参数，实际传了 %d 个"
                "（线程入口由 VM 直接调用，默认参数不会被补齐）",
                want, got);
            native_throw_error(amsg);
            return val_null();
        }
    }

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

    TypeKind any_params[] = {TYPE_ANY};

    // Thread 实例方法
    thread_register_method("join", make_native(thread_method_join, 1, "join"), TYPE_ANY, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    thread_register_method("state", make_native(thread_method_state, 1, "state"), TYPE_STRING, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    // join 返回规格 = **接收者的 T**（ARG0_ELEM）：`threads.start` 的语义特判返回 Thread[T]
    //   （T = 闭包/函数的返回类型，见 semantic_type.c 的 ⓪-b）⇒ `t.join()` 直接得到 T，
    //   调用点不再需要 `is Array[string] => x` 手工收窄 ✓（推不出 T 时 start 回落裸 Thread、
    //   这条规格解析不出元素 ⇒ join 退回 any —— 宁漏勿误报，与 map 同口径 ✓）
    native_register_instance_method_return_spec("Thread", "join", &NATIVE_T_ARG0_ELEM);

    // Channel 实例方法
    channel_register_method("send", make_native(channel_method_send, 2, "send"), TYPE_ANY, TYPE_UNKNOWN, NATIVE_FIXED(any_params));
    // send/close 的实现都是 `return val_null()`（只产生副作用）⇒ 返回类型收紧为 `null`（v3.2.6）。
    //   receive/try_receive 保持 any **不是**偷懒：语言里没有 `Channel[T]` 标注（实测 .leno 源码 0 处）
    //   ⇒ 通道没有"元素类型"可推，any 是当前设计的正确结果（要精确得先给 Channel 加类型参数）。
    native_register_instance_method_return_spec("Channel", "send", &NATIVE_T_NULL);
    channel_register_method("receive", make_native(channel_method_receive, 1, "receive"), TYPE_ANY, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    channel_register_method("close", make_native(channel_method_close, 1, "close"), TYPE_ANY, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    native_register_instance_method_return_spec("Channel", "close", &NATIVE_T_NULL);
    channel_register_method("try_send", make_native(channel_method_try_send, 2, "try_send"), TYPE_BOOL, TYPE_UNKNOWN, NATIVE_FIXED(any_params));
    channel_register_method("try_receive", make_native(channel_method_try_receive, 1, "try_receive"), TYPE_ANY, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    channel_register_method("is_closed", make_native(channel_method_is_closed, 1, "is_closed"), TYPE_BOOL, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    channel_register_method("len", make_native(channel_method_len, 1, "len"), TYPE_INT, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
}

// ==================== 模块初始化 ====================

void threads_init_module(void) {
    TypeKind start_params[] = {TYPE_ANY};
    // start 返回 Thread[T]（T = 回调返回类型，⓪ 族声明化）：join 的 ARG0_ELEM 规格读这个 T ✓
    native_register_module_method("threads", "start", threads_start, &NATIVE_T_THREAD_CB_RET0, NATIVE_VARARG(1, NATIVE_ARITY_ANY, 1, start_params, TYPE_ANY));

    TypeKind channel_params[] = {TYPE_INT};
    native_register_module_method("threads", "channel", threads_channel, &NATIVE_T_CHANNEL, NATIVE_FIXED(channel_params));

    TypeKind sleep_params[] = {TYPE_INT};
    // 返回 `null`（v3.2.6）：实现就是 `return val_null()`（睡完不产生值）⇒ 别再说它是 any。
    native_register_module_method("threads", "sleep", threads_sleep, &NATIVE_T_NULL, NATIVE_FIXED(sleep_params));

    // 调用 threads_init_instance_methods 注册线程和通道实例方法
    threads_init_instance_methods();
}

void threads_init_globals(void) {
}
