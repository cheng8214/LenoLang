/**
 * LSP Signature Help - 函数参数提示
 *
 * 检测光标是否在函数调用的括号内，匹配函数名，
 * 从内置函数/用户函数/原生模块方法/模块导入函数获取签名信息。
 */

#include "leno_lsp.h"
#include "lsp_completion.h"
#include "leno_builtins.h"
#include "leno_compiler_lib.h"
#include "../src/include/leno_types.h"
#include "../src/include/native.h"
#include "../src/include/module_symbol_table.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ========== 内部辅助 ========== */

// 从光标位置向前查找最近的未闭合 '(' 及其前面的函数名
// 返回：函数名（需 free），设置 *out_active_param 为当前参数索引（0-based）
static char* find_func_call_before_cursor(const char* content, int cursor_offset,
                                           int* out_active_param) {
    *out_active_param = 0;

    if (cursor_offset < 0 || cursor_offset >= (int)strlen(content)) return NULL;

    int depth = 0;
    int paren_pos = -1;

    // 从光标向前扫描，找最近的未闭合 '('
    for (int i = cursor_offset; i >= 0; i--) {
        char c = content[i];
        if (c == ')') depth++;
        else if (c == '(') {
            if (depth == 0) {
                paren_pos = i;
                break;
            }
            depth--;
        }
        // 跳过字符串内容
        else if (c == '"') {
            int bs = 0;
            int j = i - 1;
            while (j >= 0 && content[j] == '\\') { bs++; j--; }
            if (bs % 2 == 0) {
                // 进入字符串，跳过整个字符串
                i--;
                while (i >= 0) {
                    if (content[i] == '"') {
                        bs = 0;
                        int k = i - 1;
                        while (k >= 0 && content[k] == '\\') { bs++; k--; }
                        if (bs % 2 == 0) break;
                    }
                    i--;
                }
            }
        }
    }

    if (paren_pos < 0) return NULL;

    // 计算当前参数索引（基于逗号）
    int param_idx = 0;
    int pdepth = 0;  // 嵌套括号深度
    int in_str = 0;
    char str_q = 0;
    for (int i = paren_pos + 1; i <= cursor_offset && i < (int)strlen(content); i++) {
        char c = content[i];
        if (in_str) {
            if (c == str_q) { in_str = 0; }
            continue;
        }
        if (c == '"' || c == '\'') { in_str = 1; str_q = c; continue; }
        if (c == '(' || c == '[' || c == '{') pdepth++;
        else if (c == ')' || c == ']' || c == '}') pdepth--;
        else if (c == ',' && pdepth == 0) param_idx++;
    }
    *out_active_param = param_idx;

    // 从 '(' 向前提取函数名
    int end = paren_pos - 1;
    while (end >= 0 && isspace((unsigned char)content[end])) end--;
    if (end < 0) return NULL;

    int start = end;
    // 支持点号表达式（如 io.print 或 obj.method）
    while (start >= 0 && (isalnum((unsigned char)content[start]) || content[start] == '_' || content[start] == '.')) {
        start--;
    }
    start++;

    int name_len = end - start + 1;
    if (name_len <= 0) return NULL;

    char* name = (char*)malloc(name_len + 1);
    if (!name) return NULL;
    memcpy(name, content + start, name_len);
    name[name_len] = '\0';

    return name;
}

/* ========== 构建签名信息 ========== */

static char* build_builtin_signature(const BuiltinFunctionMeta* meta, int active_param) {
    // 构建 parameter labels
    JsonValue* params = json_array_new();
    // 内置函数的 signature 格式如 "print(...values)" 或 "len(collection)"
    // 简化处理：直接使用 signature 文本作为单个 label
    JsonValue* param = json_object_new();
    json_object_set(param, "label", json_string_new(meta->signature));
    json_array_add(params, param);

    // 构建 SignatureInformation
    JsonValue* sig = json_object_new();
    json_object_set(sig, "label", json_string_new(meta->signature));
    json_object_set(sig, "documentation", json_string_new(meta->description));
    json_object_set(sig, "parameters", params);
    json_object_set(sig, "activeParameter", json_int_new(active_param));

    // 构建 SignatureHelp
    JsonValue* result = json_object_new();
    JsonValue* sigs = json_array_new();
    json_array_add(sigs, sig);
    json_object_set(result, "signatures", sigs);
    json_object_set(result, "activeSignature", json_int_new(0));

    char* response = (char*)json_stringify(result);
    json_free(result);
    return response;
}

