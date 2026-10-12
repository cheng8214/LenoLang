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

// ==================== 入口校验（start / parallel_for / map 共用 —— 单一实现）====================
//
// ★ 两条判据（都在**主线程**把话说清楚，别让用户从 join() 的垃圾文案里猜）：
//   ① 所属模块必须**已初始化**（TH2，2026-10-12）：
//      模块函数现在可以直接作线程入口 ✓（根因只是协程首帧漏设 `frame->module`，
//      见 `docs/线程并发能力修复追踪.md` 的 TH2）。模块对象由 `loaded_modules` **进程级缓存**共享，
//      init_chunk 主线程已跑过 ⇒ globals / 函数槽都在 ✓。**唯一例外**是模块此刻还没初始化
//      （不在入口脚本的 import 链上）⇒ globals 全空、进去只会拿到一堆 null、报错毫无线索 ✗
//      ⇒ 这里明确拦住（正常流程下不可能发生 ✓）。
//   ② 实参个数必须**恰好**等于形参个数（B13，2026-10-07）：
//      此前少传实参的症状是 `join()` 抛「非字符串异常…常见于进入函数体之前就失败」
//      —— 把人往"模块没加载"上引 ✗。
//      ⚠ 判据用**严格相等**：Leno 的默认参数是**调用点**补齐的（codegen 的 fill_default_args），
//        而线程入口由 VM 直接调用 ⇒ 默认值不会被补，少传就是真错位 ✓。
//
// 返回 0 = 通过；返回 -1 = 已报错（调用方直接 `return val_null()`）
static int threads_check_entry(ObjClosure* closure, int got_args, const char* who) {
    if (!closure || !closure->function) {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s: 入口不是函数", who);
        native_throw_error(msg);
        return -1;
    }

    if (closure->function->module) {
        ObjModule* mod = (ObjModule*)closure->function->module;
        if (!mod->initialized) {
            char msg[512];
            snprintf(msg, sizeof(msg),
                "%s: 入口函数 '%s' 所属的模块 '%s' 尚未初始化 ✗ —— "
                "模块的 init_chunk 还没跑过（globals 是空的）⇒ 进去只会拿到 null。"
                "常见原因：该模块没有出现在入口脚本的 import 链上（或用了非常规的加载方式）。"
                "解决办法：在入口脚本里 `import` 该模块（正常用法）✓",
                who,
                closure->function->name ? closure->function->name : "?",
                mod->name ? mod->name : "?");
            native_throw_error(msg);
            return -1;
        }
    }

    int want = closure->function->arity;
    if (got_args != want) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "%s: 入口函数期望 %d 个参数，实际传了 %d 个"
            "（线程入口由 VM 直接调用，默认参数不会被补齐）",
            who, want, got_args);
        native_throw_error(msg);
        return -1;
    }

    return 0;
}

// ==================== 模块静态方法 ====================

