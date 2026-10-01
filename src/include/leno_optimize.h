#ifndef LENO_OPTIMIZE_H
#define LENO_OPTIMIZE_H

#include "leno_ast.h"

void optimize_constant_fold(Ast* ast);
void optimize_dead_code_elimination(Ast* ast);

/* 编译期条件剪枝（2026-10-01 新增；详见 optimize.c 顶部的大段注释）：
 *   语义：`if` 的条件**只含 `target.*` 常量**且静态可判 ⇒ 死分支被剪掉，
 *         **不参与语义分析 / 不进字节码**（其余条件一律不动 ⇒ 现有语义零变化 ✓）
 *   用法：语义分析**之前**调用 `optimize_target_prune(root)`；
 *        目标由 `optimize_set_target(spec)` 指定（不设 ⇒ 用宿主编译目标 ✓） */

/* 设置编译目标。spec 形如 "windows" / "linux-arm64" / "macos" / "linux"；
 *   · NULL 或 "" ⇒ 宿主目标（与 _os()/_arch() 同口径 ✓）
 *   · 只给 os（如 "linux"）⇒ 架构沿用宿主
 * 返回 0 成功；-1 = 不认识的 os/arch（调用方应报错退出 ✓） */
int  optimize_set_target(const char* spec);
void optimize_target_prune(Ast* ast);

#endif
