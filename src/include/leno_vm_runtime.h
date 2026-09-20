#ifndef LENO_VM_RUNTIME_H
#define LENO_VM_RUNTIME_H

// VM 运行时最小头文件集合
// 仅包含 VM 执行字节码所需的类型定义，不依赖编译器（parser/semantic/codegen）
// 用于 VM 独立构建（运行 .lenb 二进制文件）

#include "leno_types.h"
#include "leno_error.h"
#include "leno_value.h"
#include "leno_vm.h"
#include "module_loader.h"
#include "module_dispatch.h"

// VM 运行时导出的函数
int lenolang_run_chunk_only(Chunk* chunk);
int lenolang_run_lenb(const char* filename);

#endif // LENO_VM_RUNTIME_H
