#ifndef LENO_DCE_H
#define LENO_DCE_H

#include "leno_value.h"

// ============================================================================
// 入口产物（.lenb）的**方法级死代码消除** —— 编译期引用图 + 可达性
// ----------------------------------------------------------------------------
// 目标：模块里绝大多数函数/方法永远不会被入口程序调到（SDL3 的 44 个模块，一个游戏
//   只用得到几十个），但它们的**函数体**都躺在 init_chunk 的常量池里、随入口 .lenb
//   一起发出去。2026-09-24 探针实测可剪 223728 B / 238175 B（hello_window / pvz，
//   占现产物 25.9% / 25.0%）。
//
// 做法（判定只有一处：序列化**入口 .lenb** 时，不可达的函数只写 1 字节标签）：
//   · 编译期收集引用图：def 点（槽位 ↔ 函数）+ use 点（槽位 / 名字 / (类型,方法) / 类型）
//   · 全部 codegen 结束后做一次可达性：根 = 各编译单元的**模块级/顶层代码**
//     （模块 init_chunk 与入口顶层代码一定会执行），再走不动点收敛
//     （名字/类型是间接引用 —— 方法由 (类型,名) 精确命中，或「名字通配 ∩ 类型被引用」命中）
//   · serialize.c 的 OBJ_FUNCTION 分支查询 dce_func_is_live()：不可达 ⇒ CONST_TAG_DEAD_FUNCTION
//
// ⚠ 三条 fail-safe（宁可少剪，不可剪错 —— 剪错的表现是**静默返回 null**）：
//   ① 没登记进引用图的函数一律当"活"（正常路径下所有函数都会登记，这条只兜底）
//   ② 只要本轮编译有**任何模块来自 .lenomc 缓存**（该模块压根没走 codegen ⇒ 图缺一半），
//      整张图判为不完整 ⇒ 一律不剪
//   ③ 构造/析构函数由 VM 按 ctor_index/dtor_index **隐式**调用（静态没有引用点）⇒ 无条件全留。
//      实测不花体积（这两个产物里它们本就被别的规则覆盖），删掉这条却会静默剪掉构造器。
//
// 入口编译（-c / -p）会**不读模块缓存**（见 main.c 的 lenolang_compile）以保证图完整。
//   这是有代价的：读模块缓存本来快得多（hello_window `-c`：读缓存 168 ms vs 全量重编译
//   531 ms）⇒ 每次分发编译多付约 360 ms；换来产物体积 −50%，值。
//   另：产物大小还会随缓存状态漂移（863795 vs 862048 B），DCE 会把这个差放大到 25%
//   ⇒ 分发产物必须只由源码决定，不能用"图不完整就少剪一点"折中。
//
// 开关：LENO_NO_DCE=1 关闭（裁剪只影响**写出**，不影响 .leno 直跑）；
//       LENO_DCE_VERBOSE=1 打印统计；LENO_DCE_WILD_SAFE=1 名字通配不再按"类型被引用"过滤。
// ============================================================================

// 是否启用（默认启用；LENO_NO_DCE=1 关闭）
int dce_enabled(void);

// 一次入口编译开始前清空引用图
void dce_reset(void);

// 编译单元（模块）栈：与 codegen_set_module 同步。入口程序 = 单元 0（reset 时建好）
void dce_enter_unit(ObjModule* module);
void dce_exit_unit(void);

// 「紧随其后生成的函数属于该 struct」——gen_struct_def 在方法循环前后设置/清空
void dce_set_method_owner(const char* struct_name);

// 当前正在生成的函数（gen_func_closure 进出时调用）；对应引用记在它名下
void dce_enter_func(ObjFunction* func, int is_ctor, int is_dtor);
void dce_exit_func(void);

// 记录一条引用
void dce_note_func_def(int slot, ObjFunction* func);                     // 槽位 ← 函数
void dce_note_slot_ref(int slot);                                        // 读该槽位
void dce_note_name_ref(const char* name);                                // 按名字（只命中顶层函数）
void dce_note_method_ref(const char* type_name, const char* method_name); // type=NULL ⇒ 名字通配
void dce_note_func_value(ObjFunction* func);                             // 匿名闭包 / 局部函数的创建点
void dce_note_type_ref(const char* type_name);                           // 类型被引用

// 有模块来自 .lenomc 缓存 ⇒ 引用图不完整 ⇒ 一律不剪（module_loader 命中缓存时调用）
void dce_note_module_cached(void);

// 序列化侧
void dce_set_active(int on);   // 只在**入口 .lenb**写出期间置位（其余产物一律写完整函数体）
int dce_active(void);
int dce_func_is_live(ObjFunction* func);   // 未登记 / 图不完整 ⇒ 返回 1（保活）
void dce_report(void);                     // LENO_DCE_VERBOSE=1 时打印统计

#endif // LENO_DCE_H