// ============================================================================
// 分代垃圾回收器（Generational GC）
//
// 架构：
//   年轻代（Young Generation）：
//     - 新对象分配在年轻代
//     - Minor GC 只扫描年轻代，存活对象 survived 计数递增
//     - survived >= promote_age 的对象晋升到老年代
//
//   老年代（Old Generation）：
//     - 长寿命对象存储在老年代
//     - Major GC 扫描全部（年轻代 + 老年代）
//     - 触发频率远低于 Minor GC
//
//   写屏障（Write Barrier）：
//     - 当老年代对象引用年轻代对象时，将老年代对象加入 remembered set
//     - Minor GC 标记阶段扫描 remembered set 中对象的子引用，
//       标记老年代对象引用的年轻代对象（老年代对象本身不再递归扫描）
//     - remembered set 维护（三条不变式）：
//       1. 晋升：对象晋升老年代时整体加入（sweep_young）
//       2. 写入：老年代对象获得新的年轻代引用时由写屏障加入
//       3. 剪枝：每轮 GC 后移除子引用中已无年轻代对象的条目
//
// 收集策略：
//   gc_alloc() → 年轻代分配超阈值 → Minor GC
//   Minor GC 后仍超阈值 → 总内存超老年代阈值 → Major GC
//   gc_collect() → 根据总内存自动选择 Minor/Major GC
// ============================================================================

/* Socket 资源释放所需的平台头文件（必须在 windows.h 之前） */
#ifdef _WIN32
    #include <winsock2.h>
    #include <windows.h>
    #ifdef _MSC_VER
        #pragma comment(lib, "ws2_32.lib")
    #endif
#else
    #include <unistd.h>
#endif

#include "include/lenolang.h"
#include "include/string_table.h"
#include "include/native.h"
#include "include/leno_vm.h"
// guis module removed
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>

// 数组对象回收（object/object_array.c）
extern int arr_try_recycle(ObjArray* arr);



// GC 全局实例（线程局部存储）
THREAD_LOCAL LenoGC gc = {0};

// 安全的字符串长度计算（防止野指针导致崩溃）
static size_t safe_strlen(const char* str) {
    if (!str) return 0;
    // 简单的边界检查，防止野指针导致无限循环
    size_t len = 0;
    const char* p = str;
    while (len < 1024 * 1024) {  // 最大1MB
        if (*p == '\0') return len;
        p++;
        len++;
    }
    return 0;  // 超过边界，可能是野指针
}

// 检查对象类型是否有效
static int is_valid_obj_type(ObjType type) {
    return type >= OBJ_STRING && type < OBJ_NONE;
}

// ============================================================================
// 写屏障（Write Barrier）
// ============================================================================

// 将对象加入 remembered set（去重）
static void remembered_set_add(Object* obj) {
    // 动态扩容
    if (gc.remembered_count >= gc.remembered_capacity) {
        int new_cap = gc.remembered_capacity * 2;
        if (new_cap < GC_REMEMBERED_INIT) new_cap = GC_REMEMBERED_INIT;
        Object** new_set = (Object**)realloc(gc.remembered_set, new_cap * sizeof(Object*));
        if (!new_set) return;
        gc.remembered_set = new_set;
        gc.remembered_capacity = new_cap;
    }
    // 去重检查
    for (int i = 0; i < gc.remembered_count; i++) {
        if (gc.remembered_set[i] == obj) return;
    }
    gc.remembered_set[gc.remembered_count++] = obj;
}

// 从 remembered set 移除对象（Major GC 释放老年代对象时调用）
static void remembered_set_remove(Object* obj) {
    if (!obj) return;
    for (int i = 0; i < gc.remembered_count; i++) {
        if (gc.remembered_set[i] == obj) {
            gc.remembered_set[i] = gc.remembered_set[--gc.remembered_count];
            return;
        }
    }
}

// 写屏障：老年代对象 holder 的 Value 字段被写入年轻代引用时调用
void gc_write_barrier(Object* holder, Value value) {
    if (!val_is_obj(value)) return;
    if (holder->generation != GEN_OLD) return;
    Object* val_obj = val_as_obj(value);
    if (val_obj->generation == GEN_YOUNG) {
        remembered_set_add(holder);
    }
}

// 写屏障：老年代对象 holder 的 Object* 字段被写入年轻代引用时调用
void gc_write_barrier_obj(Object* holder, Object* value_obj) {
    if (holder->generation != GEN_OLD) return;
    if (value_obj->generation == GEN_YOUNG) {
        remembered_set_add(holder);
    }
}

// ============================================================================
// remembered set 扫描与维护
// ============================================================================

// remembered set 扫描标志：scan_remembered_set 执行期间置位。
// gc_mark_object 据此统计扫描中发现的年轻代子引用数（用于条目剪枝判断）。
// 计数必须发生在 marked 检查之前：已标记的年轻代对象同样代表老年代条目持有年轻引用。
static THREAD_LOCAL int in_remembered_scan = 0;
static THREAD_LOCAL int young_mark_counter = 0;

// 晋升专用入集：无去重直接加入。
// 新晋升对象此前是年轻代，不可能已在 remembered set 中（集合只持有 GEN_OLD 对象），
// 跳过去重的 O(n) 线性扫描，避免大批量晋升时的 O(n²) 开销。
static void remembered_set_add_promoted(Object* obj) {
    if (gc.remembered_count >= gc.remembered_capacity) {
        int new_cap = gc.remembered_capacity * 2;
        if (new_cap < GC_REMEMBERED_INIT) new_cap = GC_REMEMBERED_INIT;
        Object** new_set = (Object**)realloc(gc.remembered_set, new_cap * sizeof(Object*));
        if (!new_set) return;
        gc.remembered_set = new_set;
        gc.remembered_capacity = new_cap;
    }
    gc.remembered_set[gc.remembered_count++] = obj;
}

// 将老年代对象加入 remembered set（供批量字段写入后保守调用）。
// 例如协程挂起时整块拷贝 saved_frames（含大量 Value 字段），
// 逐字段调用写屏障开销大，直接对持有者入集更简单；
// 若实际不持有年轻代引用，下一轮 GC 的剪枝会将其移出。
void gc_remember_object(Object* holder) {
    if (!holder) return;
    if (holder->generation != GEN_OLD) return;
    remembered_set_add(holder);
}

// ============================================================================
// GC 初始化与控制
// ============================================================================

void gc_init(void) {
    // 注意：不清零 young_heap/old_heap，因为 semantic 阶段可能已经通过 gc_alloc 分配了对象
    // 这些对象会被 gc_free_all 正确释放
    gc.young_allocated = 0;
    gc.old_allocated = 0;
    gc.young_threshold = GC_YOUNG_THRESHOLD;
    gc.old_threshold = GC_OLD_THRESHOLD;
    gc.running = 0;
    gc.mode = GC_MODE_FULL;
    gc.vm = NULL;
    gc.enabled = 1;
    gc.deferred_gc = 0;            // 延迟 GC 标志
    gc.remembered_count = 0;
    gc.remembered_capacity = 0;
    gc.promote_age = GC_PROMOTE_AGE;
    gc.minor_gc_count = 0;
    gc.extra_root_count = 0;
    gc.extra_root_capacity = 0;
}

// 设置 GC 开关（1=启用，0=禁用）
void gc_set_enabled(int enabled) {
    gc.enabled = enabled;
}

// 获取 GC 开关状态
int gc_get_enabled(void) {
    return gc.enabled;
}

void gc_push_root(Value* ptr) {
    if (gc.extra_root_count >= gc.extra_root_capacity) {
        int new_cap = gc.extra_root_capacity * 2;
        if (new_cap < 64) new_cap = 64;
        Value** new_roots = (Value**)realloc(gc.extra_roots, new_cap * sizeof(Value*));
        if (!new_roots) return;
        gc.extra_roots = new_roots;
        gc.extra_root_capacity = new_cap;
    }
    gc.extra_roots[gc.extra_root_count++] = ptr;
}

void gc_pop_root(void) {
    if (gc.extra_root_count > 0) {
        gc.extra_root_count--;
    }
}

// ============================================================================
// 小对象 size-class 池
//
// 目标：消除高频小对象（struct 实例如 Hit、闭包、短字符串头等）的系统
// malloc/free 开销（对象版光线追踪实测：Hit 分配 ~114ns vs Python pymalloc
// ~84ns 的主要差距来源，见 docs/函数调用性能优化_调用帧与多返回值解构.md 方向 5）
//
// 设计：
//   - 仅池化 [16B, 256B] 的对象，按 16B 对齐分 16 个 size-class
//   - 每类维护 free-list（复用对象头的 next 指针串链）
//   - free-list 空时分配新块（头部 16B + 64 槽），整块切割入链
//   - 池持有内存上限 GC_POOL_MEM_LIMIT，超限回退系统 malloc
//     （池化内存不归还 OS，靠上限防止常驻内存无限膨胀）
//
// 安全性：
//   - 池类别编码在 Object.flags（0x80=池化位 | 低 4 位=class 索引），
//     不依赖 obj->size——gc_track_memory 会改写 size（如字符串/字典扩容
//     记账），若按 size 反推 class 会把槽挂错链导致堆损坏
//   - OBJ_ARRAY 不池化：数组有自己的 header 回收列表（arr_try_recycle），
//     且 arr_new 复用时会将 flags 清零，双重持有会破坏池链
//   - GC 非移动（晋升仅翻转 generation 标志），槽地址稳定
//   - 与 gc_free_all：遍历池块列表统一 free，槽内存随块释放
// ============================================================================

