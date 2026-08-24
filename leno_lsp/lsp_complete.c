/**
 * LSP 补全服务 - 重构版
 * 
 * 架构：
 * 1. 解析补全上下文（comp_context.c）
 * 2. 使用 CompletionSet 收集补全项（comp_set.c）
 * 3. 各提供者模块独立添加补全项（comp_keywords.c, comp_symbols.c）
 * 4. 按优先级排序，转换为 LspCompletionItem 输出
 * 
 * 关键修复：
 * - 关键字/类型/builtin 始终可用（不再与上下文互斥）
 * - 服务器端不做前缀过滤，完全交由客户端
 * - 补全结果按优先级排序（关键字 → 类型 → 内置 → 符号）
 * - 去重：同 label 仅保留优先级最高者
 */

#include "leno_lsp.h"
#include "lsp_completion.h"
#include "leno_builtins.h"
#include "leno_compiler_lib.h"
#include "../src/include/leno_types.h"
#include "../src/include/native.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

/* ========== 导入模块（来自 comp_import.c） ========== */

extern ImportAlias* parse_imports(const char* content, int* count);
extern void free_import_aliases(ImportAlias* aliases, int count);
extern const char* find_module_by_alias(ImportAlias* aliases, int count, const char* alias);
extern const char* find_module_path_by_alias(ImportAlias* aliases, int count, const char* alias);

/* ========== 模块文件读取（来自其他编译单元） ========== */

extern char* read_module_file(const char* file_path, const char* current_file);

/* ========== 主入口：lsp_get_completions ========== */

