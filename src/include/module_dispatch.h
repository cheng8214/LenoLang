#ifndef MODULE_DISPATCH_H
#define MODULE_DISPATCH_H

#include "leno_value.h"

// ★ 2026-11-04：128 → 1024。
//   128（后来提到 256）会把合法标识符的导出名**静默截断** ⇒ 导入方报「没有方法」，
//   而报错里还是完整名字（看起来像拼错 ✗）。Leno 的标识符长度本身没有硬限制
//   （`MAX_NAME_LEN` 只是注释），1024 对真实代码足够，且导出清单已改成**按实际数量
//   动态分配**（见 module_loader.c 的 ExportList）⇒ 不会为每个模块白占 4MB ✓
//   本宏是**单一来源**：module_loader.h 直接 include 本头，不再各自定义一份 ✓
#define MAX_EXPORT_NAME_LEN 1024

// 更新模块对象中所有函数/闭包的 module 指针（从 src_module 更新为 dst_module）
void update_module_function_ptrs(ObjModule* src_module, ObjModule* dst_module);

// 设置/获取模块编译器函数指针（用于解耦 module_loader 和编译器）
typedef ObjModule* (*ModuleCompileFunc)(const char* source, const char* module_name,
                                         char export_names[][MAX_EXPORT_NAME_LEN], int export_count);
void set_module_compile_func(ModuleCompileFunc func);
ModuleCompileFunc get_module_compile_func(void);

#endif // MODULE_DISPATCH_H