#define GC_POOL_ALIGN        16                       // 对齐粒度（同时也是最小池化尺寸）
#define GC_POOL_MAX_SIZE     256                      // 池化对象尺寸上限
#define GC_POOL_CLASSES      (GC_POOL_MAX_SIZE / GC_POOL_ALIGN)  // 16 类
#define GC_POOL_BLOCK_SLOTS  64                       // 每块槽数
#define GC_POOL_MEM_LIMIT    (4 * 1024 * 1024)        // 池持有内存上限 4MB
#define OBJ_FLAG_POOLED      0x80                     // flags 位：池化对象
#define GC_POOL_CLASS_MASK   0x0F                     // flags 低 4 位：class 索引

typedef struct PoolBlockHdr {
    struct PoolBlockHdr* next;   // 块链表（gc_free_all 统一释放）
    size_t slot_size;            // 本块槽大小（调试/统计）
} PoolBlockHdr;                  // 恰好 16B，保证槽起始 16B 对齐

static THREAD_LOCAL Object*  pool_free_list[GC_POOL_CLASSES];  // 各 class 的空闲槽链
static THREAD_LOCAL PoolBlockHdr* pool_block_list;             // 全部池块（跨 class）
static THREAD_LOCAL size_t   pool_total;                       // 池持有字节（块计）

// 释放对象内存：池化对象归还 free-list，否则系统 free
// （sweep_young / sweep_old / gc_free_all 共用；free_object_resources 已先行调用）
static void gc_free_object(Object* obj) {
    if ((obj->flags & OBJ_FLAG_POOLED) != 0) {
        uint32_t idx = obj->flags & GC_POOL_CLASS_MASK;
        size_t slot_size = (size_t)(idx + 1) * GC_POOL_ALIGN;
        memset(obj, 0, slot_size);              // 清零整个槽（含对象头，防悬挂数据）
        obj->next = pool_free_list[idx];        // 复用 next 串空闲链
        pool_free_list[idx] = obj;
    } else {
        free(obj);
    }
}

// gc_free_all 收尾：释放所有池块并重置池状态
// （调用前所有对象已走 gc_free_object，槽内存归块所有，直接 free 块即可）
static void gc_pool_free_all(void) {
    PoolBlockHdr* blk = pool_block_list;
    while (blk) {
        PoolBlockHdr* next = blk->next;
        free(blk);
        blk = next;
    }
    pool_block_list = NULL;
    pool_total = 0;
    for (int i = 0; i < GC_POOL_CLASSES; i++) {
        pool_free_list[i] = NULL;
    }
}

// ============================================================================
// 对象分配
// ============================================================================

// 分配 GC 管理的对象，新对象分配到年轻代
// 注意：不再同步触发 GC，仅设置 deferred_gc 标志。
// 由 GUI 事件循环在帧间（draw callback 返回后）调用 gc_try_collect_deferred() 执行。
// 这样一帧内创建几百个对象时不会产生多次 GC 暂停。
Object* gc_alloc(size_t size, ObjType type) {
    // 年轻代分配超阈值时设置延迟 GC 标志（不在此处同步执行）
    if (gc.enabled && gc.young_allocated + size > gc.young_threshold && !gc.running) {
        gc.deferred_gc = 1;
    }

    Object* obj = NULL;
    uint8_t pooled_flags = 0;

    // 小对象 size-class 池快速路径：命中 free-list 时免系统 malloc/free
    // （OBJ_ARRAY 不池化，见池设计注释；类型见下方安全性说明）
    if (size >= GC_POOL_ALIGN && size <= GC_POOL_MAX_SIZE && type != OBJ_ARRAY) {
        uint32_t idx = (uint32_t)((size + GC_POOL_ALIGN - 1) / GC_POOL_ALIGN) - 1;  // 0..15
        size_t slot_size = (size_t)(idx + 1) * GC_POOL_ALIGN;

        obj = pool_free_list[idx];
        if (obj != NULL) {
            pool_free_list[idx] = obj->next;     // 摘链（槽已在上次归还时清零）
            memset(obj, 0, slot_size);
            pooled_flags = (uint8_t)(OBJ_FLAG_POOLED | idx);
        } else if (pool_total + GC_POOL_BLOCK_SLOTS * slot_size <= GC_POOL_MEM_LIMIT) {
            // free-list 空：分配新块整块切割（头部 16B + 64 槽）
            PoolBlockHdr* blk = (PoolBlockHdr*)malloc(sizeof(PoolBlockHdr) + GC_POOL_BLOCK_SLOTS * slot_size);
            if (blk) {
                char* base = (char*)blk + sizeof(PoolBlockHdr);
                // 除首槽外全部入 free-list（倒序链接，首槽本次使用）
                for (int i = GC_POOL_BLOCK_SLOTS - 1; i >= 1; i--) {
                    Object* s = (Object*)(base + (size_t)i * slot_size);
                    memset(s, 0, slot_size);
                    s->next = pool_free_list[idx];
                    pool_free_list[idx] = s;
                }
                blk->next = pool_block_list;
                blk->slot_size = slot_size;
                pool_block_list = blk;
                pool_total += GC_POOL_BLOCK_SLOTS * slot_size;
                obj = (Object*)base;
                memset(obj, 0, slot_size);
                pooled_flags = (uint8_t)(OBJ_FLAG_POOLED | idx);
            }
        }
        // 池超限或块分配失败：回退系统 malloc 路径
    }

    if (!obj) {
        obj = (Object*)malloc(size);
        if (obj) memset(obj, 0, size);
    }

    // 分配失败时尝试 Major GC 释放内存
    if (!obj && gc.enabled && !gc.running) {
        gc_major_collect();
        obj = (Object*)malloc(size);
        if (!obj) {
            // 放宽阈值，避免反复触发 GC
            gc.old_threshold = gc.old_allocated + gc.young_allocated + (1024 * 1024);
        }
    }

    if (!obj) {
        error_add_at(ERR_RUNTIME, 0, 0, "内存分配失败");
        return NULL;
    }

    // 初始化对象头部，新对象默认为年轻代
    obj->type = type;
    // 所有新对象标记为存活，避免 GC 误回收刚分配但尚未存入根集合的对象。
    // 存活对象在下轮 GC 时会被 mark_roots 重新标记；已失效对象会被 clear_all_marks
    // 清零后在下轮 sweep 中回收。额外的存活一个 GC 周期不会导致明显的内存浪费。
    obj->marked = 1;
    obj->flags = pooled_flags;   // 池化位 + class 索引（malloc 路径为 0）
    obj->generation = GEN_YOUNG;
    obj->survived = 0;
    obj->size = size;
    obj->next = gc.young_heap;
    gc.young_heap = obj;
    gc.young_allocated += size;

    return obj;
}

// VM 安全点 GC 检查：在指令边界调用，确保 VM 状态一致
// 同样使用延迟模式：只置标志，不同步触发
void gc_check_safe_point(void) {
    if (gc.enabled && !gc.running) {
        if (gc.young_allocated > gc.young_threshold) {
            gc.deferred_gc = 1;
        }
    }
}

// 跟踪非 GC 对象的内存变化（如动态数组扩容）
// 同时更新 obj->size，确保分代晋升时内存计数正确转移
void gc_track_memory(Object* obj, size_t old_size, size_t new_size) {
    if (new_size > old_size) {
        size_t diff = new_size - old_size;
        if (obj && obj->generation == GEN_OLD) {
            gc.old_allocated += diff;
        } else {
            gc.young_allocated += diff;
        }
        if (obj) obj->size += diff;
    } else if (old_size > new_size) {
        size_t diff = old_size - new_size;
        if (obj && obj->generation == GEN_OLD) {
            if (gc.old_allocated >= diff) gc.old_allocated -= diff; else gc.old_allocated = 0;
        } else {
            if (gc.young_allocated >= diff) gc.young_allocated -= diff; else gc.young_allocated = 0;
        }
        if (obj) obj->size -= diff;
    }
}

// ============================================================================
// 标记阶段（Mark Phase）— 迭代式标记（显式栈替代递归）
// ============================================================================
//
// 三色标记模型：
//   White  — 未标记（未被 mark_roots 触及）
//   Gray   — 已标记（marked=1），但子引用尚未扫描（在 mark_stack 上）
//   Black  — 已标记且子引用已扫描（已从 mark_stack 弹出）
//
// 流程：
//   1. mark_roots() 对每个根调用 gc_mark_object/gc_mark_value
//   2. gc_mark_object(obj): 标记 obj → 压入 mark_stack（不递归）
//   3. gc_drain_mark_stack(): 弹出 obj → gc_scan_children(obj) 扫描子引用
//      → 对每个子引用调用 gc_mark_object（标记+压栈）
//   4. 重复 3 直到栈空
//
// 优势：标记深度不再受 C 调用栈限制，10000 层链表也能安全标记。

// --- 前向声明 ---
void gc_scan_children(Object* obj);

