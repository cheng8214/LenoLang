/**
 * 符号补全提供者
 * 
 * - 用户定义符号（从编译器符号表获取）
 * - 原生模块方法（io, types, times, maths 等）
 * - 模块符号（import 的 .leno 文件中的导出）
 * - struct/face/enum 成员
 * - 实例方法（基于类型推断）
 */

#include "lsp_completion.h"
#include "leno_builtins.h"
#include "leno_compiler_lib.h"
#include "../src/include/leno_types.h"
#include "../src/include/native.h"
#include "../src/include/module_symbol_table.h"
#include "../src/include/leno_value.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ========== Import 相关（兼容旧接口） ========== */

extern char* read_module_file(const char* file_path, const char* current_file);

// 声明：定义在 lsp_hover.c 中
extern char* detect_closure_param_type_at_position(const char* content, int cursor_offset, const char* var_name);
extern char* detect_var_type_from_text(const char* content, const char* var_name);

/* ========== 文本匹配变量类型推断（编译器分析失败时的回退） ========== */

// 从源代码文本中推断变量类型
// 搜索 "Type varname" 或 "var varname = value" 模式
// 返回类型名称字符串（如 "string", "Array", "Dict", "number" 等），需调用者 free
static char* detect_variable_type_from_text(const char* content, const char* var_name) {
    if (!content || !var_name) return NULL;
    
    int var_len = strlen(var_name);
    const char* p = content;
    
    while (*p) {
        // 查找变量名的出现位置
        const char* found = strstr(p, var_name);
        if (!found) break;
        
        // 确保是完整的单词（前面和后面不是字母/数字/下划线）
        if ((found == content || (!isalnum((unsigned char)found[-1]) && found[-1] != '_')) &&
            (found[var_len] == '\0' || (!isalnum((unsigned char)found[var_len]) && found[var_len] != '_'))) {
            
            // 检查 found 之前的内容，看是否有类型声明
            const char* before = found - 1;
            
            // 跳过空白
            while (before >= content && isspace((unsigned char)*before)) before--;
            
            // 跳过可空类型后缀 '?'（如 "Font? _font" 中的 '?'）
            // 也处理泛型后缀，如 "Array[int]?" 中的 ']' 后的 '?'
            if (before >= content && *before == '?') {
                before--;
                while (before >= content && isspace((unsigned char)*before)) before--;
            }
            
            // 检查 "Type varname" 模式（如 "string a", "Array tokens", "Dict d"）
            // 也支持 "Type? varname" 模式（如 "Font? _font"）
            if (before >= content && (isalnum((unsigned char)*before) || *before == '_')) {
                // 提取类型名
                const char* type_end = before + 1;
                const char* type_start = before;
                while (type_start > content && (isalnum((unsigned char)type_start[-1]) || type_start[-1] == '_')) {
                    type_start--;
                }
                int type_len = type_end - type_start;
                if (type_len > 0 && type_len < 64) {
                    char type_buf[64];
                    memcpy(type_buf, type_start, type_len);
                    type_buf[type_len] = '\0';
                    
                    // 映射类型名
                    if (strcmp(type_buf, "string") == 0) return strdup("string");
                    if (strcmp(type_buf, "Array") == 0) return strdup("Array");
                    if (strcmp(type_buf, "Dict") == 0) return strdup("Dict");
                    if (strcmp(type_buf, "int") == 0 || strcmp(type_buf, "float") == 0 ||
                        strcmp(type_buf, "bigint") == 0) return strdup("number");
                    if (strcmp(type_buf, "bool") == 0) return strdup("bool");
                    if (strcmp(type_buf, "File") == 0) return strdup("File");
                    if (strcmp(type_buf, "any") == 0) return strdup("any");
                    if (strcmp(type_buf, "Ptr") == 0) return strdup("Ptr");
                    if (strcmp(type_buf, "Socket") == 0) return strdup("Socket");
                    if (strcmp(type_buf, "Channel") == 0) return strdup("Channel");
                    if (strcmp(type_buf, "Thread") == 0) return strdup("Thread");
                    if (strcmp(type_buf, "Future") == 0) return strdup("Future");
                    if (strcmp(type_buf, "i8") == 0 || strcmp(type_buf, "u8") == 0 ||
                        strcmp(type_buf, "i16") == 0 || strcmp(type_buf, "u16") == 0 ||
                        strcmp(type_buf, "i32") == 0 || strcmp(type_buf, "u32") == 0 ||
                        strcmp(type_buf, "i64") == 0 || strcmp(type_buf, "u64") == 0 ||
                        strcmp(type_buf, "f32") == 0 || strcmp(type_buf, "f64") == 0 ||
                        strcmp(type_buf, "c_int") == 0 || strcmp(type_buf, "c_uint") == 0 ||
                        strcmp(type_buf, "c_long") == 0 || strcmp(type_buf, "c_ulong") == 0 ||
                        strcmp(type_buf, "c_longlong") == 0 || strcmp(type_buf, "c_ulonglong") == 0 ||
                        strcmp(type_buf, "c_size") == 0 || strcmp(type_buf, "c_ssize") == 0)
                        return strdup("number");
                    if (strcmp(type_buf, "str8") == 0 || strcmp(type_buf, "str16") == 0) return strdup("string");
                    // struct/cstruct 类型
                    if (strstr(type_buf, "struct")) return strdup(type_buf);
                    // clib 类型（首字母小写的自定义类型名，如 sqlite3）
                    if (!isupper((unsigned char)type_buf[0]) &&
                        strcmp(type_buf, "var") != 0 &&
                        strcmp(type_buf, "void") != 0 &&
                        strcmp(type_buf, "null") != 0 &&
                        strcmp(type_buf, "any") != 0 &&
                        strcmp(type_buf, "func") != 0 &&
                        strcmp(type_buf, "Ptr") != 0 &&
                        strcmp(type_buf, "Socket") != 0 &&
                        strcmp(type_buf, "Channel") != 0 &&
                        strcmp(type_buf, "Thread") != 0 &&
                        strcmp(type_buf, "Future") != 0) {
                        char* result = (char*)malloc(strlen(type_buf) + 6);
                        sprintf(result, "clib %s", type_buf);
                        return result;
                    }
                    // 其他类型名（如 struct 名，首字母大写）
                    if (isupper((unsigned char)type_buf[0])) return strdup(type_buf);
                }
            }
            
            // 检查闭包参数模式 "func(Type varname" 
            // 也支持 "func(Type? varname" 模式
            if (before >= content) {
                const char* check = before;
                // 如果 before 指向 '?'，先跳过它再提取类型名
                if (*before == '?') {
                    check = before - 1;
                    while (check > content && (isalnum((unsigned char)check[-1]) || check[-1] == '_')) check--;
                } else {
                    while (check > content && (isalnum((unsigned char)check[-1]) || check[-1] == '_')) check--;
                }
                if (check > content) {
                    const char* before_type = check - 1;
                    while (before_type >= content && isspace((unsigned char)*before_type)) before_type--;
                    // 跳过可能存在的 '?'
                    if (before_type >= content && *before_type == '?') {
                        before_type--;
                        while (before_type >= content && isspace((unsigned char)*before_type)) before_type--;
                    }
                    if (before_type >= content && *before_type == '(') {
                        int type_len = before - check + 1;
                        if (type_len > 0 && type_len < 64) {
                            char type_buf[64];
                            memcpy(type_buf, check, type_len);
                            type_buf[type_len] = '\0';
                            
                            if (strcmp(type_buf, "string") == 0) return strdup("string");
                            if (strcmp(type_buf, "Array") == 0) return strdup("Array");
                            if (strcmp(type_buf, "Dict") == 0) return strdup("Dict");
                            if (strcmp(type_buf, "int") == 0 || strcmp(type_buf, "float") == 0 ||
                                strcmp(type_buf, "bigint") == 0) return strdup("number");
                            if (strcmp(type_buf, "bool") == 0) return strdup("bool");
                            if (strcmp(type_buf, "File") == 0) return strdup("File");
                            if (strcmp(type_buf, "any") == 0) return strdup("any");
                            if (strcmp(type_buf, "Ptr") == 0) return strdup("Ptr");
                            if (strcmp(type_buf, "Socket") == 0) return strdup("Socket");
                            if (strcmp(type_buf, "Channel") == 0) return strdup("Channel");
                            if (strcmp(type_buf, "Thread") == 0) return strdup("Thread");
                            if (strcmp(type_buf, "Future") == 0) return strdup("Future");
                            if (strcmp(type_buf, "i8") == 0 || strcmp(type_buf, "u8") == 0 ||
                                strcmp(type_buf, "i16") == 0 || strcmp(type_buf, "u16") == 0 ||
                                strcmp(type_buf, "i32") == 0 || strcmp(type_buf, "u32") == 0 ||
                                strcmp(type_buf, "i64") == 0 || strcmp(type_buf, "u64") == 0 ||
                                strcmp(type_buf, "f32") == 0 || strcmp(type_buf, "f64") == 0 ||
                                strcmp(type_buf, "c_int") == 0 || strcmp(type_buf, "c_uint") == 0 ||
                                strcmp(type_buf, "c_long") == 0 || strcmp(type_buf, "c_ulong") == 0 ||
                                strcmp(type_buf, "c_longlong") == 0 || strcmp(type_buf, "c_ulonglong") == 0 ||
                                strcmp(type_buf, "c_size") == 0 || strcmp(type_buf, "c_ssize") == 0)
                                return strdup("number");
                            if (strcmp(type_buf, "str8") == 0 || strcmp(type_buf, "str16") == 0) return strdup("string");
                            if (isupper((unsigned char)type_buf[0])) return strdup(type_buf);
                        }
                    }
                }
            }
            
            // 检查 "var varname = value" 模式
            // 回溯查找 "var " 关键字
            const char* check = found - 1;
            while (check >= content && isspace((unsigned char)*check)) check--;
            // 检查是否是 "var"
            if (check >= content + 2 && 
                check[-2] == 'v' && check[-1] == 'a' && check[0] == 'r' &&
                (check - 3 < content || (!isalnum((unsigned char)check[-3]) && check[-3] != '_'))) {
                // 找到了 "var varname"，查看赋值表达式
                const char* after = found + var_len;
                while (*after && isspace((unsigned char)*after)) after++;
                if (*after == '=') {
                    after++;
                    while (*after && isspace((unsigned char)*after)) after++;
                    // 根据值推断类型
                    if (*after == '"') return strdup("string");
                    if (*after == '[') return strdup("Array");
                    if (*after == '{') return strdup("Dict");
                    if (*after >= '0' && *after <= '9') return strdup("number");
                    if (strncmp(after, "true", 4) == 0 || strncmp(after, "false", 5) == 0) return strdup("bool");
                }
            }
        }
        
        p = found + var_len;
    }
    
    return NULL;
}

