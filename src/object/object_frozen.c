// ============================================================================
// 冻结对象（frozen）—— 大对象跨线程零拷贝共享
// ----------------------------------------------------------------------------
// 设计文档：LenoDocsSrc/大对象零拷贝共享设计.md
//
// 本文件负责四件事：
//   ① `_freeze` / `_thaw` / `_is_frozen` 的语义（深克隆 + 环/类型预检 + 解冻）
//   ② 冻结图的引用计数（父→子边 + 每个 VM 一份外部引用）
//   ③ 每个 VM 的 frozen 注册表（堆外根的等价物；含池任务的世代局部释放）
//   ④ 冻结图的迭代释放（深链不能用递归）
//
// ⚠ 冻结对象**不归任何线程堆**（gc_alloc 在 g_frozen_build 期间改走堆外分配）：
//   GC 的 mark/sweep 永不回收它，存活只靠 refcnt。改这里前先读设计文档 §4/§5。
// ============================================================================

#include "include/lenolang.h"
#include "include/native.h"
#include "include/leno_vm.h"
#include "include/leno_frozen.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// 注册表墓碑：删除后的槽位（开放寻址里不能直接置空，否则探针链断掉）
#define FZ_REG_TOMBSTONE ((Object*)(uintptr_t)1)

// ---------------------------------------------------------------- 统计
// 为什么不并进 gc.young_allocated：frozen 永远不会被 GC 收回去 ⇒ 计入只会让
// GC 被反复触发却一个字节都收不回（空转）。这里只做账，`_mem()` 单列字段。
static volatile long g_frozen_bytes = 0;
static volatile long g_frozen_objs = 0;

void frozen_stats_add(long bytes, long objs) {
    if (bytes) __atomic_add_fetch(&g_frozen_bytes, bytes, __ATOMIC_RELAXED);
    if (objs) __atomic_add_fetch(&g_frozen_objs, objs, __ATOMIC_RELAXED);
}

void frozen_get_stats(size_t* out_bytes, size_t* out_objs) {
    if (out_bytes) *out_bytes = (size_t)__atomic_load_n(&g_frozen_bytes, __ATOMIC_RELAXED);
    if (out_objs) *out_objs = (size_t)__atomic_load_n(&g_frozen_objs, __ATOMIC_RELAXED);
}

// ---------------------------------------------------------------- 引用计数
void frozen_ref(Object* o) {
    if (!o) return;
    __atomic_add_fetch(&o->refcnt, 1, __ATOMIC_ACQ_REL);
}

// ---------------------------------------------------------------- 子引用枚举
// **槽位口径**：同一个指针占了两个槽就算两次 —— finalize 与 free 必须用同一套枚举，
// 否则引用计数会漂（少减=泄漏，多减=提前 free）。
static void fz_foreach_child(Object* o, void (*fn)(Object* child, void* ud), void* ud) {
    if (!o) return;
    switch (o->type) {
        case OBJ_ARRAY: {
            ObjArray* a = (ObjArray*)o;
            for (int i = 0; i < a->count; i++) {
                Value v = a->elements[i];
                if (val_is_obj(v)) fn(val_as_obj(v), ud);
            }
            break;
        }
        case OBJ_DICT: {
            ObjDict* d = (ObjDict*)o;
            if (d->array) {
                for (int i = 0; i < d->asize; i++) {
                    Value v = d->array[i];
                    if (val_is_obj(v)) fn(val_as_obj(v), ud);
                }
            }
            if (d->entries) {
                for (int i = 0; i < d->capacity; i++) {
                    Value k = d->entries[i].key;
                    if (val_is_obj(k) && k != DICT_TOMBSTONE_VAL) fn(val_as_obj(k), ud);
                    Value v = d->entries[i].value;
                    if (val_is_obj(v)) fn(val_as_obj(v), ud);
                }
            }
            // order 里存的是键的副本指针（与 entries 同一批对象）—— 计数与释放都算上，
            // 只要两边一致就不会漂。⚠ 别只改一边。
            if (d->order) {
                for (int i = 0; i < d->order_count; i++) {
                    Value k = d->order[i];
                    if (val_is_obj(k)) fn(val_as_obj(k), ud);
                }
            }
            break;
        }
        case OBJ_STRUCT: {
            ObjStruct* s = (ObjStruct*)o;
            int n = (s->def) ? s->def->field_count : 0;
            for (int i = 0; i < n; i++) {
                Value v = s->field_values[i];
                if (val_is_obj(v)) fn(val_as_obj(v), ud);
            }
            if (s->declared_face) fn((Object*)s->declared_face, ud);
            break;
        }
        default:
            break;   // string 只有 chars（malloc，不是对象）
    }
}