// --- 显式标记栈 ---
// ★ 必须是 THREAD_LOCAL：GC 状态（LenoGC gc）是线程局部的，
// 每个 worker 线程独立触发 GC。如果 mark_stack 是全局共享的，
// 多个线程同时 GC 时会互相冲覆 mark_stack（一个线程 push 的对象
// 被另一个线程 pop，或 realloc 导致指针失效），导致标记遗漏 →
// 存活对象被误回收 → use-after-free → 内存损坏。
static THREAD_LOCAL Object** mark_stack = NULL;
static THREAD_LOCAL int mark_stack_count = 0;
static THREAD_LOCAL int mark_stack_capacity = 0;

static void mark_stack_push(Object* obj) {
    if (mark_stack_count >= mark_stack_capacity) {
        int new_cap = mark_stack_capacity * 2;
        if (new_cap < 256) new_cap = 256;
        Object** new_stack = (Object**)realloc(mark_stack, new_cap * sizeof(Object*));
        if (!new_stack) {
            // realloc 失败：回退到递归标记，避免丢失对象
            gc_scan_children(obj);
            return;
        }
        mark_stack = new_stack;
        mark_stack_capacity = new_cap;
    }
    mark_stack[mark_stack_count++] = obj;
}

// 扫描对象的子引用（原 gc_mark_object 的 switch 主体）
// 对每个子引用调用 gc_mark_object / gc_mark_value（标记 + 压栈）
void gc_scan_children(Object* obj) {
    switch (obj->type) {
        // 闭包：标记函数和所有 upvalue
        case OBJ_CLOSURE: {
            ObjClosure* closure = (ObjClosure*)obj;
            gc_mark_object((Object*)closure->function);
            for (int i = 0; i < closure->upvalue_count; i++) {
                gc_mark_object((Object*)closure->upvalues[i]);
            }
            break;
        }
        // 函数：标记常量池和所属模块
        case OBJ_FUNCTION: {
            ObjFunction* func = (ObjFunction*)obj;
            if (func->chunk) {
                for (int i = 0; i < func->chunk->const_cnt; i++) {
                    gc_mark_value(func->chunk->constants[i]);
                }
            }
            if (func->module) {
                gc_mark_object((Object*)func->module);
            }
            break;
        }
        // 原生函数：无子引用
        case OBJ_NATIVE:
            break;
        case OBJ_SOCKET:
            break;
        /* 数组：标记所有元素 */
        case OBJ_ARRAY: {
            ObjArray* arr = (ObjArray*)obj;
            for (int i = 0; i < arr->count; i++) {
                gc_mark_value(arr->elements[i]);
            }
            break;
        }
        // 字典：标记数组部分、哈希表部分和插入顺序数组中的键值
        case OBJ_DICT: {
            ObjDict* dict = (ObjDict*)obj;
            for (int i = 0; i < dict->asize; i++) {
                if (!val_is_null(dict->array[i])) {
                    gc_mark_value(dict->array[i]);
                }
            }
            for (int i = 0; i < dict->capacity; i++) {
                Value entry_key = dict->entries[i].key;
                if (!val_is_null(entry_key) && entry_key != DICT_TOMBSTONE_VAL) {
                    if (val_is_obj(entry_key)) {
                        gc_mark_object(val_as_obj(entry_key));
                    }
                    gc_mark_value(dict->entries[i].value);
                }
            }
            for (int i = 0; i < dict->order_count; i++) {
                Value order_key = dict->order[i];
                if (val_is_obj(order_key)) {
                    gc_mark_object(val_as_obj(order_key));
                }
            }
            break;
        }
        // Upvalue：标记引用的值、关闭值和链表下一个节点
        case OBJ_UPVALUE: {
            Upvalue* upvalue = (Upvalue*)obj;
            gc_mark_value(*upvalue->location);
            gc_mark_value(upvalue->closed);
            if (upvalue->next) {
                gc_mark_object((Object*)upvalue->next);
            }
            break;
        }
        // 字符串：无子引用（字符数据由 free_object_resources 释放）
        case OBJ_STRING:
            break;
        // 大整数：无子引用（limbs 数组由 free_object_resources 释放）
        case OBJ_BIGINT:
            break;
        // 模块：标记导出字典和所有全局变量
        case OBJ_MODULE: {
            ObjModule* module = (ObjModule*)obj;
            if (module->exports) {
                gc_mark_object((Object*)module->exports);
            }
            for (int i = 0; i < module->global_count; i++) {
                gc_mark_value(module->globals[i]);
            }
            if (module->init_chunk) {
                for (int i = 0; i < module->init_chunk->const_cnt; i++) {
                    gc_mark_value(module->init_chunk->constants[i]);
                }
            }
            break;
        }
        // 绑定方法：标记接收者和方法
        case OBJ_BOUND_METHOD: {
            ObjBoundMethod* bound = (ObjBoundMethod*)obj;
            gc_mark_value(bound->receiver);
            if (bound->method) gc_mark_object((Object*)bound->method);
            if (bound->closure) gc_mark_object((Object*)bound->closure);
            break;
        }
        // 文件对象：标记路径和模式字符串
        case OBJ_FILE: {
            ObjFile* file = (ObjFile*)obj;
            gc_mark_object((Object*)file->path);
            gc_mark_object((Object*)file->mode);
            break;
        }
        // 结构体定义：标记字段默认值和方法（闭包或函数）
        case OBJ_STRUCT_DEF: {
            ObjStructDef* def = (ObjStructDef*)obj;
            for (int i = 0; i < def->field_count; i++) {
                if (def->fields[i].has_default) {
                    gc_mark_value(def->fields[i].default_value);
                }
            }
            for (int i = 0; i < def->method_count; i++) {
                if (def->methods[i].closure) {
                    gc_mark_object((Object*)def->methods[i].closure);
                } else if (def->methods[i].func) {
                    gc_mark_object((Object*)def->methods[i].func);
                }
            }
            // 标记关联常量值
            for (int i = 0; i < def->const_count; i++) {
                gc_mark_value(def->const_values[i]);
            }
            break;
        }
        case OBJ_FACE_DEF:
            break;
        // 结构体实例：标记定义和所有字段值
        case OBJ_STRUCT: {
            ObjStruct* struct_obj = (ObjStruct*)obj;
            if (struct_obj->def) {
                gc_mark_object((Object*)struct_obj->def);
                for (int i = 0; i < struct_obj->def->field_count; i++) {
                    gc_mark_value(struct_obj->field_values[i]);
                }
            }
            if (struct_obj->declared_face) {
                gc_mark_object((Object*)struct_obj->declared_face);
            }
            break;
        }
        // 协程：标记闭包、结果、等待对象、链表下一个、保存的帧和初始参数
        case OBJ_COROUTINE: {
            ObjCoroutine* co = (ObjCoroutine*)obj;
            gc_mark_object((Object*)co->closure);
            gc_mark_value(co->result);
            gc_mark_object((Object*)co->waiting_for);
            gc_mark_object((Object*)co->task_future);
            if (co->next) {
                gc_mark_object((Object*)co->next);
            }
            // 标记保存的调用帧副本数组
            if (co->saved_frames && co->saved_frame_count > 0) {
                for (int fi = 0; fi < co->saved_frame_count; fi++) {
                    CallFrame* frame_copy = &co->saved_frames[fi];
                    gc_mark_object((Object*)frame_copy->closure);
                    if (frame_copy->chunk) {
                        for (int j = 0; j < frame_copy->chunk->const_cnt; j++) {
                            gc_mark_value(frame_copy->chunk->constants[j]);
                        }
                    }
                    if (frame_copy->locals) {
                        for (int i = 0; i < frame_copy->local_count; i++) {
                            gc_mark_value(frame_copy->locals[i]);
                        }
                    }
                    // FRAME_TRY_VALID：无 try 函数的帧 has_try_return 为脏值（帧复用优化）
                    if (FRAME_TRY_VALID(frame_copy) && frame_copy->has_try_return) {
                        gc_mark_value(frame_copy->try_return_value);
                    }
                    if (frame_copy->module) {
                        gc_mark_object((Object*)frame_copy->module);
                    }
                }
            }
            if (co->initial_args) {
                for (int i = 0; i < co->initial_arg_count; i++) {
                    gc_mark_value(co->initial_args[i]);
                }
            }
            break;
        }
        // Future：标记结果、错误和等待者
        case OBJ_FUTURE: {
            ObjFuture* future = (ObjFuture*)obj;
            gc_mark_value(future->result);
            gc_mark_value(future->error);
            gc_mark_object((Object*)future->waiter);
            break;
        }
        // 线程对象：标记返回值和 pending_args
        // ★ 加锁扫描：pending_args 在 mutex 保护下被子线程读取/清空，
        // result 也在 mutex 保护下被设置/克隆。GC 必须加锁同步。
        // 死锁分析：与 OBJ_CHANNEL 相同——GC 在安全点触发，当前线程不持有
        // thread mutex；其他线程持有 mutex 时 GC 短暂阻塞，不影响正确性。
        case OBJ_THREAD: {
            ObjThread* thread = (ObjThread*)obj;
            platform_mutex_lock(&thread->mutex);
            gc_mark_value(thread->result);
            if (thread->pending_args && thread->pending_arg_count > 0) {
                for (int i = 0; i < thread->pending_arg_count; i++) {
                    gc_mark_value(thread->pending_args[i]);
                }
            }
            platform_mutex_unlock(&thread->mutex);
            break;
        }
        // 通道：标记缓冲区中的值
        // ★ 线程安全修复：加锁后扫描 buffer，防止与并发 channel_send/try_send/
        // receive/try_receive 的数据竞争。
        // 参考 Go channel 的 GC 处理：Go 的 GC 在 stop-the-world 阶段扫描 channel
        // buffer（所有 goroutine 暂停），而 Leno 是 per-thread GC（其他线程仍在运行），
        // 因此必须加锁同步。
        //
        // 死锁分析：
        // - GC 在安全点（OP_RETURN / 帧间）触发，当前线程不持有 channel mutex
        // - channel_try_receive/receive 中 value_clone_for_channel 已禁用同步 GC
        //   （gc_set_enabled(0)），不会在持有 mutex 时触发 gc_major_collect
        // - channel_send/try_send 中 value_clone_for_channel 在 mutex 外调用
        // - 因此 GC lock 不会与当前线程的 channel 操作产生自死锁
        // - 其他线程持有 mutex 时，GC 线程短暂阻塞等待，不影响正确性
        case OBJ_CHANNEL: {
            ObjChannel* channel = (ObjChannel*)obj;
            platform_mutex_lock(&channel->mutex);
            if (channel->buffer && channel->count > 0) {
                for (int i = 0; i < channel->count; i++) {
                    int idx = (channel->head + i) % channel->capacity;
                    gc_mark_value(channel->buffer[idx]);
                }
            }
            platform_mutex_unlock(&channel->mutex);
            break;
        }
        // C 结构体数组视图：标记所属 C 结构体
        case OBJ_CSTRUCT_ARRAY_VIEW: {
            ObjCStructArrayView* view = (ObjCStructArrayView*)obj;
            gc_mark_object((Object*)view->cstruct);
            break;
        }
        // C 结构体实例：标记结构体定义
        case OBJ_CSTRUCT: {
            ObjCStruct* cs = (ObjCStruct*)obj;
            gc_mark_object((Object*)cs->def);
            break;
        }
        // C 结构体数组：标记结构体定义
        case OBJ_CSTRUCT_ARRAY: {
            ObjCStructArray* csa = (ObjCStructArray*)obj;
            gc_mark_object((Object*)csa->def);
            break;
        }
        // 以下类型无 GC 子引用
        case OBJ_ENUM_DEF:
            break;
        case OBJ_CSTRUCT_DEF:
            break;
        case OBJ_RANGE:
            break;
        case OBJ_FFI_LIBRARY:
            break;
        case OBJ_FFI_POINTER:
            break;
        case OBJ_FFI_CALLBACK: {
            ObjFFICallback* cb = (ObjFFICallback*)obj;
            gc_mark_value(cb->func_val);
            break;
        }
        case OBJ_NONE:
        case OBJ_INT:
        case OBJ_FLOAT:
            break;
    }
}

