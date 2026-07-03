#ifndef MODULE_DISPATCH_H
#define MODULE_DISPATCH_H

#include "leno_value.h"

// 更新模块对象中所有函数/闭包的 module 指针（从 src_module 更新为 dst_module）
void update_module_function_ptrs(ObjModule* src_module, ObjModule* dst_module);

// 设置/获取模块编译器函数指针（用于解耦 module_loader 和编译器）
typedef ObjModule* (*ModuleCompileFunc)(const char* source, const char* module_name,
                                         char export_names[][128], int export_count);
void set_module_compile_func(ModuleCompileFunc func);
ModuleCompileFunc get_module_compile_func(void);

#endif // MODULE_DISPATCH_H
