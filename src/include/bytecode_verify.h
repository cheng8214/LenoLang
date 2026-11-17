#ifndef LENO_BYTECODE_VERIFY_H
#define LENO_BYTECODE_VERIFY_H

#include "leno_vm.h"

// ============================================================================
// .lenb 字节码校验器
// ============================================================================
// 目的：`.lenb` 是**不可信输入**（可能损坏、可能被手改）。反序列化之后的
// chunk 在交给 VM 执行前必须先过一遍自洽性检查，否则以下任一都会被 VM 当成
// 合法指令直接寻址 ⇒ 越界读 / 野指针 / 进程崩溃：
//   · 指令边界（尾部变长操作数被截断、opcode 越界）
//   · 跳转目标（跳进尾随数据区、跳到 chunk 之外）
//   · 常量索引（OP_LOADK / OP_CLOSURE / 结构体元数据里的名字常量…）
//   · 寄存器 / 槽位索引（超出该帧 locals 的物理容量）
//   · OP_CLOSURE 的 upvalue 捕获描述（upvalue 个数、is_local 的槽位号）
//
// 返回值：1 = 通过；0 = 损坏（*bad_offset 记录首个可疑字节位置，可为 NULL）。
//
// `local_count` 说明（**不能直接用 chunk->local_count**）：
//   · 主程序 / 模块 init chunk：其 chunk->local_count 由 codegen 写回 = 寄存器高水位；
//   · 函数 chunk：序列化时写的是 chunk->local_count，而 codegen 只把高水位写到
//     **func->local_count**（函数 chunk 自身的 local_count 恒为 0）⇒ 调用方必须传
//     func->local_count，否则会把合法的大函数（寄存器号 64..255）误判成损坏。
// ============================================================================
int bytecode_verify_chunk(const Chunk* chunk, int local_count, int* bad_offset);

// ★ 2026-11-17：带原因版本（诊断用）。失败时 reason 里写一句人话，例如
//   `偏移 1293 处的指令或操作数非法（opcode=…）`
//   `类型规格非法：偏移 1289 起的规格，第 0 字节 —— 类型 kind 非法（超出 TypeKind 范围）`
// 旧版只有"结构损坏（错误码 6）"，损坏的 .lenb 根本没法定位（实测手工改一个 kind 字节，
// 光看报文只能猜）。成功时 reason 置空字符串。
int bytecode_verify_chunk_ex(const Chunk* chunk, int local_count, int* bad_offset,
                             char* reason, int reason_cap);

#endif  // LENO_BYTECODE_VERIFY_H