// 标记对象：标记 + 压入显式栈（不递归，由 gc_drain_mark_stack 负责扫描子引用）
void gc_mark_object(Object* obj) {
    if (!obj) return;

    // 安全检查：验证对象类型是否有效
    if (!is_valid_obj_type(obj->type)) {
        return;  // 无效的对象类型，跳过
    }

    // ★ Minor GC 分代标记核心：老年代对象直接返回，不标记也不扫描子引用。
    //   其引用的年轻代对象由 scan_remembered_set 扫描 remembered set 覆盖：
    //   - 晋升时整体加入 remembered set（sweep_young）
    //   - 后续获得新的年轻代引用时由写屏障重新加入
    //   老年代对象在 Minor GC 中不参与 sweep，marked 标志无意义。
    //   这是分代 GC 的本质收益：Minor GC 不再遍历整个老年代对象图。
    if (gc.mode == GC_MODE_MINOR && obj->generation == GEN_OLD) {
        return;
    }

    // remembered set 扫描期间的年轻代引用计数（条目剪枝依据）。
    // 必须放在 marked 检查之前：子对象已被根标记时同样说明条目持有年轻代引用。
    if (in_remembered_scan && obj->generation == GEN_YOUNG) {
        young_mark_counter++;
    }

    // ★ Channel 和 Thread 对象的 marked 标志可能被其他线程的 GC 留下 stale 值：
    // - Channel: malloc 分配，不在任何线程的 GC heap 链表中，clear_all_marks 不会清除
    // - Thread: result/pending_args 中的值可能指向子线程 GC 堆上的对象，
    //   子线程 GC 会设置 marked=1，但主线程 clear_all_marks 不会清除子线程堆对象
    // 修复：对这两种类型，即使 marked==1 也仍然扫描子对象。
    // 去重由 gc_mark_object 对每个子对象各自的 marked 检查来保证，不会无限递归。
    if (obj->type == OBJ_CHANNEL || obj->type == OBJ_THREAD) {
        if (!obj->marked) {
            obj->marked = 1;
            mark_stack_push(obj);
        } else {
            // 已标记但仍需扫描子对象（marked 可能是其他线程 GC 留下的 stale 值）
            gc_scan_children(obj);
        }
        return;
    }

    if (obj->marked) return;

    obj->marked = 1;
    mark_stack_push(obj);
}

// 标记 Value（如果是对象类型则标记）
void gc_mark_value(Value v) {
    if (val_is_obj(v)) {
        gc_mark_object(val_as_obj(v));
    }
}

// 迭代式排空标记栈：循环弹出对象并扫描子引用，直到栈空
static void gc_drain_mark_stack(void) {
    while (mark_stack_count > 0) {
        Object* obj = mark_stack[--mark_stack_count];
        gc_scan_children(obj);
    }
}

// ============================================================================
// remembered set 扫描（Minor GC 分代标记的补充根）与条目维护
// ============================================================================
//
// 每轮 GC 的标记阶段调用（mark_roots 之后）：
//   1. 扫描每个老年代条目的子引用，标记其引用的年轻代对象。
//      这弥补了 gc_mark_object 在 Minor 模式下跳过老年代对象导致的标记遗漏。
//   2. 剪枝：子引用中已无年轻代对象的条目移出集合。
//      后续若重新获得年轻代引用，写屏障会将其加回，不变式保持成立。
//
// 注意：条目本身不作为存活对象标记——不可达的老年代条目由
// Major GC 的 sweep_old 回收（remembered_set_remove 同步移除）。
// FULL 模式下跳过未标记的条目（不可达垃圾），避免其子引用被错误保活，
// 这正是旧版方案"remembered set 标记垃圾导致 sweep_old freed=0"的问题根源。
static void scan_remembered_set(void) {
    int j = 0;
    in_remembered_scan = 1;
    for (int i = 0; i < gc.remembered_count; i++) {
        Object* obj = gc.remembered_set[i];
        if (!obj || !is_valid_obj_type(obj->type) || obj->generation != GEN_OLD) {
            continue;  // 无效条目：剪枝
        }
        if (gc.mode == GC_MODE_FULL && !obj->marked) {
            continue;  // 不可达老年代条目：将由 sweep_old 回收并移除，剪枝
        }
        young_mark_counter = 0;
        gc_scan_children(obj);
        gc_drain_mark_stack();
        if (young_mark_counter > 0) {
            gc.remembered_set[j++] = obj;  // 仍持有年轻代子引用，保留
        }
        // 无年轻代子引用：剪枝（写入路径的写屏障可随时将其加回）
    }
    in_remembered_scan = 0;
    gc.remembered_count = j;
}

// ============================================================================
// 根集合标记（Root Set Marking）
// ============================================================================