/* ========== 原生模块方法 ========== */

void comp_provider_add_native_modules(CompletionSet* set, const char* filter) {
    (void)filter;
    
    int module_count = 0;
    char** modules = native_get_all_modules(&module_count);
    if (!modules || module_count <= 0) return;
    
    for (int m = 0; m < module_count; m++) {
        const char* mod_name = modules[m];
        
        // 获取详细方法元数据
        int method_count = 0;
        ModuleMethodMeta* metas = native_get_module_method_metas(mod_name, &method_count);
        
        if (metas && method_count > 0) {
            for (int i = 0; i < method_count; i++) {
                char full_name[256];
                snprintf(full_name, sizeof(full_name), "%s.%s", mod_name, metas[i].method_name);
                
                // 构建签名 detail
                const char* ret_str = type_kind_to_string(metas[i].return_type);
                char detail[512];
                
                if (metas[i].arity == 0) {
                    snprintf(detail, sizeof(detail), "%s.%s() -> %s", mod_name, metas[i].method_name, ret_str);
                } else if (metas[i].arity < 0) {
                    snprintf(detail, sizeof(detail), "%s.%s(...) -> %s", mod_name, metas[i].method_name, ret_str);
                } else {
                    char params[256] = "";
                    int off = 0;
                    for (int pp = 0; pp < metas[i].arity && pp < MAX_METHOD_PARAMS; pp++) {
                        const char* pt = type_kind_to_string(metas[i].param_types[pp]);
                        if (pp > 0) off += snprintf(params + off, sizeof(params) - off, ", ");
                        off += snprintf(params + off, sizeof(params) - off, "%s", pt);
                    }
                    snprintf(detail, sizeof(detail), "%s.%s(%s) -> %s", mod_name, metas[i].method_name, params, ret_str);
                }
                
                char doc[1024];
                snprintf(doc, sizeof(doc), "```leno\n%s\n```", detail);
                
                comp_set_add(set, full_name, LSP_COMP_METHOD, PRIO_MODULE,
                             detail, doc, NULL, NULL);
            }
            native_free_module_method_metas(metas);
        }
        
        // 模块常量
        int const_count = 0;
        char** consts = native_get_module_consts(mod_name, &const_count);
        if (consts && const_count > 0) {
            for (int i = 0; i < const_count; i++) {
                bool found = false;
                int val = native_find_module_const(mod_name, consts[i], &found);
                char full_name[256];
                snprintf(full_name, sizeof(full_name), "%s.%s", mod_name, consts[i]);
                char detail[256];
                if (found) {
                    snprintf(detail, sizeof(detail), "%s.%s = %d", mod_name, consts[i], val);
                } else {
                    snprintf(detail, sizeof(detail), "%s.%s", mod_name, consts[i]);
                }
                comp_set_add(set, full_name, LSP_COMP_CONSTANT, PRIO_MODULE,
                             detail, NULL, NULL, NULL);
            }
            native_free_module_const_list(consts, const_count);
        }
    }
    
    native_free_module_list(modules, module_count);
}

/* ========== 用户定义符号（编译器符号表） ========== */

void comp_provider_add_user_symbols(CompletionSet* set,
                                     const char* content,
                                     const char* file_path,
                                     const char* filter) {
    (void)filter;
    if (!set || !content) return;
    
    CompilerContext ctx;
    compiler_context_init(&ctx);
    
    if (!compiler_analyze_with_filename(&ctx, content, file_path) && !ctx.root_scope) {
        compiler_context_cleanup(&ctx);
        return;
    }
    
    if (!ctx.root_scope) {
        compiler_context_cleanup(&ctx);
        return;
    }
    
    for (int i = 0; i < ctx.root_scope->sym_cnt; i++) {
        Symbol* sym = ctx.root_scope->syms[i];
        if (!sym || !sym->name) continue;
        
        const char* type_str = type_to_string(sym->type);
        const char* category = "variable";
        int kind = LSP_COMP_VARIABLE;
        
        switch (sym->kind) {
            case SYM_GLOBAL_FUNC:
                category = "function";
                kind = LSP_COMP_FUNCTION;
                break;
            case SYM_TYPE:
            case SYM_STRUCT:
            case SYM_CSTRUCT:
                category = "struct";
                kind = LSP_COMP_STRUCT;
                break;
            case SYM_ENUM:
                category = "enum";
                kind = LSP_COMP_ENUM;
                break;
            case SYM_CLIB:
                category = "clib";
                kind = LSP_COMP_CLASS;
                break;
            case SYM_CFUNC:
                category = "cfunc";
                kind = LSP_COMP_CLASS;
                break;
            default:
                break;
        }
        
        char detail[256];
        snprintf(detail, sizeof(detail), "%s: %s", category, type_str);
        
        comp_set_add(set, sym->name, kind, PRIO_USER_SYM,
                     detail, NULL, NULL, NULL);
    }
    
    compiler_context_cleanup(&ctx);
}

/* ========== 模块符号（import 的 .leno 文件） ========== */

void comp_provider_add_module_symbols(
    CompletionSet* set,
    const char* content,
    const char* file_path,
    const char* module_alias,
    const char* member_prefix,
    int import_count,
    ImportAlias* import_aliases
) {
    (void)content;
    (void)member_prefix;
    
    if (!set || !module_alias) return;
    
    const char* module_path = find_module_path_by_alias(import_aliases, import_count, module_alias);
    if (!module_path) {
        // 尝试枚举补全（文件中定义的 enum）
        // 这里已经在主流程中处理
        return;
    }
    
    // 重置模块扫描栈（防止 LSP 多次请求间残留导致误报循环依赖）
    module_symbol_table_reset_scan_stack();
    
    // 使用轻量级 module_symbol_table 替代完整语义分析
    // 优点：1) 不跟随嵌套 import  2) 自动 .lenosymc 磁盘缓存  3) 快速提取符号
    ModuleSymbolTable* table = module_symbol_table_create(module_path);
    if (!table) return;
    
    int scan_result = module_symbol_table_scan(table, file_path);
    if (scan_result != 0) {
        module_symbol_table_destroy(table);
        return;
    }
    
    // 添加函数
    for (int i = 0; i < table->func_count; i++) {
        ModuleFuncSymbol* func = &table->funcs[i];
        const char* ret_str = type_kind_to_string(func->return_type);
        
        char detail[256];
        snprintf(detail, sizeof(detail), "%s.%s() -> %s", module_alias, func->name, ret_str);
        
        comp_set_add(set, func->name, LSP_COMP_FUNCTION, PRIO_MODULE,
                     detail, NULL, NULL, NULL);
    }
    
    // 添加 struct 名称
    for (int i = 0; i < table->struct_count; i++) {
        ModuleStructSymbol* st = &table->structs[i];
        char detail[256];
        snprintf(detail, sizeof(detail), "%s.%s: %s", module_alias, st->name,
                 st->is_cstruct ? "cstruct" : "struct");
        
        comp_set_add(set, st->name, LSP_COMP_STRUCT, PRIO_MODULE,
                     detail, NULL, NULL, NULL);
    }
    
    // 添加 enum 名称
    for (int i = 0; i < table->enum_count; i++) {
        ModuleEnumSymbol* en = &table->enums[i];
        char detail[256];
        snprintf(detail, sizeof(detail), "%s.%s: enum", module_alias, en->name);
        
        comp_set_add(set, en->name, LSP_COMP_ENUM, PRIO_MODULE,
                     detail, NULL, NULL, NULL);
    }
    
    // 添加 face 名称
    for (int i = 0; i < table->face_count; i++) {
        ModuleFaceSymbol* fc = &table->faces[i];
        char detail[256];
        snprintf(detail, sizeof(detail), "%s.%s: face", module_alias, fc->name);
        
        comp_set_add(set, fc->name, LSP_COMP_INTERFACE, PRIO_MODULE,
                     detail, NULL, NULL, NULL);
    }
    
    // 添加 var（模块常量/全局变量）
    for (int i = 0; i < table->var_count; i++) {
        ModuleVarSymbol* var = &table->vars[i];
        const char* type_str = type_kind_to_string(var->type);
        
        char detail[256];
        snprintf(detail, sizeof(detail), "%s.%s: %s", module_alias, var->name, type_str);
        
        comp_set_add(set, var->name, LSP_COMP_VARIABLE, PRIO_MODULE,
                     detail, NULL, NULL, NULL);
    }
    
    module_symbol_table_destroy(table);
}

/* ========== use 语句补全 ========== */

void comp_provider_add_use_modules(CompletionSet* set,
                                    int import_count,
                                    ImportAlias* import_aliases,
                                    const char* filter) {
    (void)filter;
    if (!set) return;
    
    for (int i = 0; i < import_count; i++) {
        char detail[256];
        snprintf(detail, sizeof(detail), "module %s", import_aliases[i].alias);
        comp_set_add(set, import_aliases[i].alias, LSP_COMP_MODULE, PRIO_MODULE,
                     detail, NULL, NULL, NULL);
    }
}

/* ========== new 关键字后补全 ========== */

