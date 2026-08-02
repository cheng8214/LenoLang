/**
 * LenoC 编译器集成库
 * 为 LSP 提供符号分析和类型信息
 */

#include "leno_compiler_lib.h"
#include "../src/include/lenolang.h"
#include "../src/include/native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#endif

// LSP 分析时临时抑制 stderr（防止 "错误收集器已满" 洪水输出导致 CPU 飙升）
static int lsp_saved_stderr_fd = -1;

static void lsp_suppress_stderr(void) {
    fflush(stderr);
#ifdef _WIN32
    lsp_saved_stderr_fd = _dup(_fileno(stderr));
    FILE* nul = fopen("NUL", "w");
    if (nul) { _dup2(_fileno(nul), _fileno(stderr)); fclose(nul); }
#else
    lsp_saved_stderr_fd = dup(fileno(stderr));
    FILE* nul = fopen("/dev/null", "w");
    if (nul) { dup2(fileno(nul), fileno(stderr)); fclose(nul); }
#endif
}

static void lsp_restore_stderr(void) {
    if (lsp_saved_stderr_fd < 0) return;
    fflush(stderr);
#ifdef _WIN32
    _dup2(lsp_saved_stderr_fd, _fileno(stderr));
    _close(lsp_saved_stderr_fd);
#else
    dup2(lsp_saved_stderr_fd, fileno(stderr));
    close(lsp_saved_stderr_fd);
#endif
    lsp_saved_stderr_fd = -1;
}

// 初始化编译器上下文
bool compiler_context_init(CompilerContext* ctx) {
    if (!ctx) return false;
    
    ctx->root_scope = NULL;
    ctx->ast_root = NULL;
    ctx->has_errors = false;
    
    return true;
}

// 清理编译器上下文
void compiler_context_cleanup(CompilerContext* ctx) {
    if (!ctx) return;
    
    // 释放 AST（之前未释放，导致严重内存泄露）
    if (ctx->ast_root) {
        ast_free(ctx->ast_root);
        ctx->ast_root = NULL;
    }
    
    // 释放作用域
    if (ctx->root_scope) {
        scope_free(ctx->root_scope);
        ctx->root_scope = NULL;
    }
}