// 标记所有根集合：栈、帧、全局变量、upvalue 链、异常、协程、线程等
static void mark_roots(void) {
    if (!gc.vm) return;

    // 1. 标记操作数栈上的所有值
    int sp = gc.vm->sp;
    if (sp < 0) sp = 0;
    if (sp > gc.vm->stack_capacity) sp = gc.vm->stack_capacity;
    for (int i = 0; i < sp; i++) {
        gc_mark_value(gc.vm->stack[i]);
    }
    
    // 1.5 对所有活跃帧的栈帧区域做保守扫描
    // 确保帧的 stack_base ~ stack_base+slot_count 范围内的所有值也被标记。
    // 不截断到 sp：因为当函数调用返回或参数出栈后，sp 可能回退到帧范围内的局部变量之下，
    // 此时 sp 之上的局部变量在步骤 1 中不会被扫描到。
    // val_is_obj 通过 NaN-boxing 标签过滤，对 int/float/bool 无副作用。
    // 注意：当 frame->locals != NULL 时（函数调用帧），局部变量在独立的 locals 数组中，
    // 步骤 2 已扫描。栈上的 slot_count 可能因函数内联扩展而远大于实际栈使用量，
    // 导致扫描到未初始化的栈内存中的垃圾值（可能被 NaN-boxing 误判为对象指针）。
    // 因此仅对 frame->locals == NULL 的帧（模块初始化帧）做保守栈扫描。
    for (int fi = 0; fi < gc.vm->frame_cnt; fi++) {
        CallFrame* f = &gc.vm->frames[fi];
        if (f->locals != NULL) continue;  // 函数帧：locals 已在步骤 2 扫描
        int start = f->stack_base;
        int end = f->stack_base + f->slot_count;
        if (start < 0) start = 0;
        if (end > gc.vm->stack_capacity) end = gc.vm->stack_capacity;
        for (int i = start; i < end; i++) {
            Value v = gc.vm->stack[i];
            if (val_is_obj(v)) gc_mark_object(val_as_obj(v));
        }
    }

    // 2. 标记所有调用帧（闭包、常量池、局部变量、try 返回值、模块）
    for (int i = 0; i < gc.vm->frame_cnt; i++) {
        CallFrame* frame = &gc.vm->frames[i];
        if (frame->closure) {
            gc_mark_object((Object*)frame->closure);
        }
        if (frame->chunk) {
            for (int j = 0; j < frame->chunk->const_cnt; j++) {
                gc_mark_value(frame->chunk->constants[j]);
            }
        }
        if (frame->locals) {
            for (int j = 0; j < frame->local_count; j++) {
                gc_mark_value(frame->locals[j]);
            }
        }
        // FRAME_TRY_VALID：无 try 函数的帧 has_try_return 为脏值（帧复用优化），
        // 误扫脏 try_return_value 会导致 GC 崩溃
        if (FRAME_TRY_VALID(frame) && frame->has_try_return) {
            gc_mark_value(frame->try_return_value);
        }
        if (frame->module) {
            gc_mark_object((Object*)frame->module);
        }
    }

    // 3. 标记全局变量
    for (int i = 0; i < gc.vm->global_count; i++) {
        gc_mark_value(gc.vm->globals[i]);
    }

    // 3.5 标记当前模块帧及其全局变量
    ModuleFrame* module_frame = gc.vm->current_module_frame;
    while (module_frame) {
        if (module_frame->module) {
            gc_mark_object((Object*)module_frame->module);
        }
        if (module_frame->globals) {
            for (int i = 0; i < module_frame->global_count; i++) {
                gc_mark_value(module_frame->globals[i]);
            }
        }
        module_frame = module_frame->parent;
    }

    // 4. 标记全局函数
    for (int i = 0; i < gc.vm->global_func_count; i++) {
        gc_mark_value(gc.vm->global_funcs[i]);
    }

    // 5. 标记 open upvalue 链表（闭包捕获的栈上变量）
    Upvalue* upvalue = gc.vm->open_upvalues;
    while (upvalue) {
        gc_mark_object((Object*)upvalue);
        upvalue = upvalue->next;
    }

    // 6. 标记当前异常值（防止 GC 在异常处理期间回收异常对象）
    gc_mark_value(gc.vm->exception);

    // 7. 标记最后一次返回值
    gc_mark_value(gc.vm->last_return_value);

    // 8. 标记原生函数方法表
    native_mark_all_functions();

    // 9. 标记内置类型方法表
    extern void array_mark_methods(void);
    extern void string_mark_methods(void);
    extern void dict_mark_methods(void);
    extern void file_mark_methods(void);
    extern void socket_mark_methods(void);
    extern void number_mark_methods(void);
    extern void cstruct_mark_methods(void);
    extern void struct_mark_methods(void);
    array_mark_methods();
    string_mark_methods();
    dict_mark_methods();
    file_mark_methods();
    socket_mark_methods();
    number_mark_methods();
    thread_mark_methods();
    channel_mark_methods();
    cstruct_mark_methods();
    struct_mark_methods();

    // 10. 标记内化字符串表（仅 Minor GC 使用，Major GC 改用 intern_sweep_unmarked）

    // 11. 标记全局结构体定义表
    extern void struct_def_mark_all(void);
    struct_def_mark_all();

    // 12. 标记全局 face 定义表
    extern void face_def_mark_all(void);
    face_def_mark_all();

    // 13. 标记全局 C 结构体定义表
    extern void cstruct_def_mark_all(void);
    cstruct_def_mark_all();

    // 13.5 标记已加载模块缓存
    extern void loaded_modules_mark_all(void);
    loaded_modules_mark_all();

    // 14. 标记当前协程
    if (gc.vm->current_coroutine) {
        gc_mark_object((Object*)gc.vm->current_coroutine);
    }

    // 15. 标记所有协程链表（包括挂起等待的协程）
    ObjCoroutine* co = gc.vm->all_coroutines;
    while (co) {
        gc_mark_object((Object*)co);
        co = co->next;
    }

    // 15. 标记活动线程列表
    if (gc.vm->active_threads) {
        for (int i = 0; i < gc.vm->active_thread_count; i++) {
            if (gc.vm->active_threads[i]) {
                gc_mark_object((Object*)gc.vm->active_threads[i]);
            }
        }
    }

    // 16. 标记事件循环中的就绪队列、定时器协程和失败协程的错误值
    if (gc.vm->event_loop) {
        for (int i = 0; i < gc.vm->event_loop->ready_count; i++) {
            gc_mark_object((Object*)gc.vm->event_loop->ready_queue[i]);
        }
        for (int i = 0; i < gc.vm->event_loop->timer_count; i++) {
            gc_mark_object((Object*)gc.vm->event_loop->timers[i].coroutine);
        }
        // 失败协程的错误值：事件循环运行期间 GC 触发时，
        // errors[] 中的值尚未转移到 vm->exception，必须作为根标记，
        // 否则会被误回收导致 asyncs.run() 结束后读取悬垂指针
        for (int i = 0; i < gc.vm->event_loop->error_count; i++) {
            gc_mark_value(gc.vm->event_loop->errors[i]);
        }
    }

    // 17. 标记额外根集合（gc_push_root 注册的 Value 指针）
    for (int i = 0; i < gc.extra_root_count; i++) {
        gc_mark_value(*gc.extra_roots[i]);
    }

    // 19. 标记字典 tombstone 哨兵字符串已不再需要（已改用 Value 哨兵值）
    // DICT_TOMBSTONE_VAL 是 NaN-boxed 值，不涉及 GC 管理的对象
}

// ============================================================================
// 对象大小计算
// ============================================================================

// 计算对象的总内存占用（包含动态分配的子资源）
// 注：当前未使用，保留供调试和内存分析使用
__attribute__((unused))
static size_t get_object_size(Object* obj) {
    if (!obj) return sizeof(Object);
    switch (obj->type) {
        case OBJ_STRING: return sizeof(ObjString);
        case OBJ_ARRAY: {
            ObjArray* arr = (ObjArray*)obj;
            return sizeof(ObjArray) + (arr->capacity * sizeof(Value));
        }
        case OBJ_DICT: {
            ObjDict* dict = (ObjDict*)obj;
            return sizeof(ObjDict)
                 + dict->asize * sizeof(Value)
                 + dict->capacity * sizeof(ObjDictEntry)
                 + dict->order_capacity * sizeof(ObjString*);
        }
        case OBJ_CLOSURE: return sizeof(ObjClosure);
        case OBJ_FUNCTION: return sizeof(ObjFunction);
        case OBJ_NATIVE: return sizeof(ObjNative);
        case OBJ_SOCKET: return sizeof(ObjSocket);
        case OBJ_BIGINT: {
            ObjBigInt* bigint = (ObjBigInt*)obj;
            return sizeof(ObjBigInt) + bigint->limb_count * sizeof(uint32_t);
        }
        case OBJ_MODULE: return sizeof(ObjModule);
        case OBJ_BOUND_METHOD: return sizeof(ObjBoundMethod);
        case OBJ_FILE: return sizeof(ObjFile);
        case OBJ_RANGE: return sizeof(ObjRange);
        case OBJ_UPVALUE: return sizeof(Upvalue);
        case OBJ_STRUCT_DEF: {
            ObjStructDef* def = (ObjStructDef*)obj;
            size_t size = sizeof(ObjStructDef);
            if (def->name) size += safe_strlen(def->name) + 1;
            if (def->fields) {
                size += def->field_count * sizeof(StructFieldInfo);
                for (int i = 0; i < def->field_count; i++) {
                    if (def->fields[i].name) size += safe_strlen(def->fields[i].name) + 1;
                    if (def->fields[i].struct_type_name) size += safe_strlen(def->fields[i].struct_type_name) + 1;
                }
            }
            if (def->methods) {
                size += def->method_count * sizeof(StructMethodInfo);
                for (int i = 0; i < def->method_count; i++) {
                    if (def->methods[i].name) size += safe_strlen(def->methods[i].name) + 1;
                }
            }
            if (def->type_param_names) {
                size += def->type_param_count * sizeof(char*);
                for (int i = 0; i < def->type_param_count; i++) {
                    if (def->type_param_names[i]) size += safe_strlen(def->type_param_names[i]) + 1;
                }
            }
            // 关联常量大小
            if (def->const_names) {
                size += def->const_count * sizeof(char*);
                for (int i = 0; i < def->const_count; i++) {
                    if (def->const_names[i]) size += safe_strlen(def->const_names[i]) + 1;
                }
            }
            if (def->const_values) {
                size += def->const_count * sizeof(Value);
            }
            return size;
        }
        case OBJ_FACE_DEF: {
            ObjFaceDef* def = (ObjFaceDef*)obj;
            size_t size = sizeof(ObjFaceDef);
            if (def->name) size += safe_strlen(def->name) + 1;
            if (def->methods) {
                size += def->method_count * sizeof(FaceMethodInfo);
                for (int i = 0; i < def->method_count; i++) {
                    if (def->methods[i].name) size += safe_strlen(def->methods[i].name) + 1;
                }
            }
            return size;
        }
        case OBJ_STRUCT: {
            ObjStruct* struct_obj = (ObjStruct*)obj;
            size_t size = sizeof(ObjStruct);
            if (struct_obj->def) {
                size += struct_obj->def->field_count * sizeof(Value);
            }
            if (struct_obj->generic_type_args) {
                size += struct_obj->generic_type_arg_count * sizeof(char*);
                for (int i = 0; i < struct_obj->generic_type_arg_count; i++) {
                    if (struct_obj->generic_type_args[i]) size += safe_strlen(struct_obj->generic_type_args[i]) + 1;
                }
            }
            return size;
        }
        case OBJ_COROUTINE: {
            ObjCoroutine* co = (ObjCoroutine*)obj;
            size_t size = sizeof(ObjCoroutine);
            if (co->saved_frames && co->saved_frame_count > 0) {
                size += co->saved_frame_count * sizeof(CallFrame);
                for (int i = 0; i < co->saved_frame_count; i++) {
                    if (co->saved_frames[i].locals_is_dynamic && co->saved_frames[i].locals) {
                        size += co->saved_frames[i].local_count * sizeof(Value);
                    }
                }
            }
            if (co->initial_args) {
                size += co->initial_arg_count * sizeof(Value);
            }
            return size;
        }
        case OBJ_FUTURE: {
            return sizeof(ObjFuture);
        }
        case OBJ_THREAD: {
            return sizeof(ObjThread);
        }
        case OBJ_CHANNEL: {
            ObjChannel* channel = (ObjChannel*)obj;
            return sizeof(ObjChannel) + channel->capacity * sizeof(Value);
        }
        case OBJ_CSTRUCT_DEF: {
            ObjCStructDef* def = (ObjCStructDef*)obj;
            size_t s = sizeof(ObjCStructDef);
            if (def->name) s += safe_strlen(def->name) + 1;
            if (def->fields) {
                s += def->field_count * sizeof(CStructFieldInfo);
                for (int i = 0; i < def->field_count; i++) {
                    if (def->fields[i].name) s += safe_strlen(def->fields[i].name) + 1;
                    if (def->fields[i].struct_name) s += safe_strlen(def->fields[i].struct_name) + 1;
                }
            }
            if (def->field_hash_table) {
                s += def->field_hash_capacity * sizeof(CStructFieldHashEntry*);
            }
            return s;
        }
        case OBJ_CSTRUCT: {
            ObjCStruct* cs = (ObjCStruct*)obj;
            size_t s = sizeof(ObjCStruct);
            if (cs->owns_memory && cs->data && cs->def) {
                s += cs->def->total_size;
            }
            return s;
        }
        case OBJ_CSTRUCT_ARRAY: {
            ObjCStructArray* csa = (ObjCStructArray*)obj;
            return sizeof(ObjCStructArray) + (size_t)csa->count * csa->element_size;
        }
        case OBJ_CSTRUCT_ARRAY_VIEW: return sizeof(ObjCStructArrayView);
        case OBJ_ENUM_DEF: {
            ObjEnumDef* edef = (ObjEnumDef*)obj;
            size_t s = sizeof(ObjEnumDef);
            if (edef->name) s += safe_strlen(edef->name) + 1;
            if (edef->members) {
                s += edef->member_count * sizeof(EnumMemberInfo);
                for (int i = 0; i < edef->member_count; i++) {
                    if (edef->members[i].name) s += safe_strlen(edef->members[i].name) + 1;
                }
            }
            return s;
        }
        case OBJ_FFI_LIBRARY: return sizeof(Object) + 256;
        case OBJ_FFI_POINTER: {
            extern size_t ffi_pointer_get_data_size(Object* obj);
            return sizeof(Object) + 64 + ffi_pointer_get_data_size(obj);
        }
        case OBJ_FFI_CALLBACK: return sizeof(ObjFFICallback);
        default: return sizeof(Object);
    }
}

