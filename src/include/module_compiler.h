#ifndef MODULE_COMPILER_H
#define MODULE_COMPILER_H

#include "leno_value.h"
#include "module_dispatch.h"

// 编译模块源代码为模块对象
// source: 模块源代码
// module_name: 模块名称
// export_names: 导出项名称数组
// export_count: 导出项数量
// 返回: 编译后的模块对象，失败返回 NULL
ObjModule* compile_module_new(const char* source, const char* module_name,
                               char export_names[][128], int export_count);

#endif // MODULE_COMPILER_H
