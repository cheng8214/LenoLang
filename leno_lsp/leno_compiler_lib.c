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
    
    // 释放作用域
    if (ctx->root_scope) {
        scope_free(ctx->root_scope);
        ctx->root_scope = NULL;
    }
    
    // 注意：AST 由 parser 管理，这里不释放
    ctx->ast_root = NULL;
}

// 分析源代码，构建符号表
bool compiler_analyze_with_filename(CompilerContext* ctx, const char* source, const char* filename) {
    if (!ctx || !source) return false;
    
    // 清理之前的状态
    if (ctx->root_scope) {
        scope_free(ctx->root_scope);
        ctx->root_scope = NULL;
    }
    
    // 重置错误状态
    error_clear();
    
    // 设置文件名（用于模块路径解析）
    if (filename) {
        error_set_filename(filename);
    }
    
    // 1. 词法分析
    Lexer lexer;
    lexer_init(&lexer, source);
    
    // 2. 语法分析
    Parser parser;
    parser_init(&parser, source);
    if (parser_parse(&parser) < 0) {
        ctx->has_errors = true;
        ctx->ast_root = parser.root;
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
    ctx->has_errors = error_has_any() != 0;

    // 注意：即使有错误也保留作用域，这样 LSP 仍然可以提供部分符号信息
    // 清理语义分析资源（保留 root_scope）
    semantic_cleanup(&sem);

    return !ctx->has_errors;
}

// 获取符号信息
bool compiler_get_symbol_info(CompilerContext* ctx, const char* name, 
                               char** type_str_out, bool* is_global_out) {
    if (!ctx || !name || !ctx->root_scope) {
        fprintf(stderr, "[COMPILER DEBUG] get_symbol_info: invalid params ctx=%p name=%s root_scope=%p\n", 
                (void*)ctx, name ? name : "(null)", (void*)(ctx ? ctx->root_scope : NULL));
        return false;
    }
    
    // 在作用域树中查找符号（使用 BFS 遍历所有嵌套作用域）
    Symbol* sym = scope_resolve_tree_bfs(ctx->root_scope, name);
    fprintf(stderr, "[COMPILER DEBUG] scope_resolve_tree_bfs returned: %p for name '%s'\n", (void*)sym, name);
    if (!sym) return false;
    
    if (type_str_out) {
        *type_str_out = strdup(type_to_string(sym->type));
    }
    
    if (is_global_out) {
        *is_global_out = (sym->scope == ctx->root_scope);
    }
    
    return true;
}

// 获取所有符号
int compiler_get_all_symbols(CompilerContext* ctx, char*** names_out, char*** types_out) {
    if (!ctx || !ctx->root_scope) return 0;
    
    Scope* scope = ctx->root_scope;
    int count = scope->sym_cnt;
    
    if (count == 0) return 0;
    
    char** names = (char**)malloc(sizeof(char*) * count);
    char** types = (char**)malloc(sizeof(char*) * count);
    
    if (!names || !types) {
        free(names);
        free(types);
        return 0;
    }
    
    for (int i = 0; i < count; i++) {
        Symbol* sym = scope->syms[i];
        names[i] = strdup(sym->name);
        types[i] = strdup(type_to_string(sym->type));
    }
    
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