// ---------------------------------------------------------------- 预检
// 一次 DFS 同时做两件事：**支持面**（Ptr/cstruct/闭包/Channel/Atomic 一律拒）
// 与**环检测**（超深即拒）。为什么不在克隆里做环检测：
//   克隆是"边分配边下降"，发现环时已经分配了半个图、没有根可以回收 ⇒ 泄漏；
//   预检先失败，一个字节都不分配。
#define FZ_MAX_DEPTH 64

static int fz_check_value(Value v, int depth);

static int fz_reject_type(Object* o, int depth) {
    (void)depth;
    switch (o->type) {
        case OBJ_FFI_POINTER:  native_throw_error("不能冻结 Ptr（原生指针）—— 冻结只支持 标量 / string / Array / Dict / struct"); return 0;
        case OBJ_CSTRUCT:      native_throw_error("不能冻结 cstruct（裸 C 内存）—— 冻结只支持 标量 / string / Array / Dict / struct"); return 0;
        case OBJ_CSTRUCT_ARRAY:
        case OBJ_CSTRUCT_ARRAY_VIEW:
                               native_throw_error("不能冻结 cstruct 数组 —— 冻结只支持 标量 / string / Array / Dict / struct"); return 0;
        case OBJ_CHANNEL:      native_throw_error("不能冻结 Channel —— 它本来就是跨线程共享的可变通道；请传冻结后的数据本身"); return 0;
        case OBJ_ATOMIC:       native_throw_error("不能冻结 Atomic —— 它是唯一允许共享的**可变**值"); return 0;
        case OBJ_CLOSURE:      native_throw_error("不能冻结闭包 —— 闭包捕获的变量属于某条线程；需要共享函数请传 `func`"); return 0;
        case OBJ_FUNCTION:     native_throw_error("不能冻结函数 —— 函数对象挂在创建线程的堆上，生命周期无法保证"); return 0;
        case OBJ_MODULE:       native_throw_error("不能冻结模块"); return 0;
        case OBJ_FILE:         native_throw_error("不能冻结 File（文件句柄）"); return 0;
        case OBJ_SOCKET:       native_throw_error("不能冻结 Socket（连接句柄）"); return 0;
        case OBJ_THREAD:       native_throw_error("不能冻结 Thread"); return 0;
        case OBJ_COROUTINE:    native_throw_error("不能冻结协程"); return 0;
        case OBJ_FUTURE:       native_throw_error("不能冻结 Future"); return 0;
        case OBJ_FFI_LIBRARY:  native_throw_error("不能冻结 FFI 库句柄"); return 0;
        case OBJ_FFI_CALLBACK: native_throw_error("不能冻结 FFI 回调"); return 0;
        default:               native_throw_error("不能冻结此类型"); return 0;
    }
}

static int fz_check_value(Value v, int depth) {
    if (!val_is_obj(v)) return 1;          // 标量
    if (depth >= FZ_MAX_DEPTH) {
        native_throw_error("冻结失败：嵌套超过 64 层（可能含循环引用，冻结图不支持环）");
        return 0;
    }
    Object* o = val_as_obj(v);
    if (o->flags & OBJ_FLAG_FROZEN) return 1;   // 已冻结的子图直接复用
    switch (o->type) {
        case OBJ_STRING:
            return 1;
        case OBJ_ARRAY: {
            ObjArray* a = (ObjArray*)o;
            for (int i = 0; i < a->count; i++) {
                if (!fz_check_value(a->elements[i], depth + 1)) return 0;
            }
            return 1;
        }
        case OBJ_DICT: {
            ObjDict* d = (ObjDict*)o;
            if (d->array) {
                for (int i = 0; i < d->asize; i++) {
                    if (!fz_check_value(d->array[i], depth + 1)) return 0;
                }
            }
            if (d->entries) {
                for (int i = 0; i < d->capacity; i++) {
                    Value k = d->entries[i].key;
                    if (val_is_obj(k) && k != DICT_TOMBSTONE_VAL) {
                        if (!fz_check_value(k, depth + 1)) return 0;
                    }
                    if (!fz_check_value(d->entries[i].value, depth + 1)) return 0;
                }
            }
            return 1;
        }
        case OBJ_STRUCT: {
            ObjStruct* s = (ObjStruct*)o;
            if (!s->def) {
                native_throw_error("冻结 struct 失败：实例缺少结构体定义");
                return 0;
            }
            int n = s->def->field_count;
            for (int i = 0; i < n; i++) {
                if (!fz_check_value(s->field_values[i], depth + 1)) return 0;
            }
            return 1;
        }
        default:
            return fz_reject_type(o, depth);
    }
}