void comp_provider_add_new_structs(CompletionSet* set,
                                    const char* content,
                                    const char* file_path,
                                    int import_count,
                                    ImportAlias* import_aliases) {
    if (!set || !content) return;
    
    // 从当前文件的符号表获取所有 struct
    CompilerContext ctx;
    compiler_context_init(&ctx);
    compiler_analyze_with_filename(&ctx, content, file_path);
    
    if (ctx.root_scope) {
        for (int i = 0; i < ctx.root_scope->sym_cnt; i++) {
            Symbol* sym = ctx.root_scope->syms[i];
            if (sym->type && (sym->type->kind == TYPE_STRUCT || sym->type->kind == TYPE_CSTRUCT)) {
                char detail[256];
                snprintf(detail, sizeof(detail), "struct %s", sym->name);
                comp_set_add(set, sym->name, LSP_COMP_STRUCT, PRIO_TYPE,
                             detail, NULL, NULL, NULL);
            }
        }
    }
    compiler_context_cleanup(&ctx);
    
    // 也列出模块别名（用于 new module.StructName()）
    for (int i = 0; i < import_count; i++) {
        char detail[256];
        snprintf(detail, sizeof(detail), "module %s", import_aliases[i].alias);
        comp_set_add(set, import_aliases[i].alias, LSP_COMP_MODULE, PRIO_MODULE,
                     detail, NULL, NULL, NULL);
    }
}

/* ========== 变量类型推断后的成员补全 ========== */

