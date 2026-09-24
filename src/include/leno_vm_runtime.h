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

// 单文件打包（-p --onefile）本次运行实际使用的**资源目录**（含结尾分隔符）。
//   · 打包 exe：内嵌资源段释放到的缓存目录（按 payload_hash 分目录）；
//   · 未打包：空串。
// 用途：`dirs.res_dir()` 在打包模式下返回它（**只读**随包资源）；
//   写用户数据仍用 `dirs.script_dir()`（exe 目录）—— 资源目录按 hash 分目录，
//   每次重新打包就会换目录，往里写数据等于"升级即丢"。
// ⚠ 存储定义在 `src/vm/vm.c`（**core**，两个构建都链），**不是** vm_main.c：
//   后者只属于 VM-only 构建（sources_vm.txt），而读它的 `dirs.c` 在 core
//   ⇒ 放错位置会让带编译器的 `lenoreg.exe` 链接失败。
const char* vm_res_dir(void);
// 由 VM 运行时（vm_main.c）在解包完成后写入；NULL / 空串 = 本次未解包。
void vm_set_res_dir(const char* dir);

#endif // LENO_VM_RUNTIME_H
