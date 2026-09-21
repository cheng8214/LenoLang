// ============================================================================
// 寄存器式 VM 执行核心逻辑 —— **聚合文件**（只负责按顺序拼装）
// ----------------------------------------------------------------------------
// 它被 vm.c 的 vm_run() 与 vm_run_with_vm() 各 include 一次
// （后者用 `#define vm (*vm_ptr)` 把 vm 换成 (*vm_ptr)），因此三条约束必须守住：
//   ① **顺序即语义**：拼装顺序不能改（两份 include 共用这一处顺序）；
//   ② 分块文件里**不得出现文件级 static**：同一 TU 里 include 两次会重复定义
//      （块作用域的 static / static inline 函数没问题）；
//   ③ 每块本身是"函数体内的一段代码"，不是独立函数 ——
//      解释器用 LABEL(OP_x) + 计算跳转，拆出函数体就不在同一次分发里了。
//
// 切分只按 **opcode 主题**，且切点一律落在 `LABEL(...)` 之前，绝不切开 handler 体。
//
// 历史：2026-09 之前这里是 4249 行单文件。拆分是**纯文本搬运**（不重排、不重格式化），
// 按下面的顺序拼回去与原文件**逐字节一致**（拆分当时做过校验）。
// ⚠ 旧提交 / 文档里写的"vm_run.inc 第 N 行"指**拆分前**的行号。
// ============================================================================
#include "run/01_prologue.inc"
#include "run/02_loads.inc"
#include "run/03_arith.inc"
#include "run/04_compare_bit_cast.inc"
#include "run/05_control.inc"
#include "run/06_call.inc"
#include "run/07_collections.inc"
#include "run/08_slice_iter.inc"
#include "run/09_exception.inc"
#include "run/10_vars_module.inc"
#include "run/11_struct_face_enum.inc"
#include "run/12_fields_object.inc"
#include "run/13_async_ffi.inc"
#include "run/14_switch_for.inc"