// ---------------------------------------------------------------- finalize
// 构建完成后统一做三件事：打 FROZEN 位、统计父→子边、记字节账。
// 为什么不在分配时做：容器插入顺序与"谁是父"在克隆过程中还没定型；
//   而且 FROZEN 位一旦提前打上，构建期自己的写入会被只读检查拦住。
typedef struct {
    Object** stack;
    int sp;
    int cap;
    int oom;
} FzStack;

static int fz_push(FzStack* s, Object* o) {
    if (s->oom) return 0;
    if (s->sp >= s->cap) {
        int ncap = s->cap ? s->cap * 2 : 256;
        Object** ns = (Object**)realloc(s->stack, sizeof(Object*) * (size_t)ncap);
        if (!ns) { s->oom = 1; return 0; }
        s->stack = ns;
        s->cap = ncap;
    }
    s->stack[s->sp++] = o;
    return 1;
}

static void fz_count_child(Object* child, void* ud) {
    // 父持有一份：直接加在 child 上（与"槽位口径"一致）
    __atomic_add_fetch(&child->refcnt, 1, __ATOMIC_RELAXED);
    FzStack* s = (FzStack*)ud;
    // 未处理过的子节点入栈继续遍历；已处理过的（FROZEN 已置位）不必再走
    if ((child->flags & OBJ_FLAG_FROZEN) == 0) fz_push(s, child);
}

static size_t fz_node_bytes(Object* o) {
    switch (o->type) {
        case OBJ_STRING: {
            ObjString* s = (ObjString*)o;
            return sizeof(ObjString) + (size_t)(s->capacity > 0 ? s->capacity : s->len + 1);
        }
        case OBJ_ARRAY: {
            ObjArray* a = (ObjArray*)o;
            return sizeof(ObjArray) + (size_t)a->capacity * sizeof(Value);
        }
        case OBJ_DICT: {
            ObjDict* d = (ObjDict*)o;
            return sizeof(ObjDict) + (size_t)d->asize * (sizeof(Value) + sizeof(int))
                 + (size_t)d->capacity * sizeof(ObjDictEntry)
                 + (size_t)d->order_capacity * sizeof(Value);
        }
        default:
            return o->size;   // struct 头 + 内联字段一次分配，size 就是全量
    }
}

static void frozen_finalize(Object* root) {
    FzStack st = {0};
    // 根也可能被重复冻结？不：_freeze 对已冻结值直接返回。这里仍按通用流程走。
    root->flags |= OBJ_FLAG_FROZEN;
    root->owner_thread = 0xFF;
    root->refcnt = 0;          // 从头统计（构建期恒 0，这行是防御）
    long bytes = (long)fz_node_bytes(root);
    long objs = 1;
    if (!fz_push(&st, root)) { free(st.stack); return; }

    while (st.sp > 0) {
        Object* o = st.stack[--st.sp];
        // 先把"子节点打标 + 计数 + 入栈"做完，再用一个暗标防止同一个对象被处理两次
        if (o != root) {
            if (o->flags & OBJ_FLAG_FROZEN) continue;
            o->flags |= OBJ_FLAG_FROZEN;
            o->owner_thread = 0xFF;
            o->refcnt = 0;
            bytes += (long)fz_node_bytes(o);
            objs++;
        }
        fz_foreach_child(o, fz_count_child, &st);
    }
    free(st.stack);
    frozen_stats_add(bytes, objs);
}