// ============================================================================
// 对象资源释放
// ============================================================================

// 释放对象持有的动态资源（不释放对象本身）
static void free_object_resources(Object* obj) {
    if (!obj) return;

    switch (obj->type) {
        // 字符串：释放字符数组，并从字符串表中移除
        case OBJ_STRING: {
            ObjString* str = (ObjString*)obj;
            intern_remove(str);
            free(str->chars);
            break;
        }
        // 函数：释放名称和字节码块
        case OBJ_FUNCTION: {
            ObjFunction* func = (ObjFunction*)obj;
            free(func->name);
            free(func->param_types);
            if (func->param_generic_names) {
                for (int i = 0; i < func->arity; i++) {
                    free(func->param_generic_names[i]);
                }
                free(func->param_generic_names);
            }
            if (func->type_param_names) {
                for (int i = 0; i < func->type_param_count; i++) {
                    free(func->type_param_names[i]);
                }
                free(func->type_param_names);
            }
            if (func->type_param_constraints) {
                for (int i = 0; i < func->type_param_count; i++) {
                    free(func->type_param_constraints[i]);
                }
                free(func->type_param_constraints);
            }
            if (func->chunk) {
                chunk_free(func->chunk);
                free(func->chunk);
            }
            break;
        }
        // 闭包：释放类型参数
        case OBJ_CLOSURE: {
            ObjClosure* closure = (ObjClosure*)obj;
            if (closure->type_param_args) {
                for (int i = 0; i < closure->type_param_count; i++) {
                    free(closure->type_param_args[i]);
                }
                free(closure->type_param_args);
            }
            break;
        }
        // 原生函数：释放名称
        case OBJ_NATIVE: {
            ObjNative* native = (ObjNative*)obj;
            free(native->name);
            break;
        }
        case OBJ_ARRAY: {
            ObjArray* arr = (ObjArray*)obj;
            type_free(arr->type_info);
            free(arr->elements);
            break;
        }
        // 字典：释放数组部分、哈希表和插入顺序数组
        case OBJ_DICT: {
            ObjDict* dict = (ObjDict*)obj;
            type_free(dict->type_info);
            free(dict->array);
            free(dict->entries);
            free(dict->order);
            break;
        }
        // 大整数：释放 limbs 数组
        case OBJ_BIGINT: {
            ObjBigInt* bigint = (ObjBigInt*)obj;
            free(bigint->limbs);
            break;
        }
        // 模块：释放名称和全局变量数组
        case OBJ_MODULE: {
            ObjModule* module = (ObjModule*)obj;
            free(module->name);
            free(module->source_path);
            free(module->globals);
            if (module->init_chunk) {
                chunk_free(module->init_chunk);
                free(module->init_chunk);
            }
            if (module->export_mappings) {
                for (int i = 0; i < module->export_mapping_count; i++) {
                    free(module->export_mappings[i].name);
                }
                free(module->export_mappings);
            }
            // 释放 use re-export 类型信息
            if (module->use_reexport_names) {
                for (int i = 0; i < module->use_reexport_count; i++) {
                    free(module->use_reexport_names[i]);
                }
                free(module->use_reexport_names);
            }
            if (module->use_reexport_kinds) {
                free(module->use_reexport_kinds);
            }
            // 释放原生模块引用名
            if (module->native_imports) {
                for (int i = 0; i < module->native_import_count; i++) {
                    free(module->native_imports[i]);
                }
                free(module->native_imports);
            }
            break;
        }
        // 绑定方法：无额外资源
        case OBJ_BOUND_METHOD:
            break;
        // 文件对象：关闭文件句柄
        case OBJ_FILE: {
            ObjFile* file = (ObjFile*)obj;
            if (file->fp && !file->is_closed) {
                fclose(file->fp);
            }
            break;
        }
        // Socket 对象：关闭 socket 描述符
        case OBJ_SOCKET: {
            ObjSocket* sock = (ObjSocket*)obj;
            if (sock->fd != INVALID_SOCKET_FD) {
#ifdef _WIN32
                closesocket(sock->fd);
#else
                close(sock->fd);
#endif
                sock->fd = INVALID_SOCKET_FD;
            }
            break;
        }
        // 范围：无额外资源
        case OBJ_RANGE:
            break;
        // Upvalue：无额外资源
        case OBJ_UPVALUE:
            break;
        // 结构体定义：释放名称、字段信息和方法信息
        case OBJ_STRUCT_DEF: {
            ObjStructDef* def = (ObjStructDef*)obj;
            free(def->name);
            if (def->fields) {
                for (int i = 0; i < def->field_count; i++) {
                    free(def->fields[i].name);
                    free(def->fields[i].struct_type_name);
                }
                free(def->fields);
            }
            if (def->methods) {
                for (int i = 0; i < def->method_count; i++) {
                    free(def->methods[i].name);
                }
                free(def->methods);
            }
            if (def->type_param_names) {
                for (int i = 0; i < def->type_param_count; i++) {
                    free(def->type_param_names[i]);
                }
                free(def->type_param_names);
            }
            if (def->type_param_constraints) {
                for (int i = 0; i < def->type_param_count; i++) {
                    free(def->type_param_constraints[i]);
                }
                free(def->type_param_constraints);
            }
            // 释放关联常量
            if (def->const_names) {
                for (int i = 0; i < def->const_count; i++) {
                    free(def->const_names[i]);
                }
                free(def->const_names);
            }
            if (def->const_values) {
                free(def->const_values);
            }
            break;
        }
        case OBJ_FACE_DEF: {
            ObjFaceDef* def = (ObjFaceDef*)obj;
            free(def->name);
            if (def->methods) {
                for (int i = 0; i < def->method_count; i++) {
                    free(def->methods[i].name);
                }
                free(def->methods);
            }
            if (def->type_param_names) {
                for (int i = 0; i < def->type_param_count; i++) {
                    free(def->type_param_names[i]);
                }
                free(def->type_param_names);
            }
            if (def->type_param_constraints) {
                for (int i = 0; i < def->type_param_count; i++) {
                    free(def->type_param_constraints[i]);
                }
                free(def->type_param_constraints);
            }
            break;
        }
        // 结构体实例：释放字段值数组和泛型类型参数
        case OBJ_STRUCT: {
            ObjStruct* struct_obj = (ObjStruct*)obj;
            // 内联字段数组随对象整体分配，不可单独 free
            if (!struct_obj->fields_inline) {
                free(struct_obj->field_values);
            }
            if (struct_obj->generic_type_args) {
                for (int i = 0; i < struct_obj->generic_type_arg_count; i++) {
                    free(struct_obj->generic_type_args[i]);
                }
                free(struct_obj->generic_type_args);
            }
            break;
        }
        // 协程：释放保存的调用帧副本数组和初始参数
        case OBJ_COROUTINE: {
            ObjCoroutine* co = (ObjCoroutine*)obj;
            if (co->saved_frames) {
                for (int i = 0; i < co->saved_frame_count; i++) {
                    if (co->saved_frames[i].locals_is_dynamic && co->saved_frames[i].locals) {
                        free(co->saved_frames[i].locals);
                    }
                }
                free(co->saved_frames);
            }
            if (co->initial_args) {
                free(co->initial_args);
            }
            break;
        }
        // Future：无额外资源
        case OBJ_FUTURE:
            break;
        // 线程对象：释放错误消息、pending_args 和同步原语
        case OBJ_THREAD: {
            ObjThread* thread = (ObjThread*)obj;
            if (thread->error_msg) {
                free(thread->error_msg);
            }
            if (thread->pending_args) {
                free(thread->pending_args);
                thread->pending_args = NULL;
            }
            platform_mutex_destroy(&thread->mutex);
            platform_cond_destroy(&thread->done_cond);
            break;
        }
        // 通道：释放缓冲区和同步原语
        case OBJ_CHANNEL: {
            ObjChannel* channel = (ObjChannel*)obj;
            if (channel->buffer) {
                free(channel->buffer);
            }
            platform_mutex_destroy(&channel->mutex);
            platform_cond_destroy(&channel->not_empty);
            platform_cond_destroy(&channel->not_full);
            break;
        }
        // C 结构体定义：释放名称、字段信息和哈希表
        case OBJ_CSTRUCT_DEF: {
            ObjCStructDef* def = (ObjCStructDef*)obj;
            free(def->name);
            if (def->fields) {
                for (int i = 0; i < def->field_count; i++) {
                    free(def->fields[i].name);
                    free(def->fields[i].struct_name);
                }
                free(def->fields);
            }
            if (def->field_hash_table) {
                for (int i = 0; i < def->field_hash_capacity; i++) {
                    CStructFieldHashEntry* entry = def->field_hash_table[i];
                    while (entry) {
                        CStructFieldHashEntry* next = entry->next;
                        free(entry->name);
                        free(entry);
                        entry = next;
                    }
                }
                free(def->field_hash_table);
            }
            break;
        }
        // C 结构体实例：释放拥有的内存
        case OBJ_CSTRUCT: {
            ObjCStruct* cs = (ObjCStruct*)obj;
            if (cs->owns_memory && cs->data) {
                free(cs->data);
            }
            break;
        }
        // C 结构体数组：释放数据缓冲区
        case OBJ_CSTRUCT_ARRAY: {
            ObjCStructArray* csa = (ObjCStructArray*)obj;
            free(csa->data);
            break;
        }
        // Enum 定义：释放名称和成员信息
        case OBJ_ENUM_DEF: {
            ObjEnumDef* edef = (ObjEnumDef*)obj;
            free(edef->name);
            if (edef->members) {
                for (int i = 0; i < edef->member_count; i++) {
                    free(edef->members[i].name);
                }
                free(edef->members);
            }
            break;
        }
        // FFI 库：释放路径和 OS 句柄（委托给 ffi.c）
        case OBJ_FFI_LIBRARY: {
            extern void ffi_library_free_resources(Object* obj);
            ffi_library_free_resources(obj);
            break;
        }
        // FFI 指针：释放拥有的内存（委托给 ffi.c）
        case OBJ_FFI_POINTER: {
            extern void ffi_pointer_free_resources(Object* obj);
            ffi_pointer_free_resources(obj);
            break;
        }
        case OBJ_FFI_CALLBACK: {
            extern void ffi_callback_free_resources(Object* obj);
            ffi_callback_free_resources(obj);
            break;
        }
        default:
            break;
    }
}

