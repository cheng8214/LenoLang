#include "include/lenolang.h"
#include "include/native.h"
#include "include/platform_thread.h"
#include "include/leno_vm.h"
#include "include/method_table.h"
#include "include/string_table.h"   // intern_should_intern（长串克隆的快路径判据）
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ==================== spawn 耗时探针（LENO_THREAD_DEBUG=1 时打印）====================
// 用途：`threads.start` 单个约 190us，需要知道钱花在哪一段（VM/GC 初始化 / 模块注册 /
//   struct 定义导入 / globals 克隆）。默认关闭，零开销（一次 getenv）。
static uint64_t thr_dbg_now_us(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int inited = 0;
    if (!inited) { QueryPerformanceFrequency(&freq); inited = 1; }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1000000.0 / (double)freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
#endif
}

static int thr_dbg_on(void) {
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("LENO_THREAD_DEBUG");
        on = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return on;
}

// 子线程使用独立的 VM 数组（多线程模式下）
// 主程序直接使用全局 vm，保持效率第一

// 外部函数声明
extern int vm_run_coroutine(ObjCoroutine* co);

// ============================================================================
// Thread 方法注册表（使用通用 MethodTable）
// ============================================================================

#define THREAD_METHOD_TABLE_INITIAL_CAPACITY 16

static THREAD_LOCAL MethodTable threadMethodTable = {NULL, 0, 0};

// 2026-10-19：线程退出清理（见 object_array.c 的同名函数说明）
void thread_method_table_cleanup(void) { method_table_free(&threadMethodTable); }
void channel_method_table_cleanup(void);

