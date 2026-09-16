#ifndef MODULE_LOADER_H
#define MODULE_LOADER_H

#include "leno_value.h"
#include "leno_vm.h"

#define MAX_EXPORT_NAME_LEN 128

// 加载并编译模块文件
// file_path: 模块文件路径
// current_file: 当前文件路径（用于解析相对路径）
// alias_name: 模块别名（可选）
// 返回: 模块对象，失败返回 NULL
ObjModule* load_module_file(const char* file_path, const char* current_file, const char* alias_name);

ObjModule* find_loaded_module(const char* path);

void add_loaded_module_public(const char* path, ObjModule* module);

// 重置已加载模块列表（用于新程序运行）
void reset_loaded_modules(void);

// 从模块文件中提取导出项（用于语义分析）
// file_path: 模块文件路径
// current_file: 当前文件路径（用于解析相对路径）
// exports: 输出参数，存储导出项名称数组
// max_exports: 最大导出项数量
// 返回: 实际导出项数量，失败返回 -1
int extract_module_exports_from_file(const char* file_path, const char* current_file, 
                                      char exports[][MAX_EXPORT_NAME_LEN], int max_exports);

// 检查模块中是否存在指定的方法
// file_path: 模块文件路径
// current_file: 当前文件路径（用于解析相对路径）
// method_name: 方法名
// 返回: 1 存在，0 不存在，-1 错误
int module_has_method(const char* file_path, const char* current_file, const char* method_name);

// 扫描 chunk 常量池中的 cstruct/face 定义并注册到全局表
void register_defs_from_chunk(Chunk* chunk);

// ============================================================================
// 模块编译缓存控制（跨运行缓存）
// ============================================================================

// 设置模块缓存目录（NULL 表示不启用磁盘缓存）
// 内部会 strdup 保存并确保目录存在
void module_loader_set_cache_dir(const char* dir);

// 启用/禁用模块编译缓存
void module_loader_set_cache_enabled(int enabled);

// 查询缓存是否启用
int module_loader_is_cache_enabled(void);

// 获取缓存目录路径（返回内部指针，不需要调用者 free；NULL 表示未配置）
const char* module_loader_get_cache_dir(void);

// 获取已加载模块数量（供序列化遍历依赖）
int loaded_modules_get_count(void);

// 获取指定索引的已加载模块
ObjModule* loaded_modules_get(int index);

#endif // MODULE_LOADER_H