// ============================================================================
// 清除阶段（Sweep Phase）
// ============================================================================

// 清除年轻代：回收未标记对象，晋升存活次数达标的对象到老年代
static void sweep_young(void) {
    Object** obj = &gc.young_heap;
    int freed_cnt = 0, promote_cnt = 0;
    while (*obj) {
        // 安全检查：验证对象类型是否有效
        if (!is_valid_obj_type((*obj)->type)) {
            Object* invalid = *obj;
            *obj = invalid->next;
            gc.young_allocated -= invalid->size;
            gc_free_object(invalid);   // flags 池化位可信（除 type 外字段未破坏时）
            continue;
        }
        if (!(*obj)->marked) {
            Object* unreached = *obj;
            *obj = unreached->next;
            if (gc.young_allocated >= unreached->size)
                gc.young_allocated -= unreached->size;
            else
                gc.young_allocated = 0;
            freed_cnt++;
            // 尝试回收小数组到 free_list（保留元素缓冲区避免重复 malloc）
            if (unreached->type == OBJ_ARRAY && arr_try_recycle((ObjArray*)unreached)) {
                continue;
            }
            free_object_resources(unreached);
            gc_free_object(unreached);
        } else {
            (*obj)->survived++;
            if ((*obj)->survived >= gc.promote_age) {
                Object* promote = *obj;
                *obj = promote->next;
                promote_cnt++;
                // Major GC 时 sweep_old 紧接着执行，晋升对象必须保持 marked=1
                promote->marked = (gc.mode == GC_MODE_FULL) ? 1 : 0;
                promote->generation = GEN_OLD;
                promote->survived = 0;
                promote->next = gc.old_heap;
                gc.old_heap = promote;
                // ★ 晋升对象加入 remembered set：
                //   晋升时其子引用中可能仍有年轻代对象（本轮存活但未达晋升年龄），
                //   Minor GC 中老年代对象不再被扫描，必须通过 remembered set 标记这些子引用。
                //   若实际无年轻代子引用，下轮 scan_remembered_set 剪枝移出
                remembered_set_add_promoted(promote);
                size_t obj_size = promote->size;
                if (gc.young_allocated >= obj_size)
                    gc.young_allocated -= obj_size;
                else
                    gc.young_allocated = 0;
                gc.old_allocated += obj_size;
            } else {
                (*obj)->marked = 0;
                obj = &(*obj)->next;
            }
        }
    }
}

// 清除老年代：回收未标记的对象
static void sweep_old(void) {
    Object** obj = &gc.old_heap;
    while (*obj) {
        if (!is_valid_obj_type((*obj)->type)) {
            Object* invalid = *obj;
            *obj = invalid->next;
            if (gc.old_allocated >= invalid->size)
                gc.old_allocated -= invalid->size;
            else
                gc.old_allocated = 0;
            remembered_set_remove(invalid);
            gc_free_object(invalid);   // flags 池化位可信（除 type 外字段未破坏时）
            continue;
        }
        if (!(*obj)->marked) {
            Object* unreached = *obj;
            *obj = unreached->next;
            if (gc.old_allocated >= unreached->size)
                gc.old_allocated -= unreached->size;
            else
                gc.old_allocated = 0;
            // 尝试回收小数组到 free_list
            if (unreached->type == OBJ_ARRAY && arr_try_recycle((ObjArray*)unreached)) {
                continue;
            }
            remembered_set_remove(unreached);
            free_object_resources(unreached);
            gc_free_object(unreached);
        } else {
            (*obj)->marked = 0;
            obj = &(*obj)->next;
        }
    }
}

// GC 后维护 remembered set 的职责已由 scan_remembered_set 在标记阶段完成
// （扫描 + 剪枝）；sweep_old 释放老年代对象时通过 remembered_set_remove 同步移除。

// 清除年轻代所有对象的 marked 标志（纯 Minor GC 前调用）
// 老年代对象在纯 Minor GC 中既不标记也不清扫，无需清标志；
// FULL 模式（Major GC / Minor 升级路径）仍使用 clear_all_marks
static void clear_young_marks(void) {
    Object* obj = gc.young_heap;
    while (obj) {
        obj->marked = 0;
        obj = obj->next;
    }
}

// 清除老年代所有对象的 marked 标志（Minor GC 前调用）
// 确保老年代对象在 mark_roots 阶段能被重新扫描，
// 否则老年代对象保持 marked=1 会导致 gc_mark_object 跳过它们，
// 其引用的年轻代对象不会被标记，从而被误回收
static void clear_all_marks(void) {
    Object* obj = gc.young_heap;
    while (obj) {
        obj->marked = 0;
        obj = obj->next;
    }
    obj = gc.old_heap;
    while (obj) {
        obj->marked = 0;
        obj = obj->next;
    }
}