// ---------------------------------------------------------------- 释放
static void fz_free_node(Object* o) {
    switch (o->type) {
        case OBJ_STRING: {
            ObjString* s = (ObjString*)o;
            free(s->chars);
            break;
        }
        case OBJ_ARRAY: {
            ObjArray* a = (ObjArray*)o;
            type_free(a->type_info);
            free(a->elements);
            break;
        }
        case OBJ_DICT: {
            ObjDict* d = (ObjDict*)o;
            type_free(d->type_info);
            free(d->array);
            free(d->aorder);
            free(d->entries);
            free(d->order);
            break;
        }
        case OBJ_STRUCT: {
            ObjStruct* s = (ObjStruct*)o;
            if (!s->fields_inline) free(s->field_values);
            if (s->generic_type_args) {
                for (int i = 0; i < s->generic_type_arg_count; i++) free(s->generic_type_args[i]);
                free(s->generic_type_args);
            }
            break;
        }
        default:
            break;
    }
    free(o);
}

static void fz_unref_child(Object* child, void* ud) {
    // 先减（父不在了），减到 0 说明没有别的父 ⇒ 入栈释放
    if (__atomic_sub_fetch(&child->refcnt, 1, __ATOMIC_ACQ_REL) == 0) {
        fz_push((FzStack*)ud, child);
    }
}

static void frozen_free_graph(Object* root) {
    FzStack st = {0};
    if (!fz_push(&st, root)) { free(st.stack); return; }   // OOM：宁可泄漏也不乱 free
    while (st.sp > 0) {
        Object* o = st.stack[--st.sp];
        long bytes = (long)fz_node_bytes(o);
        fz_foreach_child(o, fz_unref_child, &st);
        fz_free_node(o);
        frozen_stats_add(-bytes, -1);
    }
    free(st.stack);
}

void frozen_unref(Object* o) {
    if (!o) return;
    int32_t after = __atomic_sub_fetch(&o->refcnt, 1, __ATOMIC_ACQ_REL);
    if (after <= 0) frozen_free_graph(o);
}

// ---------------------------------------------------------------- 注册表
static uint32_t fz_hash_ptr(Object* p) {
    uintptr_t x = (uintptr_t)p;
    x >>= 4;
    x ^= x >> 17;
    x *= 2654435761u;
    return (uint32_t)x;
}

static int fz_reg_grow(VM* vm, int want) {
    int ncap = vm->frozen_cap ? vm->frozen_cap * 2 : 64;
    while (ncap < want * 2) ncap *= 2;
    Object** nk = (Object**)calloc((size_t)ncap, sizeof(Object*));
    int* nc = (int*)calloc((size_t)ncap, sizeof(int));
    int* ne = (int*)calloc((size_t)ncap, sizeof(int));
    if (!nk || !nc || !ne) { free(nk); free(nc); free(ne); return 0; }
    for (int i = 0; i < vm->frozen_cap; i++) {
        Object* k = vm->frozen_keys[i];
        if (!k || k == FZ_REG_TOMBSTONE) continue;
        uint32_t h = fz_hash_ptr(k) & (uint32_t)(ncap - 1);
        while (nk[h]) h = (h + 1) & (uint32_t)(ncap - 1);
        nk[h] = k;
        nc[h] = vm->frozen_counts[i];
        ne[h] = vm->frozen_epochs[i];
    }
    free(vm->frozen_keys);
    free(vm->frozen_counts);
    free(vm->frozen_epochs);
    vm->frozen_keys = nk;
    vm->frozen_counts = nc;
    vm->frozen_epochs = ne;
    vm->frozen_cap = ncap;
    return 1;
}

int frozen_reg_acquire(VM* vm, Object* o) {
    if (!vm || !o) return 0;
    if (vm->frozen_cap == 0 || (vm->frozen_cnt + 1) * 2 > vm->frozen_cap) {
        if (!fz_reg_grow(vm, vm->frozen_cnt + 1)) {
            // 表建不出来 ⇒ 单独持有一份引用（不登记，退出时收不回来，但绝不让它悬空）
            frozen_ref(o);
            return 1;
        }
    }
    uint32_t mask = (uint32_t)(vm->frozen_cap - 1);
    uint32_t h = fz_hash_ptr(o) & mask;
    int first_tomb = -1;
    for (int probe = 0; probe < vm->frozen_cap; probe++) {
        Object* k = vm->frozen_keys[h];
        if (k == o) return 0;                    // 已登记：去重（引用计数不变）
        if (k == FZ_REG_TOMBSTONE) {
            if (first_tomb < 0) first_tomb = (int)h;
        } else if (!k) {
            int slot = (first_tomb >= 0) ? first_tomb : (int)h;
            vm->frozen_keys[slot] = o;
            vm->frozen_counts[slot] = 1;
            vm->frozen_epochs[slot] = vm->frozen_epoch;
            vm->frozen_cnt++;
            frozen_ref(o);
            return 1;
        }
        h = (h + 1) & mask;
    }
    // 全是墓碑：用第一个墓碑
    if (first_tomb >= 0) {
        vm->frozen_keys[first_tomb] = o;
        vm->frozen_counts[first_tomb] = 1;
        vm->frozen_epochs[first_tomb] = vm->frozen_epoch;
        vm->frozen_cnt++;
        frozen_ref(o);
        return 1;
    }
    frozen_ref(o);
    return 1;
}