void thread_register_method(const char* name, ObjNative* method,
                                  TypeKind return_type, TypeKind return_element_type,
                                  NativeParamSpec params) {
    method_table_register_method(&threadMethodTable, "Thread", name, method, return_type, return_element_type, params);
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

void channel_method_table_cleanup(void) { method_table_free(&channelMethodTable); }

void channel_register_method(const char* name, ObjNative* method,
                                  TypeKind return_type, TypeKind return_element_type,
                                  NativeParamSpec params) {
    method_table_register_method(&channelMethodTable, "Channel", name, method, return_type, return_element_type, params);
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

// ============================================================================
// 跨线程克隆的嵌套深度上限
// ----------------------------------------------------------------------------
// 为什么需要：struct 图**可以成环**（`a.b = a`）⇒ 纯递归会无限分配/爆栈。
//   这里不做环检测（合理消息数据不该有环 ✓），超限就**明确报错** ✓（宁报勿挂 ✓）。
//   容器（Array/Dict）同样受这个上限保护 ✓（多层嵌套的容器也是同一条路径 ✓）。
// ============================================================================
#define CHANNEL_CLONE_MAX_DEPTH 64

// 诊断用：正在克隆的字段（形如 "Point.x"），只在"不可跨线程类型"的报错文案里带上。
//   ⚠ 纯诊断、**不参与语义** ✓。
//   ★★ 2026-11-14（并发压测）：必须是 **THREAD_LOCAL** ★★
//     克隆路径本来就**跑在多个线程上**（每个子线程启动时克隆 globals、收消息时克隆值），
//     原来写成进程级 static ⇒ 并发 snprintf/strlen 同一个 192 字节缓冲。
//     实测症状（`spawn 时 globals 里有个 4000 元素 Array`，18 次里崩 4 次）：
//       strlen 可能读到**没被 NUL 终止**的中间态 ⇒ 返回 >192 ⇒
//       `sizeof - cl` 下溢成巨大值 ⇒ snprintf **无界写**，把后面的 .bss（正是全局 `vm`）
//       整段刷成 "[340][341]..." 这样的 ctx 文本 ⇒ 主线程 vm_run 访问野指针
//       0xC0000005（崩在 vm_ffi_end / thread_reap，完全看不出和 clone 有关）。
//     改 TLS 后同一压测 0 崩溃 ✓；下面再补一道 `cl` 上界钳位（防御性，杜绝再次下溢）✓。
static THREAD_LOCAL char g_channel_clone_ctx[192] = "";

static Value channel_clone_value(Value val, int depth);

// 跨线程克隆**长字符串**（> INTERN_MAX_SHORT_LEN，永远不进内化表）：直接沿用源串的
// `char_len` / `hash`，省掉两遍整串扫描。
//   为什么只在长串上用（2026-11-14 压测，HTTP/IO 管线场景）：
//     `str_copy` = `str_new` 的完整流程 = gc_alloc → malloc → memcpy →
//     `utf8_char_len`（扫一遍）→ `hash_string`（再扫一遍）→（短串才）intern_find/register
//     ⇒ 一条消息要克隆两次（send 进发送堆、receive 进接收堆）⇒ 每字节实际过 6 遍。
//     实测 string body 过 channel 只有 **208~247 MB/s**（同样数据的 `ffi.memcpy` 是 22 GB/s）：
//     128KB 一条 514 µs，其中拷贝本身 <10%，剩下全是这两遍字节级扫描。
//   长串复用后：每字节只剩 memcpy（+ 一次 malloc）⇒ 吞吐翻倍（见 docs 的复测数据）。
//   ⚠ 短串（≤40）**不能**走这里：它们本来靠内化命中做到"重复串零分配"，
//     改成不内化反而更慢（实测 357 → 456 ns/条），那条路径保持不变 ✓
static ObjString* str_clone_long(ObjString* src) {
    ObjString* copy = (ObjString*)gc_alloc(sizeof(ObjString), OBJ_STRING);
    if (!copy) return NULL;
    int len = src->len;
    copy->chars = (char*)malloc((size_t)len + 1);
    if (!copy->chars) {
        native_throw_error("内存分配失败");
        return NULL;
    }
    gc_track_memory((Object*)copy, sizeof(ObjString), sizeof(ObjString) + (size_t)len + 1);
    memcpy(copy->chars, src->chars, (size_t)len);
    copy->chars[len] = '\0';
    copy->len = len;
    copy->capacity = len + 1;      // 精确大小 ⇒ 不可原地追加（与 str_new 同口径）
    // 元数据沿用源串；**源串本身没算过时按老行为补算**（防御：`str_alloc` 系列的
    //   调用方可能还没填 char_len/hash，那时老实现会在克隆时顺手算好 ⇒ 不能让它退化）
    copy->char_len = src->char_len;
    if (copy->char_len == 0 && len > 0) copy->char_len = utf8_char_len(src->chars, len);
    copy->hash = src->hash;
    if (copy->hash == 0 && len > 0) copy->hash = hash_string(src->chars, len);
    return copy;
}

// 不可跨线程类型的**统一报错**（单一文案来源 ✓，下面两处 default 都调它 ✓）
//   ⚠ 报错**不是**可选项：放行跨堆引用 ⇒ 收尾时同一块内存被两个堆各释放一次 ⇒ 0xC0000374 堆损坏。
//   `type_tag` 是运行时类型标记（VAL_* 或 ObjectType），只用于诊断。
// 诊断：对象标记 → 人话（**只用于报错文案**；认不出返回 NULL）
//   ★ 2026-11-12（压测）：原来报「类型标记 17」，用户看到的是个内部枚举值，等于没说话。
static const char* obj_tag_name(int tag) {
    switch (tag) {
        case OBJ_CSTRUCT:            return "cstruct（裸 C 内存）";
        case OBJ_CSTRUCT_ARRAY:      return "cstruct 数组";
        case OBJ_CSTRUCT_ARRAY_VIEW: return "cstruct 数组字段视图";
        case OBJ_FFI_POINTER:        return "Ptr（原生指针）";
        case OBJ_FFI_LIBRARY:        return "FFI 库句柄";
        case OBJ_FFI_CALLBACK:       return "FFI 回调";
        case OBJ_THREAD:             return "Thread";
        case OBJ_COROUTINE:          return "协程";
        case OBJ_FUTURE:             return "Future";
        case OBJ_SOCKET:             return "Socket";
        default:                     return NULL;
    }
}

static void channel_clone_reject(int type_tag) {
    const char* support = "跨线程只支持 标量 / string / Array / Dict / struct / Channel / 闭包 / 函数";
    char kindbuf[64];
    const char* nm = obj_tag_name(type_tag);
    if (nm) {
        snprintf(kindbuf, sizeof(kindbuf), "%s", nm);
    } else {
        snprintf(kindbuf, sizeof(kindbuf), "值类型标记 %d", type_tag);
    }
    if (g_channel_clone_ctx[0] != '\0') {
        // 上界 = 上下文 192 字节 + 提示 68 + 中文固定部分 ⇒ 320 会被 GCC 判 -Wformat-truncation
        char msg[384];
        snprintf(msg, sizeof(msg),
                 "不能跨线程传递此类型（来自 %s，%s）—— %s",
                 g_channel_clone_ctx, kindbuf, support);
        native_throw_error(msg);
        return;
    }
    // ★ 2026-11-26：**没有上下文时也要报出是什么类型**。
    //   原先这条分支把 kindbuf 整个丢了 ⇒ 直接 `threads.start(f, ptr)` 只说
    //   "不能跨线程传递此类型 —— 跨线程只支持…"，看不出是 Ptr（实测）：
    //   而藏在 struct 字段里那条反而点名（"来自 Holder.p，Ptr（原生指针）"）—— 不一致。
    char msg[320];
    snprintf(msg, sizeof(msg), "不能跨线程传递此类型（%s）—— %s", kindbuf, support);
    native_throw_error(msg);
}

// struct 实例深克隆（跨线程）—— TH1（2026-10-12，见 LenoDocsSrc/site/线程并发能力修复追踪.html）
// ----------------------------------------------------------------------------
// 为什么现在能做：struct **定义**的子线程快照机制早就落地（`struct_def_import_from_thread` ✓）
//   ⇒ 接收侧一定查得到类型定义，重建实例没有障碍 ✓。
// 语义：**深克隆**（与 Array/Dict 同口径 ✓）——
//   接收线程拿到的是挂在自己堆上的**独立副本**，改它不影响发送方 ✓。
// 拒绝面：字段里出现白名单之外的对象（`Ptr` / cstruct …）⇒ 走同一个 default 分支
//   （闭包/函数自 TH10 起已支持跨线程，不再是拒绝面 ✓）
//   报「不能跨线程传递此类型」（由 g_channel_clone_ctx 指认是哪个字段 ✓）。
//   ⚠ 这一层**必须**在（跨堆引用 ⇒ 收尾时同一块内存被两个堆各释放一次 ⇒ 0xC0000374 堆损坏）。
static Value channel_clone_struct(ObjStruct* src, int depth) {
    if (!src || !src->def) {
        native_throw_error("跨线程传递 struct 失败：实例缺少结构体定义");
        return val_null();
    }
    if (depth >= CHANNEL_CLONE_MAX_DEPTH) {
        native_throw_error("跨线程传递 struct 失败：嵌套层数超过 64（可能含循环引用）");
        return val_null();
    }

    ObjStructDef* def = src->def;
    ObjStruct* copy = struct_instance_new(def);
    if (!copy) return val_null();   // gc_alloc 失败时已报过

    for (int i = 0; i < def->field_count; i++) {
        Value fv = src->field_values[i];
        // 记录"当前字段"供报错文案使用：保存/恢复成对出现，嵌套 struct 不会把外层上下文冲掉
        char saved_ctx[sizeof(g_channel_clone_ctx)];
        memcpy(saved_ctx, g_channel_clone_ctx, sizeof(saved_ctx));

        const char* fname = (def->fields && def->fields[i].name) ? def->fields[i].name : "?";
        snprintf(g_channel_clone_ctx, sizeof(g_channel_clone_ctx), "%s.%s",
                 def->name ? def->name : "?", fname);

        Value cloned = channel_clone_value(fv, depth + 1);

        memcpy(g_channel_clone_ctx, saved_ctx, sizeof(g_channel_clone_ctx));

        // 走 struct_set_field（struct 字段写入的**唯一入口** ✓）：它负责边界与写屏障 ✓
        struct_set_field(copy, i, cloned);
    }

    // 元数据也要带上：face 声明（`Speaker s = new Dog()` ⇒ 跨线程后仍是 face ✓）
    if (src->declared_face) {
        ObjString* face = str_copy(src->declared_face->chars, src->declared_face->len);
        copy->declared_face = face;
        if (face) gc_write_barrier((Object*)copy, val_obj((Object*)face));
    }
    // 泛型实参（`Box[int]` 这类）：非 GC 托管内存（gc 用 free 释放 ✓）⇒ strdup 深拷贝 ✓
    if (src->generic_type_args && src->generic_type_arg_count > 0) {
        int n = src->generic_type_arg_count;
        char** args = (char**)malloc(sizeof(char*) * (size_t)n);
        if (args) {
            for (int i = 0; i < n; i++) {
                args[i] = src->generic_type_args[i] ? strdup(src->generic_type_args[i]) : NULL;
            }
            copy->generic_type_args = args;
            copy->generic_type_arg_count = n;
        }
    }

    return val_obj((Object*)copy);
}

static Value channel_clone_value(Value val, int depth) {
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
                    // 长串（>40）永远不在内化表里 ⇒ 无"重复串零分配"快路径可丢，直接复用元数据 ✓
                    if (!intern_should_intern(str->len)) {
                        return val_obj((Object*)str_clone_long(str));
                    }
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
                            // ★ 2026-11-14（压测）：**只有可能被拒的元素**才构造诊断上下文 ✗→✓
                            //   下面这段 `数组[3]` 上下文是给「Array 里混进一个 cstruct / Ptr」
                            //   报错用的，可它原本对**每个元素**都做 2×192B memcpy + strlen
                            //   + snprintf("%d") —— 实测 Array[int] 克隆 67 ns/元素（10 万元素 7 ms），
                            //   元素拷贝本身只要 ~2 ns。而标量 / string 根本不可能被拒
                            //   （`channel_clone_value` 对它们永远成功）⇒ 直接跳过，报错文案一字不变 ✓
                            Value elem = arr->elements[i];
                            bool need_ctx = false;
                            if (val_is_obj(elem)) {
                                int et = val_as_obj(elem)->type;
                                need_ctx = (et == OBJ_ARRAY || et == OBJ_DICT ||
                                            et == OBJ_STRUCT || et == OBJ_CLOSURE);
                            }
                            if (need_ctx) {
                                char saved_ctx[sizeof(g_channel_clone_ctx)];
                                memcpy(saved_ctx, g_channel_clone_ctx, sizeof(saved_ctx));
                                size_t cl = strlen(g_channel_clone_ctx);
                                // 上界钳位：`cl` 若超过缓冲（并发写曾把 NUL 冲掉），
                                //   `sizeof - cl` 会下溢成巨大值 ⇒ snprintf 无界写 ⇒ 刷坏全局数据 ✗
                                if (cl > sizeof(g_channel_clone_ctx) - 1) cl = sizeof(g_channel_clone_ctx) - 1;
                                snprintf(g_channel_clone_ctx + cl,
                                         sizeof(g_channel_clone_ctx) - cl,
                                         "%s[%d]", cl > 0 ? "" : "数组", i);
                                copy->elements[i] = channel_clone_value(elem, depth + 1);
                                memcpy(g_channel_clone_ctx, saved_ctx, sizeof(g_channel_clone_ctx));
                            } else {
                                copy->elements[i] = channel_clone_value(elem, depth + 1);
                            }
                        }
                    }
                    return val_obj((Object*)copy);
                }
                case OBJ_DICT: {
                    ObjDict* dict = (ObjDict*)obj;
                    ObjDict* copy = dict_new(dict->capacity > 0 ? dict->capacity : 8);
                    if (!copy) return val_null();
                    // 拷贝数组部分（整数键 0..asize-1）
                    // ⚠⚠ v3.2.8 修（实例三十五）：**必须走 `dict_set`**，不能直写 `copy->array[i]`。
                    //   原来的写法有两个错：
                    //   ① 上界用了 `copy->capacity`（**哈希槽数** ≥ 8），而 `dict_new()` 只给数组部分
                    //      **4** 个槽（`dict_array_resize(dict, 4)`）⇒ 源 `asize > 4` 时**越界写**堆内存
                    //      （实测 0xC0000374 偶发堆损坏）；
                    //   ② 直写还把 `asize++ / acount++` 当成"槽数"累加，而 `acount` 是**非空**元素数
                    //      ⇒ 字典状态错乱 ⇒ 收下侧 `len()` 直接错（实测传 `{k,a,b}` 收到后 `len=11`，
                    //      应为 3 —— 这条**确定性**症状比偶发崩溃更好当回归判据）。
                    //   走 `dict_set` 则由字典自己负责扩容/计数/写屏障 ✓。
                    if (dict->array && dict->asize > 0) {
                        for (int i = 0; i < dict->asize; i++) {
                            Value elem = dict->array[i];
                            if (val_is_null(elem)) continue;
                            dict_set(copy, val_int(i), channel_clone_value(elem, depth + 1));
                        }
                    }
                    // 拷贝哈希表部分
                    for (int i = 0; i < dict->capacity; i++) {
                        Value entry_key = dict->entries[i].key;
                        if (!val_is_null(entry_key) && entry_key != DICT_TOMBSTONE_VAL) {
                            // ⚠⚠ v3.2.8 修（实例三十五）：**键也必须克隆**。
                            //   原代码把发送线程堆上的 key 直接塞进接收线程的 dict ⇒ **跨堆引用**：
                            //   接收方后续的写屏障会把"别的堆的对象"记进自己的记忆集/灰栈，
                            //   进程收尾时同一块内存被两个堆各释放一次 ⇒ **0xC0000374 堆损坏**。
                            //   实测对照（8 次采样）：跳过接收侧克隆 0/8、保留克隆 6/8；
                            //   消息换成**字符串**（无键）0/8 ⇒ 正是"dict 的键"这一条特有路径。
                            dict_set(copy, channel_clone_value(entry_key, depth + 1),
                                    channel_clone_value(dict->entries[i].value, depth + 1));
                        }
                    }
                    return val_obj((Object*)copy);
                }
                case OBJ_STRUCT:
                    // TH1（2026-10-12）：struct 实例也能跨线程了（深克隆 + 接收侧重建）
                    return channel_clone_struct((ObjStruct*)obj, depth);
                case OBJ_CLOSURE: {
                    // TH10（2026-10-20）：闭包也能跨线程（深克隆）
                    //   ① `function` 是**只读字节码**，直接共享 —— 与"线程入口闭包"的重建同口径
                    //      （`thread_entry_point` 里就是 `child_closure->function = closure->function`）
                    //   ② `upvalue` 必须递归克隆：原 upvalue 的 location 可能指着**发送线程的栈**
                    //      ⇒ 取值后建成 closed upvalue（`location = &uv->closed`），否则子线程读别家栈 ✗
                    //   ③ 泛型实参（type_param_args）不深拷 —— 与入口闭包一致（`type_param_count = 0`）
                    if (depth >= CHANNEL_CLONE_MAX_DEPTH) {
                        native_throw_error("跨线程传递闭包失败：捕获嵌套超过 64 层（可能互相捕获成环）");
                        return val_null();
                    }
                    ObjClosure* src = (ObjClosure*)obj;
                    ObjClosure* copy = (ObjClosure*)gc_alloc(sizeof(ObjClosure), OBJ_CLOSURE);
                    if (!copy) return val_null();
                    copy->function = src->function;
                    copy->upvalue_count = src->upvalue_count;
                    copy->type_param_count = 0;
                    copy->type_param_args = NULL;
                    for (int ui = 0; ui < MAX_UPVALUES; ui++) copy->upvalues[ui] = NULL;

                    for (int i = 0; i < src->upvalue_count && i < MAX_UPVALUES; i++) {
                        Upvalue* suv = src->upvalues[i];
                        Value captured = suv ? *suv->location : val_null();
                        Value cloned = channel_clone_value(captured, depth + 1);
                        Upvalue* uv = (Upvalue*)gc_alloc(sizeof(Upvalue), OBJ_UPVALUE);
                        if (!uv) {
                            copy->upvalue_count = i;   // 部分初始化（调用时缺捕获会报未定义）
                            return val_obj((Object*)copy);
                        }
                        uv->location = &uv->closed;
                        uv->closed = cloned;
                        uv->next = NULL;
                        copy->upvalues[i] = uv;
                    }
                    return val_obj((Object*)copy);
                }
                case OBJ_FUNCTION:
                    // TH10：裸函数对象（跨模块 exports 里取到的那种）是**只读代码**，直接共享 ✓
                    //   子线程调用时由 OP_CALL 的 OBJ_FUNCTION 分支现场包一层闭包（分配在子线程堆）
                    return val;
                case OBJ_CHANNEL: {
                    ObjChannel* ch = (ObjChannel*)obj;
                    channel_ref(ch);
                    return val_obj((Object*)ch);
                }
                default:
                    channel_clone_reject(obj->type);
                    return val_null();
            }
        }
        default:
            // 对象就报**对象类型**（能翻成人话），非对象才退回值标记
            channel_clone_reject(val_is_obj(val) ? (int)val_as_obj(val)->type
                                                : (int)val_get_type(val));
            return val_null();
    }
}