// ============================================================================
// GC 收集入口
// ============================================================================

// 延迟 GC 执行：由事件循环在帧间空闲时调用
// 检查 deferred_gc 标志，若置位则执行 Minor（或 Major）GC 并清除标志。
// 这确保 GC 不会在帧中途（如 draw callback 创建几百个粒子时）触发暂停。
void gc_try_collect_deferred(void) {
    if (!gc.deferred_gc || gc.running || !gc.enabled) return;
    gc.deferred_gc = 0;

    // 使用与原 gc_alloc 相同的判断逻辑：先 Minor，不够再 Major
    if (gc.young_allocated > gc.young_threshold) {
        gc_minor_collect();
        if (gc.young_allocated > gc.young_threshold) {
            if (gc.old_allocated + gc.young_allocated > gc.old_threshold) {
                gc_major_collect();
            }
        }
    }
}

// Minor GC：只收集年轻代，若老年代堆积超阈值则顺带清扫
// 流程：清除年轻代标记 → 标记根集合 → 扫描 remembered set → 清除年轻代 → [清除老年代]
// ★ 分代标记：老年代对象不递归扫描（gc_mark_object 提前返回），
//   其引用的年轻代对象由 remembered set 扫描覆盖，
//   Minor GC 的标记成本与年轻代 + remembered set 规模成正比，与老年代规模无关
void gc_minor_collect(void) {
    if (gc.running || !gc.enabled) return;

    gc.running = 1;
    gc.mode = GC_MODE_MINOR;
    gc.minor_gc_count++;

    // 老年代堆积超过 2MB 时升级为含 old sweep 的轻量完整回收
    int do_old_sweep = (gc.old_allocated > GC_OLD_FLUSH_THRESHOLD);
    if (do_old_sweep) {
        gc.mode = GC_MODE_FULL;  // 确保 sweep_young 中晋升对象保持 marked=1
        clear_all_marks();
    } else {
        clear_young_marks();     // 纯 Minor：老年代不标记不清扫，只清年轻代
    }

    mark_roots();
    gc_drain_mark_stack();      // 先排空根标记：FULL 模式剪枝依赖 marked 完整性
    scan_remembered_set();      // 扫描老年代→年轻代引用 + 条目剪枝
    gc_drain_mark_stack();

    sweep_young();

    if (do_old_sweep) {
        sweep_old();
        gc.mode = GC_MODE_MINOR;

        // Windows：定期归还 C 堆碎片化页面给 OS
        // sweep_old 释放大量对象后，C 运行时的 free() 不会主动归还空闲页面给 OS，
        // 导致进程工作集持续增长。HeapCompact 将连续的空闲页面合并并释放回系统。
        // 每 60 秒最多执行一次，避免频繁调用导致帧率波动。
#ifdef _WIN32
        {
            static THREAD_LOCAL uint32_t last_compact_tick = 0;
            uint32_t now = (uint32_t)(clock() * 1000 / CLOCKS_PER_SEC);
            if (now - last_compact_tick >= 60000) {
                last_compact_tick = now;
                HeapCompact(GetProcessHeap(), 0);
            }
        }
#endif
    }

    gc.running = 0;

    // 动态调整年轻代阈值
    if (gc.young_threshold < gc.young_allocated * 2) {
        gc.young_threshold = gc.young_allocated * 2;
    }
    if (gc.young_threshold < GC_YOUNG_THRESHOLD) {
        gc.young_threshold = GC_YOUNG_THRESHOLD;
    }
}

// Major GC：收集全部（年轻代 + 老年代）
// 流程：清除全部标记 → 标记根集合 → 扫描 remembered set（剪枝）→ 清除年轻代 → 清除老年代
void gc_major_collect(void) {
    if (gc.running || !gc.enabled) return;

    gc.running = 1;
    gc.mode = GC_MODE_FULL;

    clear_all_marks();
    mark_roots();
    gc_drain_mark_stack();      // 先排空根标记：剪枝依赖 marked 完整性
    scan_remembered_set();      // FULL 模式全量标记已完成，此处仅做条目维护/剪枝
    gc_drain_mark_stack();

    // Major GC：在 sweep 之前清理无引用的内化字符串（此时 marked 标志仍有效）
    intern_sweep_unmarked();

    sweep_young();
    sweep_old();

    gc.running = 0;

    // 动态调整老年代阈值（上限 512MB）
    size_t total = gc.young_allocated + gc.old_allocated;
    size_t new_threshold = total * 2;
    if (new_threshold < GC_OLD_THRESHOLD) {
        new_threshold = GC_OLD_THRESHOLD;
    } else if (new_threshold > 1024 * 1024 * 512) {
        new_threshold = 1024 * 1024 * 512;
    }
    gc.old_threshold = new_threshold;

    // 动态调整年轻代阈值
    if (gc.young_threshold < gc.young_allocated * 2) {
        gc.young_threshold = gc.young_allocated * 2;
    }
    if (gc.young_threshold < GC_YOUNG_THRESHOLD) {
        gc.young_threshold = GC_YOUNG_THRESHOLD;
    }

    // Windows：归还 C 堆碎片化页面给 OS
    // Major GC 释放大量对象后，C 运行时的 free() 不会主动归还空闲页面给 OS，
    // 导致进程工作集持续增长。HeapCompact 将连续的空闲页面合并并释放回系统。
#ifdef _WIN32
    HeapCompact(GetProcessHeap(), 0);
#endif
}

// 自动选择 Minor 或 Major GC
void gc_collect(void) {
    if (gc.running || !gc.enabled) return;

    // 手动调用 gc_collect 时执行 Major GC，确保完整回收
    gc_major_collect();
}

// ============================================================================
// 程序退出时释放所有资源
// ============================================================================

// 释放所有 GC 对象和 VM 资源（程序退出时调用）
void gc_free_all(void) {
    // 等待所有活动线程完成
    if (gc.vm && gc.vm->active_threads) {
        for (int i = 0; i < gc.vm->active_thread_count; i++) {
            ObjThread* thread = gc.vm->active_threads[i];
            if (thread) {
                platform_mutex_lock(&thread->mutex);
                while (thread->state == THREAD_RUNNING) {
                    platform_cond_wait(&thread->done_cond, &thread->mutex);
                }
                platform_mutex_unlock(&thread->mutex);
                platform_thread_join(thread->os_thread, NULL);
            }
        }
        free(gc.vm->active_threads);
        gc.vm->active_threads = NULL;
        gc.vm->active_thread_count = 0;
        gc.vm->active_thread_capacity = 0;
    }

    // 释放全局作用域
    if (gc.vm && gc.vm->global_scope) {
        scope_free(gc.vm->global_scope);
        gc.vm->global_scope = NULL;
    }

    // 释放 VM 栈和帧
    if (gc.vm) {
        if (gc.vm->stack) {
            free(gc.vm->stack);
            gc.vm->stack = NULL;
        }
        gc.vm->stack_capacity = 0;
        if (gc.vm->frames) {
            // 释放每个帧中动态分配的 locals
            for (int i = 0; i < gc.vm->frame_cnt; i++) {
                CallFrame* frame = &gc.vm->frames[i];
                if (frame->locals_is_dynamic && frame->locals) {
                    free(frame->locals);
                    frame->locals = NULL;
                }
            }
            free(gc.vm->frames);
            gc.vm->frames = NULL;
        }
        gc.vm->frame_capacity = 0;
        // 释放全局变量和全局函数数组
        if (gc.vm->globals) {
            free(gc.vm->globals);
            gc.vm->globals = NULL;
        }
        gc.vm->global_count = 0;
        gc.vm->global_capacity = 0;
        if (gc.vm->global_funcs) {
            free(gc.vm->global_funcs);
            gc.vm->global_funcs = NULL;
        }
        gc.vm->global_func_count = 0;
        gc.vm->global_func_capacity = 0;
    }

    // 释放年轻代所有对象
    Object* obj = gc.young_heap;
    while (obj) {
        Object* next = obj->next;
        free_object_resources(obj);
        gc_free_object(obj);
        obj = next;
    }

    // 释放老年代所有对象
    obj = gc.old_heap;
    while (obj) {
        Object* next = obj->next;
        free_object_resources(obj);
        gc_free_object(obj);
        obj = next;
    }

    // 释放所有池块（池化对象槽内存随块释放）
    gc_pool_free_all();

    // 释放内化字符串表
    intern_table_free();

    // 释放 remembered set
    free(gc.remembered_set);

    // 释放额外根集合
    free(gc.extra_roots);

    // 重置 GC 状态
    gc.young_heap = NULL;
    gc.old_heap = NULL;
    gc.young_allocated = 0;
    gc.old_allocated = 0;
    gc.young_threshold = GC_YOUNG_THRESHOLD;
    gc.old_threshold = GC_OLD_THRESHOLD;
    gc.running = 0;
    gc.mode = GC_MODE_FULL;
    gc.vm = NULL;
    gc.enabled = 1;
    gc.remembered_set = NULL;
    gc.remembered_count = 0;
    gc.remembered_capacity = 0;
    gc.promote_age = GC_PROMOTE_AGE;
    gc.minor_gc_count = 0;
    gc.extra_roots = NULL;
    gc.extra_root_count = 0;
    gc.extra_root_capacity = 0;
}