void comp_provider_add_variable_members(
    CompletionSet* set,
    const char* content,
    const char* file_path,
    const char* var_name,
    int import_count,
    ImportAlias* import_aliases,
    LspPosition pos
) {
    if (!set || !content || !var_name) return;

    // 首先尝试基于光标位置的闭包参数类型检测
    // 闭包参数（如 func(Renderer r) 中的 r）在编译器作用域树中可能无法正确查找
    int cursor_offset = lsp_position_to_offset(content, pos);
    char* closure_type = detect_closure_param_type_at_position(content, cursor_offset, var_name);
    if (closure_type) {
        fprintf(stderr, "[COMPLETE-DEBUG] var='%s' closure param type detected='%s'\n", var_name, closure_type);
        fflush(stderr);
        
        // 将闭包参数类型转换为 type_resolved 格式
        char* type_resolved = NULL;
        if (strcmp(closure_type, "string") == 0) {
            type_resolved = strdup("string");
        } else if (strcmp(closure_type, "Array") == 0) {
            type_resolved = strdup("Array");
        } else if (strcmp(closure_type, "Dict") == 0) {
            type_resolved = strdup("Dict");
        } else if (strcmp(closure_type, "number") == 0) {
            type_resolved = strdup("number");
        } else if (strcmp(closure_type, "bool") == 0) {
            type_resolved = strdup("bool");
        } else if (strcmp(closure_type, "File") == 0) {
            type_resolved = strdup("File");
        } else if (strcmp(closure_type, "any") == 0) {
            type_resolved = strdup("any");
        } else if (strcmp(closure_type, "Ptr") == 0) {
            type_resolved = strdup("Ptr");
        } else if (strcmp(closure_type, "Socket") == 0) {
            type_resolved = strdup("Socket");
        } else if (strcmp(closure_type, "Channel") == 0) {
            type_resolved = strdup("Channel");
        } else if (strcmp(closure_type, "Thread") == 0) {
            type_resolved = strdup("Thread");
        } else if (strcmp(closure_type, "Future") == 0) {
            type_resolved = strdup("Future");
        } else if (isupper((unsigned char)closure_type[0])) {
            // struct 类型名
            type_resolved = (char*)malloc(strlen(closure_type) + 16);
            sprintf(type_resolved, "struct %s", closure_type);
        } else {
            // 可能是 clib 类型（首字母小写的自定义类型名，如 sqlite3）
            type_resolved = (char*)malloc(strlen(closure_type) + 6);
            sprintf(type_resolved, "clib %s", closure_type);
        }
        free(closure_type);
        
        if (type_resolved) {
            // 使用 type_resolved 提供成员补全
            // struct 类型：查找字段和方法
            if (strstr(type_resolved, "struct") || strstr(type_resolved, "cstruct")) {
                const char* keyword = strstr(type_resolved, "cstruct") ? "cstruct" : "struct";
                const char* struct_name = strstr(type_resolved, keyword);
                if (struct_name) {
                    struct_name += strlen(keyword);
                    while (*struct_name && isspace((unsigned char)*struct_name)) struct_name++;
                    
                    // 从导入的模块查找 struct 字段和方法
                    for (int i = 0; i < import_count; i++) {
                        const char* mp = find_module_path_by_alias(import_aliases, import_count, import_aliases[i].alias);
                        if (mp) {
                            module_symbol_table_reset_scan_stack();
                            ModuleSymbolTable* mtable = module_symbol_table_create(mp);
                            if (!mtable) continue;
                            if (module_symbol_table_scan(mtable, file_path) == 0) {
                                ModuleStructSymbol* mst = module_symbol_table_find_struct(mtable, struct_name);
                                if (mst) {
                                    for (int j = 0; j < mst->field_count; j++) {
                                        const char* fname = mst->fields[j].name;
                                        const char* fts = mst->fields[j].struct_name ?
                                            mst->fields[j].struct_name : type_kind_to_string(mst->fields[j].type);
                                        char detail[256];
                                        snprintf(detail, sizeof(detail), "%s.%s: %s", var_name, fname, fts);
                                        comp_set_add(set, fname, LSP_COMP_FIELD, PRIO_FIELD,
                                                     detail, NULL, NULL, NULL);
                                    }
                                    for (int j = 0; j < mst->method_count; j++) {
                                        const char* mname = mst->methods[j].name;
                                        const char* sep = strstr(mname, "::");
                                        if (sep) sep += 2; else sep = mname;
                                        char detail[256];
                                        snprintf(detail, sizeof(detail), "%s.%s()", var_name, sep);
                                        comp_set_add(set, sep, LSP_COMP_METHOD, PRIO_METHOD,
                                                     detail, NULL, NULL, NULL);
                                    }
                                }
                            }
                            module_symbol_table_destroy(mtable);
                        }
                    }
                }
            } else if (strncmp(type_resolved, "clib ", 5) == 0) {
                // clib 类型：查找 clib 函数并添加补全项
                const char* clib_name = type_resolved + 5;
                if (*clib_name) {
                    // 从当前文件的编译器作用域中查找
                    CompilerContext cctx;
                    compiler_context_init(&cctx);
                    compiler_analyze_with_filename(&cctx, content, file_path);
                    bool clib_found = false;
                    if (cctx.root_scope) {
                        Symbol* clib_sym = scope_resolve_tree_bfs(cctx.root_scope, clib_name);
                        if (clib_sym && clib_sym->clib_func_count > 0) {
                            clib_found = true;
                            for (int i = 0; i < clib_sym->clib_func_count; i++) {
                                const char* fname = clib_sym->clib_func_names[i];
                                TypeInfo* ret_type = clib_sym->clib_func_return_types[i];
                                const char* rt_str = ret_type ? type_to_string(ret_type) : "unknown";
                                int pc = clib_sym->clib_func_param_counts[i];
                                char detail[512];
                                if (pc == 0) {
                                    snprintf(detail, sizeof(detail), "%s.%s() -> %s", var_name, fname, rt_str);
                                } else {
                                    snprintf(detail, sizeof(detail), "%s.%s(...) -> %s", var_name, fname, rt_str);
                                }
                                comp_set_add(set, fname, LSP_COMP_METHOD, PRIO_METHOD,
                                             detail, NULL, NULL, NULL);
                            }
                        }
                    }
                    compiler_context_cleanup(&cctx);
                    // 从导入的模块符号表中查找
                    if (!clib_found && file_path) {
                        for (int i = 0; i < import_count; i++) {
                            const char* mp = find_module_path_by_alias(import_aliases, import_count, import_aliases[i].alias);
                            if (!mp) continue;
                            module_symbol_table_reset_scan_stack();
                            ModuleSymbolTable* mtable = module_symbol_table_create(mp);
                            if (!mtable) continue;
                            if (module_symbol_table_scan(mtable, file_path) == 0) {
                                ModuleClibSymbol* mclib = module_symbol_table_find_clib(mtable, clib_name);
                                if (mclib) {
                                    for (int j = 0; j < mclib->func_count; j++) {
                                        const char* fname = mclib->funcs[j].name;
                                        const char* rt_str = mclib->funcs[j].return_struct_name ?
                                            mclib->funcs[j].return_struct_name :
                                            type_kind_to_string(mclib->funcs[j].return_type);
                                        int pc = mclib->funcs[j].param_count;
                                        char detail[512];
                                        if (pc == 0) {
                                            snprintf(detail, sizeof(detail), "%s.%s() -> %s", var_name, fname, rt_str);
                                        } else {
                                            snprintf(detail, sizeof(detail), "%s.%s(...) -> %s", var_name, fname, rt_str);
                                        }
                                        comp_set_add(set, fname, LSP_COMP_METHOD, PRIO_METHOD,
                                                     detail, NULL, NULL, NULL);
                                    }
                                }
                            }
                            module_symbol_table_destroy(mtable);
                        }
                    }
                    // 途径3：扫描当前文件本身（纯文本解析，不依赖编译器）
                    if (!clib_found && file_path) {
                        module_symbol_table_reset_scan_stack();
                        ModuleSymbolTable* cur_table = module_symbol_table_create(file_path);
                        if (cur_table) {
                            if (module_symbol_table_scan(cur_table, file_path) == 0) {
                                ModuleClibSymbol* mclib = module_symbol_table_find_clib(cur_table, clib_name);
                                if (mclib) {
                                    clib_found = true;
                                    for (int j = 0; j < mclib->func_count; j++) {
                                        const char* fname = mclib->funcs[j].name;
                                        const char* rt_str = mclib->funcs[j].return_struct_name ?
                                            mclib->funcs[j].return_struct_name :
                                            type_kind_to_string(mclib->funcs[j].return_type);
                                        int pc = mclib->funcs[j].param_count;
                                        char detail[512];
                                        if (pc == 0) {
                                            snprintf(detail, sizeof(detail), "%s.%s() -> %s", var_name, fname, rt_str);
                                        } else {
                                            snprintf(detail, sizeof(detail), "%s.%s(...) -> %s", var_name, fname, rt_str);
                                        }
                                        comp_set_add(set, fname, LSP_COMP_METHOD, PRIO_METHOD,
                                                     detail, NULL, NULL, NULL);
                                    }
                                }
                            }
                            module_symbol_table_destroy(cur_table);
                        }
                    }
                }
            } else {
                // 基本类型：实例方法
                int method_count = 0;
                char** methods = native_get_instance_methods(type_resolved, &method_count);
                if (methods && method_count > 0) {
                    for (int i = 0; i < method_count; i++) {
                        int arity = native_get_instance_method_arity(type_resolved, methods[i]);
                        TypeKind rt = native_get_instance_method_return_type(type_resolved, methods[i], NULL);
                        const char* rt_str = type_kind_to_string(rt);
                        char detail[256];
                        if (arity == 0) {
                            snprintf(detail, sizeof(detail), "%s.%s() -> %s", var_name, methods[i], rt_str);
                        } else if (arity < 0) {
                            snprintf(detail, sizeof(detail), "%s.%s(...) -> %s", var_name, methods[i], rt_str);
                        } else {
                            snprintf(detail, sizeof(detail), "%s.%s(...) -> %s", var_name, methods[i], rt_str);
                        }
                        comp_set_add(set, methods[i], LSP_COMP_METHOD, PRIO_METHOD,
                                     detail, NULL, NULL, NULL);
                    }
                    native_free_instance_method_list(methods, method_count);
                }
            }
            free(type_resolved);
            return;  // 闭包参数类型已处理，不需要继续
        }
    }

    // 获取变量类型
    CompilerContext ctx;
    compiler_context_init(&ctx);
    compiler_analyze_with_filename(&ctx, content, file_path);
    
    char* type_str = NULL;
    if (ctx.root_scope) {
        compiler_get_symbol_info(&ctx, var_name, &type_str, NULL);
    }

    // 回退1：如果编译器分析失败或未找到符号，检查是否是 struct 字段
    // struct 方法体中可以直接使用字段名（如 _font），编译器会将其转换为 self.field
    // 但 LSP 的作用域查找可能找不到这些字段，需要通过 struct 定义回退查找
    if (!type_str && ctx.root_scope) {
        char** struct_names = NULL;
        int struct_count = compiler_find_structs_with_field(&ctx, var_name, &struct_names);
        if (struct_count > 0) {
            char* field_type_str = NULL;
            bool found = compiler_get_struct_field_info(&ctx, struct_names[0], var_name, &field_type_str);
            if (found && field_type_str) {
                type_str = field_type_str;
            }
            // 释放 struct 名称列表
            for (int i = 0; i < struct_count; i++) {
                free(struct_names[i]);
            }
            free(struct_names);
        }
    }
    compiler_context_cleanup(&ctx);

    // 回退2：如果仍然未找到，使用文本匹配推断类型
    if (!type_str) {
        type_str = detect_variable_type_from_text(content, var_name);
    }
    
    char* type_resolved = NULL;

    // 剥离可空类型后缀 '?'（如 "Font?" -> "Font"）
    // 补全逻辑基于基础类型查找成员，可空性不影响成员列表
    if (type_str) {
        size_t ts_len = strlen(type_str);
        while (ts_len > 0 && type_str[ts_len - 1] == '?') {
            type_str[ts_len - 1] = '\0';
            ts_len--;
        }
        // 如果剥离后为空，释放并置空
        if (ts_len == 0) {
            free(type_str);
            type_str = NULL;
        }
    }
    
    if (type_str) {
        if (strstr(type_str, "struct") || strstr(type_str, "cstruct")) {
            type_resolved = strdup(type_str);
        } else if (strstr(type_str, "face")) {
            type_resolved = strdup(type_str);
        } else if (strcmp(type_str, "int") == 0 || strcmp(type_str, "float") == 0 || strcmp(type_str, "bigint") == 0) {
            type_resolved = strdup("number");
        } else if (strcmp(type_str, "string") == 0) {
            type_resolved = strdup("string");
        } else if (strncmp(type_str, "Array", 5) == 0) {
            type_resolved = strdup("Array");
        } else if (strncmp(type_str, "Dict", 4) == 0) {
            type_resolved = strdup("Dict");
        } else if (strcmp(type_str, "bool") == 0) {
            type_resolved = strdup("bool");
        } else if (strcmp(type_str, "File") == 0) {
            type_resolved = strdup("File");
        } else if (strcmp(type_str, "any") == 0) {
            type_resolved = strdup("any");
        } else if (strcmp(type_str, "Ptr") == 0) {
            type_resolved = strdup("Ptr");
        } else if (strcmp(type_str, "Socket") == 0) {
            type_resolved = strdup("Socket");
        } else if (strcmp(type_str, "Channel") == 0) {
            type_resolved = strdup("Channel");
        } else if (strcmp(type_str, "Thread") == 0) {
            type_resolved = strdup("Thread");
        } else if (strcmp(type_str, "Future") == 0) {
            type_resolved = strdup("Future");
        } else if (strcmp(type_str, "i8") == 0 || strcmp(type_str, "u8") == 0 ||
                   strcmp(type_str, "i16") == 0 || strcmp(type_str, "u16") == 0 ||
                   strcmp(type_str, "i32") == 0 || strcmp(type_str, "u32") == 0 ||
                   strcmp(type_str, "i64") == 0 || strcmp(type_str, "u64") == 0 ||
                   strcmp(type_str, "f32") == 0 || strcmp(type_str, "f64") == 0 ||
                   strcmp(type_str, "c_int") == 0 || strcmp(type_str, "c_uint") == 0 ||
                   strcmp(type_str, "c_long") == 0 || strcmp(type_str, "c_ulong") == 0 ||
                   strcmp(type_str, "c_longlong") == 0 || strcmp(type_str, "c_ulonglong") == 0 ||
                   strcmp(type_str, "c_size") == 0 || strcmp(type_str, "c_ssize") == 0) {
            type_resolved = strdup("number");
        } else if (strcmp(type_str, "str8") == 0 || strcmp(type_str, "str16") == 0) {
            type_resolved = strdup("string");
        } else if (isupper((unsigned char)type_str[0])) {
            // 可能是用户定义的 struct 类型名（如 MenuBar, TreeView, Window）
            // 注意：不要求 struct_def_find 成功，因为 struct 可能定义在导入的模块中
            // 后续会通过模块符号表查找字段和方法
            type_resolved = (char*)malloc(strlen(type_str) + 16);
            sprintf(type_resolved, "struct %s", type_str);
        } else if (strncmp(type_str, "clib ", 5) == 0) {
            // clib 类型（如 "clib sqlite3"）
            type_resolved = strdup(type_str);
        }
        free(type_str);
    }
    
    if (!type_resolved) return;
    
    // struct 类型：查找字段和方法
    if (strstr(type_resolved, "struct") || strstr(type_resolved, "cstruct")) {
        const char* keyword = strstr(type_resolved, "cstruct") ? "cstruct" : "struct";
        const char* struct_name = strstr(type_resolved, keyword);
        if (struct_name) {
            struct_name += strlen(keyword);
            while (*struct_name && isspace((unsigned char)*struct_name)) struct_name++;
            
            // 从当前文件查找字段
            CompilerContext sctx;
            compiler_context_init(&sctx);
            compiler_analyze_with_filename(&sctx, content, file_path);
            
            if (sctx.root_scope) {
                Symbol* struct_sym = scope_resolve_tree_bfs(sctx.root_scope, struct_name);
                if (struct_sym && (struct_sym->type->kind == TYPE_STRUCT || struct_sym->type->kind == TYPE_CSTRUCT)) {
                    for (int i = 0; i < struct_sym->struct_field_count; i++) {
                        const char* field_name = struct_sym->struct_field_names[i];
                        TypeInfo* field_type = struct_sym->struct_field_types[i];
                        const char* ftype_str = type_to_string(field_type);
                        
                        char detail[256];
                        snprintf(detail, sizeof(detail), "%s.%s: %s", var_name, field_name, ftype_str);
                        comp_set_add(set, field_name, LSP_COMP_FIELD, PRIO_FIELD,
                                     detail, NULL, NULL, NULL);
                    }
                    
                    // 添加方法
                    if (struct_sym->type->struct_name) {
                        ObjStructDef* sdef = struct_def_find(struct_sym->type->struct_name);
                        if (sdef) {
                            for (int m = 0; m < sdef->method_count; m++) {
                                char detail[256];
                                snprintf(detail, sizeof(detail), "%s.%s()", var_name, sdef->methods[m].name);
                                comp_set_add(set, sdef->methods[m].name, LSP_COMP_METHOD, PRIO_METHOD,
                                             detail, NULL, NULL, NULL);
                            }
                            // 添加关联常量
                            for (int c = 0; c < sdef->const_count; c++) {
                                char detail[256];
                                snprintf(detail, sizeof(detail), "%s.%s", var_name, sdef->const_names[c]);
                                comp_set_add(set, sdef->const_names[c], LSP_COMP_FIELD, PRIO_FIELD,
                                             detail, NULL, NULL, NULL);
                            }
                        }
                    }
                }
            }
            compiler_context_cleanup(&sctx);
            
            // 也尝试从导入的模块查找 struct 字段（使用轻量级符号表，不触发完整编译）
            for (int i = 0; i < import_count; i++) {
                const char* mp = find_module_path_by_alias(import_aliases, import_count, import_aliases[i].alias);
                if (mp) {
                    module_symbol_table_reset_scan_stack();
                    ModuleSymbolTable* mtable = module_symbol_table_create(mp);
                    if (!mtable) continue;
                    if (module_symbol_table_scan(mtable, file_path) == 0) {
                        ModuleStructSymbol* mst = module_symbol_table_find_struct(mtable, struct_name);
                        if (mst) {
                            for (int j = 0; j < mst->field_count; j++) {
                                const char* fname = mst->fields[j].name;
                                const char* fts = mst->fields[j].struct_name ?
                                    mst->fields[j].struct_name : type_kind_to_string(mst->fields[j].type);
                                char detail[256];
                                snprintf(detail, sizeof(detail), "%s.%s: %s", var_name, fname, fts);
                                comp_set_add(set, fname, LSP_COMP_FIELD, PRIO_FIELD,
                                             detail, NULL, NULL, NULL);
                            }
                            // 也添加方法
                            for (int j = 0; j < mst->method_count; j++) {
                                const char* mname = mst->methods[j].name;
                                // 方法名可能带 "StructName::" 前缀（如 "Event::isQuit"），去除前缀
                                const char* sep = strstr(mname, "::");
                                if (sep) sep += 2; else sep = mname;
                                char detail[256];
                                snprintf(detail, sizeof(detail), "%s.%s()", var_name, sep);
                                comp_set_add(set, sep, LSP_COMP_METHOD, PRIO_METHOD,
                                             detail, NULL, NULL, NULL);
                            }
                            // 添加关联常量
                            for (int j = 0; j < mst->const_count; j++) {
                                char detail[256];
                                snprintf(detail, sizeof(detail), "%s.%s", var_name, mst->const_names[j]);
                                comp_set_add(set, mst->const_names[j], LSP_COMP_FIELD, PRIO_FIELD,
                                             detail, NULL, NULL, NULL);
                            }
                        }
                    }
                    module_symbol_table_destroy(mtable);
                }
            }
        }
    } else if (strncmp(type_resolved, "clib ", 5) == 0) {
        // clib 类型：列出所有 clib 函数作为补全项
        const char* clib_name = type_resolved + 5;
        if (*clib_name) {
            // 途径1：从当前文件的编译器作用域中查找 clib 定义
            CompilerContext cctx;
            compiler_context_init(&cctx);
            compiler_analyze_with_filename(&cctx, content, file_path);
            bool found = false;
            if (cctx.root_scope) {
                Symbol* clib_sym = scope_resolve_tree_bfs(cctx.root_scope, clib_name);
                if (clib_sym && clib_sym->clib_func_count > 0) {
                    found = true;
                    for (int i = 0; i < clib_sym->clib_func_count; i++) {
                        const char* fname = clib_sym->clib_func_names[i];
                        TypeInfo* ret_type = clib_sym->clib_func_return_types[i];
                        const char* rt_str = ret_type ? type_to_string(ret_type) : "unknown";
                        int pc = clib_sym->clib_func_param_counts[i];
                        char detail[512];
                        if (pc == 0) {
                            snprintf(detail, sizeof(detail), "%s.%s() -> %s", var_name, fname, rt_str);
                        } else {
                            char params[256] = "";
                            int off = 0;
                            for (int j = 0; j < pc && off < (int)sizeof(params) - 20; j++) {
                                TypeInfo* pt = clib_sym->clib_func_param_types[i][j];
                                const char* pt_str = pt ? type_to_string(pt) : "unknown";
                                if (j > 0) off += snprintf(params + off, sizeof(params) - off, ", ");
                                off += snprintf(params + off, sizeof(params) - off, "%s", pt_str);
                            }
                            snprintf(detail, sizeof(detail), "%s.%s(%s) -> %s", var_name, fname, params, rt_str);
                        }
                        char doc[1024];
                        snprintf(doc, sizeof(doc), "```leno\n%s\n```", detail);
                        comp_set_add(set, fname, LSP_COMP_METHOD, PRIO_METHOD,
                                     detail, doc, NULL, NULL);
                    }
                }
            }
            compiler_context_cleanup(&cctx);

            // 途径2：从导入的模块符号表中查找
            if (!found && file_path) {
                for (int i = 0; i < import_count; i++) {
                    const char* mp = find_module_path_by_alias(import_aliases, import_count, import_aliases[i].alias);
                    if (!mp) continue;
                    module_symbol_table_reset_scan_stack();
                    ModuleSymbolTable* mtable = module_symbol_table_create(mp);
                    if (!mtable) continue;
                    if (module_symbol_table_scan(mtable, file_path) == 0) {
                        ModuleClibSymbol* mclib = module_symbol_table_find_clib(mtable, clib_name);
                        if (mclib) {
                            for (int j = 0; j < mclib->func_count; j++) {
                                const char* fname = mclib->funcs[j].name;
                                const char* rt_str = mclib->funcs[j].return_struct_name ?
                                    mclib->funcs[j].return_struct_name :
                                    type_kind_to_string(mclib->funcs[j].return_type);
                                int pc = mclib->funcs[j].param_count;
                                char detail[512];
                                if (pc == 0) {
                                    snprintf(detail, sizeof(detail), "%s.%s() -> %s", var_name, fname, rt_str);
                                } else {
                                    char params[256] = "";
                                    int off = 0;
                                    for (int k = 0; k < pc && off < (int)sizeof(params) - 20; k++) {
                                        const char* pt_str = type_kind_to_string(mclib->funcs[j].param_types[k]);
                                        if (k > 0) off += snprintf(params + off, sizeof(params) - off, ", ");
                                        off += snprintf(params + off, sizeof(params) - off, "%s", pt_str);
                                    }
                                    snprintf(detail, sizeof(detail), "%s.%s(%s) -> %s", var_name, fname, params, rt_str);
                                }
                                char doc[1024];
                                snprintf(doc, sizeof(doc), "```leno\n%s\n```", detail);
                                comp_set_add(set, fname, LSP_COMP_METHOD, PRIO_METHOD,
                                             detail, doc, NULL, NULL);
                            }
                        }
                    }
                    module_symbol_table_destroy(mtable);
                }
            }
            // 途径3：当编译器解析失败时，使用模块符号表扫描当前文件本身
            // 模块符号表的扫描是纯文本解析，不依赖编译器
            if (!found && file_path) {
                module_symbol_table_reset_scan_stack();
                ModuleSymbolTable* cur_table = module_symbol_table_create(file_path);
                if (cur_table) {
                    if (module_symbol_table_scan(cur_table, file_path) == 0) {
                        ModuleClibSymbol* mclib = module_symbol_table_find_clib(cur_table, clib_name);
                        if (mclib) {
                            found = true;
                            for (int j = 0; j < mclib->func_count; j++) {
                                const char* fname = mclib->funcs[j].name;
                                const char* rt_str = mclib->funcs[j].return_struct_name ?
                                    mclib->funcs[j].return_struct_name :
                                    type_kind_to_string(mclib->funcs[j].return_type);
                                int pc = mclib->funcs[j].param_count;
                                char detail[512];
                                if (pc == 0) {
                                    snprintf(detail, sizeof(detail), "%s.%s() -> %s", var_name, fname, rt_str);
                                } else {
                                    char params[256] = "";
                                    int off = 0;
                                    for (int k = 0; k < pc && off < (int)sizeof(params) - 20; k++) {
                                        const char* pt_str = type_kind_to_string(mclib->funcs[j].param_types[k]);
                                        if (k > 0) off += snprintf(params + off, sizeof(params) - off, ", ");
                                        off += snprintf(params + off, sizeof(params) - off, "%s", pt_str);
                                    }
                                    snprintf(detail, sizeof(detail), "%s.%s(%s) -> %s", var_name, fname, params, rt_str);
                                }
                                char doc[1024];
                                snprintf(doc, sizeof(doc), "```leno\n%s\n```", detail);
                                comp_set_add(set, fname, LSP_COMP_METHOD, PRIO_METHOD,
                                             detail, doc, NULL, NULL);
                            }
                        }
                    }
                    module_symbol_table_destroy(cur_table);
                }
            }
        }
    } else {
        // 基本类型：实例方法
        int method_count = 0;
        char** methods = native_get_instance_methods(type_resolved, &method_count);
        
        if (methods && method_count > 0) {
            for (int i = 0; i < method_count; i++) {
                int arity = native_get_instance_method_arity(type_resolved, methods[i]);
                TypeKind rt = native_get_instance_method_return_type(type_resolved, methods[i], NULL);
                const char* rt_str = type_kind_to_string(rt);
                
                char detail[256];
                if (arity == 0) {
                    snprintf(detail, sizeof(detail), "%s.%s() -> %s", var_name, methods[i], rt_str);
                } else if (arity < 0) {
                    snprintf(detail, sizeof(detail), "%s.%s(...) -> %s", var_name, methods[i], rt_str);
                } else {
                    snprintf(detail, sizeof(detail), "%s.%s(...) -> %s", var_name, methods[i], rt_str);
                }
                
                comp_set_add(set, methods[i], LSP_COMP_METHOD, PRIO_METHOD,
                             detail, NULL, NULL, NULL);
            }
            native_free_instance_method_list(methods, method_count);
        }
    }
    
    free(type_resolved);
}