LspCompletionItem* lsp_get_completions(const char* content, LspPosition pos, int* count, const char* file_path) {
    *count = 0;
    
    if (!content) return NULL;
    
    clock_t t_start = clock();
    fprintf(stderr, "[COMPLETE] lsp_get_completions START file=%s line=%d char=%d\n",
            file_path ? file_path : "null", pos.line, pos.character);
    fflush(stderr);
    
    // 创建补全集合
    CompletionSet* set = comp_set_create();
    if (!set) return NULL;
    
    // 解析导入语句
    int import_count = 0;
    ImportAlias* import_aliases = parse_imports(content, &import_count);
    
    // 检测补全上下文
    CompletionContextInfo ctx = comp_detect_context(content, pos, file_path, import_count, import_aliases);
    
    // === 上下文特定补全 ===
    // 基础补全（关键字/类型/内置）只在需要时添加，避免污染点访问上下文
    switch (ctx.type) {
        case CTX_NONE:
            // 注释/字符串等非代码位置：不提供任何补全
            comp_context_free(&ctx);
            free_import_aliases(import_aliases, import_count);
            comp_set_destroy(set);
            *count = 0;
            return NULL;
            
        case CTX_NORMAL:
            // 普通位置：关键字 + 类型 + 内置函数 + 符号
            comp_provider_add_keywords(set, NULL);
            comp_provider_add_types(set, NULL);
            comp_provider_add_builtins(set, NULL);
            
            // self 关键字（结构体方法体内）
            comp_provider_add_self_keyword(set, content, pos);
            
            // 用户定义符号
            comp_provider_add_user_symbols(set, content, file_path, NULL);
            
            // use 语句导入的类型
            comp_provider_add_use_symbols(set, content, file_path, import_count, import_aliases);
            
            // 原生模块方法
            comp_provider_add_native_modules(set, NULL);
            break;
            
        case CTX_TYPE_ANNOTATION:
            // 类型注解位置
            comp_provider_add_type_annotation_types(set, content, file_path, import_count, import_aliases);
            // use 语句导入的类型也应在类型注解位置可用
            comp_provider_add_use_symbols(set, content, file_path, import_count, import_aliases);
            // 也添加 self（结构体方法体中可能有类型注解 + self）
            comp_provider_add_self_keyword(set, content, pos);
            break;
            
        case CTX_NEW:
            // new 后补全 struct 名称
            comp_provider_add_new_structs(set, content, file_path, import_count, import_aliases);
            // use 语句导入的 struct 也应在 new 后可用
            comp_provider_add_use_symbols(set, content, file_path, import_count, import_aliases);
            break;
            
        case CTX_USE_MODULE:
            // use 后补全模块别名
            comp_provider_add_use_modules(set, import_count, import_aliases, NULL);
            break;
            
        case CTX_USE_MEMBER:
            // use module. 后补全成员
            comp_provider_add_module_symbols(set, content, file_path,
                                             ctx.module_alias, ctx.member_prefix,
                                             import_count, import_aliases);
            // 也补全原生模块的实际方法
            if (ctx.module_alias) {
                const char* actual_mod = find_module_by_alias(import_aliases, import_count, ctx.module_alias);
                int mtd_count = 0;
                ModuleMethodMeta* metas = native_get_module_method_metas(actual_mod, &mtd_count);
                if (metas && mtd_count > 0) {
                    for (int i = 0; i < mtd_count; i++) {
                        const char* ret_str = type_kind_to_string(metas[i].return_type);
                        char detail[512];
                        if (metas[i].arity == 0) {
                            snprintf(detail, sizeof(detail), "%s.%s() -> %s", ctx.module_alias, metas[i].method_name, ret_str);
                        } else {
                            snprintf(detail, sizeof(detail), "%s.%s(...) -> %s", ctx.module_alias, metas[i].method_name, ret_str);
                        }
                        comp_set_add(set, metas[i].method_name, LSP_COMP_METHOD, PRIO_METHOD,
                                     detail, NULL, NULL, NULL);
                    }
                    native_free_module_method_metas(metas);
                }
            }
            break;
            
        case CTX_DOT_ACCESS: {
            // 点访问：区分模块访问 vs 变量成员访问
            const char* mod_path = find_module_path_by_alias(import_aliases, import_count, ctx.module_alias);
            bool is_leno_module = (mod_path != NULL);
            
            // 检查是否是原生模块（如 io, types, maths 等）
            int native_mod_count = 0;
            char** native_mods = native_get_all_modules(&native_mod_count);
            bool is_native_module = false;
            if (native_mods) {
                for (int i = 0; i < native_mod_count; i++) {
                    if (strcmp(native_mods[i], ctx.module_alias) == 0) {
                        is_native_module = true;
                        break;
                    }
                }
                native_free_module_list(native_mods, native_mod_count);
            }
            
            if (is_leno_module) {
                // .leno 模块.成员 补全
                comp_provider_add_module_symbols(set, content, file_path,
                                                 ctx.module_alias, ctx.member_prefix,
                                                 import_count, import_aliases);
                
                // 也添加原生模块方法
                const char* actual_mod = find_module_by_alias(import_aliases, import_count, ctx.module_alias);
                int mtd_count = 0;
                ModuleMethodMeta* metas = native_get_module_method_metas(actual_mod, &mtd_count);
                if (metas && mtd_count > 0) {
                    for (int i = 0; i < mtd_count; i++) {
                        const char* ret_str = type_kind_to_string(metas[i].return_type);
                        char detail[512];
                        if (metas[i].arity == 0) {
                            snprintf(detail, sizeof(detail), "%s.%s() -> %s", ctx.module_alias, metas[i].method_name, ret_str);
                        } else {
                            snprintf(detail, sizeof(detail), "%s.%s(...) -> %s", ctx.module_alias, metas[i].method_name, ret_str);
                        }
                        char doc[1024];
                        snprintf(doc, sizeof(doc), "```leno\n%s\n```", detail);
                        comp_set_add(set, metas[i].method_name, LSP_COMP_METHOD, PRIO_METHOD,
                                     detail, doc, NULL, NULL);
                    }
                    native_free_module_method_metas(metas);
                }
                
                // 模块常量
                int const_count = 0;
                char** consts = native_get_module_consts(actual_mod, &const_count);
                if (consts && const_count > 0) {
                    for (int i = 0; i < const_count; i++) {
                        bool found = false;
                        int val = native_find_module_const(actual_mod, consts[i], &found);
                        char detail[256];
                        snprintf(detail, sizeof(detail), "%s.%s = %d", ctx.module_alias, consts[i], val);
                        comp_set_add(set, consts[i], LSP_COMP_CONSTANT, PRIO_MODULE,
                                     detail, NULL, NULL, NULL);
                    }
                    native_free_module_const_list(consts, const_count);
                }
            } else if (is_native_module) {
                // 原生模块.成员 补全（如 io., maths., types. 等）
                int mtd_count = 0;
                ModuleMethodMeta* metas = native_get_module_method_metas(ctx.module_alias, &mtd_count);
                if (metas && mtd_count > 0) {
                    for (int i = 0; i < mtd_count; i++) {
                        const char* ret_str = type_kind_to_string(metas[i].return_type);
                        char detail[512];
                        if (metas[i].arity == 0) {
                            snprintf(detail, sizeof(detail), "%s.%s() -> %s", ctx.module_alias, metas[i].method_name, ret_str);
                        } else if (metas[i].arity < 0) {
                            snprintf(detail, sizeof(detail), "%s.%s(...) -> %s", ctx.module_alias, metas[i].method_name, ret_str);
                        } else {
                            char params[256] = "";
                            int off = 0;
                            for (int pp = 0; pp < metas[i].arity && pp < MAX_METHOD_PARAMS; pp++) {
                                const char* pt = type_kind_to_string(metas[i].param_types[pp]);
                                if (pp > 0) off += snprintf(params + off, sizeof(params) - off, ", ");
                                off += snprintf(params + off, sizeof(params) - off, "%s", pt);
                            }
                            snprintf(detail, sizeof(detail), "%s.%s(%s) -> %s", ctx.module_alias, metas[i].method_name, params, ret_str);
                        }
                        char doc[1024];
                        snprintf(doc, sizeof(doc), "```leno\n%s\n```", detail);
                        comp_set_add(set, metas[i].method_name, LSP_COMP_METHOD, PRIO_METHOD,
                                     detail, doc, NULL, NULL);
                    }
                    native_free_module_method_metas(metas);
                }
                
                // 原生模块常量
                int const_count = 0;
                char** consts = native_get_module_consts(ctx.module_alias, &const_count);
                if (consts && const_count > 0) {
                    for (int i = 0; i < const_count; i++) {
                        bool found = false;
                        int val = native_find_module_const(ctx.module_alias, consts[i], &found);
                        char detail[256];
                        snprintf(detail, sizeof(detail), "%s.%s = %d", ctx.module_alias, consts[i], val);
                        comp_set_add(set, consts[i], LSP_COMP_CONSTANT, PRIO_MODULE,
                                     detail, NULL, NULL, NULL);
                    }
                    native_free_module_const_list(consts, const_count);
                }
            } else {
                // 变量.成员 补全（类型推断）
                // 首先检查是否是 cstruct/struct 类型名（如 Normal. 中的 Normal）
                // 如果是 cstruct 类型名，提供方法补全（size, alignment, offset_of, malloc 等）
                bool is_type_name = comp_provider_add_cstruct_type_methods(
                    set, content, file_path,
                    ctx.module_alias, import_count, import_aliases);

                if (!is_type_name) {
                    // 不是 cstruct 类型名，继续检查其他可能性
                    // 检查是否是函数调用链（如 ttfLib().）
                    if (ctx.is_func_call_chain) {
                        comp_provider_add_func_call_chain_members(set, content, file_path,
                                                                     ctx.module_alias, import_count, import_aliases);
                    } else if (ctx.is_array_index) {
                        // 数组索引成员访问（如 _texCache[i].field）
                        comp_provider_add_array_index_members(set, content, file_path,
                                                              ctx.module_alias, import_count, import_aliases,
                                                              pos);
                    } else {
                        comp_provider_add_variable_members(set, content, file_path,
                                                           ctx.module_alias, import_count, import_aliases,
                                                           pos);
                    }
                }
                
                // 也尝试 enum 成员
                // TODO: 解析文件中的 enum 成员
            }
            break;
        }
            
        case CTX_STRING_LITERAL:
            comp_provider_add_string_methods(set);
            break;
    }
    
    // === 释放上下文 ===
    comp_context_free(&ctx);
    free_import_aliases(import_aliases, import_count);
    
    // === 转换为输出 ===
    int out_count = 0;
    LspCompletionItem* items = comp_set_to_lsp_array(set, &out_count);
    
    // === 清理 ===
    comp_set_destroy(set);
    
    *count = out_count;
    
    clock_t t_end = clock();
    fprintf(stderr, "[COMPLETE] END count=%d time=%.0fms\n",
            out_count, (double)(t_end - t_start) * 1000.0 / CLOCKS_PER_SEC);
    fflush(stderr);
    
    return items;
}

