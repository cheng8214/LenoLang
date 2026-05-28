#ifndef LENOLANG_H
#define LENOLANG_H

// 统一入口头文件
// 默认只包含 VM 运行时所需的头文件（不依赖编译器）
// 编译器文件需要自行 include 编译器相关头文件

#include "leno_types.h"
#include "leno_error.h"
#include "leno_value.h"
#include "leno_vm.h"
#include "module_loader.h"

// ============================================================================
// 运行时配置
// ============================================================================

// 运行时类型检查开关（默认开启，发布版本可关闭以提高性能）
// 在 main() 调用 lenolang_run() 之前设置 runtime_type_check = 0 可关闭检查
extern int runtime_type_check;

// ============================================================================
// 主执行流程
// ============================================================================

int lenolang_run(const char* source);
void lenolang_repl(void);

#endif // LENOLANG_H
