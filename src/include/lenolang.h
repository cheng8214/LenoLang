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

// 全局标志：编译器语义分析阶段检测到 _console(false) 调用时置为 1
// 打包（--pack）时据此选择 leno_vm_gui.exe（无控制台版）
extern int g_use_gui_vm;

// ============================================================================
// 主执行流程
// ============================================================================

int lenolang_run(const char* source);
void lenolang_repl(void);

// ============================================================================
// 模块函数指针修复（反序列化后需要调用）
// ============================================================================

void fix_module_function_ptrs(Chunk* chunk);
void fix_single_module(ObjModule* mod);

#endif // LENOLANG_H