static char* build_native_module_signature(const char* module, const char* method,
                                            ModuleMethodMeta* meta, int active_param) {
    int arity = meta->arity;
    const char* ret_str = type_kind_to_string(meta->return_type);

    // 构建签名标签：module.method(params) -> ret
    char label[512];
    char params_str[256] = {0};

    if (arity == 0) {
        snprintf(label, sizeof(label), "%s.%s() -> %s", module, method, ret_str);
    } else if (arity < 0) {
        snprintf(label, sizeof(label), "%s.%s(...) -> %s", module, method, ret_str);
    } else {
        int off = 0;
        for (int i = 0; i < arity && i < MAX_METHOD_PARAMS; i++) {
            const char* pt = type_kind_to_string(meta->param_types[i]);
            if (i > 0) off += snprintf(params_str + off, sizeof(params_str) - off, ", ");
            off += snprintf(params_str + off, sizeof(params_str) - off, "%s p%d", pt, i + 1);
        }
        snprintf(label, sizeof(label), "%s.%s(%s) -> %s", module, method, params_str, ret_str);
    }

    // 构建参数 labels
    JsonValue* params = json_array_new();
    if (arity > 0) {
        for (int i = 0; i < arity && i < MAX_METHOD_PARAMS; i++) {
            const char* pt = type_kind_to_string(meta->param_types[i]);
            char plabel[64];
            snprintf(plabel, sizeof(plabel), "%s p%d", pt, i + 1);
            JsonValue* param = json_object_new();
            json_object_set(param, "label", json_string_new(plabel));
            json_array_add(params, param);
        }
    } else if (arity < 0) {
        JsonValue* param = json_object_new();
        json_object_set(param, "label", json_string_new("...args"));
        json_array_add(params, param);
    }

    JsonValue* sig = json_object_new();
    json_object_set(sig, "label", json_string_new(label));
    json_object_set(sig, "parameters", params);
    if (active_param >= 0 && active_param < arity) {
        json_object_set(sig, "activeParameter", json_int_new(active_param));
    }

    JsonValue* result = json_object_new();
    JsonValue* sigs = json_array_new();
    json_array_add(sigs, sig);
    json_object_set(result, "signatures", sigs);
    json_object_set(result, "activeSignature", json_int_new(0));

    char* response = (char*)json_stringify(result);
    json_free(result);
    return response;
}

static char* build_user_func_signature(const char* func_name, const char* param_text,
                                       int param_count, const char* return_str,
                                       int active_param) {
    char label[512];
    snprintf(label, sizeof(label), "func %s(%s) -> %s", func_name,
             param_text ? param_text : "", return_str ? return_str : "void");

    // 构建参数 labels（从 param_text 简单分割）
    JsonValue* params = json_array_new();
    if (param_text && param_text[0]) {
        // 简单处理：将整个 param_text 作为单个参数 label
        // 更精确的分割需要解析逗号，但考虑嵌套泛型比较复杂
        JsonValue* param = json_object_new();
        json_object_set(param, "label", json_string_new(param_text));
        json_array_add(params, param);
    }

    JsonValue* sig = json_object_new();
    json_object_set(sig, "label", json_string_new(label));
    json_object_set(sig, "parameters", params);
    if (active_param >= 0 && active_param < param_count) {
        json_object_set(sig, "activeParameter", json_int_new(active_param));
    }

    JsonValue* result = json_object_new();
    JsonValue* sigs = json_array_new();
    json_array_add(sigs, sig);
    json_object_set(result, "signatures", sigs);
    json_object_set(result, "activeSignature", json_int_new(0));

    char* response = (char*)json_stringify(result);
    json_free(result);
    return response;
}

/* ========== 主入口 ========== */

