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
//   ⇒ 放错位置会让带编译器的 `leno.exe` 链接失败。
const char* vm_res_dir(void);
// 由 VM 运行时（vm_main.c）在解包完成后写入；NULL / 空串 = 本次未解包。
void vm_set_res_dir(const char* dir);

// ============================================================================
// 嵌入宿主（Android / iOS / 桌面壳）注入点
// ----------------------------------------------------------------------------
// 为什么需要：`_script()` 原来只从 argv 里取"第一个非选项参数"，`_tmpdir()` 只认
//   TMPDIR/+"/tmp"。这两个前提在嵌入场景下都不成立 ——
//   Android app 进程**没有 argv**（JNI 直接调 `lenolang_run_lenb(path)`），
//   也没有可写的 `/tmp`（沙箱里只有 filesDir 可写）。
//   实测（2026-11-18，Android 10 模拟器）：`_script()` 返回 null、
//   `_tmpdir()` 返回 "/tmp" ⇒ 一切落盘操作全部失败，而且失败点离真实原因很远。
// 约定：宿主在跑脚本**之前**调用；不调用则完全退回原行为（桌面不受影响）。
// ============================================================================
// 本次运行的脚本绝对路径（供 `_script()` / `dirs.script_dir()` 用）
void leno_set_script_path(const char* path);
// 宿主提供的可写数据目录（Android 传 getFilesDir()）；`_tmpdir()` 在 TMPDIR 缺失时用它
void leno_set_data_dir(const char* dir);
// 读回当前注入的数据目录；NULL = 未注入
const char* leno_data_dir(void);

#endif // LENO_VM_RUNTIME_H