/* ========== 函数调用链成员补全 ========== */

void comp_provider_add_func_call_chain_members(
    CompletionSet* set,
    const char* content,
    const char* file_path,
    const char* func_name,
    int import_count,
    ImportAlias* import_aliases
) {
    if (!set || !content || !func_name) return;

    // 通过编译器解析函数返回类型
    CompilerContext ctx;
    compiler_context_init(&ctx);
    compiler_analyze_with_filename(&ctx, content, file_path);

    char* type_resolved = NULL;

    if (ctx.root_scope) {
        Symbol* func_sym = scope_resolve_tree_bfs(ctx.root_scope, func_name);
        if (func_sym && func_sym->type && func_sym->type->kind == TYPE_FUNCTION &&
            func_sym->type->return_type) {
            TypeInfo* ret_type = func_sym->type->return_type;

            if (ret_type->kind == TYPE_CLIB && ret_type->struct_name) {
                // clib 返回类型
                type_resolved = (char*)malloc(strlen(ret_type->struct_name) + 6);
                sprintf(type_resolved, "clib %s", ret_type->struct_name);
            } else if (ret_type->kind == TYPE_STRUCT && ret_type->struct_name) {
                // struct 返回类型
                type_resolved = (char*)malloc(strlen(ret_type->struct_name) + 16);
                sprintf(type_resolved, "struct %s", ret_type->struct_name);
            } else if (ret_type->kind == TYPE_CSTRUCT && ret_type->struct_name) {
                // cstruct 返回类型
                type_resolved = (char*)malloc(strlen(ret_type->struct_name) + 16);
                sprintf(type_resolved, "cstruct %s", ret_type->struct_name);
            } else {
                // 其他返回类型
                const char* ts = type_to_string(ret_type);
                if (ts) {
                    type_resolved = strdup(ts);
                }
            }
        }
    }
    compiler_context_cleanup(&ctx);

    // 如果编译器途径失败，尝试从模块符号表查找函数返回类型
    if (!type_resolved && file_path) {
        for (int i = 0; i < import_count; i++) {
            const char* mp = find_module_path_by_alias(import_aliases, import_count, import_aliases[i].alias);
            if (!mp) continue;
            module_symbol_table_reset_scan_stack();
            ModuleSymbolTable* mtable = module_symbol_table_create(mp);
            if (!mtable) continue;
            if (module_symbol_table_scan(mtable, file_path) == 0) {
                for (int j = 0; j < mtable->func_count; j++) {
                    if (strcmp(mtable->funcs[j].name, func_name) == 0) {
                        if (mtable->funcs[j].return_struct_name) {
                            // 尝试判断是 clib 还是 struct
                            // 先在所有模块中查找同名的 clib
                            for (int k = 0; k < import_count; k++) {
                                const char* mp2 = find_module_path_by_alias(import_aliases, import_count, import_aliases[k].alias);
                                if (!mp2) continue;
                                module_symbol_table_reset_scan_stack();
                                ModuleSymbolTable* mtable2 = module_symbol_table_create(mp2);
                                if (!mtable2) continue;
                                if (module_symbol_table_scan(mtable2, file_path) == 0) {
                                    ModuleClibSymbol* mclib = module_symbol_table_find_clib(mtable2, mtable->funcs[j].return_struct_name);
                                    if (mclib) {
                                        type_resolved = (char*)malloc(strlen(mtable->funcs[j].return_struct_name) + 6);
                                        sprintf(type_resolved, "clib %s", mtable->funcs[j].return_struct_name);
                                        module_symbol_table_destroy(mtable2);
                                        break;
                                    }
                                }
                                module_symbol_table_destroy(mtable2);
                            }
                            if (!type_resolved) {
                                type_resolved = (char*)malloc(strlen(mtable->funcs[j].return_struct_name) + 16);
                                sprintf(type_resolved, "struct %s", mtable->funcs[j].return_struct_name);
                            }
                        }
                        break;
                    }
                }
            }
            module_symbol_table_destroy(mtable);
            if (type_resolved) break;
        }
        // 也扫描当前文件本身
        if (!type_resolved) {
            module_symbol_table_reset_scan_stack();
            ModuleSymbolTable* cur_table = module_symbol_table_create(file_path);
            if (cur_table) {
                if (module_symbol_table_scan(cur_table, file_path) == 0) {
                    for (int j = 0; j < cur_table->func_count; j++) {
                        if (strcmp(cur_table->funcs[j].name, func_name) == 0) {
                            if (cur_table->funcs[j].return_struct_name) {
                                // 检查是否是 clib
                                ModuleClibSymbol* mclib = module_symbol_table_find_clib(cur_table, cur_table->funcs[j].return_struct_name);
                                if (mclib) {
                                    type_resolved = (char*)malloc(strlen(cur_table->funcs[j].return_struct_name) + 6);
                                    sprintf(type_resolved, "clib %s", cur_table->funcs[j].return_struct_name);
                                } else {
                                    type_resolved = (char*)malloc(strlen(cur_table->funcs[j].return_struct_name) + 16);
                                    sprintf(type_resolved, "struct %s", cur_table->funcs[j].return_struct_name);
                                }
                            }
                            break;
                        }
                    }
                }
                module_symbol_table_destroy(cur_table);
            }
        }
    }

    if (!type_resolved) return;

    // 根据返回类型添加补全项
    if (strncmp(type_resolved, "clib ", 5) == 0) {
        // clib 类型补全（复用 comp_provider_add_variable_members 的 clib 逻辑）
        const char* clib_name = type_resolved + 5;
        if (*clib_name) {
            // 途径1：当前文件编译器作用域
            CompilerContext cctx;
            compiler_context_init(&cctx);
            compiler_analyze_with_filename(&cctx, content, file_path);
            bool found = false;
            if (cctx.root_scope) {
                Symbol* clib_sym = scope_resolve_tree_bfs(cctx.root_scope, clib_name);
                if (clib_sym && clib_sym->clib_func_count > 0) {
                    found = true;
                    for (int i = 0; i < clib_sym->clib_func_count; i++) {
                        const char* fname = clib_sym->clib_func_names[i];
                        TypeInfo* ret_type = clib_sym->clib_func_return_types[i];
                        const char* rt_str = ret_type ? type_to_string(ret_type) : "unknown";
                        int pc = clib_sym->clib_func_param_counts[i];
                        char detail[512];
                        if (pc == 0) {
                            snprintf(detail, sizeof(detail), "%s.%s() -> %s", func_name, fname, rt_str);
                        } else {
                            snprintf(detail, sizeof(detail), "%s.%s(...) -> %s", func_name, fname, rt_str);
                        }
                        comp_set_add(set, fname, LSP_COMP_METHOD, PRIO_METHOD,
                                     detail, NULL, NULL, NULL);
                    }
                }
            }
            compiler_context_cleanup(&cctx);

            // 途径2：导入的模块
            if (!found && file_path) {
                for (int i = 0; i < import_count; i++) {
                    const char* mp = find_module_path_by_alias(import_aliases, import_count, import_aliases[i].alias);
                    if (!mp) continue;
                    module_symbol_table_reset_scan_stack();
                    ModuleSymbolTable* mtable = module_symbol_table_create(mp);
                    if (!mtable) continue;
                    if (module_symbol_table_scan(mtable, file_path) == 0) {
                        ModuleClibSymbol* mclib = module_symbol_table_find_clib(mtable, clib_name);
                        if (mclib) {
                            for (int j = 0; j < mclib->func_count; j++) {
                                const char* fname = mclib->funcs[j].name;
                                const char* rt_str = mclib->funcs[j].return_struct_name ?
                                    mclib->funcs[j].return_struct_name :
                                    type_kind_to_string(mclib->funcs[j].return_type);
                                int pc = mclib->funcs[j].param_count;
                                char detail[512];
                                if (pc == 0) {
                                    snprintf(detail, sizeof(detail), "%s.%s() -> %s", func_name, fname, rt_str);
                                } else {
                                    snprintf(detail, sizeof(detail), "%s.%s(...) -> %s", func_name, fname, rt_str);
                                }
                                comp_set_add(set, fname, LSP_COMP_METHOD, PRIO_METHOD,
                                             detail, NULL, NULL, NULL);
                            }
                        }
                    }
                    module_symbol_table_destroy(mtable);
                }
            }
            // 途径3：扫描当前文件
            if (!found && file_path) {
                module_symbol_table_reset_scan_stack();
                ModuleSymbolTable* cur_table = module_symbol_table_create(file_path);
                if (cur_table) {
                    if (module_symbol_table_scan(cur_table, file_path) == 0) {
                        ModuleClibSymbol* mclib = module_symbol_table_find_clib(cur_table, clib_name);
                        if (mclib) {
                            for (int j = 0; j < mclib->func_count; j++) {
                                const char* fname = mclib->funcs[j].name;
                                const char* rt_str = mclib->funcs[j].return_struct_name ?
                                    mclib->funcs[j].return_struct_name :
                                    type_kind_to_string(mclib->funcs[j].return_type);
                                int pc = mclib->funcs[j].param_count;
                                char detail[512];
                                if (pc == 0) {
                                    snprintf(detail, sizeof(detail), "%s.%s() -> %s", func_name, fname, rt_str);
                                } else {
                                    snprintf(detail, sizeof(detail), "%s.%s(...) -> %s", func_name, fname, rt_str);
                                }
                                comp_set_add(set, fname, LSP_COMP_METHOD, PRIO_METHOD,
                                             detail, NULL, NULL, NULL);
                            }
                        }
                    }
                    module_symbol_table_destroy(cur_table);
                }
            }
        }
    } else if (strstr(type_resolved, "struct") || strstr(type_resolved, "cstruct")) {
        // struct/cstruct 类型：查找字段和方法
        const char* keyword = strstr(type_resolved, "cstruct") ? "cstruct" : "struct";
        const char* struct_name = strstr(type_resolved, keyword);
        if (struct_name) {
            struct_name += strlen(keyword);
            while (*struct_name && isspace((unsigned char)*struct_name)) struct_name++;

            // 从当前文件查找
            CompilerContext sctx;
            compiler_context_init(&sctx);
            compiler_analyze_with_filename(&sctx, content, file_path);
            if (sctx.root_scope) {
                Symbol* struct_sym = scope_resolve_tree_bfs(sctx.root_scope, struct_name);
                if (struct_sym && (struct_sym->type->kind == TYPE_STRUCT || struct_sym->type->kind == TYPE_CSTRUCT)) {
                    for (int i = 0; i < struct_sym->struct_field_count; i++) {
                        const char* field_name = struct_sym->struct_field_names[i];
                        const char* ftype_str = type_to_string(struct_sym->struct_field_types[i]);
                        char detail[256];
                        snprintf(detail, sizeof(detail), "%s.%s: %s", func_name, field_name, ftype_str);
                        comp_set_add(set, field_name, LSP_COMP_FIELD, PRIO_FIELD,
                                     detail, NULL, NULL, NULL);
                    }
                }
            }
            compiler_context_cleanup(&sctx);

            // 从导入的模块查找
            for (int i = 0; i < import_count; i++) {
                const char* mp = find_module_path_by_alias(import_aliases, import_count, import_aliases[i].alias);
                if (!mp) continue;
                module_symbol_table_reset_scan_stack();
                ModuleSymbolTable* mtable = module_symbol_table_create(mp);
                if (!mtable) continue;
                if (module_symbol_table_scan(mtable, file_path) == 0) {
                    ModuleStructSymbol* mst = module_symbol_table_find_struct(mtable, struct_name);
                    if (mst) {
                        for (int j = 0; j < mst->field_count; j++) {
                            const char* fname = mst->fields[j].name;
                            const char* fts = mst->fields[j].struct_name ?
                                mst->fields[j].struct_name : type_kind_to_string(mst->fields[j].type);
                            char detail[256];
                            snprintf(detail, sizeof(detail), "%s.%s: %s", func_name, fname, fts);
                            comp_set_add(set, fname, LSP_COMP_FIELD, PRIO_FIELD,
                                         detail, NULL, NULL, NULL);
                        }
                        for (int j = 0; j < mst->method_count; j++) {
                            const char* mname = mst->methods[j].name;
                            const char* sep = strstr(mname, "::");
                            if (sep) sep += 2; else sep = mname;
                            char detail[256];
                            snprintf(detail, sizeof(detail), "%s.%s()", func_name, sep);
                            comp_set_add(set, sep, LSP_COMP_METHOD, PRIO_METHOD,
                                         detail, NULL, NULL, NULL);
                        }
                    }
                }
                module_symbol_table_destroy(mtable);
            }
        }
    } else {
        // 基本类型：实例方法
        int method_count = 0;
        char** methods = native_get_instance_methods(type_resolved, &method_count);
        if (methods && method_count > 0) {
            for (int i = 0; i < method_count; i++) {
                int arity = native_get_instance_method_arity(type_resolved, methods[i]);
                TypeKind rt = native_get_instance_method_return_type(type_resolved, methods[i], NULL);
                const char* rt_str = type_kind_to_string(rt);
                char detail[256];
                if (arity == 0) {
                    snprintf(detail, sizeof(detail), "%s.%s() -> %s", func_name, methods[i], rt_str);
                } else {
                    snprintf(detail, sizeof(detail), "%s.%s(...) -> %s", func_name, methods[i], rt_str);
                }
                comp_set_add(set, methods[i], LSP_COMP_METHOD, PRIO_METHOD,
                             detail, NULL, NULL, NULL);
            }
            native_free_instance_method_list(methods, method_count);
        }
    }

    free(type_resolved);
}