char* lsp_handle_signature_help(LspServer* server, int id, JsonValue* params) {
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
        return lsp_create_error(id, LSP_ERROR_INVALID_PARAMS, "Invalid position");
    }

    LspTextDocument* doc = lsp_document_get(server, json_string_value(uri));
    if (!doc) {
        return lsp_create_error(id, LSP_ERROR_INVALID_PARAMS, "Document not found");
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

    int cursor_offset = lsp_position_to_offset(doc->content, pos);
    int active_param = 0;
    char* func_name = find_func_call_before_cursor(doc->content, cursor_offset, &active_param);
    if (!func_name) {
        // 不在函数调用括号内，返回空签名
        JsonValue* result = json_object_new();
        json_object_set(result, "signatures", json_array_new());
        char* response = lsp_create_response(id, result);
        json_free(result);
        return response;
    }

    fprintf(stderr, "[SIG] func='%s' active_param=%d\n", func_name, active_param);
    fflush(stderr);

    char* sig_response = NULL;

    // 1. 内置函数
    const BuiltinFunctionMeta* builtin = find_builtin_function(func_name);
    if (builtin) {
        sig_response = build_builtin_signature(builtin, active_param);
    }

    // 2. 原生模块方法（如 io.print, strings.replace）
    if (!sig_response) {
        // 检查是否是 module.method 格式
        char* dot = strchr(func_name, '.');
        if (dot) {
            char mod_name[128] = {0};
            int mod_len = dot - func_name;
            if (mod_len > 0 && mod_len < 127) {
                memcpy(mod_name, func_name, mod_len);
                mod_name[mod_len] = '\0';
                const char* meth = dot + 1;

                int arity = native_get_module_method_arity(mod_name, meth);
                if (arity >= 0 || arity == -1) {
                    ModuleMethodMeta* metas = native_get_module_method_metas(mod_name, NULL);
                    int mtd_count = 0;
                    metas = native_get_module_method_metas(mod_name, &mtd_count);
                    if (metas) {
                        for (int i = 0; i < mtd_count; i++) {
                            if (strcmp(metas[i].method_name, meth) == 0) {
                                sig_response = build_native_module_signature(mod_name, meth, &metas[i], active_param);
                                break;
                            }
                        }
                        native_free_module_method_metas(metas);
                    }
                }
            }
        }
    }

    // 3. 用户定义函数（当前文件）
    if (!sig_response && !strchr(func_name, '.')) {
        CompilerContext ctx;
        compiler_context_init(&ctx);
        if (compiler_analyze_with_filename(&ctx, doc->content, file_path) && ctx.root_scope) {
            for (int i = 0; i < ctx.root_scope->sym_cnt; i++) {
                Symbol* sym = ctx.root_scope->syms[i];
                if (sym && sym->name && strcmp(sym->name, func_name) == 0 &&
                    sym->kind == SYM_GLOBAL_FUNC && sym->type) {
                    const char* ret_str = type_to_string(sym->type);
                    // 用户函数的参数信息有限，使用函数类型信息
                    char param_text[256] = "";
                    int pcount = sym->type ? sym->type->param_count : 0;
                    if (pcount > 0 && sym->type->param_types) {
                        int off = 0;
                        for (int j = 0; j < pcount; j++) {
                            const char* pt = type_to_string(sym->type->param_types[j]);
                            if (j > 0) off += snprintf(param_text + off, sizeof(param_text) - off, ", ");
                            off += snprintf(param_text + off, sizeof(param_text) - off, "%s arg%d", pt, j + 1);
                        }
                    }
                    sig_response = build_user_func_signature(func_name, param_text,
                                                              pcount, ret_str, active_param);
                    break;
                }
            }
        }
        compiler_context_cleanup(&ctx);
    }

    // 4. 模块导入函数
    if (!sig_response && strchr(func_name, '.')) {
        char* dot = strchr(func_name, '.');
        char mod_alias[128] = {0};
        int mod_len = dot - func_name;
        if (mod_len > 0 && mod_len < 127) {
            memcpy(mod_alias, func_name, mod_len);
            mod_alias[mod_len] = '\0';
            const char* meth = dot + 1;

            // 解析导入
            int imp_count = 0;
            ImportAlias* imports = parse_imports(doc->content, &imp_count);
            const char* mod_path = find_module_path_by_alias(imports, imp_count, mod_alias);

            char resolved[MAX_PATH_LEN] = {0};
            if (!mod_path) {
                extern int package_resolve_module_file(const char* mn, char* op, int ol);
                if (package_resolve_module_file(mod_alias, resolved, sizeof(resolved)) == 1) {
                    mod_path = resolved;
                }
            }

            if (mod_path) {
                module_symbol_table_reset_scan_stack();
                ModuleSymbolTable* table = module_symbol_table_create(mod_path);
                if (table && module_symbol_table_scan(table, file_path) == 0) {
                    ModuleFuncSymbol* fn = module_symbol_table_find_func(table, meth);
                    if (fn) {
                        const char* ret_str = fn->return_struct_name ? fn->return_struct_name :
                                              type_kind_to_string(fn->return_type);
                        sig_response = build_user_func_signature(meth, fn->param_text,
                                                                 fn->param_count, ret_str, active_param);
                    }
                }
                if (table) module_symbol_table_destroy(table);
            }
            free_import_aliases(imports, imp_count);
        }
    }

    free(func_name);

    if (sig_response) {
        // sig_response 已经是 JSON 字符串，包装为 LSP 响应
        JsonValue* result = json_parse(sig_response);
        free(sig_response);
        char* response = lsp_create_response(id, result);
        json_free(result);
        return response;
    }

    // 未找到匹配的函数签名，返回空
    JsonValue* result = json_object_new();
    json_object_set(result, "signatures", json_array_new());
    char* response = lsp_create_response(id, result);
    json_free(result);
    return response;
}