void frozen_reg_release_all(VM* vm) {
    if (!vm || !vm->frozen_keys) return;
    for (int i = 0; i < vm->frozen_cap; i++) {
        Object* k = vm->frozen_keys[i];
        if (!k || k == FZ_REG_TOMBSTONE) continue;
        frozen_unref(k);
    }
    free(vm->frozen_keys);
    free(vm->frozen_counts);
    free(vm->frozen_epochs);
    vm->frozen_keys = NULL;
    vm->frozen_counts = NULL;
    vm->frozen_epochs = NULL;
    vm->frozen_cap = 0;
    vm->frozen_cnt = 0;
}

int frozen_reg_mark(VM* vm) {
    if (!vm) return 0;
    return vm->frozen_epoch++;
}

void frozen_reg_release_since(VM* vm, int mark) {
    if (!vm || !vm->frozen_keys) return;
    for (int i = 0; i < vm->frozen_cap; i++) {
        Object* k = vm->frozen_keys[i];
        if (!k || k == FZ_REG_TOMBSTONE) continue;
        if (vm->frozen_epochs[i] <= mark) continue;
        vm->frozen_keys[i] = FZ_REG_TOMBSTONE;
        vm->frozen_cnt--;
        frozen_unref(k);
    }
}

// ---------------------------------------------------------------- 语义入口
int frozen_is(Value v) {
    return obj_is_frozen(v);
}

Value frozen_freeze(Value v) {
    if (!val_is_obj(v)) return v;                  // 标量无需冻结
    Object* o = val_as_obj(v);
    if (o->flags & OBJ_FLAG_FROZEN) return v;      // 幂等：已经是冻结图
    if (!fz_check_value(v, 0)) return val_null();  // 类型/环预检（已在内部报错）

    int prev = gc_frozen_build_begin();
    Value out = leno_clone_value(v, LENO_CLONE_FREEZE);
    gc_frozen_build_end(prev);
    if (!val_is_obj(out)) return val_null();       // 克隆路径已报错

    Object* root = val_as_obj(out);
    frozen_finalize(root);

    // 交给当前 VM 的注册表持有一份引用（否则根本没有"根"，计数一直是 0）
    extern THREAD_LOCAL VM* current_exec_vm;
    VM* tvm = current_exec_vm;
    if (!tvm) {
        extern VM vm;
        tvm = &vm;
    }
    frozen_reg_acquire(tvm, root);
    return out;
}

Value frozen_thaw(Value v) {
    if (!obj_is_frozen(v)) {
        native_throw_error("_thaw() 只接受冻结对象（普通值本来就是可写的，直接改它即可）");
        return val_null();
    }
    // 解冻 = 深拷贝回本线程堆（mode=THAW：连 frozen 节点也复制），结果可写
    return leno_clone_value(v, LENO_CLONE_THAW);
}

// ---------------------------------------------------------------- natives
Value frozen_native_freeze(int argCount, Value* args) {
    if (argCount < 1) {
        native_throw_error("_freeze() 需要 1 个参数");
        return val_null();
    }
    return frozen_freeze(args[0]);
}

Value frozen_native_thaw(int argCount, Value* args) {
    if (argCount < 1) {
        native_throw_error("_thaw() 需要 1 个参数");
        return val_null();
    }
    return frozen_thaw(args[0]);
}

Value frozen_native_is_frozen(int argCount, Value* args) {
    return val_bool(argCount >= 1 && obj_is_frozen(args[0]));
}