/* ========== 字符串字面量方法 ========== */

void comp_provider_add_string_methods(CompletionSet* set) {
    if (!set) return;
    
    const char* display = "\"\"";
    int method_count = 0;
    char** methods = native_get_instance_methods("string", &method_count);
    
    if (methods && method_count > 0) {
        for (int i = 0; i < method_count; i++) {
            int arity = native_get_instance_method_arity("string", methods[i]);
            TypeKind rt = native_get_instance_method_return_type("string", methods[i], NULL);
            const char* rt_str = type_kind_to_string(rt);
            
            char detail[256];
            if (arity == 0) {
                snprintf(detail, sizeof(detail), "%s.%s() -> %s", display, methods[i], rt_str);
            } else {
                snprintf(detail, sizeof(detail), "%s.%s(...) -> %s", display, methods[i], rt_str);
            }
            
            comp_set_add(set, methods[i], LSP_COMP_METHOD, PRIO_METHOD,
                         detail, NULL, NULL, NULL);
        }
        native_free_instance_method_list(methods, method_count);
    }
}

/* ========== 类型注解上下文补全 ========== */

void comp_provider_add_type_annotation_types(
    CompletionSet* set,
    const char* content,
    const char* file_path,
    int import_count,
    ImportAlias* import_aliases
) {
    if (!set) return;
    
    // 1. 内置类型
    comp_provider_add_types(set, NULL);
    
    // 2. 添加 var（类型推断）
    comp_set_add(set, "var", LSP_COMP_KEYWORD, PRIO_TYPE,
                 "type inference", NULL, NULL, NULL);
    
    // 3. 编译器符号表中的类型
    if (content) {
        CompilerContext ctx;
        compiler_context_init(&ctx);
        compiler_analyze_with_filename(&ctx, content, file_path);
        
        if (ctx.root_scope) {
            for (int i = 0; i < ctx.root_scope->sym_cnt; i++) {
                Symbol* sym = ctx.root_scope->syms[i];
                if (sym->kind == SYM_TYPE || sym->kind == SYM_STRUCT ||
                    sym->kind == SYM_CSTRUCT || sym->kind == SYM_ENUM) {
                    const char* type_label = "type";
                    int comp_kind = LSP_COMP_CLASS;
                    if (sym->type) {
                        switch (sym->type->kind) {
                            case TYPE_STRUCT: type_label = "struct"; comp_kind = LSP_COMP_STRUCT; break;
                            case TYPE_CSTRUCT: type_label = "cstruct"; comp_kind = LSP_COMP_STRUCT; break;
                            case TYPE_ENUM: type_label = "enum"; comp_kind = LSP_COMP_ENUM; break;
                            case TYPE_FACE: type_label = "face"; comp_kind = LSP_COMP_INTERFACE; break;
                            default: break;
                        }
                    }
                    char detail[256];
                    snprintf(detail, sizeof(detail), "%s %s", type_label, sym->name);
                    comp_set_add(set, sym->name, comp_kind, PRIO_TYPE, detail, NULL, NULL, NULL);
                }
            }
        }
        compiler_context_cleanup(&ctx);
    }
    
    // 4. 模块别名
    for (int i = 0; i < import_count; i++) {
        char detail[256];
        snprintf(detail, sizeof(detail), "module %s", import_aliases[i].alias);
        comp_set_add(set, import_aliases[i].alias, LSP_COMP_MODULE, PRIO_MODULE,
                     detail, NULL, NULL, NULL);
    }
}

