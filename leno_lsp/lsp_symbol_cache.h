/**
 * LSP 符号缓存
 * 混合缓存策略：
 *   - 模块导出符号 → ModuleSymbolTable（进程内缓存 + .lenosymc 磁盘缓存，按 mtime 失效）
 *   - 当前文件分析 → CompilerContext（进程内缓存，按 content hash 失效）
 *
 * 避免 LSP 补全/hover 每次按键都重新 read_module_file + compiler_analyze。
 */

#ifndef LSP_SYMBOL_CACHE_H
#define LSP_SYMBOL_CACHE_H

#include "leno_compiler_lib.h"
#include "../src/include/module_symbol_table.h"

// ==================== 模块符号表缓存 ====================

// 获取模块符号表（进程内缓存，按文件 mtime 失效）
// module_path: 模块文件路径
// current_file: 当前文件路径（用于解析相对路径，可为 NULL）
// 返回 ModuleSymbolTable*（所有权归缓存，调用者不可 destroy）
// 失败返回 NULL
ModuleSymbolTable* lsp_cache_get_module_symtable(const char* module_path, const char* current_file);

// ==================== 当前文件分析缓存 ====================

// 分析当前文件并缓存（按 content hash 失效）
// content: 文件内容
// file_path: 文件路径
// 返回 CompilerContext*（所有权归缓存，调用者不可 cleanup）
// 注意：返回的 ctx 在下次调用或 invalidate 后可能失效，请立即使用
CompilerContext* lsp_cache_analyze_current(const char* content, const char* file_path);

// ==================== 缓存管理 ====================

// 使指定路径的缓存失效（didChange/didClose 时调用）
void lsp_cache_invalidate_path(const char* path);

// 清理所有缓存（server 销毁时调用）
void lsp_cache_cleanup(void);

#endif // LSP_SYMBOL_CACHE_H