// 分析源代码，构建符号表
bool compiler_analyze_with_filename(CompilerContext* ctx, const char* source, const char* filename) {
    if (!ctx || !source) return false;
    
    // 清理之前的状态（包括 AST，防止内存泄露）
    if (ctx->ast_root) {
        ast_free(ctx->ast_root);
        ctx->ast_root = NULL;
    }
    if (ctx->root_scope) {
        scope_free(ctx->root_scope);
        ctx->root_scope = NULL;
    }
    
    // 重置错误状态
    error_clear();

    // 设置文件名（用于模块路径解析，read_module_file 依赖此值解析相对路径）
    if (filename) {
        error_set_filename(filename);
    }

    // LSP 分析时抑制 stderr（防止 "错误收集器已满" 洪水导致 CPU 飙升）
    lsp_suppress_stderr();

    // 1. 词法分析
    Lexer lexer;
    lexer_init(&lexer, source);

    // 2. 语法分析
    Parser parser;
    parser_init(&parser, source);
    if (parser_parse(&parser) < 0) {
        ctx->has_errors = true;
        ctx->ast_root = parser.root;
        // 不清除错误 — 语法错误需要被诊断服务收集
        lsp_restore_stderr();
        return false;
    }

    ctx->ast_root = parser.root;

    // 3. 语义分析（使用标准的 semantic_analyze 或 semantic_analyze_module）
    Semantic sem;
    semantic_init(&sem, parser.root);

    // 标记为 LSP 模式（保留所有作用域）
    sem.is_lsp_mode = true;

    // 检测是否是模块文件（检查是否有 export 语句）
    bool is_module = false;
    if (parser.root->kind == AST_BLOCK) {
        for (int i = 0; i < parser.root->u.block.count; i++) {
            if (parser.root->u.block.items[i]->kind == AST_EXPORT) {
                is_module = true;
                break;
            }
        }
    }

    // 根据文件类型调用相应的分析函数
    if (is_module) {
        semantic_analyze_module(&sem, parser.root);
    } else {
        semantic_analyze(&sem, parser.root);
    }

    ctx->root_scope = sem.root_scope;

    // 保存调试信息（cleanup 前保存）
    int dbg_imported = sem.imported_module_count;
    int err_count = errors.count;

    // LSP 模式下过滤误报错误（因 .leno import 可能未完全解析）
    // 不再无条件清除所有错误，而是只移除与 import 模块解析相关的误报
    // 保留真正的语法错误、未定义变量、类型不匹配等
    {
        int write_idx = 0;
        for (int read_idx = 0; read_idx < errors.count; read_idx++) {
            Error* err = &errors.list[read_idx];
            bool is_import_false_positive = false;

            // 过滤掉因模块导入未完全解析而产生的误报
            // 这些错误的消息通常包含 "模块" 和 "未导入" 或 "use 语句错误"
            if (err->type == ERR_SEMANTIC && err->msg[0] != '\0') {
                if (strstr(err->msg, "未导入") != NULL ||
                    strstr(err->msg, "use 语句错误") != NULL ||
                    strstr(err->msg, "use 错误") != NULL) {
                    is_import_false_positive = true;
                }
            }

            if (!is_import_false_positive) {
                if (write_idx != read_idx) {
                    errors.list[write_idx] = errors.list[read_idx];
                }
                write_idx++;
            }
        }
        errors.count = write_idx;
    }

    ctx->has_errors = (errors.count > 0);

    // 清理语义分析资源（保留 root_scope）
    semantic_cleanup(&sem);

    // 恢复 stderr
    lsp_restore_stderr();

    // 调试日志
    fprintf(stderr, "[LSP-ANALYZE] file=%s imported=%d errors=%d (filtered=%d)\n",
            filename ? filename : "null", dbg_imported, err_count,
            err_count - errors.count);
    fflush(stderr);

    return true;
}

// 获取符号信息
bool compiler_get_symbol_info(CompilerContext* ctx, const char* name, 
                               char** type_str_out, bool* is_global_out) {
    if (!ctx || !name || !ctx->root_scope) {
        return false;
    }
    
    // 在作用域树中查找符号（使用 BFS 遍历所有嵌套作用域）
    Symbol* sym = scope_resolve_tree_bfs(ctx->root_scope, name);
    if (!sym) return false;
    
    if (type_str_out) {
        *type_str_out = strdup(type_to_string(sym->type));
    }
    
    if (is_global_out) {
        *is_global_out = (sym->scope == ctx->root_scope);
    }
    
    return true;
}