/* ========== 数组索引成员补全 ========== */

void comp_provider_add_array_index_members(
    CompletionSet* set,
    const char* content,
    const char* file_path,
    const char* var_name,
    int import_count,
    ImportAlias* import_aliases,
    LspPosition pos
) {
    if (!set || !content || !var_name) return;
    (void)pos;  // 保留参数以兼容接口，当前使用 scope_resolve_tree_bfs 不需要位置信息

    // 通过编译器解析数组变量类型
    CompilerContext ctx;
    compiler_context_init(&ctx);
    compiler_analyze_with_filename(&ctx, content, file_path);

    char* type_resolved = NULL;

    if (ctx.root_scope) {
        Symbol* var_sym = scope_resolve_tree_bfs(ctx.root_scope, var_name);

        if (var_sym && var_sym->type) {
            // 如果是数组类型，获取元素类型
            TypeInfo* elem_type = NULL;
            if (var_sym->type->kind == TYPE_ARRAY && var_sym->type->element_type) {
                elem_type = var_sym->type->element_type;
            }

            if (elem_type) {
                if (elem_type->kind == TYPE_STRUCT && elem_type->struct_name) {
                    type_resolved = (char*)malloc(strlen(elem_type->struct_name) + 16);
                    if (type_resolved) sprintf(type_resolved, "struct %s", elem_type->struct_name);
                } else if (elem_type->kind == TYPE_CSTRUCT && elem_type->struct_name) {
                    type_resolved = (char*)malloc(strlen(elem_type->struct_name) + 16);
                    if (type_resolved) sprintf(type_resolved, "cstruct %s", elem_type->struct_name);
                } else if (elem_type->kind == TYPE_CLIB && elem_type->struct_name) {
                    type_resolved = (char*)malloc(strlen(elem_type->struct_name) + 6);
                    if (type_resolved) sprintf(type_resolved, "clib %s", elem_type->struct_name);
                } else {
                    // 其他元素类型（如 string, int 等）
                    const char* ts = type_to_string(elem_type);
                    if (ts) type_resolved = strdup(ts);
                }
            }
        }
    }
    compiler_context_cleanup(&ctx);

    // 如果编译器途径失败，尝试从源文件文本中解析变量类型
    if (!type_resolved) {
        char* var_type_str = detect_var_type_from_text(content, var_name);
        if (var_type_str) {
            // 解析类型字符串，提取 Array[X] 中的 X
            const char* arr_prefix = strstr(var_type_str, "Array[");
            if (arr_prefix) {
                const char* inner_start = arr_prefix + 6;
                const char* inner_end = strchr(inner_start, ']');
                if (inner_end && inner_end > inner_start) {
                    int elem_len = inner_end - inner_start;
                    char elem_name[256];
                    if (elem_len < (int)sizeof(elem_name)) {
                        memcpy(elem_name, inner_start, elem_len);
                        elem_name[elem_len] = '\0';
                        // 先尝试作为 clib
                        if (file_path) {
                            for (int i = 0; i < import_count; i++) {
                                const char* mp = find_module_path_by_alias(import_aliases, import_count, import_aliases[i].alias);
                                if (!mp) continue;
                                module_symbol_table_reset_scan_stack();
                                ModuleSymbolTable* mtable = module_symbol_table_create(mp);
                                if (!mtable) continue;
                                if (module_symbol_table_scan(mtable, file_path) == 0) {
                                    ModuleClibSymbol* mclib = module_symbol_table_find_clib(mtable, elem_name);
                                    if (mclib) {
                                        type_resolved = (char*)malloc(strlen(elem_name) + 6);
                                        if (type_resolved) sprintf(type_resolved, "clib %s", elem_name);
                                    } else {
                                        ModuleStructSymbol* mst = module_symbol_table_find_struct(mtable, elem_name);
                                        if (mst) {
                                            type_resolved = (char*)malloc(strlen(elem_name) + 16);
                                            if (type_resolved) sprintf(type_resolved, "struct %s", elem_name);
                                        }
                                    }
                                }
                                module_symbol_table_destroy(mtable);
                                if (type_resolved) break;
                            }
                        }
                        // 也扫描当前文件
                        if (!type_resolved && file_path) {
                            module_symbol_table_reset_scan_stack();
                            ModuleSymbolTable* cur_table = module_symbol_table_create(file_path);
                            if (cur_table) {
                                if (module_symbol_table_scan(cur_table, file_path) == 0) {
                                    ModuleClibSymbol* mclib = module_symbol_table_find_clib(cur_table, elem_name);
                                    if (mclib) {
                                        type_resolved = (char*)malloc(strlen(elem_name) + 6);
                                        if (type_resolved) sprintf(type_resolved, "clib %s", elem_name);
                                    } else {
                                        ModuleStructSymbol* mst = module_symbol_table_find_struct(cur_table, elem_name);
                                        if (mst) {
                                            type_resolved = (char*)malloc(strlen(elem_name) + 16);
                                            if (type_resolved) sprintf(type_resolved, "struct %s", elem_name);
                                        }
                                    }
                                }
                                module_symbol_table_destroy(cur_table);
                            }
                        }
                    }
                }
            }
            free(var_type_str);
        }
    }

    if (!type_resolved) return;

    fprintf(stderr, "[COMPLETE-DEBUG] array index var='%s' elem type='%s'\n", var_name, type_resolved);
    fflush(stderr);

    // 根据元素类型添加补全项
    if (strstr(type_resolved, "struct") || strstr(type_resolved, "cstruct")) {
        // struct/cstruct 类型：查找字段和方法
        const char* keyword = strstr(type_resolved, "cstruct") ? "cstruct" : "struct";
        const char* struct_name = strstr(type_resolved, keyword);
        if (struct_name) {
            struct_name += strlen(keyword);
            while (*struct_name && isspace((unsigned char)*struct_name)) struct_name++;

            // 从当前文件编译器作用域查找
            CompilerContext sctx;
            compiler_context_init(&sctx);
            compiler_analyze_with_filename(&sctx, content, file_path);
            if (sctx.root_scope) {
                Symbol* struct_sym = scope_resolve_tree_bfs(sctx.root_scope, struct_name);
                if (struct_sym && struct_sym->type &&
                    (struct_sym->type->kind == TYPE_STRUCT || struct_sym->type->kind == TYPE_CSTRUCT)) {
                    for (int i = 0; i < struct_sym->struct_field_count; i++) {
                        const char* field_name = struct_sym->struct_field_names[i];
                        const char* ftype_str = type_to_string(struct_sym->struct_field_types[i]);
                        char detail[256];
                        snprintf(detail, sizeof(detail), "%s[i].%s: %s", var_name, field_name, ftype_str);
                        comp_set_add(set, field_name, LSP_COMP_FIELD, PRIO_FIELD,
                                     detail, NULL, NULL, NULL);
                    }
                }
            }
            compiler_context_cleanup(&sctx);

            // 从导入的模块查找
            for (int i = 0; i < import_count; i++) {
                const char* mp = find_module_path_by_alias(import_aliases, import_count, import_aliases[i].alias);
                if (!mp) continue;
                module_symbol_table_reset_scan_stack();
                ModuleSymbolTable* mtable = module_symbol_table_create(mp);
                if (!mtable) continue;
                if (module_symbol_table_scan(mtable, file_path) == 0) {
                    ModuleStructSymbol* mst = module_symbol_table_find_struct(mtable, struct_name);
                    if (mst) {
                        for (int j = 0; j < mst->field_count; j++) {
                            const char* fname = mst->fields[j].name;
                            const char* fts = mst->fields[j].struct_name ?
                                mst->fields[j].struct_name : type_kind_to_string(mst->fields[j].type);
                            char detail[256];
                            snprintf(detail, sizeof(detail), "%s[i].%s: %s", var_name, fname, fts);
                            comp_set_add(set, fname, LSP_COMP_FIELD, PRIO_FIELD,
                                         detail, NULL, NULL, NULL);
                        }
                        for (int j = 0; j < mst->method_count; j++) {
                            const char* mname = mst->methods[j].name;
                            const char* sep = strstr(mname, "::");
                            if (sep) sep += 2; else sep = mname;
                            char detail[256];
                            snprintf(detail, sizeof(detail), "%s[i].%s()", var_name, sep);
                            comp_set_add(set, sep, LSP_COMP_METHOD, PRIO_METHOD,
                                         detail, NULL, NULL, NULL);
                        }
                    }
                }
                module_symbol_table_destroy(mtable);
            }

            // 也扫描当前文件
            module_symbol_table_reset_scan_stack();
            ModuleSymbolTable* cur_table = module_symbol_table_create(file_path);
            if (cur_table) {
                if (module_symbol_table_scan(cur_table, file_path) == 0) {
                    ModuleStructSymbol* mst = module_symbol_table_find_struct(cur_table, struct_name);
                    if (mst) {
                        for (int j = 0; j < mst->field_count; j++) {
                            const char* fname = mst->fields[j].name;
                            const char* fts = mst->fields[j].struct_name ?
                                mst->fields[j].struct_name : type_kind_to_string(mst->fields[j].type);
                            char detail[256];
                            snprintf(detail, sizeof(detail), "%s[i].%s: %s", var_name, fname, fts);
                            comp_set_add(set, fname, LSP_COMP_FIELD, PRIO_FIELD,
                                         detail, NULL, NULL, NULL);
                        }
                        for (int j = 0; j < mst->method_count; j++) {
                            const char* mname = mst->methods[j].name;
                            const char* sep = strstr(mname, "::");
                            if (sep) sep += 2; else sep = mname;
                            char detail[256];
                            snprintf(detail, sizeof(detail), "%s[i].%s()", var_name, sep);
                            comp_set_add(set, sep, LSP_COMP_METHOD, PRIO_METHOD,
                                         detail, NULL, NULL, NULL);
                        }
                    }
                }
                module_symbol_table_destroy(cur_table);
            }
        }
    } else if (strncmp(type_resolved, "clib ", 5) == 0) {
        // clib 类型：查找 clib 函数并添加补全项
        const char* clib_name = type_resolved + 5;
        if (*clib_name) {
            // 从当前文件的编译器作用域中查找
            CompilerContext cctx;
            compiler_context_init(&cctx);
            compiler_analyze_with_filename(&cctx, content, file_path);
            bool clib_found = false;
            if (cctx.root_scope) {
                Symbol* clib_sym = scope_resolve_tree_bfs(cctx.root_scope, clib_name);
                if (clib_sym && clib_sym->clib_func_count > 0) {
                    clib_found = true;
                    for (int i = 0; i < clib_sym->clib_func_count; i++) {
                        const char* fname = clib_sym->clib_func_names[i];
                        TypeInfo* ret_type = clib_sym->clib_func_return_types[i];
                        const char* rt_str = ret_type ? type_to_string(ret_type) : "unknown";
                        int pc = clib_sym->clib_func_param_counts[i];
                        char detail[512];
                        if (pc == 0) {
                            snprintf(detail, sizeof(detail), "%s[i].%s() -> %s", var_name, fname, rt_str);
                        } else {
                            snprintf(detail, sizeof(detail), "%s[i].%s(...) -> %s", var_name, fname, rt_str);
                        }
                        comp_set_add(set, fname, LSP_COMP_METHOD, PRIO_METHOD,
                                     detail, NULL, NULL, NULL);
                    }
                }
            }
            compiler_context_cleanup(&cctx);

            // 从导入的模块符号表中查找
            if (!clib_found && file_path) {
                for (int i = 0; i < import_count; i++) {
                    const char* mp = find_module_path_by_alias(import_aliases, import_count, import_aliases[i].alias);
                    if (!mp) continue;
                    module_symbol_table_reset_scan_stack();
                    ModuleSymbolTable* mtable = module_symbol_table_create(mp);
                    if (!mtable) continue;
                    if (module_symbol_table_scan(mtable, file_path) == 0) {
                        ModuleClibSymbol* mclib = module_symbol_table_find_clib(mtable, clib_name);
                        if (mclib) {
                            for (int j = 0; j < mclib->func_count; j++) {
                                const char* fname = mclib->funcs[j].name;
                                const char* rt_str = mclib->funcs[j].return_struct_name ?
                                    mclib->funcs[j].return_struct_name :
                                    type_kind_to_string(mclib->funcs[j].return_type);
                                int pc = mclib->funcs[j].param_count;
                                char detail[512];
                                if (pc == 0) {
                                    snprintf(detail, sizeof(detail), "%s[i].%s() -> %s", var_name, fname, rt_str);
                                } else {
                                    snprintf(detail, sizeof(detail), "%s[i].%s(...) -> %s", var_name, fname, rt_str);
                                }
                                comp_set_add(set, fname, LSP_COMP_METHOD, PRIO_METHOD,
                                             detail, NULL, NULL, NULL);
                            }
                        }
                    }
                    module_symbol_table_destroy(mtable);
                }
            }
            // 也扫描当前文件
            if (!clib_found && file_path) {
                module_symbol_table_reset_scan_stack();
                ModuleSymbolTable* cur_table = module_symbol_table_create(file_path);
                if (cur_table) {
                    if (module_symbol_table_scan(cur_table, file_path) == 0) {
                        ModuleClibSymbol* mclib = module_symbol_table_find_clib(cur_table, clib_name);
                        if (mclib) {
                            for (int j = 0; j < mclib->func_count; j++) {
                                const char* fname = mclib->funcs[j].name;
                                const char* rt_str = mclib->funcs[j].return_struct_name ?
                                    mclib->funcs[j].return_struct_name :
                                    type_kind_to_string(mclib->funcs[j].return_type);
                                int pc = mclib->funcs[j].param_count;
                                char detail[512];
                                if (pc == 0) {
                                    snprintf(detail, sizeof(detail), "%s[i].%s() -> %s", var_name, fname, rt_str);
                                } else {
                                    snprintf(detail, sizeof(detail), "%s[i].%s(...) -> %s", var_name, fname, rt_str);
                                }
                                comp_set_add(set, fname, LSP_COMP_METHOD, PRIO_METHOD,
                                             detail, NULL, NULL, NULL);
                            }
                        }
                    }
                    module_symbol_table_destroy(cur_table);
                }
            }
        }
    } else {
        // 基本类型：实例方法
        int method_count = 0;
        char** methods = native_get_instance_methods(type_resolved, &method_count);
        if (methods && method_count > 0) {
            for (int i = 0; i < method_count; i++) {
                int arity = native_get_instance_method_arity(type_resolved, methods[i]);
                TypeKind rt = native_get_instance_method_return_type(type_resolved, methods[i], NULL);
                const char* rt_str = type_kind_to_string(rt);
                char detail[256];
                if (arity == 0) {
                    snprintf(detail, sizeof(detail), "%s[i].%s() -> %s", var_name, methods[i], rt_str);
                } else {
                    snprintf(detail, sizeof(detail), "%s[i].%s(...) -> %s", var_name, methods[i], rt_str);
                }
                comp_set_add(set, methods[i], LSP_COMP_METHOD, PRIO_METHOD,
                             detail, NULL, NULL, NULL);
            }
            native_free_instance_method_list(methods, method_count);
        }
    }

    free(type_resolved);
}