// 跨线程传值的**唯一闸门**（对外入口，符号名不变 —— 多处调用点依赖它 ✓）
Value value_clone_for_channel(Value val) {
    return channel_clone_value(val, 0);
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

    // ★ 2026-11-15：线程序号（主线程 VM 恒为 0，这里从 1 开始）。
    //   分配对象时抄进 Object.owner_thread ⇒ module.c 的 module_set_var 据此发现
    //   "子线程把本线程堆的对象写进共享的模块级 var"（跨堆引用，GC 后会变残值）。
    //   只用于诊断，计数用原子自增避免多线程同时 start 时数据竞争。
    {
        static volatile long g_child_thread_serial = 0;
#ifdef _MSC_VER
        child_vm.thread_serial = (uint8_t)_InterlockedIncrement(&g_child_thread_serial);
#else
        child_vm.thread_serial = (uint8_t)__sync_add_and_fetch(&g_child_thread_serial, 1);
#endif
    }

    int dbg = thr_dbg_on();
    uint64_t dt0 = dbg ? thr_dbg_now_us() : 0;

    // 初始化子线程 VM
    child_vm.chunk = NULL;
    child_vm.ip = NULL;
    child_vm.sp = 0;
    child_vm.stack_capacity = 256;
    child_vm.stack = (Value*)malloc(child_vm.stack_capacity * sizeof(Value));
    memset(child_vm.stack, 0, child_vm.stack_capacity * sizeof(Value));
    // FFI 参数区（2026-11-01）：每个 VM 一份，固定容量、永不 realloc
    child_vm.ffi_args_sp = 0;
    child_vm.ffi_args = (Value*)malloc(VM_FFI_ARGS_CAP * sizeof(Value));
    if (child_vm.ffi_args) {
        memset(child_vm.ffi_args, 0, VM_FFI_ARGS_CAP * sizeof(Value));
    }

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
    // ★ 子线程的**线程局部存储**是**独立一份**（TH5）—— 每个子线程的 VM 是自己的结构体，
    //   这个槽天然隔离 ✓（`threads.local_get/set` 就是按这条实现"每线程一份"的 ✓）
    child_vm.thread_local_store = val_null();

    // 复制全局变量：这里**只分配槽位 + 填 null**，真正的克隆延后到 gc_init() 之后
    //   （TH8：数据对象要克隆进子线程堆，而 clone 要 gc_alloc ⇒ 必须等 GC 初始化）
    if (saved_globals && saved_global_count > 0) {
        child_vm.globals = (Value*)malloc(saved_global_capacity * sizeof(Value));
        if (child_vm.globals) {
            for (int i = 0; i < saved_global_capacity; i++) child_vm.globals[i] = val_null();
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
    uint64_t dt1 = dbg ? thr_dbg_now_us() : 0;

    // 注册内置 Native 全局函数（与主 VM 一致）
    extern void native_register_globals(void);
    native_register_globals();
    uint64_t dt1a = dbg ? thr_dbg_now_us() : 0;   // native globals 注册结束

    // 初始化所有模块方法（与主 VM 一致）
    //   ★ 2026-11-24：这里原先**手抄了 13 个模块名**，新增 reflect 时漏掉 ⇒
    //     子线程里 `reflect.kind(x)` 找不到模块对象、静默抛异常退出，而主线程卡在
    //     `ch.receive()` 上永久等待 —— 表现为"整个程序卡死"，且日志里什么都看不到。
    //     清单与 module_init_table 是同一份事实 ⇒ 改成遍历表，以后加模块不会再漏。
    extern void native_init_all_modules(void);
    native_init_all_modules();
    uint64_t dt1b = dbg ? thr_dbg_now_us() : 0;   // 所有模块方法注册结束
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
    uint64_t dt2 = dbg ? thr_dbg_now_us() : 0;   // 模块/实例方法注册结束

    // ★ 将主线程已定义的 struct 类型导入子线程的定义表。
    // 普通 struct 定义表是线程局部，子线程默认为空；
    // 不导入则子线程 new StructName() 会报"未定义的结构体"。
    // struct 定义是只读类型元数据，与 cstruct 一样可跨线程共享。
    if (saved_struct_defs && saved_struct_def_count > 0) {
        struct_def_import_from_thread(saved_struct_defs, saved_struct_def_count);
    }
    uint64_t dt3 = dbg ? thr_dbg_now_us() : 0;   // struct 定义导入结束

    // ==================== 全局变量：数据对象深拷贝（TH8，2026-10-20）====================
    // 旧行为：只把 Value 指针抄过去 ⇒ 主线程与子线程**共用同一个** Array / Dict / struct
    //   ⇒ 子线程改内容时走的是**子线程的写屏障**，却在改**主线程堆**上的对象
    //   ⇒ 两个堆对同一块内存的标记/释放状态不一致 ⇒ 偶发 `0xC0000374` 堆损坏 ✗
    //   （实测：`arr.add(4)` / `box.set("n", 99)` 主线程全看得见）
    // 现在：数据对象克隆到子线程堆；**只读元数据 / 资源句柄仍共享**（它们克隆不了，共享也对）：
    //   module（进程级 loaded_modules 缓存）、function（只读字节码）、native、cstruct_def、
    //   socket / file / ffi 句柄（克隆出来是假句柄）、channel / thread（引用语义）
    // ⚠ 位置有讲究：必须在 `gc_init()` 之后（clone 要 gc_alloc），且在上面的 struct 定义导入之后
    //   （全局 struct 实例重建时要靠它的 def 找回字段布局）。
    if (saved_globals && child_vm.globals) {
        extern THREAD_LOCAL VM* current_exec_vm;
        VM* saved_exec_vm = current_exec_vm;
        current_exec_vm = &child_vm;      // ★ clone 失败时报错要落到**子线程** VM，不能污染主 VM
        int saved_gc_enabled = gc_get_enabled();
        gc_set_enabled(0);                // saved_globals 是 malloc 数组、不在 GC 根集合里
        for (int i = 0; i < child_vm.global_count; i++) {
            Value val = saved_globals[i];
            if (!val_is_obj(val)) {
                child_vm.globals[i] = val;   // 标量：自带副本
                continue;
            }
            switch (val_as_obj(val)->type) {
                case OBJ_STRING:
                case OBJ_ARRAY:
                case OBJ_DICT:
                case OBJ_STRUCT:
                case OBJ_CLOSURE: {
                    Value cloned = value_clone_for_channel(val);
                    if (val_is_null(cloned) || child_vm.has_exception) {
                        // 含 Ptr / cstruct 的容器克隆不了 ⇒ 回退成共享（旧行为），
                        //   **不让一个全局变量把整条线程搞挂** ✗
                        child_vm.exception = val_null();
                        child_vm.has_exception = 0;
                        cloned = val;
                    }
                    child_vm.globals[i] = cloned;
                    break;
                }
                default:
                    child_vm.globals[i] = val;   // 只读元数据 / 资源句柄 / 引用语义对象
                    break;
            }
        }
        gc_set_enabled(saved_gc_enabled);
        current_exec_vm = saved_exec_vm;
    }
    if (dbg) {
        uint64_t dt4 = thr_dbg_now_us();
        fprintf(stderr, "[THR] vm/GC=%llu  native全局=%llu  模块注册=%llu  实例方法=%llu  struct导入=%llu  globals克隆=%llu  合计=%llu us\n",
                (unsigned long long)(dt1 - dt0),
                (unsigned long long)(dt1a - dt1),
                (unsigned long long)(dt1b - dt1a),
                (unsigned long long)(dt2 - dt1b),
                (unsigned long long)(dt3 - dt2),
                (unsigned long long)(dt4 - dt3),
                (unsigned long long)(dt4 - dt0));
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
        if (child_vm.ffi_args) free(child_vm.ffi_args);   // FFI 参数区（2026-11-01）
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
        if (child_vm.ffi_args) free(child_vm.ffi_args);   // FFI 参数区（2026-11-01）
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
        // ★ 错误消息**不再丢弃**（2026-09-28）：
        //   原先只有"异常恰好是字符串"才保留，其余一律写成 "unknown error" ✗
        //   ⇒ 排查线程问题的人只能看到一句废话（作者本人为定位一个入口问题打了 9 轮探针 ✗）
        //   现在：① 非字符串异常也带**类型**与提示 ② 无论如何都打到 stderr（含入口函数名）✓
        const char* tname = (closure && closure->function && closure->function->name)
                            ? closure->function->name : "?";
        if (child_vm.has_exception && val_is_obj(child_vm.exception) && val_as_obj(child_vm.exception)->type == OBJ_STRING) {
            ObjString* err = (ObjString*)val_as_obj(child_vm.exception);
            thread_obj->error_msg = strdup(err->chars);
        } else if (child_vm.has_exception && val_is_obj(child_vm.exception)
                   && val_as_obj(child_vm.exception)->type == OBJ_DICT) {
            // ★ TH11（2026-10-20）：`throw "xxx"` 会被 `wrap_string_exception` 包成
            //   `{msg, line, file, stack}`（见 `vm_exception.inc:295`）⇒ 异常值其实是 **Dict**。
            //   旧代码只认 OBJ_STRING，于是**所有**用户 `throw "..."` 都被写成
            //   "非字符串异常（对象类型 2）—— 常见于进入函数体之前就失败" ⇒ 把人往"模块没加载"上引 ✗
            ObjString* mk = str_copy("msg", 3);
            Value mv = mk ? dict_get((ObjDict*)val_as_obj(child_vm.exception), val_obj((Object*)mk)) : val_null();
            if (val_is_string(mv)) {
                thread_obj->error_msg = strdup(((ObjString*)val_as_obj(mv))->chars);
            } else {
                thread_obj->error_msg = strdup("非字符串异常（错误对象里没有 msg 字段）");
            }
        } else if (child_vm.has_exception && val_is_obj(child_vm.exception)) {
            char buf[200];
            snprintf(buf, sizeof(buf),
                "非字符串异常（对象类型 %d）—— 常见于**进入函数体之前**就失败（如入口函数所属模块未加载）",
                (int)val_as_obj(child_vm.exception)->type);
            thread_obj->error_msg = strdup(buf);
        } else {
            char buf[200];
            snprintf(buf, sizeof(buf),
                "线程在进入函数体前失败（无异常值；vm_run_coroutine 返回 %d）", result);
            thread_obj->error_msg = strdup(buf);
        }
        // ⚠ 这里**不打印**（去掉 fprintf/fflush 的临时实验）：先前加上它后，应用在**退出阶段**
        //   必卡（可复现 ✗），而该分支根本没被执行过 ⇒ 说明卡点是退出路径的既有问题被触发了 ✗
        //   （诊断信息已写进 `error_msg`，join() 时会带出去 ⇒ 不依赖 stderr ✓）
        (void)tname;
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
    if (child_vm.ffi_args) free(child_vm.ffi_args);   // FFI 参数区（2026-11-01）
    if (child_vm.globals) free(child_vm.globals);
    if (child_vm.global_funcs) free(child_vm.global_funcs);
    // ★ 2026-11-15：`thread_local var` 的槽位数组（值与对象由 GC 统一释放，这里只放数组本身）
    if (child_vm.tls_slots) free(child_vm.tls_slots);
    if (child_vm.tls_slot_ready) free(child_vm.tls_slot_ready);

    // ★ 2026-10-19：释放本线程的**注册表**（它们也是 THREAD_LOCAL，且都在线程初始化时各自分配）
    //   与堆无关：即使因为 channel / 未 join 子线程而**不**整堆释放，这些表也必须释放
    //   （表里存的是 ObjNative* 指针，不拥有对象；释放表本身不会动堆里的对象 ✓）
    {
        extern void native_thread_cleanup(void);
        extern void array_method_table_cleanup(void);
        extern void string_method_table_cleanup(void);
        extern void number_method_table_cleanup(void);
        extern void file_method_table_cleanup(void);
        extern void dict_method_table_cleanup(void);
        extern void cstruct_method_table_cleanup(void);
        extern void socket_method_table_cleanup(void);
        extern void thread_method_table_cleanup(void);
        extern void channel_method_table_cleanup(void);
        extern void struct_method_table_cleanup(void);
        extern void arr_free_list_release_all(void);
        native_thread_cleanup();
        array_method_table_cleanup();
        string_method_table_cleanup();
        number_method_table_cleanup();
        file_method_table_cleanup();
        dict_method_table_cleanup();
        cstruct_method_table_cleanup();
        socket_method_table_cleanup();
        thread_method_table_cleanup();
        channel_method_table_cleanup();
        struct_method_table_cleanup();
        arr_free_list_release_all();
    }

    // ★★ 2026-10-19：把子线程的 GC 堆摘下来（THREAD_LOCAL 堆没人会回收 ✗）
    // ★★ 2026-11-20：**不再因为"用过 channel"就跳过**。旧闸门让所有 worker 线程的整堆
    //   被遗弃（Aria 切歌每首起 3 条线程 ⇒ 内存只涨不降）。现在：
    //     · 摘下来的堆先由 channel 槽引用（发送时登记的 owner），refs==0 才真正释放；
    //     · result 是对象时钉住它，等 join 克隆到主堆后再放（见 gc.c 的 GCDeadHeap ✓）。
    //   ⚠⚠ 还有一个**必须**满足的条件：本线程**没有活着的子线程**。
    //     子线程的 ObjThread（含 mutex/cond）与 pending_args 都分配在**本线程的堆**上；
    //     若本线程先退出、heap 被释放，而子线程还在跑（要拿 mutex 写 state、
    //     读 pending_args）⇒ 读写已释放内存（实测：进程**静默退出**，连后续 print 都不执行 ✗）。
    //     `active_thread_count == 0` = 起过的子线程都 join 过了（join 会把自己从表里移除 ✓）；
    //     有未 join 的（哪怕已跑完）就放弃本次整堆回收 —— 宁可少回收，不能 UAF ✓
    if (child_vm.active_thread_count == 0) {
        void* detached = gc_detach_heap();
        if (detached) {
            // 结果是**对象**（string/Array/Dict/struct）⇒ join 还要从堆里克隆它 ⇒ 先转给
            //   ObjThread 持有一份引用；标量结果 join 不需要堆 ⇒ 没人引用就当场整堆释放 ✓
            // ⚠ gc_detach_heap_finish 会放掉"本线程"那一份引用，返回后 detached 可能已释放，
            //   所以 thread_obj->dead_heap 必须**在调用前**赋好 ✓
            int keep = thread_obj->has_result && val_is_obj(thread_obj->result);
            if (keep) thread_obj->dead_heap = detached;
            gc_detach_heap_finish(detached, keep);
        }
    }
    // 子线程都 join 过 ⇒ 本 VM 的活动线程表没人再用了（原来每条线程漏一份 malloc 数组）
    if (child_vm.active_thread_count == 0 && child_vm.active_threads) {
        free(child_vm.active_threads);
        child_vm.active_threads = NULL;
        child_vm.active_thread_capacity = 0;
    }

    // ★ 本线程的 intern 表与 GC extra_roots 也是 THREAD_LOCAL 独占资源：
    //   线程退出后没有任何人会再释放它们（intern_table_free 只释放条目桶，
    //   不碰 GC 堆里的字符串 ⇒ 与 detach 后的堆可安全共存）。
    {
        extern void intern_table_free(void);
        intern_table_free();
    }
    gc_release_thread_aux();

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
    thread->dead_heap = NULL;

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

// 等待线程结束：timeout_ms < 0 ⇒ 无限等。返回 1 = 已结束，0 = 超时（线程仍在跑，状态未动）
//   ⚠ 超时是"最多等约 timeout_ms"的近似：被 signal 但 state 还没变（spurious wakeup）时
//     循环会重新计满 timeout_ms。平台层没有统一取毫秒时钟的函数，不值得为这点精度引入一个。
static int thread_wait(ObjThread* thread, int timeout_ms) {
    platform_mutex_lock(&thread->mutex);
    if (timeout_ms < 0) {
        while (thread->state == THREAD_RUNNING) {
            platform_cond_wait(&thread->done_cond, &thread->mutex);
        }
    } else {
        while (thread->state == THREAD_RUNNING) {
            if (platform_cond_timedwait(&thread->done_cond, &thread->mutex, timeout_ms) != 0) break;
        }
    }
    int done = (thread->state != THREAD_RUNNING) ? 1 : 0;
    platform_mutex_unlock(&thread->mutex);
    return done;
}

// 收尾（必须在"线程已结束"后调用）：克隆 result 到本线程堆 → join OS 线程 → 释放子线程堆
//   → 从 active_threads 摘除 → 线程出错则抛异常。重复调用是幂等的（joined / dead_heap 都有守卫）
static Value thread_reap(ObjThread* thread) {
    platform_mutex_lock(&thread->mutex);

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

    // ★★ 2026-10-19：result 已经克隆到主线程堆（上面那段）⇒ 子线程那个堆可以放了 ✓
    // ★ 2026-11-20：走 unpin（可能还被 channel 槽引用着，由最后一个槽释放 ✓）
    if (thread->dead_heap) {
        void* dh = thread->dead_heap;
        thread->dead_heap = NULL;
        gc_dead_heap_unref(dh);
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
        snprintf(msg, sizeof(msg), "线程内抛出异常：%s",
                 thread->error_msg ? thread->error_msg : "（未记录消息）");
        native_throw_error(msg);
        return val_null();
    }

    return thread->has_result ? thread->result : val_null();
}

Value thread_join(ObjThread* thread) {
    thread_wait(thread, -1);
    return thread_reap(thread);
}

// join 的超时版本（TH9，2026-10-20）：
//   1 = 线程已结束（并且已收尾：result 克隆到本线程堆、子线程堆已释放）
//   0 = 超时，**状态不变** —— 线程继续跑，之后还能再 join() / join_for() 取结果
//   ⚠ 线程出错时与 join() 一样**抛异常**（布尔返回值表示不了"结束了但失败了"）
int thread_join_for(ObjThread* thread, int timeout_ms) {
    if (thread->joined) return 1;              // 已收尾过 ⇒ 幂等返回，不重复克隆/抛异常
    if (!thread_wait(thread, timeout_ms)) return 0;
    thread_reap(thread);
    return 1;
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

// ----------------------------------------------------------------------------
// Channel 全局登记链（见 ObjChannel.all_next 的说明）
//   Channel 用 malloc 分配、不在任何 GC 堆链表里 ⇒ GC 的 gc_free_all 永远看不到它们。
//   引用计数只有 channel_ref（跨线程克隆）在用，**没有任何地方 unref** ⇒ 整块内存
//   （结构 + buffer）在进程结束前一直挂着（LeakSanitizer 实测 6 个用例、约 2.6KB）。
//   这里用无锁原子交换压入全局链，程序收尾时 channel_free_all() 整链释放。
//   ⚠ 只在"所有工作线程已 join"的进程退出点调用（main.c 的两个包装器）。
// ----------------------------------------------------------------------------
static ObjChannel* g_channel_all_head = NULL;

// ----------------------------------------------------------------------------
// 槽位的“放进 / 取走”（★ 2026-11-20）
//   放进时必须先登记 owner（本线程的发送记录）：这个克隆分配在**本线程堆**上，
//   线程退出时会把整堆摘下来，靠这条记录上的 refs 才知道“还有几个槽压着它”✓
//   取走时把 owner 一起摘出来，由调用方在**解锁之后** gc_dead_heap_unref（可能是整堆 free，
//   不要在持有 channel mutex 时做 ✓）
// ----------------------------------------------------------------------------
static void channel_slot_put(ObjChannel* ch, int idx, Value v) {
    void* rec = gc_dead_heap_record();
    ch->buffer[idx] = v;
    if (ch->buffer_owner) ch->buffer_owner[idx] = rec;
    if (rec) gc_dead_heap_ref(rec);
}

static void* channel_slot_take(ObjChannel* ch, int idx) {
    void* rec = ch->buffer_owner ? ch->buffer_owner[idx] : NULL;
    ch->buffer[idx] = val_null();
    if (ch->buffer_owner) ch->buffer_owner[idx] = NULL;
    return rec;
}

void channel_free_all(void) {
    ObjChannel* ch = __atomic_exchange_n(&g_channel_all_head, NULL, __ATOMIC_ACQ_REL);
    while (ch) {
        ObjChannel* next = ch->all_next;
        if (!ch->released) {
            if (ch->buffer) free(ch->buffer);
            if (ch->buffer_owner) free(ch->buffer_owner);
            platform_mutex_destroy(&ch->mutex);
            platform_cond_destroy(&ch->not_empty);
            platform_cond_destroy(&ch->not_full);
        }
        free(ch);
        ch = next;
    }
}

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
    channel->buffer_owner = (void**)calloc(channel->capacity, sizeof(void*));
    if (!channel->buffer) {
        free(channel->buffer_owner);
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

    // 登记到全局链（无锁压栈）；released 已被 memset 置 0
    channel->all_next = __atomic_exchange_n(&g_channel_all_head, channel, __ATOMIC_ACQ_REL);

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
    void* small[8];
    void** owners = small;
    int n = 0;
    platform_mutex_lock(&channel->mutex);
    channel->ref_count--;
    if (channel->ref_count <= 0) {
        // ★ 2026-11-20：把还压着的槽位一起放掉（否则那些子线程堆的引用计数永远减不到 0，
        //   那些整堆就永远不释放 —— 正是本次修的那条泄漏 ✓）
        if (channel->buffer_owner) {
            if (channel->capacity > 8) {
                void** big = (void**)malloc(sizeof(void*) * (size_t)channel->capacity);
                if (big) owners = big;
            }
            if (owners == small && channel->capacity > 8) {
                // malloc 失败：宁可不减计数（漏这几个堆）也不能让 owner 悬空 ✓
            } else {
                for (int i = 0; i < channel->capacity; i++) {
                    if (channel->buffer_owner[i]) {
                        owners[n++] = channel->buffer_owner[i];
                        channel->buffer_owner[i] = NULL;
                    }
                }
            }
        }
        platform_mutex_unlock(&channel->mutex);
        if (channel->buffer) {
            free(channel->buffer);
            channel->buffer = NULL;
        }
        if (channel->buffer_owner) {
            free(channel->buffer_owner);
            channel->buffer_owner = NULL;
        }
        platform_mutex_destroy(&channel->mutex);
        platform_cond_destroy(&channel->not_empty);
        platform_cond_destroy(&channel->not_full);
        // 结构本身不在这里 free：它还在全局登记链上（g_channel_all_head），
        // 若在这里释放，channel_free_all 会二次释放同一块内存。
        // 打上 released 标记后，由进程收尾的 channel_free_all() 只释放结构体。
        channel->buffer = NULL;
        channel->released = 1;
        // 解锁后再减引用：减到 0 会 free 整个子线程堆，别在持锁时做 ✓
        for (int i = 0; i < n; i++) { gc_dead_heap_unref(owners[i]); }
        if (owners != small) free(owners);
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

// 克隆是否**被拒**（跨线程不允许的类型：cstruct / Ptr / 闭包…）
//   `channel_clone_reject` 已经登记了异常，这里只负责让发送方**别把 null 入队**。
//   判据：原值非 null 而克隆结果是 null —— 克隆永远不会把非 null 合法地变成 null
//   （唯一例外是 OOM，那时同样不该入队 ✓）。
//   ★ 2026-11-12（压测）：原先没有这道闸 ⇒ `try { ch.send(cstruct) } catch {}` 之后
//     buffer 里留下一个**幽灵 null**：紧接着 `send(42); receive()` 得到 null、
//     真正的 42 被推后一格（静默错位，实测 tmp_d7）。
#define CHANNEL_CLONE_REJECTED(orig, cloned) (!val_is_null(orig) && val_is_null(cloned))

int channel_send(ObjChannel* channel, Value value) {
    Value cloned = value_clone_for_channel(value);
    if (CHANNEL_CLONE_REJECTED(value, cloned)) return -3;   // -3 = 克隆被拒（异常已登记）

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

    channel_slot_put(channel, channel->tail, cloned);        // 连同 owner 一起登记 ✓
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

    void* owner = channel_slot_take(channel, channel->head);
    channel->head = (channel->head + 1) % channel->capacity;
    channel->count--;

    platform_cond_signal(&channel->not_full);
    platform_mutex_unlock(&channel->mutex);

    // 槽已清空 ⇒ 减引用（减到 0 会整堆 free，所以放在解锁之后 ✓）
    gc_dead_heap_unref(owner);
    return re_cloned;
}

// 非阻塞取一个值（TH13，2026-10-20）：1 = 取到（**值可以是 null**），0 = 空，-1 = 参数无效
//   为什么不复用 channel_try_receive：它用 null 表示"空"，而 Channel **可以发送 null**
//   （例如线程池用 null 当停机信号）⇒ 调用方无法区分"空"和"收到的就是 null" ✗
//   select 需要这个区分，所以单独给一个带出参的原子接口（同一把锁内完成"判断 + 取走"）。
int channel_try_take(ObjChannel* channel, Value* out) {
    if (!channel || !out) return -1;
    platform_mutex_lock(&channel->mutex);

    if (channel->count == 0) {
        platform_mutex_unlock(&channel->mutex);
        return 0;
    }

    Value value = channel->buffer[channel->head];
    // 与 channel_try_receive 同口径：先克隆再移除（防"移除后源对象被发送线程 GC 回收"），
    // 克隆期间禁用同步 GC（否则 gc_scan_children 要 lock 本 mutex ⇒ 自死锁）
    int saved_gc_enabled = gc_get_enabled();
    gc_set_enabled(0);
    Value re_cloned = value_clone_for_channel(value);
    gc_set_enabled(saved_gc_enabled);

    void* owner = channel_slot_take(channel, channel->head);
    channel->head = (channel->head + 1) % channel->capacity;
    channel->count--;

    platform_cond_signal(&channel->not_full);
    platform_mutex_unlock(&channel->mutex);

    gc_dead_heap_unref(owner);       // 同上：解锁后再减 ✓
    *out = re_cloned;
    return 1;
}

// 带超时的发送（TH13）：0 = 成功，-1 = 通道已关闭，-2 = 超时（未发送）
//   timeout_ms < 0 = 无限等（等价 channel_send）
//   为什么要它：`send` 在缓冲满时会**无限阻塞**，一个忘记取值的消费者就能让生产者永久卡死
//   （服务端停机、UI 线程上报进度这类场景最怕）。有了超时就能放弃并走降级路径。
int channel_send_for(ObjChannel* channel, Value value, int timeout_ms) {
    Value cloned = value_clone_for_channel(value);   // 克隆在发送方堆；超时丢弃时由本线程 GC 回收 ✓
    if (CHANNEL_CLONE_REJECTED(value, cloned)) return -3;   // 同上：被拒时不许入队

    platform_mutex_lock(&channel->mutex);

    if (channel->closed) {
        platform_mutex_unlock(&channel->mutex);
        return -1;
    }

    if (timeout_ms < 0) {
        while (channel->count >= channel->capacity && !channel->closed) {
            platform_cond_wait(&channel->not_full, &channel->mutex);
        }
    } else {
        while (channel->count >= channel->capacity && !channel->closed) {
            if (platform_cond_timedwait(&channel->not_full, &channel->mutex, timeout_ms) != 0) {
                platform_mutex_unlock(&channel->mutex);
                return -2;                        // 超时：不写入
            }
        }
    }

    if (channel->closed) {
        platform_mutex_unlock(&channel->mutex);
        return -1;
    }

    channel_slot_put(channel, channel->tail, cloned);   // 连同 owner 一起登记 ✓
    gc_write_barrier((Object*)channel, cloned);
    channel->tail = (channel->tail + 1) % channel->capacity;
    channel->count++;

    platform_cond_signal(&channel->not_empty);
    platform_mutex_unlock(&channel->mutex);
    return 0;
}

int channel_try_send(ObjChannel* channel, Value value) {
    Value cloned = value_clone_for_channel(value);
    if (CHANNEL_CLONE_REJECTED(value, cloned)) return -3;   // 同上

    platform_mutex_lock(&channel->mutex);

    if (channel->closed || channel->count >= channel->capacity) {
        platform_mutex_unlock(&channel->mutex);
        return -1;
    }

    channel_slot_put(channel, channel->tail, cloned);   // 连同 owner 一起登记 ✓
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

    void* owner = channel_slot_take(channel, channel->head);
    channel->head = (channel->head + 1) % channel->capacity;
    channel->count--;

    platform_cond_signal(&channel->not_full);
    platform_mutex_unlock(&channel->mutex);

    // 槽已清空 ⇒ 减引用（减到 0 会整堆 free，所以放在解锁之后 ✓）
    gc_dead_heap_unref(owner);
    return re_cloned;
}

// 注意：Thread 和 Channel 的实例方法已移到 threads.c 模块中