// 递归收集作用域树中所有符号的辅助函数
static void collect_scope_symbols(Scope* scope, char*** names, char*** types,
                                   int* count, int* capacity) {
    if (!scope) return;

    // 收集当前作用域的符号
    for (int i = 0; i < scope->sym_cnt; i++) {
        Symbol* sym = scope->syms[i];
        if (!sym || !sym->name) continue;

        // 检查是否已存在同名符号（避免重复，优先保留外层作用域的）
        bool duplicate = false;
        for (int j = 0; j < *count; j++) {
            if ((*names)[j] && strcmp((*names)[j], sym->name) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        // 扩容
        if (*count >= *capacity) {
            *capacity *= 2;
            *names = (char**)realloc(*names, sizeof(char*) * (*capacity));
            *types = (char**)realloc(*types, sizeof(char*) * (*capacity));
        }

        (*names)[*count] = strdup(sym->name);
        (*types)[*count] = sym->type ? strdup(type_to_string(sym->type)) : strdup("unknown");
        (*count)++;
    }

    // 递归遍历子作用域
    for (int i = 0; i < scope->child_count; i++) {
        collect_scope_symbols(scope->children[i], names, types, count, capacity);
    }
}

// 获取所有符号（递归遍历所有子作用域，包括局部变量）
int compiler_get_all_symbols(CompilerContext* ctx, char*** names_out, char*** types_out) {
    if (!ctx || !ctx->root_scope) return 0;

    int capacity = 64;
    int count = 0;
    char** names = (char**)malloc(sizeof(char*) * capacity);
    char** types = (char**)malloc(sizeof(char*) * capacity);

    if (!names || !types) {
        free(names);
        free(types);
        return 0;
    }

    collect_scope_symbols(ctx->root_scope, &names, &types, &count, &capacity);

    *names_out = names;
    *types_out = types;

    return count;
}

// 释放符号列表
void compiler_free_symbol_list(char** names, char** types, int count) {
    if (names) {
        for (int i = 0; i < count; i++) {
            free(names[i]);
        }
        free(names);
    }
    
    if (types) {
        for (int i = 0; i < count; i++) {
            free(types[i]);
        }
        free(types);
    }
}

// 从指定 struct 中获取字段类型信息
// 如果找到字段，返回 true 并设置 type_str_out 和 struct_name_out
bool compiler_get_struct_field_info(CompilerContext* ctx, const char* struct_name,
                                     const char* field_name, char** type_str_out) {
    if (!ctx || !ctx->root_scope || !struct_name || !field_name) {
        return false;
    }

    // 在作用域中查找 struct 定义
    Symbol* struct_sym = scope_resolve_tree_bfs(ctx->root_scope, struct_name);
    if (!struct_sym || !struct_sym->type || struct_sym->type->kind != TYPE_STRUCT) {
        return false;
    }

    // 在 struct 中查找字段
    for (int i = 0; i < struct_sym->struct_field_count; i++) {
        if (struct_sym->struct_field_names[i] &&
            strcmp(struct_sym->struct_field_names[i], field_name) == 0) {
            if (type_str_out) {
                *type_str_out = strdup(type_to_string(struct_sym->struct_field_types[i]));
            }
            return true;
        }
    }

    return false;
}

// 获取所有包含指定字段的 struct 名称列表
// 返回找到的 struct 数量，names_out 需要调用者释放
int compiler_find_structs_with_field(CompilerContext* ctx, const char* field_name,
                                      char*** struct_names_out) {
    if (!ctx || !ctx->root_scope || !field_name || !struct_names_out) {
        return 0;
    }

    // 先统计有多少个 struct 包含该字段
    int count = 0;
    for (int i = 0; i < ctx->root_scope->sym_cnt; i++) {
        Symbol* sym = ctx->root_scope->syms[i];
        if (sym->type && sym->type->kind == TYPE_STRUCT && sym->struct_field_count > 0) {
            for (int j = 0; j < sym->struct_field_count; j++) {
                if (sym->struct_field_names[j] &&
                    strcmp(sym->struct_field_names[j], field_name) == 0) {
                    count++;
                    break;
                }
            }
        }
    }

    if (count == 0) {
        return 0;
    }

    // 分配内存并填充 struct 名称
    char** names = (char**)malloc(sizeof(char*) * count);
    if (!names) {
        return 0;
    }

    int idx = 0;
    for (int i = 0; i < ctx->root_scope->sym_cnt && idx < count; i++) {
        Symbol* sym = ctx->root_scope->syms[i];
        if (sym->type && sym->type->kind == TYPE_STRUCT && sym->struct_field_count > 0) {
            for (int j = 0; j < sym->struct_field_count; j++) {
                if (sym->struct_field_names[j] &&
                    strcmp(sym->struct_field_names[j], field_name) == 0) {
                    names[idx] = strdup(sym->name);
                    idx++;
                    break;
                }
            }
        }
    }

    *struct_names_out = names;
    return count;
}
