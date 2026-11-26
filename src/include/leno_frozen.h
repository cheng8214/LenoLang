#ifndef LENO_FROZEN_H
#define LENO_FROZEN_H

// ============================================================================
// 冻结对象（frozen）—— 大对象跨线程零拷贝共享
// ----------------------------------------------------------------------------
// 设计文档：LenoDocsSrc/大对象零拷贝共享设计.md
//
// 一句话模型：
//   `_freeze(x)` 把对象图一次性深拷贝到**堆外共享区**，打上 OBJ_FLAG_FROZEN；
//   此后跨线程传递只做引用计数（O(1)），不再拷贝；写它一律报错。
//
// 三条不可动摇的规矩（改这里之前先读设计文档 §5）：
//   1. frozen 对象**不进任何线程堆**，GC 的 mark/sweep 永远不碰它 ⇒ 存活只靠 refcnt；
//   2. 指针**必须先登记进当前 VM 的注册表**才允许持有（否则图可能被别的 VM 放掉）；
//   3. 释放必须**迭代**（显式栈）：深链用递归会爆栈（100 万层的 Array 套 Array 不是病态）。
// ============================================================================

#include "leno_value.h"
#include "leno_vm.h"

// 通用深拷贝的三种模式（实现在 src/object/object_thread.c 的 channel_clone_value）
#define LENO_CLONE_CHANNEL 0   // 跨线程传递：frozen 原样共享（S2 起生效）
#define LENO_CLONE_FREEZE  1   // 冻结：闭包/函数/Channel/Atomic 一律拒绝
#define LENO_CLONE_THAW    2   // 解冻：连 frozen 节点也深拷贝（得到可变副本）

// 唯一深拷贝入口（冻结 / 解冻 / 跨线程三条路共用，保证拒绝面一致）
Value leno_clone_value(Value v, int mode);

// ---- 冻结 / 判断 / 解冻 ----------------------------------------------------
// 失败（含不支持的类型、含环、内存不足）时**已经报过错**，返回 null。
Value frozen_freeze(Value v);
int   frozen_is(Value v);              // 非对象 ⇒ 0
Value frozen_thaw(Value v);            // 非 frozen ⇒ 报错并返回 null

// ---- 引用计数 --------------------------------------------------------------
void frozen_ref(Object* o);            // 原子 +1
void frozen_unref(Object* o);          // 原子 -1；归零 ⇒ 迭代释放整图

// ---- 每个 VM 的注册表（堆外根的等价物） ------------------------------------
// acquire：登记（按指针去重）+ ref++；返回 1 表示新增条目、0 表示已登记过。
// 必须在**把指针交给任何 Leno 变量之前**调用。
int  frozen_reg_acquire(VM* vm, Object* o);
void frozen_reg_release_all(VM* vm);
// 池任务：任务开始时 mark，结束时释放**本任务期间新增**的条目。
// 为什么需要：worker VM 长期存活，不按任务收敛的话注册表会涨成"所有历史任务的并集"。
int  frozen_reg_mark(VM* vm);
void frozen_reg_release_since(VM* vm, int mark);

// ---- 统计（`_mem()` / `_gc_stats()` 用；不进 GC 阈值） ---------------------
void frozen_stats_add(long bytes, long objs);
void frozen_get_stats(size_t* out_bytes, size_t* out_objs);

// ---- gc.c 侧：冻结构建期的堆外分配 ----------------------------------------
Object* gc_alloc_frozen_object(size_t size, ObjType type);
int  gc_frozen_build_begin(void);
void gc_frozen_build_end(int prev);
int  gc_in_frozen_build(void);

// ---- natives（注册在 sys 模块：`_freeze` / `_thaw` / `_is_frozen`） --------
Value frozen_native_freeze(int argCount, Value* args);
Value frozen_native_thaw(int argCount, Value* args);
Value frozen_native_is_frozen(int argCount, Value* args);

#endif