/* ========== 释放补全项 ========== */

void lsp_free_completions(LspCompletionItem* items, int count) {
    if (!items) return;
    
    for (int i = 0; i < count; i++) {
        free(items[i].label);
        free(items[i].detail);
        free(items[i].documentation);
        free(items[i].insertText);
    }
    
    free(items);
}

/* ========== 保持向后兼容：补全请求处理 ========== */

char* lsp_handle_completion(LspServer* server, int id, JsonValue* params) {
    fprintf(stderr, "[COMPLETE] Request id=%d\n", id);
    fflush(stderr);

    if (!server || server->state != LSP_STATE_INITIALIZED) {
        return lsp_create_error(id, LSP_ERROR_SERVER_NOT_INITIALIZED,
                                "Server not initialized");
    }
    
    JsonValue* text_doc = json_object_get(params, "textDocument");
    JsonValue* position = json_object_get(params, "position");
    
    if (!text_doc || !position) {
        return lsp_create_error(id, LSP_ERROR_INVALID_PARAMS,
                                "Missing textDocument or position");
    }
    
    JsonValue* uri = json_object_get(text_doc, "uri");
    JsonValue* line = json_object_get(position, "line");
    JsonValue* character = json_object_get(position, "character");
    
    if (!uri || !line || !character) {
        return lsp_create_error(id, LSP_ERROR_INVALID_PARAMS,
                                "Invalid position");
    }
    
    LspTextDocument* doc = lsp_document_get(server, json_string_value(uri));
    if (!doc) {
        return lsp_create_error(id, LSP_ERROR_INVALID_PARAMS,
                                "Document not found");
    }
    
    LspPosition pos = {
        .line = (uint32_t)json_int_value(line),
        .character = (uint32_t)json_int_value(character)
    };
    
    // 获取文件路径
    const char* uri_str = json_string_value(uri);
    char file_path_buf[1024] = {0};
    char* file_path = NULL;
    if (uri_str && strncmp(uri_str, "file:///", 8) == 0) {
        const char* p = uri_str + 8;
        char* q = file_path_buf;
        size_t len = 0;
        while (*p && len < sizeof(file_path_buf) - 1) {
            if (*p == '%' && *(p+1) && *(p+2)) {
                char hex[3] = {*(p+1), *(p+2), '\0'};
                int val = (int)strtol(hex, NULL, 16);
                *q++ = (char)val;
                p += 3;
            } else if (*p == '/') {
                *q++ = '\\';
                p++;
            } else {
                *q++ = *p++;
            }
            len++;
        }
        *q = '\0';
        file_path = file_path_buf;
    }
    
    // 获取补全项
    int count = 0;
    LspCompletionItem* items = lsp_get_completions(doc->content, pos, &count, file_path);
    fprintf(stderr, "[COMPLETE] items=%p count=%d\n", (void*)items, count);
    fflush(stderr);

    // 构建响应
    JsonValue* result = NULL;
    if (items && count > 0) {
        result = json_object_new();
        JsonValue* items_array = json_array_new();
        
        for (int i = 0; i < count; i++) {
            JsonValue* item = json_object_new();
            json_object_set(item, "label", json_string_new(items[i].label));
            json_object_set(item, "kind", json_int_new(items[i].kind));
            
            if (items[i].detail) {
                json_object_set(item, "detail", json_string_new(items[i].detail));
            }
            
            if (items[i].documentation) {
                JsonValue* doc_obj = json_object_new();
                json_object_set(doc_obj, "kind", json_string_new("markdown"));
                json_object_set(doc_obj, "value", json_string_new(items[i].documentation));
                json_object_set(item, "documentation", doc_obj);
            }
            
            if (items[i].insertText) {
                json_object_set(item, "insertText", json_string_new(items[i].insertText));
            }
            
            json_array_add(items_array, item);
        }
        
        json_object_set(result, "items", items_array);
        json_object_set(result, "isIncomplete", json_bool_new(true));  // 关键改动：告知客户端我们的结果需要动态过滤
    } else {
        result = json_array_new();  // 空数组
    }
    
    char* response = lsp_create_response(id, result);
    
    if (result) {
        json_free(result);
    }
    lsp_free_completions(items, count);
    
    return response;
}