static Value threads_start(int argc, Value* args) {
    if (argc < 1 || !val_is_obj(args[0]) || val_as_obj(args[0])->type != OBJ_CLOSURE) {
        native_throw_error("threads.start() requires a function argument");
        return val_null();
    }

    ObjClosure* closure = (ObjClosure*)val_as_obj(args[0]);

    if (threads_check_entry(closure, argc - 1, "threads.start()") != 0) {
        return val_null();
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

// ==================== 并行原语（TH3，2026-10-12）====================
//
// 设计取向：**不加新对象类型、不加新关键字**（语言对"新特性"的一贯取向是够用就行 ✓）
//   ⇒ "凑齐 N 个线程"用 `join_all(Array[Thread])`、"分 N 份并行"用 `parallel_for(n, f)`、
//     "并行 map"用 `map(arr, f)` —— 三个普通模块函数，零类型系统改动。
// 见 `docs/线程并发能力修复追踪.md` 的 TH3 ✓

// 等一组线程结束（“WaitGroup”的等价物）—— `Array[Thread]`，返回真正 joined 的个数（int）
//   ⚠ 某个线程出错时：`thread_join` 会登记异常但**不跳转** ⇒ 这里把**剩下的也等完**再让异常浮出来
//     （不然会留下没人 join 的活线程 ✓）
static Value threads_join_all(int argc, Value* args) {
    if (argc < 1 || !val_is_obj(args[0]) || val_as_obj(args[0])->type != OBJ_ARRAY) {
        native_throw_error("threads.join_all() 需要一个 Array[Thread] 参数");
        return val_null();
    }
    ObjArray* arr = (ObjArray*)val_as_obj(args[0]);
    int joined = 0;
    for (int i = 0; i < arr->count; i++) {
        Value v = arr->elements[i];
        if (val_is_obj(v) && val_as_obj(v)->type == OBJ_THREAD) {
            thread_join((ObjThread*)val_as_obj(v));
            joined++;
        }
    }
    return val_int(joined);
}

// 分 n 份并行：`f(int index)` 在 n 个线程上各跑一次（index = 0..n-1），等全部结束。
//   返回值 = **真正起来的线程数**（int；创建失败时可能小于 n）。
//   ⚠ 并行度要自己控（`n` 不是"自动按核数"）—— 与 `parallelFor` 的语义一致：说几份就几份。
static Value threads_parallel_for(int argc, Value* args) {
    if (argc < 2 || !val_is_int(args[0]) ||
        !val_is_obj(args[1]) || val_as_obj(args[1])->type != OBJ_CLOSURE) {
        native_throw_error("threads.parallel_for(n, f) 需要 (int, 函数) 两个参数");
        return val_null();
    }

    int n = (int)val_as_num(args[0]);
    ObjClosure* closure = (ObjClosure*)val_as_obj(args[1]);

    // 回调形态：f(index) —— 一个参数
    if (threads_check_entry(closure, 1, "threads.parallel_for()") != 0) {
        return val_null();
    }
    if (n <= 0) return val_int(0);

    ObjThread** threads = (ObjThread**)malloc(sizeof(ObjThread*) * (size_t)n);
    if (!threads) {
        native_throw_error("threads.parallel_for(): 内存不足");
        return val_null();
    }

    int started = 0;
    for (int i = 0; i < n; i++) {
        Value idx = val_int(i);
        ObjThread* t = thread_new_with_args(closure, &idx, 1);
        if (!t) break;                  // 起不来就停手，把已起来的等完
        threads[started++] = t;
    }
    for (int i = 0; i < started; i++) {
        thread_join(threads[i]);
    }
    free(threads);
    return val_int(started);
}

// 并行 map：`f(item, index)` 每个元素起一个线程（**回调形态与 arrays.map 同口径** ✓），
//   按**输入顺序**收回结果 ⇒ `Array[回调返回类型]`（返回类型规格见 threads_init_module ✓）。
//   ⚠ 代价：线程数 = 元素数（元素多时线程开销会盖过收益）⇒ 重活请用 `parallel_for` 自己分块 ✓
static Value threads_map(int argc, Value* args) {
    if (argc < 2 ||
        !val_is_obj(args[0]) || val_as_obj(args[0])->type != OBJ_ARRAY ||
        !val_is_obj(args[1]) || val_as_obj(args[1])->type != OBJ_CLOSURE) {
        native_throw_error("threads.map(arr, f) 需要 (Array, 函数) 两个参数");
        return val_null();
    }

    ObjArray* arr = (ObjArray*)val_as_obj(args[0]);
    ObjClosure* closure = (ObjClosure*)val_as_obj(args[1]);

    if (threads_check_entry(closure, 2, "threads.map()") != 0) {
        return val_null();
    }

    int count = arr->count;
    ObjArray* result = arr_new(count > 0 ? count : 1);
    if (!result) return val_null();
    if (count == 0) return val_obj((Object*)result);

    ObjThread** threads = (ObjThread**)malloc(sizeof(ObjThread*) * (size_t)count);
    if (!threads) {
        native_throw_error("threads.map(): 内存不足");
        return val_null();
    }

    int started = 0;
    for (int i = 0; i < count; i++) {
        Value a[2];
        a[0] = arr->elements[i];
        a[1] = val_int(i);
        ObjThread* t = thread_new_with_args(closure, a, 2);
        if (!t) break;
        threads[started++] = t;
    }
    // 按输入顺序 join 并收集（`thread_join` 已把结果克隆到本线程堆 ✓）
    for (int i = 0; i < started; i++) {
        arr_write(result, i, thread_join(threads[i]));
    }
    free(threads);
    return val_obj((Object*)result);
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
    //   ★ 2026-10-12（TH4）起语言里**有** `Channel[T]` 标注了 ⇒ 下面 receive 的返回类型
    //     从 any 收紧成"接收者的元素类型"（旧注释写"语言里没有 Channel[T]" ✗ 已作废）。
    //   ⚠ `send(v)` 的**形参**仍是 `any`：要按 T 校验实参得给参数也上元素规格（另一件事，未做）✗
    native_register_instance_method_return_spec("Channel", "send", &NATIVE_T_NULL);
    channel_register_method("receive", make_native(channel_method_receive, 1, "receive"), TYPE_ANY, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    // ★ receive 的返回类型 = **接收者的元素类型**（TH4，2026-10-12）：
    //   `Channel[int] ch` ⇒ `ch.receive()` 就是 `int`，不再需要 `_int(...)` 或 `is` 收窄 ✓
    //   ⚠ 裸 `Channel`（无元素）解析不出 ⇒ 回落 any ✓（宁漏勿误报 ✓）
    native_register_instance_method_return_spec("Channel", "receive", &NATIVE_T_ARG0_ELEM);
    channel_register_method("close", make_native(channel_method_close, 1, "close"), TYPE_ANY, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    native_register_instance_method_return_spec("Channel", "close", &NATIVE_T_NULL);
    channel_register_method("try_send", make_native(channel_method_try_send, 2, "try_send"), TYPE_BOOL, TYPE_UNKNOWN, NATIVE_FIXED(any_params));
    // ⚠ try_receive **故意仍返回 any** ✗：通道空时它给 `null` ⇒ 说它是 `T` 就是**说谎**
    //   （会让人写出 `int x = ch.try_receive()` 这种空通道下拿到 null 的代码 ✗）。
    //   要精确得先给类型系统一个"可空元素"的表达（`T?`）—— 那是独立的需求 ✓
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

    // ---- 并行原语（TH3，2026-10-12）----
    // join_all(Array[Thread]) ⇒ int（真正 joined 的个数）
    TypeKind join_all_params[] = {TYPE_ARRAY};
    native_register_module_method("threads", "join_all", threads_join_all, &NATIVE_T_INT, NATIVE_FIXED(join_all_params));

    // parallel_for(int n, func f) ⇒ int（真正起来的线程数）；f(index) 一个参数
    TypeKind pfor_params[] = {TYPE_INT, TYPE_FUNCTION};
    native_register_module_method("threads", "parallel_for", threads_parallel_for, &NATIVE_T_INT, NATIVE_FIXED(pfor_params));

    // map(Array, func f) ⇒ Array[回调返回]（复用 arrays.map 的同一份规格：回调 = 第 1 实参 ✓）
    //   f(item, index) 两个参数（与 arrays.map 同口径 ✓）
    TypeKind tmap_params[] = {TYPE_ARRAY, TYPE_FUNCTION};
    native_register_module_method("threads", "map", threads_map, &NATIVE_T_ARR_CB_RET1, NATIVE_FIXED(tmap_params));

    // 调用 threads_init_instance_methods 注册线程和通道实例方法
    threads_init_instance_methods();
}

void threads_init_globals(void) {
}
