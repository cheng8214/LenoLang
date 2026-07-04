#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "include/module_symbol_table.h"
#include "include/module_loader.h"

// 来自 module_loader.c
extern int normalize_path(char* path, int max_len);

// 初始容量
#define INITIAL_CAPACITY 16

// ---- 模块符号表容量限制（命名常量，替代硬编码） ----
#define MOD_MAX_NAMES         64   // 标识符/名称缓冲区长度
#define MOD_MAX_TYPE_STR     256   // 类型字符串缓冲区长度
#define MOD_MAX_TYPES        128   // 单模块内最大 struct/cstruct/face 定义数
#define MOD_MAX_CLIBS         32   // 单模块内最大 clib 定义数
#define MOD_MAX_ENUMS         32   // 单模块内最大 enum 定义数
#define MOD_MAX_MEMBERS      128   // 单 enum 最大成员数
#define MOD_MAX_FIELDS       128   // 单 struct/cstruct 最大字段数
#define MOD_MAX_METHODS      128   // 单 struct/cstruct/face 最大方法数
#define MOD_MAX_PARAMS        64   // 单函数最大参数数
#define MOD_MAX_TYPE_PARAMS   16   // 单类型最大泛型参数数
#define MOD_MAX_ALIASES       64   // 单模块内最大 alias 定义数
#define MOD_MAX_CLIB_FUNCS   512   // 单 clib 最大函数数
#define MOD_MAX_GENERIC_RET   16   // 返回类型最大泛型参数数

// 创建模块符号表
ModuleSymbolTable* module_symbol_table_create(const char* module_path) {
    ModuleSymbolTable* table = (ModuleSymbolTable*)malloc(sizeof(ModuleSymbolTable));
    if (!table) return NULL;

    table->module_path = strdup(module_path);
    table->funcs = NULL;
    table->func_count = 0;
    table->func_capacity = 0;
    table->structs = NULL;
    table->struct_count = 0;
    table->struct_capacity = 0;
    table->enums = NULL;
    table->enum_count = 0;
    table->enum_capacity = 0;
    table->faces = NULL;
    table->face_count = 0;
    table->face_capacity = 0;
    table->vars = NULL;
    table->var_count = 0;
    table->var_capacity = 0;
    table->aliases = NULL;
    table->alias_count = 0;
    table->alias_capacity = 0;
    table->clibs = NULL;
    table->clib_count = 0;
    table->clib_capacity = 0;

    return table;
}

// 销毁模块符号表
void module_symbol_table_destroy(ModuleSymbolTable* table) {
    if (!table) return;

    free(table->module_path);

    // 释放函数符号
    for (int i = 0; i < table->func_count; i++) {
        free(table->funcs[i].name);
        free(table->funcs[i].return_struct_name);
        if (table->funcs[i].return_type_info) {
            type_free(table->funcs[i].return_type_info);
        }
    }
    free(table->funcs);

    // 释放 struct 符号
    for (int i = 0; i < table->struct_count; i++) {
        free(table->structs[i].name);
        for (int j = 0; j < table->structs[i].field_count; j++) {
            free(table->structs[i].fields[j].name);
            free(table->structs[i].fields[j].struct_name);
        }
        free(table->structs[i].fields);
        // 释放方法数组
        for (int j = 0; j < table->structs[i].method_count; j++) {
            free(table->structs[i].methods[j].name);
            free(table->structs[i].methods[j].return_struct_name);
            if (table->structs[i].methods[j].return_type_info) {
                type_free(table->structs[i].methods[j].return_type_info);
            }
            free(table->structs[i].methods[j].return_type_param_name);
            for (int gi = 0; gi < table->structs[i].methods[j].return_generic_count; gi++) {
                free(table->structs[i].methods[j].return_generic_param_names[gi]);
            }
            free(table->structs[i].methods[j].return_generic_param_names);
            free(table->structs[i].methods[j].param_types);
            // 释放泛型参数名
            if (table->structs[i].methods[j].param_generic_names) {
                for (int pi = 0; pi < table->structs[i].methods[j].param_count; pi++) {
                    free(table->structs[i].methods[j].param_generic_names[pi]);
                }
                free(table->structs[i].methods[j].param_generic_names);
            }
        }
        free(table->structs[i].methods);
        // 释放 impl 名称数组
        for (int j = 0; j < table->structs[i].impl_count; j++) {
            free(table->structs[i].impl_names[j]);
        }
        free(table->structs[i].impl_names);
        // 释放泛型参数名数组
        for (int j = 0; j < table->structs[i].type_param_count; j++) {
            free(table->structs[i].type_param_names[j]);
        }
        free(table->structs[i].type_param_names);
    }
    free(table->structs);

    // 释放 enum 符号
    for (int i = 0; i < table->enum_count; i++) {
        free(table->enums[i].name);
        for (int j = 0; j < table->enums[i].member_count; j++) {
            free(table->enums[i].member_names[j]);
        }
        free(table->enums[i].member_names);
        free(table->enums[i].member_values);
    }
    free(table->enums);

    // 释放 face 符号
    for (int i = 0; i < table->face_count; i++) {
        free(table->faces[i].name);
        for (int j = 0; j < table->faces[i].method_count; j++) {
            free(table->faces[i].methods[j].name);
            free(table->faces[i].methods[j].return_struct_name);
        }
        free(table->faces[i].methods);
    }
    free(table->faces);

    // 释放变量符号
    for (int i = 0; i < table->var_count; i++) {
        free(table->vars[i].name);
        free(table->vars[i].struct_name);
    }
    free(table->vars);

    // 释放别名符号
    for (int i = 0; i < table->alias_count; i++) {
        free(table->aliases[i].name);
        if (table->aliases[i].type_info) {
            type_free(table->aliases[i].type_info);
        }
    }
    free(table->aliases);

    // 释放 clib 符号
    for (int i = 0; i < table->clib_count; i++) {
        free(table->clibs[i].name);
        for (int j = 0; j < table->clibs[i].func_count; j++) {
            free(table->clibs[i].funcs[j].name);
            free(table->clibs[i].funcs[j].return_struct_name);
            for (int k = 0; k < table->clibs[i].funcs[j].param_count; k++) {
                free(table->clibs[i].funcs[j].param_struct_names[k]);
            }
            free(table->clibs[i].funcs[j].param_types);
            free(table->clibs[i].funcs[j].param_struct_names);
        }
        free(table->clibs[i].funcs);
    }
    free(table->clibs);

    free(table);
}

// 添加函数符号
void module_symbol_table_add_func(ModuleSymbolTable* table, const char* name, TypeKind return_type, const char* return_struct_name, int type_param_count, TypeInfo* return_type_info) {
    if (!table || !name) return;

    // 扩容
    if (table->func_count >= table->func_capacity) {
        int new_capacity = table->func_capacity == 0 ? INITIAL_CAPACITY : table->func_capacity * 2;
        ModuleFuncSymbol* new_funcs = (ModuleFuncSymbol*)realloc(table->funcs, sizeof(ModuleFuncSymbol) * new_capacity);
        if (!new_funcs) return;
        table->funcs = new_funcs;
        table->func_capacity = new_capacity;
    }

    ModuleFuncSymbol* func = &table->funcs[table->func_count++];
    func->name = strdup(name);
    func->return_type = return_type;
    func->return_type_info = return_type_info ? type_copy(return_type_info) : NULL;
    func->return_struct_name = return_struct_name ? strdup(return_struct_name) : NULL;
    func->type_param_count = type_param_count;
}

// 添加 struct 符号
void module_symbol_table_add_struct(ModuleSymbolTable* table, const char* name, int field_count, ModuleStructField* fields, int method_count, ModuleStructMethod* methods, int is_cstruct, int type_param_count, char** type_param_names) {
    if (!table || !name) return;

    // 扩容
    if (table->struct_count >= table->struct_capacity) {
        int new_capacity = table->struct_capacity == 0 ? INITIAL_CAPACITY : table->struct_capacity * 2;
        ModuleStructSymbol* new_structs = (ModuleStructSymbol*)realloc(table->structs, sizeof(ModuleStructSymbol) * new_capacity);
        if (!new_structs) return;
        table->structs = new_structs;
        table->struct_capacity = new_capacity;
    }

    ModuleStructSymbol* st = &table->structs[table->struct_count++];
    st->name = strdup(name);
    st->field_count = field_count;
    st->fields = NULL;
    st->method_count = method_count;
    st->methods = NULL;
    st->is_cstruct = is_cstruct;
    st->impl_count = 0;
    st->impl_names = NULL;
    st->type_param_count = type_param_count;
    st->type_param_names = NULL;
    if (type_param_count > 0 && type_param_names) {
        st->type_param_names = (char**)malloc(sizeof(char*) * type_param_count);
        for (int i = 0; i < type_param_count; i++) {
            st->type_param_names[i] = strdup(type_param_names[i]);
        }
    }

    if (field_count > 0 && fields) {
        st->fields = (ModuleStructField*)malloc(sizeof(ModuleStructField) * field_count);
        for (int i = 0; i < field_count; i++) {
            st->fields[i].name = strdup(fields[i].name);
            st->fields[i].type = fields[i].type;
            st->fields[i].struct_name = fields[i].struct_name ? strdup(fields[i].struct_name) : NULL;
            st->fields[i].element_type = fields[i].element_type;
        }
    }

    if (method_count > 0 && methods) {
        st->methods = (ModuleStructMethod*)malloc(sizeof(ModuleStructMethod) * method_count);
        for (int i = 0; i < method_count; i++) {
            st->methods[i].name = strdup(methods[i].name);
            st->methods[i].return_type = methods[i].return_type;
            st->methods[i].return_type_info = methods[i].return_type_info ? type_copy(methods[i].return_type_info) : NULL;
            st->methods[i].return_struct_name = methods[i].return_struct_name ? strdup(methods[i].return_struct_name) : NULL;
            st->methods[i].return_type_param_name = methods[i].return_type_param_name ? strdup(methods[i].return_type_param_name) : NULL;
            st->methods[i].return_generic_count = methods[i].return_generic_count;
            st->methods[i].return_generic_param_names = NULL;
            if (methods[i].return_generic_count > 0 && methods[i].return_generic_param_names) {
                st->methods[i].return_generic_param_names = (char**)malloc(sizeof(char*) * methods[i].return_generic_count);
                for (int gi = 0; gi < methods[i].return_generic_count; gi++) {
                    st->methods[i].return_generic_param_names[gi] = strdup(methods[i].return_generic_param_names[gi]);
                }
            }
            st->methods[i].param_count = methods[i].param_count;
            st->methods[i].param_types = NULL;
            st->methods[i].param_generic_names = NULL;
            if (methods[i].param_count > 0 && methods[i].param_types) {
                st->methods[i].param_types = (TypeKind*)malloc(sizeof(TypeKind) * methods[i].param_count);
                for (int j = 0; j < methods[i].param_count; j++) {
                    st->methods[i].param_types[j] = methods[i].param_types[j];
                }
            }
            // 深拷贝泛型参数名
            if (methods[i].param_count > 0 && methods[i].param_generic_names) {
                int has_generic = 0;
                for (int j = 0; j < methods[i].param_count; j++) {
                    if (methods[i].param_generic_names[j]) { has_generic = 1; break; }
                }
                if (has_generic) {
                    st->methods[i].param_generic_names = (char**)malloc(sizeof(char*) * methods[i].param_count);
                    for (int j = 0; j < methods[i].param_count; j++) {
                        st->methods[i].param_generic_names[j] = methods[i].param_generic_names[j]
                            ? strdup(methods[i].param_generic_names[j]) : NULL;
                    }
                }
            }
        }
    }
}

// 查找函数符号
ModuleFuncSymbol* module_symbol_table_find_func(ModuleSymbolTable* table, const char* func_name) {
    if (!table || !func_name) return NULL;

    for (int i = 0; i < table->func_count; i++) {
        if (strcmp(table->funcs[i].name, func_name) == 0) {
            return &table->funcs[i];
        }
    }
    return NULL;
}

// 查找 struct 符号
ModuleStructSymbol* module_symbol_table_find_struct(ModuleSymbolTable* table, const char* struct_name) {
    if (!table || !struct_name) return NULL;

    for (int i = 0; i < table->struct_count; i++) {
        if (strcmp(table->structs[i].name, struct_name) == 0) {
            return &table->structs[i];
        }
    }
    return NULL;
}

// 查找 struct 方法
ModuleStructMethod* module_symbol_table_find_struct_method(ModuleSymbolTable* table, const char* struct_name, const char* method_name) {
    if (!table || !struct_name || !method_name) return NULL;

    ModuleStructSymbol* st = module_symbol_table_find_struct(table, struct_name);
    if (!st || !st->methods) return NULL;

    char full_method_name[256];
    snprintf(full_method_name, sizeof(full_method_name), "%s::%s", struct_name, method_name);

    for (int i = 0; i < st->method_count; i++) {
        if (strcmp(st->methods[i].name, full_method_name) == 0) {
            return &st->methods[i];
        }
    }
    return NULL;
}

// 查找 enum 符号
ModuleEnumSymbol* module_symbol_table_find_enum(ModuleSymbolTable* table, const char* enum_name) {
    if (!table || !enum_name) return NULL;

    for (int i = 0; i < table->enum_count; i++) {
        if (strcmp(table->enums[i].name, enum_name) == 0) {
            return &table->enums[i];
        }
    }
    return NULL;
}

// 添加 enum 符号
void module_symbol_table_add_enum(ModuleSymbolTable* table, const char* name, int member_count, char** member_names) {
    if (!table || !name) return;

    // 扩容
    if (table->enum_count >= table->enum_capacity) {
        int new_capacity = table->enum_capacity == 0 ? INITIAL_CAPACITY : table->enum_capacity * 2;
        ModuleEnumSymbol* new_enums = (ModuleEnumSymbol*)realloc(table->enums, sizeof(ModuleEnumSymbol) * new_capacity);
        if (!new_enums) return;
        table->enums = new_enums;
        table->enum_capacity = new_capacity;
    }

    ModuleEnumSymbol* en = &table->enums[table->enum_count++];
    en->name = strdup(name);
    en->member_count = member_count;
    en->member_names = NULL;
    en->member_values = NULL;
    if (member_count > 0 && member_names) {
        en->member_names = (char**)malloc(sizeof(char*) * member_count);
        en->member_values = (int*)malloc(sizeof(int) * member_count);
        for (int i = 0; i < member_count; i++) {
            en->member_names[i] = strdup(member_names[i]);
            en->member_values[i] = i;  // 默认值 = 索引
        }
    }
}

// 查找 face 符号
ModuleFaceSymbol* module_symbol_table_find_face(ModuleSymbolTable* table, const char* face_name) {
    if (!table || !face_name) return NULL;

    for (int i = 0; i < table->face_count; i++) {
        if (strcmp(table->faces[i].name, face_name) == 0) {
            return &table->faces[i];
        }
    }
    return NULL;
}

// 添加 face 符号
void module_symbol_table_add_face(ModuleSymbolTable* table, const char* name, int method_count, ModuleFaceMethodSymbol* methods, int type_param_count) {
    if (!table || !name) return;

    // 扩容
    if (table->face_count >= table->face_capacity) {
        int new_capacity = table->face_capacity == 0 ? INITIAL_CAPACITY : table->face_capacity * 2;
        ModuleFaceSymbol* new_faces = (ModuleFaceSymbol*)realloc(table->faces, sizeof(ModuleFaceSymbol) * new_capacity);
        if (!new_faces) return;
        table->faces = new_faces;
        table->face_capacity = new_capacity;
    }

    ModuleFaceSymbol* face = &table->faces[table->face_count++];
    face->name = strdup(name);
    face->method_count = method_count;
    face->type_param_count = type_param_count;
    // 深拷贝 methods 数组
    if (method_count > 0 && methods) {
        face->methods = (ModuleFaceMethodSymbol*)malloc(sizeof(ModuleFaceMethodSymbol) * method_count);
        for (int i = 0; i < method_count; i++) {
            face->methods[i].name = strdup(methods[i].name);
            face->methods[i].return_type = methods[i].return_type;
            face->methods[i].return_struct_name = methods[i].return_struct_name ? strdup(methods[i].return_struct_name) : NULL;
            face->methods[i].param_count = methods[i].param_count;
        }
    } else {
        face->methods = NULL;
    }
}

// 查找变量符号
ModuleVarSymbol* module_symbol_table_find_var(ModuleSymbolTable* table, const char* var_name) {
    if (!table || !var_name) return NULL;

    for (int i = 0; i < table->var_count; i++) {
        if (strcmp(table->vars[i].name, var_name) == 0) {
            return &table->vars[i];
        }
    }
    return NULL;
}

// 添加变量符号
void module_symbol_table_add_var(ModuleSymbolTable* table, const char* name, TypeKind type, const char* struct_name) {
    if (!table || !name) return;

    // 扩容
    if (table->var_count >= table->var_capacity) {
        int new_capacity = table->var_capacity == 0 ? INITIAL_CAPACITY : table->var_capacity * 2;
        ModuleVarSymbol* new_vars = (ModuleVarSymbol*)realloc(table->vars, sizeof(ModuleVarSymbol) * new_capacity);
        if (!new_vars) return;
        table->vars = new_vars;
        table->var_capacity = new_capacity;
    }

    ModuleVarSymbol* var = &table->vars[table->var_count++];
    var->name = strdup(name);
    var->type = type;
    var->struct_name = struct_name ? strdup(struct_name) : NULL;
}

// 查找别名符号
ModuleAliasSymbol* module_symbol_table_find_alias(ModuleSymbolTable* table, const char* alias_name) {
    if (!table || !alias_name) return NULL;

    for (int i = 0; i < table->alias_count; i++) {
        if (strcmp(table->aliases[i].name, alias_name) == 0) {
            return &table->aliases[i];
        }
    }
    return NULL;
}

// 添加别名符号
void module_symbol_table_add_alias(ModuleSymbolTable* table, const char* name, TypeInfo* type_info) {
    if (!table || !name) return;

    // 扩容
    if (table->alias_count >= table->alias_capacity) {
        int new_capacity = table->alias_capacity == 0 ? INITIAL_CAPACITY : table->alias_capacity * 2;
        ModuleAliasSymbol* new_aliases = (ModuleAliasSymbol*)realloc(table->aliases, sizeof(ModuleAliasSymbol) * new_capacity);
        if (!new_aliases) return;
        table->aliases = new_aliases;
        table->alias_capacity = new_capacity;
    }

    ModuleAliasSymbol* alias = &table->aliases[table->alias_count++];
    alias->name = strdup(name);
    alias->type_info = type_info ? type_copy(type_info) : NULL;
}

// 查找 clib 符号
ModuleClibSymbol* module_symbol_table_find_clib(ModuleSymbolTable* table, const char* clib_name) {
    if (!table || !clib_name) return NULL;
    for (int i = 0; i < table->clib_count; i++) {
        if (strcmp(table->clibs[i].name, clib_name) == 0) return &table->clibs[i];
    }
    return NULL;
}

// 添加 clib 符号
void module_symbol_table_add_clib(ModuleSymbolTable* table, const char* name, int func_count, ModuleClibFuncSymbol* funcs) {
    if (!table || !name || func_count <= 0) return;

    if (table->clib_count >= table->clib_capacity) {
        int new_capacity = table->clib_capacity == 0 ? 8 : table->clib_capacity * 2;
        ModuleClibSymbol* new_clibs = (ModuleClibSymbol*)realloc(table->clibs, sizeof(ModuleClibSymbol) * new_capacity);
        if (!new_clibs) return;
        table->clibs = new_clibs;
        table->clib_capacity = new_capacity;
    }

    ModuleClibSymbol* cl = &table->clibs[table->clib_count++];
    cl->name = strdup(name);
    cl->func_count = func_count;
    cl->funcs = (ModuleClibFuncSymbol*)malloc(sizeof(ModuleClibFuncSymbol) * func_count);
    for (int i = 0; i < func_count; i++) {
        cl->funcs[i].name = strdup(funcs[i].name);
        cl->funcs[i].return_type = funcs[i].return_type;
        cl->funcs[i].return_struct_name = funcs[i].return_struct_name ? strdup(funcs[i].return_struct_name) : NULL;
        cl->funcs[i].param_count = funcs[i].param_count;
        cl->funcs[i].param_types = (TypeKind*)malloc(sizeof(TypeKind) * funcs[i].param_count);
        cl->funcs[i].param_struct_names = (char**)calloc(funcs[i].param_count, sizeof(char*));
        for (int j = 0; j < funcs[i].param_count; j++) {
            cl->funcs[i].param_types[j] = funcs[i].param_types[j];
            if (funcs[i].param_struct_names[j]) {
                cl->funcs[i].param_struct_names[j] = strdup(funcs[i].param_struct_names[j]);
            }
        }
    }
}

// clib 符号数量
int module_symbol_table_clib_count(ModuleSymbolTable* table) {
    return table ? table->clib_count : 0;
}

// ============================================================================
// 类型字符串解析器 — 支持 Array[T]、Dict[K,V] 等复杂类型
// ============================================================================

// 前向声明（递归用）
static TypeInfo* parse_type_from_string_inner(const char** p);

// 跳过空白
static void skip_ws(const char** p) {
    while (**p && (**p == ' ' || **p == '\t')) (*p)++;
}

// 读一个标识符/类型名
static int read_ident(const char* text, int max_len, const char** end) {
    const char* s = text;
    while (*s && (isalnum((unsigned char)*s) || *s == '_')) s++;
    int len = (int)(s - text);
    if (end) *end = s;
    return len < max_len ? len : max_len - 1;
}

// 解析一个基本类型名（int/float/string/bool 等内置类型 或 自定义 struct 名）
static TypeInfo* parse_simple_type_str(const char* s, int len) {
    char buf[64];
    int n = len < 63 ? len : 63;
    memcpy(buf, s, n);
    buf[n] = '\0';
    
    // 基础类型
    if (strcmp(buf, "int") == 0)    return type_new(TYPE_INT);
    if (strcmp(buf, "float") == 0)  return type_new(TYPE_FLOAT);
    if (strcmp(buf, "string") == 0) return type_new(TYPE_STRING);
    if (strcmp(buf, "bool") == 0)   return type_new(TYPE_BOOL);
    if (strcmp(buf, "var") == 0)    return type_new(TYPE_INFER);
    if (strcmp(buf, "any") == 0)    return type_new(TYPE_ANY);
    if (strcmp(buf, "null") == 0)   return type_new(TYPE_NULL);
    if (strcmp(buf, "void") == 0)   return type_new(TYPE_NULL);  // void 映射为 TYPE_NULL
    // 文件/网络类型
    if (strcmp(buf, "File") == 0)   return type_new(TYPE_FILE);
    if (strcmp(buf, "Socket") == 0) return type_new(TYPE_SOCKET);
    if (strcmp(buf, "Channel") == 0) return type_new(TYPE_CHANNEL);
    if (strcmp(buf, "Thread") == 0) return type_new(TYPE_THREAD);
    // 指针类型
    if (strcmp(buf, "ptr") == 0 || strcmp(buf, "Ptr") == 0) return type_new(TYPE_PTR);
    // C 布局类型
    if (strcmp(buf, "i8") == 0)     return type_new(TYPE_I8);
    if (strcmp(buf, "u8") == 0)     return type_new(TYPE_U8);
    if (strcmp(buf, "i16") == 0)    return type_new(TYPE_I16);
    if (strcmp(buf, "u16") == 0)    return type_new(TYPE_U16);
    if (strcmp(buf, "i32") == 0)    return type_new(TYPE_I32);
    if (strcmp(buf, "u32") == 0)    return type_new(TYPE_U32);
    if (strcmp(buf, "i64") == 0)    return type_new(TYPE_I64);
    if (strcmp(buf, "u64") == 0)    return type_new(TYPE_U64);
    if (strcmp(buf, "f32") == 0)    return type_new(TYPE_F32);
    if (strcmp(buf, "f64") == 0)    return type_new(TYPE_F64);
    if (strcmp(buf, "c_int") == 0)  return type_new(TYPE_C_INT);
    if (strcmp(buf, "c_uint") == 0) return type_new(TYPE_C_UINT);
    if (strcmp(buf, "c_long") == 0) return type_new(TYPE_C_LONG);
    if (strcmp(buf, "c_ulong") == 0) return type_new(TYPE_C_ULONG);
    if (strcmp(buf, "c_longlong") == 0)  return type_new(TYPE_C_LONGLONG);
    if (strcmp(buf, "c_ulonglong") == 0) return type_new(TYPE_C_ULONGLONG);
    if (strcmp(buf, "c_size") == 0) return type_new(TYPE_C_SIZE);
    if (strcmp(buf, "c_ssize") == 0) return type_new(TYPE_C_SSIZE);
    if (strcmp(buf, "str8") == 0)   return type_new(TYPE_STR8);
    if (strcmp(buf, "str16") == 0)  return type_new(TYPE_STR16);
    // 自定义类型名（struct 等）→ TYPE_STRUCT
    TypeInfo* ti = type_new(TYPE_STRUCT);
    ti->struct_name = strdup(buf);
    return ti;
}

// 递归解析类型字符串
static TypeInfo* parse_type_from_string_inner(const char** p) {
    skip_ws(p);
    if (!**p) return NULL;
    
    // 读标识符
    const char* name_start = *p;
    const char* name_end;
    int name_len = read_ident(name_start, 64, &name_end);
    if (name_len == 0) return NULL;
    
    // 检查是否是 func 类型（函数类型：func(params):return_type）
    char ident_buf[65];
    int copy_len = name_len < 64 ? name_len : 64;
    memcpy(ident_buf, name_start, copy_len);
    ident_buf[copy_len] = '\0';
    
    if (strcmp(ident_buf, "func") == 0) {
        *p = name_end;
        skip_ws(p);
        
        TypeInfo** param_types = NULL;
        int param_count = 0;
        int param_capacity = 4;
        TypeInfo* return_type = NULL;
        
        // 解析参数列表 (ParamType1, ParamType2, ...)
        if (**p == '(') {
            (*p)++; // 跳过 '('
            skip_ws(p);
            
            if (**p != ')') {
                // 有参数
                param_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * param_capacity);
                while (1) {
                    if (param_count >= param_capacity) {
                        param_capacity *= 2;
                        param_types = (TypeInfo**)realloc(param_types, sizeof(TypeInfo*) * param_capacity);
                    }
                    TypeInfo* pt = parse_type_from_string_inner(p);
                    if (!pt) break;
                    param_types[param_count++] = pt;
                    skip_ws(p);
                    if (**p == ',') {
                        (*p)++;
                        skip_ws(p);
                    } else {
                        break;
                    }
                }
            }
            
            skip_ws(p);
            if (**p == ')') (*p)++; // 跳过 ')'
            skip_ws(p);
        }
        
        // 解析返回类型 :ReturnType
        if (**p == ':') {
            (*p)++; // 跳过 ':'
            skip_ws(p);
            return_type = parse_type_from_string_inner(p);
            // void 返回类型转为 TYPE_NULL
            if (return_type && return_type->kind == TYPE_STRUCT && return_type->struct_name &&
                strcmp(return_type->struct_name, "void") == 0) {
                type_free(return_type);
                return_type = type_new(TYPE_NULL);
            }
        }
        
        // 构建函数类型
        TypeInfo* func_type = type_new(TYPE_FUNCTION);
        func_type->return_type = return_type;
        func_type->param_types = param_types;
        func_type->param_count = param_count;
        return func_type;
    }
    
    *p = name_end;
    skip_ws(p);
    
    // 检查泛型后缀 [T] 或 [K,V]
    if (**p == '[') {
        // 保存标识符用于判断是否是 Array/Dict
        char ident[MOD_MAX_NAMES + 1];
        int copy_len = name_len < 64 ? name_len : 64;
        memcpy(ident, name_start, copy_len);
        ident[copy_len] = '\0';

        (*p)++; // skip '['
        skip_ws(p);
        
        // 解析第一个参数类型
        TypeInfo* param1 = parse_type_from_string_inner(p);
        if (!param1) return NULL;
        
        skip_ws(p);
        
        // 检查是否是 Dict[K, V] (有逗号)
        if (**p == ',') {
            (*p)++; // skip ','
            skip_ws(p);
            
            TypeInfo* param2 = parse_type_from_string_inner(p);
            if (!param2) { type_free(param1); return NULL; }
            
            skip_ws(p);
            if (**p != ']') { type_free(param1); type_free(param2); return NULL; }
            (*p)++; // skip ']'
            
            // Dict[K, V]
            TypeInfo* dict = type_new(TYPE_DICT);
            dict->key_type = param1;   // 键类型
            dict->value_type = param2; // 值类型
            return dict;
        }
        
        // Array[T] — 只有单个参数
        if (**p != ']') { type_free(param1); return NULL; }
        (*p)++; // skip ']'
        
        // 判断标识符类型：Array → TYPE_ARRAY，Ptr → TYPE_PTR_GENERIC，其他 → 泛型 struct（如 Holder[K]）
        if (strcmp(ident, "Array") == 0) {
            TypeInfo* arr = type_new(TYPE_ARRAY);
            arr->element_type = param1;
            return arr;
        }

        if (strcmp(ident, "Ptr") == 0) {
            return type_ptr_generic(param1);
        }

        // 泛型 struct 类型：Holder[K] → TYPE_STRUCT, struct_name="Holder", element_type=K
        TypeInfo* generic_struct = type_new(TYPE_STRUCT);
        generic_struct->struct_name = strdup(ident);
        generic_struct->element_type = param1;
        return generic_struct;
    }
    
    // 简单类型
    return parse_simple_type_str(name_start, name_len);
}

// 公开接口：从字符串解析完整类型
TypeInfo* parse_type_from_string(const char* type_str) {
    if (!type_str) return NULL;
    const char* p = type_str;
    return parse_type_from_string_inner(&p);
}

// 基本类型解析 - 识别所有内置类型（用于 struct 字段/函数参数扫描）
static TypeKind parse_base_type(const char* type_str) {
    // 基础类型
    if (strcmp(type_str, "int") == 0)    return TYPE_INT;
    if (strcmp(type_str, "float") == 0)  return TYPE_FLOAT;
    if (strcmp(type_str, "string") == 0) return TYPE_STRING;
    if (strcmp(type_str, "bool") == 0)   return TYPE_BOOL;
    if (strcmp(type_str, "any") == 0)    return TYPE_ANY;
    if (strcmp(type_str, "null") == 0)   return TYPE_NULL;
    // 文件/颜色类型
    if (strcmp(type_str, "File") == 0)   return TYPE_FILE;
    // 网络/指针类型
    if (strcmp(type_str, "Socket") == 0) return TYPE_SOCKET;
    if (strcmp(type_str, "Channel") == 0) return TYPE_CHANNEL;
    if (strcmp(type_str, "Thread") == 0) return TYPE_THREAD;
    if (strcmp(type_str, "Ptr") == 0)    return TYPE_PTR;
    // C 布局类型
    if (strcmp(type_str, "i8") == 0)     return TYPE_I8;
    if (strcmp(type_str, "u8") == 0)     return TYPE_U8;
    if (strcmp(type_str, "i16") == 0)    return TYPE_I16;
    if (strcmp(type_str, "u16") == 0)    return TYPE_U16;
    if (strcmp(type_str, "i32") == 0)    return TYPE_I32;
    if (strcmp(type_str, "u32") == 0)    return TYPE_U32;
    if (strcmp(type_str, "i64") == 0)    return TYPE_I64;
    if (strcmp(type_str, "u64") == 0)    return TYPE_U64;
    if (strcmp(type_str, "f32") == 0)    return TYPE_F32;
    if (strcmp(type_str, "f64") == 0)    return TYPE_F64;
    if (strcmp(type_str, "c_int") == 0)  return TYPE_C_INT;
    if (strcmp(type_str, "c_uint") == 0) return TYPE_C_UINT;
    if (strcmp(type_str, "c_long") == 0) return TYPE_C_LONG;
    if (strcmp(type_str, "c_ulong") == 0) return TYPE_C_ULONG;
    if (strcmp(type_str, "c_longlong") == 0)  return TYPE_C_LONGLONG;
    if (strcmp(type_str, "c_ulonglong") == 0) return TYPE_C_ULONGLONG;
    if (strcmp(type_str, "c_size") == 0) return TYPE_C_SIZE;
    if (strcmp(type_str, "c_ssize") == 0) return TYPE_C_SSIZE;
    if (strcmp(type_str, "str8") == 0)   return TYPE_STR8;
    if (strcmp(type_str, "str16") == 0)  return TYPE_STR16;
    // 容器/集合类型
    if (strcmp(type_str, "Array") == 0)  return TYPE_ARRAY;
    if (strcmp(type_str, "Dict") == 0)   return TYPE_DICT;
    return TYPE_ANY;
}

// 检查类型是否是已知的 struct 类型
static int is_known_struct(const char* type_str, char** struct_names, int struct_count) {
    for (int i = 0; i < struct_count; i++) {
        if (strcmp(struct_names[i], type_str) == 0) {
            return 1;
        }
    }
    return 0;
}

// 检查类型是否是已知的 clib 类型
static int is_known_clib(const char* type_str, char** clib_names, int clib_count) {
    for (int i = 0; i < clib_count; i++) {
        if (strcmp(clib_names[i], type_str) == 0) {
            return 1;
        }
    }
    return 0;
}

// 检查类型是否是已知的 face 类型
static int is_known_face(const char* type_str, char** face_names, int face_count) {
    for (int i = 0; i < face_count; i++) {
        if (strcmp(face_names[i], type_str) == 0) {
            return 1;
        }
    }
    return 0;
}

// 检查类型是否是已知的 cstruct 类型
static int is_known_cstruct(const char* type_str, char** cstruct_names, int cstruct_count) {
    for (int i = 0; i < cstruct_count; i++) {
        if (strcmp(cstruct_names[i], type_str) == 0) {
            return 1;
        }
    }
    return 0;
}

// 从模块文件读取源代码（需要外部提供或复制相关代码）
extern char* read_module_file(const char* file_path, const char* current_file);

// 扫描模块文件，提取所有符号信息
// 两遍扫描：第一遍收集所有 struct 名称，第二遍解析所有符号
// max_depth: 递归深度限制，防止循环 use 依赖导致栈溢出
#define MODULE_SCAN_MAX_DEPTH 32

static int module_symbol_table_scan_depth(ModuleSymbolTable* table, const char* current_file, int depth) {
    if (depth > MODULE_SCAN_MAX_DEPTH) return -1;
    if (!table || !table->module_path) return -1;

    // 只读取一次文件
    char* source = read_module_file(table->module_path, current_file);
    if (!source) return -1;

    // 构建当前模块的规范化绝对路径，供后续递归扫描使用
    // 这样子模块的相对路径可以基于正确的目录解析
    char normalized_current[MAX_PATH_LEN] = {0};
    if (current_file) {
        strncpy(normalized_current, current_file, MAX_PATH_LEN - 1);
        normalized_current[MAX_PATH_LEN - 1] = '\0';
        char* last_sep = strrchr(normalized_current, '\\');
        if (!last_sep) last_sep = strrchr(normalized_current, '/');
        if (last_sep) {
            *(last_sep + 1) = '\0';
            strncat(normalized_current, table->module_path, MAX_PATH_LEN - strlen(normalized_current) - 1);
        } else {
            strncpy(normalized_current, table->module_path, MAX_PATH_LEN - 1);
        }
        // 规范化路径（解析 ../ 等）
        normalize_path(normalized_current, MAX_PATH_LEN);
    } else {
        strncpy(normalized_current, table->module_path, MAX_PATH_LEN - 1);
    }

    // 第一遍：收集所有 struct/cstruct/clib/face/enum 名称
    char* struct_names[MOD_MAX_TYPES];
    int struct_name_count = 0;
    char* cstruct_names[MOD_MAX_TYPES];
    int cstruct_name_count = 0;
    char* face_names[MOD_MAX_TYPES];
    int face_name_count = 0;
    char* clib_names[MOD_MAX_CLIBS];
    int clib_name_count = 0;
    char* enum_names[MOD_MAX_ENUMS];
    int enum_name_count = 0;
    char* p = source;

    while (*p && struct_name_count < MOD_MAX_TYPES) {
        // 跳过空白、注释、字符串
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
        if (!*p) break;

        if (*p == '/' && *(p+1) == '/') {
            while (*p && *p != '\n') p++;
            continue;
        }
        if (*p == '/' && *(p+1) == '*') {
            p += 2;
            while (*p && !(*p == '*' && *(p+1) == '/')) p++;
            if (*p) p += 2;
            continue;
        }
        if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                if (*p == '\\' && *(p+1)) p += 2;
                else p++;
            }
            if (*p) p++;
            continue;
        }

        // 查找 export struct 或 export cstruct
        if (strncmp(p, "export", 6) == 0 && (p[6] == ' ' || p[6] == '\t' || p[6] == '\n' || p[6] == '\r' || p[6] == '\0')) {
            char* after_export = p + 6;
            while (*after_export && (*after_export == ' ' || *after_export == '\t' || *after_export == '\n' || *after_export == '\r')) after_export++;

            // 检查 export struct
            if (strncmp(after_export, "struct", 6) == 0 && (after_export[6] == ' ' || after_export[6] == '\t' || after_export[6] == '\n' || after_export[6] == '\r' || after_export[6] == '\0')) {
                char* after_struct = after_export + 6;
                while (*after_struct && (*after_struct == ' ' || *after_struct == '\t')) after_struct++;

                const char* name_start = after_struct;
                while (*after_struct && (isalnum((unsigned char)*after_struct) || *after_struct == '_')) after_struct++;
                int name_len = (int)(after_struct - name_start);

                if (name_len > 0 && name_len < 64) {
                    if (struct_name_count >= MOD_MAX_TYPES) {
                        fprintf(stderr, "[错误] struct数量超过上限 %d\n", MOD_MAX_TYPES);
                    } else {
                        struct_names[struct_name_count] = (char*)malloc(name_len + 1);
                        strncpy(struct_names[struct_name_count], name_start, name_len);
                        struct_names[struct_name_count][name_len] = '\0';
                        struct_name_count++;
                    }
                }

                p = after_struct;
                continue;
            }

            // 检查 export cstruct
            if (strncmp(after_export, "cstruct", 7) == 0 && (after_export[7] == ' ' || after_export[7] == '\t' || after_export[7] == '\n' || after_export[7] == '\r' || after_export[7] == '\0')) {
                char* after_cstruct = after_export + 7;
                while (*after_cstruct && (*after_cstruct == ' ' || *after_cstruct == '\t')) after_cstruct++;

                const char* name_start = after_cstruct;
                while (*after_cstruct && (isalnum((unsigned char)*after_cstruct) || *after_cstruct == '_')) after_cstruct++;
                int name_len = (int)(after_cstruct - name_start);

                if (name_len > 0 && name_len < 64) {
                    if (cstruct_name_count >= MOD_MAX_TYPES) {
                        fprintf(stderr, "[错误] cstruct数量超过上限 %d\n", MOD_MAX_TYPES);
                    } else {
                        cstruct_names[cstruct_name_count] = (char*)malloc(name_len + 1);
                        strncpy(cstruct_names[cstruct_name_count], name_start, name_len);
                        cstruct_names[cstruct_name_count][name_len] = '\0';
                        cstruct_name_count++;
                    }
                }

                p = after_cstruct;
                continue;
            }

            // 检查 export face
            if (strncmp(after_export, "face", 4) == 0 && (after_export[4] == ' ' || after_export[4] == '\t' || after_export[4] == '\n' || after_export[4] == '\r' || after_export[4] == '\0')) {
                char* after_face = after_export + 4;
                while (*after_face && (*after_face == ' ' || *after_face == '\t')) after_face++;

                const char* name_start = after_face;
                while (*after_face && (isalnum((unsigned char)*after_face) || *after_face == '_')) after_face++;
                int name_len = (int)(after_face - name_start);

                if (name_len > 0 && name_len < 64) {
                    if (face_name_count >= MOD_MAX_TYPES) {
                        fprintf(stderr, "[错误] face数量超过上限 %d\n", MOD_MAX_TYPES);
                    } else {
                        face_names[face_name_count] = (char*)malloc(name_len + 1);
                        strncpy(face_names[face_name_count], name_start, name_len);
                        face_names[face_name_count][name_len] = '\0';
                        face_name_count++;
                    }
                }

                p = after_face;
                continue;
            }

            // 检查 export clib
            if (strncmp(after_export, "clib", 4) == 0 && (after_export[4] == ' ' || after_export[4] == '\t' || after_export[4] == '\n' || after_export[4] == '\r' || after_export[4] == '\0')) {
                char* after_clib = after_export + 4;
                while (*after_clib && (*after_clib == ' ' || *after_clib == '\t')) after_clib++;

                const char* name_start = after_clib;
                while (*after_clib && (isalnum((unsigned char)*after_clib) || *after_clib == '_')) after_clib++;
                int name_len = (int)(after_clib - name_start);

                if (name_len > 0 && name_len < 64) {
                    if (clib_name_count >= MOD_MAX_CLIBS) {
                        fprintf(stderr, "[错误] clib数量超过上限 %d\n", MOD_MAX_CLIBS);
                    } else {
                        clib_names[clib_name_count] = (char*)malloc(name_len + 1);
                        strncpy(clib_names[clib_name_count], name_start, name_len);
                        clib_names[clib_name_count][name_len] = '\0';
                        clib_name_count++;
                    }
                }

                p = after_clib;
                continue;
            }

            // 检查 export enum
            if (strncmp(after_export, "enum", 4) == 0 && (after_export[4] == ' ' || after_export[4] == '\t' || after_export[4] == '\n' || after_export[4] == '\r' || after_export[4] == '\0')) {
                char* after_enum = after_export + 4;
                while (*after_enum && (*after_enum == ' ' || *after_enum == '\t')) after_enum++;

                const char* name_start = after_enum;
                while (*after_enum && (isalnum((unsigned char)*after_enum) || *after_enum == '_')) after_enum++;
                int name_len = (int)(after_enum - name_start);

                if (name_len > 0 && name_len < 64) {
                    if (enum_name_count >= MOD_MAX_ENUMS) {
                        fprintf(stderr, "[错误] enum数量超过上限 %d\n", MOD_MAX_ENUMS);
                    } else {
                        enum_names[enum_name_count] = (char*)malloc(name_len + 1);
                        strncpy(enum_names[enum_name_count], name_start, name_len);
                        enum_names[enum_name_count][name_len] = '\0';
                        enum_name_count++;
                    }
                }

                p = after_enum;
                continue;
            }
        }

        // 非 export 行也收集 clib 并解析函数签名（模块内的 clib 定义不带 export）
        if (strncmp(p, "clib", 4) == 0 && (p[4] == ' ' || p[4] == '\t' || p[4] == '\n' || p[4] == '\r')) {
            char* after_clib = p + 4;
            while (*after_clib && (*after_clib == ' ' || *after_clib == '\t')) after_clib++;

            const char* name_start = after_clib;
            while (*after_clib && (isalnum((unsigned char)*after_clib) || *after_clib == '_')) after_clib++;
            int name_len = (int)(after_clib - name_start);

            if (name_len > 0 && name_len < 64) {
                char clib_name_buf[64];
                strncpy(clib_name_buf, name_start, name_len);
                clib_name_buf[name_len] = '\0';

                // 收集名称
                if (clib_name_count >= MOD_MAX_CLIBS) {
                    fprintf(stderr, "[错误] clib数量超过上限 %d\n", MOD_MAX_CLIBS);
                } else {
                    int already = 0;
                    for (int ci = 0; ci < clib_name_count; ci++) {
                        if (strcmp(clib_names[ci], clib_name_buf) == 0) { already = 1; break; }
                    }
                    if (!already) {
                        clib_names[clib_name_count] = strdup(clib_name_buf);
                        clib_name_count++;
                    }
                }

                // 查找 { 并解析函数签名
                while (*after_clib && *after_clib != '{' && *after_clib != '\n') after_clib++;
                if (*after_clib == '{') {
                    after_clib++;
                    ModuleClibFuncSymbol funcs[MOD_MAX_CLIB_FUNCS];
                    int func_count = 0;

                    while (*after_clib && *after_clib != '}' && func_count < MOD_MAX_CLIB_FUNCS) {
                        while (*after_clib && (*after_clib == ' ' || *after_clib == '\t' || *after_clib == '\n' || *after_clib == '\r')) after_clib++;
                        if (*after_clib == '}' || *after_clib == '\0') break;
                        if (*after_clib == '/' && *(after_clib+1) == '/') {
                            while (*after_clib && *after_clib != '\n') after_clib++;
                            continue;
                        }

                        // 解析返回类型
                        const char* ret_start = after_clib;
                        while (*after_clib && (isalnum((unsigned char)*after_clib) || *after_clib == '_')) after_clib++;
                        int ret_len = (int)(after_clib - ret_start);
                        if (ret_len <= 0) break;

                        TypeKind ret_type = TYPE_I32; // 默认
                        if (ret_len < 64) {
                            char ret_buf[64]; strncpy(ret_buf, ret_start, ret_len); ret_buf[ret_len] = '\0';
                            ret_type = parse_base_type(ret_buf);
                        }

                        while (*after_clib && (*after_clib == ' ' || *after_clib == '\t')) after_clib++;

                        // 解析函数名
                        const char* func_name_start = after_clib;
                        while (*after_clib && (isalnum((unsigned char)*after_clib) || *after_clib == '_')) after_clib++;
                        int func_name_len = (int)(after_clib - func_name_start);
                        if (func_name_len <= 0) break;

                        funcs[func_count].name = (char*)malloc(func_name_len + 1);
                        strncpy(funcs[func_count].name, func_name_start, func_name_len);
                        funcs[func_count].name[func_name_len] = '\0';
                        funcs[func_count].return_type = ret_type;
                        funcs[func_count].return_struct_name = NULL;

                        while (*after_clib && (*after_clib == ' ' || *after_clib == '\t')) after_clib++;

                        // 解析参数列表 (...)
                        TypeKind param_types[MOD_MAX_PARAMS]; char* param_structs[MOD_MAX_PARAMS];
                        int param_count = 0;
                        if (*after_clib == '(') {
                            after_clib++;
                            while (*after_clib && *after_clib != ')') {
                                while (*after_clib && (*after_clib == ' ' || *after_clib == '\t' || *after_clib == ',' || *after_clib == '\n' || *after_clib == '\r')) after_clib++;
                                if (*after_clib == ')' || *after_clib == '\0') break;

                                const char* pt_start = after_clib;
                                while (*after_clib && (isalnum((unsigned char)*after_clib) || *after_clib == '_')) after_clib++;
                                int pt_len = (int)(after_clib - pt_start);
                                if (pt_len > 0 && pt_len < 64) {
                                    if (param_count >= MOD_MAX_PARAMS) {
                                        fprintf(stderr, "[错误] clib函数参数数量超过上限 %d\n", MOD_MAX_PARAMS);
                                    } else {
                                        char pt_buf[64]; strncpy(pt_buf, pt_start, pt_len); pt_buf[pt_len] = '\0';
                                        param_types[param_count] = parse_base_type(pt_buf);
                                        param_structs[param_count] = NULL;
                                        param_count++;
                                    }
                                }
                                while (*after_clib && (*after_clib == ' ' || *after_clib == '\t')) after_clib++;
                                // 跳过参数名
                                while (*after_clib && (isalnum((unsigned char)*after_clib) || *after_clib == '_')) after_clib++;
                            }
                            if (*after_clib == ')') after_clib++;
                        }

                        funcs[func_count].param_count = param_count;
                        funcs[func_count].param_types = (TypeKind*)malloc(sizeof(TypeKind) * param_count);
                        funcs[func_count].param_struct_names = (char**)calloc(param_count, sizeof(char*));
                        for (int pi = 0; pi < param_count; pi++) {
                            funcs[func_count].param_types[pi] = param_types[pi];
                            funcs[func_count].param_struct_names[pi] = param_structs[pi]
                                ? strdup(param_structs[pi]) : NULL;
                        }
                        func_count++;

                        // 跳到下一行
                        while (*after_clib && *after_clib != '\n' && *after_clib != '}') after_clib++;
                        if (*after_clib == '\n') after_clib++;
                    }
                    if (*after_clib == '}') after_clib++;

                    // 存储 clib 符号
                    module_symbol_table_add_clib(table, clib_name_buf, func_count, funcs);

                    // 释放临时数据
                    for (int fi = 0; fi < func_count; fi++) {
                        free(funcs[fi].name);
                        for (int pi = 0; pi < funcs[fi].param_count; pi++) {
                            free(funcs[fi].param_struct_names[pi]);
                        }
                        free(funcs[fi].param_types);
                        free(funcs[fi].param_struct_names);
                    }
                }
            }

            p = after_clib;
            continue;
        }

        p++;
    }

    // 第二遍：解析所有符号（struct 和函数）
    p = source;

    // 本地别名解析表（支持 alias B = A 链式引用）
    char* local_alias_names[MOD_MAX_ALIASES] = {0};
    TypeInfo* local_alias_types[MOD_MAX_ALIASES] = {0};
    int local_alias_count = 0;

    // 注意：不再自动收集 import 模块的 export alias 到本地别名表
    // alias 必须通过 use 语句显式导入才能使用

    // 查找本地别名
    #define FIND_LOCAL_ALIAS(n) ({ \
        TypeInfo* _r = NULL; \
        for (int _i = 0; _i < local_alias_count; _i++) { \
            if (strcmp(local_alias_names[_i], n) == 0) { _r = local_alias_types[_i]; break; } \
        } \
        _r; \
    })

    while (*p) {
        // 跳过空白、注释、字符串
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
        if (!*p) break;

        if (*p == '/' && *(p+1) == '/') {
            while (*p && *p != '\n') p++;
            continue;
        }
        if (*p == '/' && *(p+1) == '*') {
            p += 2;
            while (*p && !(*p == '*' && *(p+1) == '/')) p++;
            if (*p) p += 2;
            continue;
        }
        if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                if (*p == '\\' && *(p+1)) p += 2;
                else p++;
            }
            if (*p) p++;
            continue;
        }

        // 查找 use 语句（跨模块类型链式传递）
        if (strncmp(p, "use", 3) == 0 && (p[3] == ' ' || p[3] == '\t' || p[3] == '\n' || p[3] == '\r')) {
            p += 3;
            while (*p && (*p == ' ' || *p == '\t')) p++;
            
            // 读模块别名
            const char* alias_start = p;
            while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
            int alias_len = (int)(p - alias_start);
            
            if (alias_len > 0 && alias_len < 64 && *p == '.') {
                char alias_name[64];
                memcpy(alias_name, alias_start, alias_len);
                alias_name[alias_len] = '\0';
                p++; // skip '.'
                
                // 读类型名
                const char* type_start = p;
                while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
                int type_len = (int)(p - type_start);
                
                if (type_len > 0 && type_len < 64) {
                    char type_name[64];
                    memcpy(type_name, type_start, type_len);
                    type_name[type_len] = '\0';
                    
                    // 在源码中反向查找 import "path" as alias 来解析模块路径
                    char* import_search = source;
                    char resolved_path[1024] = {0};
                    while (import_search < p) {
                        if (strncmp(import_search, "import", 6) == 0 && (import_search[6] == ' ' || import_search[6] == '\t' || import_search[6] == '\"')) {
                            import_search += 6;
                            while (*import_search && (*import_search == ' ' || *import_search == '\t')) import_search++;
                            if (*import_search == '"') {
                                import_search++;
                                const char* path_start = import_search;
                                while (*import_search && *import_search != '"') import_search++;
                                int path_len = (int)(import_search - path_start);
                                import_search++; // skip closing "
                                
                                // 检查 as alias
                                while (*import_search && (*import_search == ' ' || *import_search == '\t')) import_search++;
                                if (strncmp(import_search, "as", 2) == 0 && (import_search[2] == ' ' || import_search[2] == '\t')) {
                                    import_search += 2;
                                    while (*import_search && (*import_search == ' ' || *import_search == '\t')) import_search++;
                                    const char* as_start = import_search;
                                    while (*import_search && (isalnum((unsigned char)*import_search) || *import_search == '_')) import_search++;
                                    int as_len = (int)(import_search - as_start);
                                    if (as_len == alias_len && strncmp(as_start, alias_name, alias_len) == 0) {
                                        // 找到匹配的 import
                                        if (path_len > 0 && path_len < 1023) {
                                            memcpy(resolved_path, path_start, path_len);
                                            resolved_path[path_len] = '\0';
                                        }
                                        break;
                                    }
                                }
                            }
                        }
                        if (*import_search) import_search++;
                    }
                    
                    // 扫描目标模块，查找类型
                    if (resolved_path[0]) {
                        ModuleSymbolTable* dep_table = module_symbol_table_create(resolved_path);
                        if (dep_table) {
                            // 使用已规范化的当前模块路径作为子模块扫描基准
                            if (module_symbol_table_scan_depth(dep_table, normalized_current, depth + 1) == 0) {
                                // 查找 struct
                                ModuleStructSymbol* s_sym = module_symbol_table_find_struct(dep_table, type_name);
                                ModuleFaceSymbol* f_sym_ptr = module_symbol_table_find_face(dep_table, type_name);
                                if (s_sym && !module_symbol_table_find_struct(table, type_name)) {
                                    module_symbol_table_add_struct(table, type_name,
                                        s_sym->field_count, s_sym->fields,
                                        s_sym->method_count, s_sym->methods,
                                        s_sym->is_cstruct, s_sym->type_param_count, s_sym->type_param_names);
                                    // 将 use 导入的 struct/cstruct 名称添加到对应数组，使后续 export func 返回类型解析能识别
                                    if (s_sym->is_cstruct) {
                                        if (cstruct_name_count >= MOD_MAX_TYPES) {
                                            fprintf(stderr, "[错误] cstruct数量超过上限 %d\n", MOD_MAX_TYPES);
                                        } else {
                                            cstruct_names[cstruct_name_count] = strdup(type_name);
                                            cstruct_name_count++;
                                        }
                                    } else {
                                        if (struct_name_count >= MOD_MAX_TYPES) {
                                            fprintf(stderr, "[错误] struct数量超过上限 %d\n", MOD_MAX_TYPES);
                                        } else {
                                            struct_names[struct_name_count] = strdup(type_name);
                                            struct_name_count++;
                                        }
                                    }
                                    if (s_sym->impl_count > 0) {
                                        ModuleStructSymbol* new_sym = module_symbol_table_find_struct(table, type_name);
                                        if (new_sym) {
                                            new_sym->impl_count = s_sym->impl_count;
                                            new_sym->impl_names = (char**)malloc(sizeof(char*) * s_sym->impl_count);
                                            for (int ii = 0; ii < s_sym->impl_count; ii++) {
                                                new_sym->impl_names[ii] = strdup(s_sym->impl_names[ii]);
                                            }
                                        }
                                    }
                                }
                                // 查找 face
                                ModuleFaceSymbol* f_sym = module_symbol_table_find_face(dep_table, type_name);
                                if (f_sym && !module_symbol_table_find_face(table, type_name)) {
                                    module_symbol_table_add_face(table, type_name,
                                        f_sym->method_count, f_sym->methods, f_sym->type_param_count);
                                    // 将 use 导入的 face 名称添加到 face_names，使后续 export func 返回类型解析能识别
                                    if (face_name_count >= MOD_MAX_TYPES) {
                                        fprintf(stderr, "[错误] face数量超过上限 %d\n", MOD_MAX_TYPES);
                                    } else {
                                        face_names[face_name_count] = strdup(type_name);
                                        face_name_count++;
                                    }
                                }
                                // 查找 clib 类型（通过函数返回类型识别）
                                if (!s_sym && !f_sym_ptr) {
                                    for (int fi = 0; fi < dep_table->func_count; fi++) {
                                        ModuleFuncSymbol* func = &dep_table->funcs[fi];
                                        if (func->return_type == TYPE_CLIB && func->return_struct_name &&
                                            strcmp(func->return_struct_name, type_name) == 0) {
                                            int already = 0;
                                            for (int ci = 0; ci < clib_name_count; ci++) {
                                                if (strcmp(clib_names[ci], type_name) == 0) { already = 1; break; }
                                            }
                                            if (!already) {
                                                if (clib_name_count >= MOD_MAX_CLIBS) {
                                                    fprintf(stderr, "[错误] clib数量超过上限 %d\n", MOD_MAX_CLIBS);
                                                } else {
                                                    clib_names[clib_name_count] = strdup(type_name);
                                                    clib_name_count++;
                                                }
                                            }
                                            break;
                                        }
                                    }
                                }
                                // 查找 enum 类型
                                if (!s_sym && !f_sym_ptr) {
                                    ModuleEnumSymbol* e_sym = module_symbol_table_find_enum(dep_table, type_name);
                                    if (e_sym) {
                                        int already = 0;
                                        for (int ei = 0; ei < enum_name_count; ei++) {
                                            if (strcmp(enum_names[ei], type_name) == 0) { already = 1; break; }
                                        }
                                        if (!already) {
                                            if (enum_name_count >= MOD_MAX_ENUMS) {
                                                fprintf(stderr, "[错误] enum数量超过上限 %d\n", MOD_MAX_ENUMS);
                                            } else {
                                                enum_names[enum_name_count] = strdup(type_name);
                                                enum_name_count++;
                                            }
                                        }
                                    }
                                }
                                // 查找 alias 类型
                                if (!s_sym && !f_sym_ptr) {
                                    ModuleAliasSymbol* a_sym = module_symbol_table_find_alias(dep_table, type_name);
                                    if (a_sym && !module_symbol_table_find_alias(table, type_name)) {
                                        module_symbol_table_add_alias(table, type_name,
                                            a_sym->type_info ? type_copy(a_sym->type_info) : NULL);
                                        // 同时添加到本地别名表，使后续方法返回类型解析能找到
                                        if (local_alias_count >= MOD_MAX_ALIASES) {
                                            fprintf(stderr, "[错误] 别名数量超过上限 %d\n", MOD_MAX_ALIASES);
                                        } else {
                                            local_alias_names[local_alias_count] = strdup(type_name);
                                            local_alias_types[local_alias_count] = a_sym->type_info ? type_copy(a_sym->type_info) : NULL;
                                            local_alias_count++;
                                        }
                                    }
                                }
                            }
                            module_symbol_table_destroy(dep_table);
                        }
                    }
                }
            }
            continue;
        }

        // 查找 export 关键字
        if (strncmp(p, "export", 6) == 0 && (p[6] == ' ' || p[6] == '\t' || p[6] == '\n' || p[6] == '\r' || p[6] == '\0')) {
            char* after_export = p + 6;
            while (*after_export && (*after_export == ' ' || *after_export == '\t' || *after_export == '\n' || *after_export == '\r')) after_export++;

            // 检查是否是 struct 定义
            if (strncmp(after_export, "struct", 6) == 0 && (after_export[6] == ' ' || after_export[6] == '\t' || after_export[6] == '\n' || after_export[6] == '\r' || after_export[6] == '\0')) {
                char* after_struct = after_export + 6;
                while (*after_struct && (*after_struct == ' ' || *after_struct == '\t')) after_struct++;

                const char* name_start = after_struct;
                while (*after_struct && (isalnum((unsigned char)*after_struct) || *after_struct == '_')) after_struct++;
                int name_len = (int)(after_struct - name_start);

                if (name_len > 0 && name_len < 64) {
                    char struct_name[64];
                    strncpy(struct_name, name_start, name_len);
                    struct_name[name_len] = '\0';

                    // 解析泛型参数 [T] 或 [K, V] 或 [T: Face, K]
                    int type_param_count = 0;
                    char* type_param_names[MOD_MAX_TYPE_PARAMS] = {NULL};
                    while (*after_struct && (*after_struct == ' ' || *after_struct == '\t')) after_struct++;
                    if (*after_struct == '[') {
                        after_struct++; // skip '['
                        while (*after_struct && *after_struct != ']') {
                            while (*after_struct && (*after_struct == ' ' || *after_struct == '\t')) after_struct++;
                            if (*after_struct && (isalnum((unsigned char)*after_struct) || *after_struct == '_')) {
                                const char* tp_name_start = after_struct;
                                while (*after_struct && (isalnum((unsigned char)*after_struct) || *after_struct == '_')) after_struct++;
                                int tp_name_len = (int)(after_struct - tp_name_start);
                                if (tp_name_len > 0 && tp_name_len < 64 && type_param_count < MOD_MAX_TYPE_PARAMS) {
                                    type_param_names[type_param_count] = (char*)malloc(tp_name_len + 1);
                                    strncpy(type_param_names[type_param_count], tp_name_start, tp_name_len);
                                    type_param_names[type_param_count][tp_name_len] = '\0';
                                }
                                type_param_count++;
                            }
                            while (*after_struct && (*after_struct == ' ' || *after_struct == '\t')) after_struct++;
                            // 跳过约束语法 T: FaceName
                            if (*after_struct == ':') {
                                after_struct++; // skip ':'
                                while (*after_struct && (*after_struct == ' ' || *after_struct == '\t')) after_struct++;
                                while (*after_struct && (isalnum((unsigned char)*after_struct) || *after_struct == '_')) after_struct++;
                            }
                            if (*after_struct == ',') after_struct++;
                        }
                        if (*after_struct == ']') after_struct++; // skip ']'
                    }

                    while (*after_struct && (*after_struct == ' ' || *after_struct == '\t' || *after_struct == '\n' || *after_struct == '\r')) after_struct++;

                    // 解析 impl 声明
                    char* impl_names[MOD_MAX_TYPES];
                    int impl_count = 0;
                    if (strncmp(after_struct, "impl", 4) == 0 && (after_struct[4] == ' ' || after_struct[4] == '\t')) {
                        after_struct += 4;
                        while (*after_struct && (*after_struct == ' ' || *after_struct == '\t')) after_struct++;

                        // 解析 face 名称（支持多个，用逗号分隔）
                        while (*after_struct && (isalnum((unsigned char)*after_struct) || *after_struct == '_')) {
                            const char* face_name_start = after_struct;
                            while (*after_struct && (isalnum((unsigned char)*after_struct) || *after_struct == '_')) after_struct++;
                            int face_name_len = (int)(after_struct - face_name_start);

                            if (face_name_len > 0 && face_name_len < 64 && impl_count < 64) {
                                impl_names[impl_count] = (char*)malloc(face_name_len + 1);
                                strncpy(impl_names[impl_count], face_name_start, face_name_len);
                                impl_names[impl_count][face_name_len] = '\0';
                                impl_count++;
                            }

                            // 跳过泛型参数 [TypeArg]（如 Comparable[int]）
                            while (*after_struct && (*after_struct == ' ' || *after_struct == '\t')) after_struct++;
                            if (*after_struct == '[') {
                                after_struct++; // skip '['
                                int bracket_depth = 1;
                                while (*after_struct && bracket_depth > 0) {
                                    if (*after_struct == '[') bracket_depth++;
                                    else if (*after_struct == ']') bracket_depth--;
                                    after_struct++;
                                }
                            }

                            // 跳过空白
                            while (*after_struct && (*after_struct == ' ' || *after_struct == '\t')) after_struct++;
                            // 跳过逗号
                            if (*after_struct == ',') {
                                after_struct++;
                                while (*after_struct && (*after_struct == ' ' || *after_struct == '\t')) after_struct++;
                            }
                        }

                        while (*after_struct && (*after_struct == ' ' || *after_struct == '\t' || *after_struct == '\n' || *after_struct == '\r')) after_struct++;
                    }

                    if (*after_struct == '{') {
                        after_struct++;

                        ModuleStructField fields[MOD_MAX_FIELDS];
                        ModuleStructMethod methods[MOD_MAX_METHODS];
                        int field_count = 0;
                        int method_count = 0;

                        while (*after_struct && *after_struct != '}' && (field_count < MOD_MAX_FIELDS || method_count < MOD_MAX_METHODS)) {
                            while (*after_struct && (*after_struct == ' ' || *after_struct == '\t' || *after_struct == '\n' || *after_struct == '\r')) after_struct++;
                            if (!*after_struct || *after_struct == '}') break;

                            if (*after_struct == '/' && *(after_struct+1) == '/') {
                                while (*after_struct && *after_struct != '\n') after_struct++;
                                continue;
                            }

                            // 检查是否是方法定义 (func 关键字)
                            if (strncmp(after_struct, "func", 4) == 0 && (after_struct[4] == ' ' || after_struct[4] == '\t')) {
                                char* after_func = after_struct + 4;
                                while (*after_func && (*after_func == ' ' || *after_func == '\t')) after_func++;

                                // 读取方法名
                                const char* method_name_start = after_func;
                                while (*after_func && (isalnum((unsigned char)*after_func) || *after_func == '_')) after_func++;
                                int method_name_len = (int)(after_func - method_name_start);

                                if (method_name_len > 0 && method_name_len < 64 && method_count < MOD_MAX_METHODS) {
                                    char method_name[64];
                                    strncpy(method_name, method_name_start, method_name_len);
                                    method_name[method_name_len] = '\0';

                                    // 使用 struct_name::method_name 格式，避免与全局函数冲突
                                    char method_key[256];
                                    snprintf(method_key, sizeof(method_key), "%s::%s", struct_name, method_name);
                                    int method_key_len = (int)strlen(method_key);

                                    // 解析参数列表
                                    int param_count = 0;
                                    TypeKind param_types[MOD_MAX_PARAMS] = {0};
                                    char param_struct_names[MOD_MAX_PARAMS][MOD_MAX_NAMES] = {{0}};
                                    char* param_generic_names[MOD_MAX_PARAMS] = {NULL};  // 泛型参数名（如 T, K, V）

                                    while (*after_func && (*after_func == ' ' || *after_func == '\t')) after_func++;
                                    if (*after_func == '(') {
                                        after_func++;
                                        int paren_depth = 1;
                                        int has_param = 0;

                                        while (*after_func && paren_depth > 0) {
                                            if (*after_func == '(') {
                                                paren_depth++;
                                                after_func++;
                                            } else if (*after_func == ')') {
                                                paren_depth--;
                                                if (paren_depth == 0) {
                                                    after_func++;
                                                    break;
                                                }
                                                after_func++;
                                            } else if (paren_depth == 1) {
                                                // 解析参数：type name 或 name（无类型）
                                                while (*after_func && (*after_func == ' ' || *after_func == '\t' || *after_func == '\n' || *after_func == '\r')) after_func++;
                                                if (*after_func == ')' || *after_func == '\0') continue;

                                                // 检查是否是类型注解（简单启发式：如果看起来像类型，则解析为类型）
                                                const char* token_start = after_func;
                                                while (*after_func && (isalnum((unsigned char)*after_func) || *after_func == '_')) after_func++;
                                                int token_len = (int)(after_func - token_start);

                                                if (token_len > 0 && token_len < 64) {
                                                    char token[64];
                                                    strncpy(token, token_start, token_len);
                                                    token[token_len] = '\0';

                                                    // 检查是否是已知类型
                                                    TypeKind param_type = parse_base_type(token);
                                                    char param_struct_name[64] = {0};
                                                    char* param_generic_name = NULL;

                                                    // 如果不是基本类型，检查是否是已知的 clib/struct/cstruct
                                                    if (param_type == TYPE_ANY) {
                                                        if (is_known_clib(token, clib_names, clib_name_count)) {
                                                            param_type = TYPE_CLIB;
                                                            size_t copy_len = strlen(token);
                                                            if (copy_len > sizeof(param_struct_name) - 1) copy_len = sizeof(param_struct_name) - 1;
                                                            memcpy(param_struct_name, token, copy_len);
                                                            param_struct_name[copy_len] = '\0';
                                                        } else if (is_known_face(token, face_names, face_name_count)) {
                                                            param_type = TYPE_FACE;
                                                            size_t copy_len = strlen(token);
                                                            if (copy_len > sizeof(param_struct_name) - 1) copy_len = sizeof(param_struct_name) - 1;
                                                            memcpy(param_struct_name, token, copy_len);
                                                            param_struct_name[copy_len] = '\0';
                                                        } else if (is_known_cstruct(token, cstruct_names, cstruct_name_count)) {
                                                            param_type = TYPE_CSTRUCT;
                                                            size_t copy_len = strlen(token);
                                                            if (copy_len > sizeof(param_struct_name) - 1) copy_len = sizeof(param_struct_name) - 1;
                                                            memcpy(param_struct_name, token, copy_len);
                                                            param_struct_name[copy_len] = '\0';
                                                        } else if (is_known_struct(token, struct_names, struct_name_count)) {
                                                            param_type = TYPE_STRUCT;
                                                            size_t copy_len = strlen(token);
                                                            if (copy_len > sizeof(param_struct_name) - 1) copy_len = sizeof(param_struct_name) - 1;
                                                            memcpy(param_struct_name, token, copy_len);
                                                            param_struct_name[copy_len] = '\0';
                                                        }
                                                    }

                                                    // 检查是否是泛型类型参数（如 T, K, V）
                                                    for (int tpi = 0; tpi < type_param_count; tpi++) {
                                                        if (type_param_names[tpi] && strcmp(token, type_param_names[tpi]) == 0) {
                                                            param_type = TYPE_GENERIC_PARAM;
                                                            param_generic_name = strdup(type_param_names[tpi]);
                                                            break;
                                                        }
                                                    }

                                                    // 跳过空白
                                                    while (*after_func && (*after_func == ' ' || *after_func == '\t')) after_func++;

                                                    // 如果后面跟着标识符，说明 token 是类型
                                                    if (*after_func && (isalnum((unsigned char)*after_func) || *after_func == '_')) {
                                                        // token 是类型，跳过参数名
                                                        while (*after_func && (isalnum((unsigned char)*after_func) || *after_func == '_')) after_func++;
                                                        has_param = 1;
                                                    } else if (*after_func == ',' || *after_func == ')' || *after_func == '=') {
                                                        // token 是参数名（无类型），使用 TYPE_ANY
                                                        param_type = TYPE_ANY;
                                                        has_param = 1;
                                                    }

                                                    if (has_param && param_count < MOD_MAX_PARAMS) {
                                                        param_types[param_count] = param_type;
                                                        param_generic_names[param_count] = param_generic_name;
                                                        param_generic_name = NULL;  // 转移所有权
                                                        if (param_struct_name[0]) {
                                                            memcpy(param_struct_names[param_count], param_struct_name, MOD_MAX_NAMES);
                                                            param_struct_names[param_count][MOD_MAX_NAMES - 1] = '\0';
                                                        }
                                                        param_count++;
                                                    } else if (has_param && param_count >= MOD_MAX_PARAMS) {
                                                        fprintf(stderr, "[错误] 函数参数数量超过上限 %d\n", MOD_MAX_PARAMS);
                                                    } else if (param_generic_name) {
                                                        free(param_generic_name);
                                                        param_generic_name = NULL;
                                                    }

                                                    // 跳过默认值（如果有）
                                                    while (*after_func && (*after_func == ' ' || *after_func == '\t')) after_func++;
                                                    if (*after_func == '=') {
                                                        after_func++;
                                                        while (*after_func && *after_func != ',' && *after_func != ')' && paren_depth == 1) {
                                                            if (*after_func == '(') paren_depth++;
                                                            else if (*after_func == ')') paren_depth--;
                                                            else after_func++;
                                                        }
                                                    }

                                                    // 跳过逗号
                                                    if (*after_func == ',') {
                                                        after_func++;
                                                        has_param = 0;
                                                    }
                                                } else {
                                                    after_func++;
                                                }
                                            } else {
                                                after_func++;
                                            }
                                        }

                                        // 跳过空白
                                        while (*after_func && (*after_func == ' ' || *after_func == '\t' || *after_func == '\n' || *after_func == '\r')) after_func++;

                                        // 解析返回类型
                                        TypeKind method_return_type = TYPE_ANY;
                                        TypeInfo* method_return_type_info = NULL;
                                        char method_return_struct[64] = {0};
                                        char method_return_type_param[64] = {0};
                                        int method_return_generic_count = 0;
                                        char* method_return_generic_params[MOD_MAX_GENERIC_RET] = {NULL};

                                        if (*after_func == ':') {
                                            after_func++;
                                            while (*after_func && (*after_func == ' ' || *after_func == '\t')) after_func++;

                                            const char* ret_type_start = after_func;
                                            // 先读取标识符部分
                                            while (*after_func && (isalnum((unsigned char)*after_func) || *after_func == '_')) after_func++;
                                            int ret_type_len = (int)(after_func - ret_type_start);

                                            // 继续读取泛型参数部分（如 [string,float]）
                                            while (*after_func && (*after_func == ' ' || *after_func == '\t')) after_func++;
                                            if (*after_func == '[') {
                                                int depth = 0;
                                                while (*after_func) {
                                                    if (*after_func == '[') depth++;
                                                    else if (*after_func == ']') { depth--; if (depth == 0) { after_func++; break; } }
                                                    after_func++;
                                                }
                                                ret_type_len = (int)(after_func - ret_type_start);
                                            }

                                            if (ret_type_len > 0 && ret_type_len < 128) {
                                                char ret_type_str[128];
                                                strncpy(ret_type_str, ret_type_start, ret_type_len);
                                                ret_type_str[ret_type_len] = '\0';

                                                // 先尝试从本地别名表解析
                                                TypeInfo* alias_ti = FIND_LOCAL_ALIAS(ret_type_str);
                                                if (alias_ti) {
                                                    method_return_type_info = type_copy(alias_ti);
                                                    method_return_type = method_return_type_info->kind;
                                                } else {
                                                    // 解析完整类型信息
                                                    method_return_type_info = parse_type_from_string(ret_type_str);
                                                    if (method_return_type_info) {
                                                        method_return_type = method_return_type_info->kind;
                                                    } else {
                                                        method_return_type = parse_base_type(ret_type_str);
                                                    }
                                                }

                                                // 用标识符部分检查
                                                char base_str[64];
                                                int base_len = ret_type_len < 64 ? ret_type_len : 63;
                                                strncpy(base_str, ret_type_str, base_len);
                                                base_str[base_len] = '\0';
                                                char* bracket = strchr(base_str, '[');
                                                if (bracket) *bracket = '\0';

                                                if (method_return_type == TYPE_ANY || method_return_type == TYPE_STRUCT) {
                                                    // 检查泛型参数名
                                                    int is_type_param = 0;
                                                    for (int tpi = 0; tpi < type_param_count; tpi++) {
                                                        if (type_param_names[tpi] && strcmp(base_str, type_param_names[tpi]) == 0) {
                                                            is_type_param = 1;
                                                            memcpy(method_return_type_param, base_str, strlen(base_str) + 1);
                                                            method_return_type = TYPE_GENERIC_PARAM;
                                                            if (method_return_type_info) { type_free(method_return_type_info); method_return_type_info = NULL; }
                                                            break;
                                                        }
                                                    }
                                                    if (!is_type_param) {
                                                        if (is_known_clib(base_str, clib_names, clib_name_count)) {
                                                            method_return_type = TYPE_CLIB;
                                                            if (method_return_type_info) method_return_type_info->kind = TYPE_CLIB;
                                                            size_t copy_len = strlen(base_str);
                                                            if (copy_len > sizeof(method_return_struct) - 1) copy_len = sizeof(method_return_struct) - 1;
                                                            memcpy(method_return_struct, base_str, copy_len);
                                                            method_return_struct[copy_len] = '\0';
                                                        } else if (is_known_face(base_str, face_names, face_name_count)) {
                                                            method_return_type = TYPE_FACE;
                                                            if (method_return_type_info) method_return_type_info->kind = TYPE_FACE;
                                                            size_t copy_len = strlen(base_str);
                                                            if (copy_len > sizeof(method_return_struct) - 1) copy_len = sizeof(method_return_struct) - 1;
                                                            memcpy(method_return_struct, base_str, copy_len);
                                                            method_return_struct[copy_len] = '\0';
                                                        } else if (is_known_cstruct(base_str, cstruct_names, cstruct_name_count)) {
                                                            method_return_type = TYPE_CSTRUCT;
                                                            if (method_return_type_info) method_return_type_info->kind = TYPE_CSTRUCT;
                                                            size_t copy_len = strlen(base_str);
                                                            if (copy_len > sizeof(method_return_struct) - 1) copy_len = sizeof(method_return_struct) - 1;
                                                            memcpy(method_return_struct, base_str, copy_len);
                                                            method_return_struct[copy_len] = '\0';
                                                        } else if (is_known_struct(base_str, struct_names, struct_name_count)) {
                                                            method_return_type = TYPE_STRUCT;
                                                            size_t copy_len = strlen(base_str);
                                                            if (copy_len > sizeof(method_return_struct) - 1) copy_len = sizeof(method_return_struct) - 1;
                                                            memcpy(method_return_struct, base_str, copy_len);
                                                            method_return_struct[copy_len] = '\0';
                                                        }
                                                    }
                                                }

                                                // 解析泛型返回类型的类型参数（如 Holder[K] 中的 [K]），仅用于非 Dict/Array
                                                if (!method_return_type_info && method_return_type != TYPE_DICT && method_return_type != TYPE_ARRAY) {
                                                    char* gen_bracket = strchr(ret_type_str, '[');
                                                    if (gen_bracket && (method_return_type == TYPE_STRUCT || method_return_type == TYPE_PTR)) {
                                                        gen_bracket++;
                                                        while (*gen_bracket && *gen_bracket != ']') {
                                                            while (*gen_bracket && (*gen_bracket == ' ' || *gen_bracket == '\t')) gen_bracket++;
                                                            if (*gen_bracket && (isalnum((unsigned char)*gen_bracket) || *gen_bracket == '_')) {
                                                                const char* gp_start = gen_bracket;
                                                                while (*gen_bracket && (isalnum((unsigned char)*gen_bracket) || *gen_bracket == '_')) gen_bracket++;
                                                                int gp_len = (int)(gen_bracket - gp_start);
                                                                if (gp_len > 0 && gp_len < 64 && method_return_generic_count < 8) {
                                                                    method_return_generic_params[method_return_generic_count] = (char*)malloc(gp_len + 1);
                                                                    strncpy(method_return_generic_params[method_return_generic_count], gp_start, gp_len);
                                                                    method_return_generic_params[method_return_generic_count][gp_len] = '\0';
                                                                    method_return_generic_count++;
                                                                }
                                                            }
                                                            while (*gen_bracket && *gen_bracket != ',' && *gen_bracket != ']') gen_bracket++;
                                                            if (*gen_bracket == ',') gen_bracket++;
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        // 跳过方法体
                                        while (*after_func && (*after_func == ' ' || *after_func == '\t' || *after_func == '\n' || *after_func == '\r')) after_func++;
                                        if (*after_func == '{') {
                                            after_func++;
                                            int brace_depth = 1;
                                            while (*after_func && brace_depth > 0) {
                                                if (*after_func == '{') brace_depth++;
                                                else if (*after_func == '}') brace_depth--;
                                                after_func++;
                                            }
                                        }

                                        // 添加方法（使用 struct_name::method_name 格式）
                                        methods[method_count].name = (char*)malloc(method_key_len + 1);
                                        memcpy(methods[method_count].name, method_key, method_key_len);
                                        methods[method_count].name[method_key_len] = '\0';
                                        methods[method_count].return_type = method_return_type;
                                        methods[method_count].return_type_info = method_return_type_info ? type_copy(method_return_type_info) : NULL;
                                        // 释放临时 method_return_type_info（已复制到 methods 数组）
                                        if (method_return_type_info) { type_free(method_return_type_info); method_return_type_info = NULL; }
                                        methods[method_count].return_struct_name = method_return_struct[0] ? strdup(method_return_struct) : NULL;
                                        methods[method_count].return_type_param_name = method_return_type_param[0] ? strdup(method_return_type_param) : NULL;
                                        methods[method_count].return_generic_count = method_return_generic_count;
                                        methods[method_count].return_generic_param_names = NULL;
                                        if (method_return_generic_count > 0) {
                                            methods[method_count].return_generic_param_names = (char**)malloc(sizeof(char*) * method_return_generic_count);
                                            for (int gi = 0; gi < method_return_generic_count; gi++) {
                                                methods[method_count].return_generic_param_names[gi] = method_return_generic_params[gi];
                                                method_return_generic_params[gi] = NULL; // 转移所有权
                                            }
                                        }
                                        methods[method_count].param_count = param_count;
                                        methods[method_count].param_types = NULL;
                                        methods[method_count].param_generic_names = NULL;
                                        if (param_count > 0) {
                                            methods[method_count].param_types = (TypeKind*)malloc(sizeof(TypeKind) * param_count);
                                            for (int pi = 0; pi < param_count; pi++) {
                                                methods[method_count].param_types[pi] = param_types[pi];
                                            }
                                            // 设置泛型参数名
                                            int has_generic = 0;
                                            for (int pi = 0; pi < param_count; pi++) {
                                                if (param_generic_names[pi]) has_generic = 1;
                                            }
                                            if (has_generic) {
                                                methods[method_count].param_generic_names = (char**)malloc(sizeof(char*) * param_count);
                                                for (int pi = 0; pi < param_count; pi++) {
                                                    methods[method_count].param_generic_names[pi] = param_generic_names[pi];
                                                    param_generic_names[pi] = NULL;  // 转移所有权
                                                }
                                            }
                                        }
                                        if (method_count >= MOD_MAX_METHODS) {
                                            fprintf(stderr, "[错误] struct方法数量超过上限 %d\n", MOD_MAX_METHODS);
                                        } else {
                                            method_count++;
                                        }
                                    }
                                }

                                after_struct = after_func;
                                continue;
                            }

                            // 解析字段
                            const char* type_start = after_struct;
                            while (*after_struct && (isalnum((unsigned char)*after_struct) || *after_struct == '_')) after_struct++;
                            int type_len = (int)(after_struct - type_start);

                            if (type_len > 0 && type_len < 64) {
                                char type_str[64];
                                strncpy(type_str, type_start, type_len);
                                type_str[type_len] = '\0';

                                // 检查是否是 Ptr[T] 类型
                                TypeKind element_type = TYPE_PTR;  // 默认元素类型
                                while (*after_struct && (*after_struct == ' ' || *after_struct == '\t')) after_struct++;
                                if (*after_struct == '[') {
                                    // 解析 Ptr[T] 中的 T
                                    after_struct++;
                                    while (*after_struct && (*after_struct == ' ' || *after_struct == '\t')) after_struct++;
                                    const char* elem_type_start = after_struct;
                                    while (*after_struct && (isalnum((unsigned char)*after_struct) || *after_struct == '_')) after_struct++;
                                    int elem_type_len = (int)(after_struct - elem_type_start);
                                    if (elem_type_len > 0 && elem_type_len < 64) {
                                        char elem_type_str[64];
                                        strncpy(elem_type_str, elem_type_start, elem_type_len);
                                        elem_type_str[elem_type_len] = '\0';
                                        element_type = parse_base_type(elem_type_str);
                                    }
                                    // 跳过到 ] 为止的内容
                                    while (*after_struct && *after_struct != ']') after_struct++;
                                    if (*after_struct == ']') after_struct++;
                                }

                                while (*after_struct && (*after_struct == ' ' || *after_struct == '\t')) after_struct++;

                                const char* field_start = after_struct;
                                while (*after_struct && (isalnum((unsigned char)*after_struct) || *after_struct == '_')) after_struct++;
                                int field_len = (int)(after_struct - field_start);

                                if (field_len > 0 && field_len < 64 && field_count < MOD_MAX_FIELDS) {
                                    TypeKind ft = parse_base_type(type_str);
                                    fields[field_count].name = (char*)malloc(field_len + 1);
                                    strncpy(fields[field_count].name, field_start, field_len);
                                    fields[field_count].name[field_len] = '\0';
                                    fields[field_count].type = ft;
                                    fields[field_count].struct_name = NULL;
                                    // 如果是未识别的自定义类型，检查是否是 clib
                                    if (ft == TYPE_ANY) {
                                        if (is_known_clib(type_str, clib_names, clib_name_count)) {
                                            fields[field_count].type = TYPE_CLIB;
                                            fields[field_count].struct_name = strdup(type_str);
                                        } else if (is_known_struct(type_str, struct_names, struct_name_count)) {
                                            fields[field_count].type = TYPE_STRUCT;
                                            fields[field_count].struct_name = strdup(type_str);
                                        }
                                    }
                                    fields[field_count].element_type = element_type;
                                    if (field_count >= MOD_MAX_FIELDS) {
                                        fprintf(stderr, "[错误] struct字段数量超过上限 %d\n", MOD_MAX_FIELDS);
                                    } else {
                                        field_count++;
                                    }
                                }
                            }

                            while (*after_struct && *after_struct != '\n' && *after_struct != '}' && *after_struct != ';') after_struct++;
                            if (*after_struct == ';') after_struct++;
                        }

                        // 添加 struct 到符号表（包括字段和方法）
                        module_symbol_table_add_struct(table, struct_name, field_count, fields, method_count, methods, 0, type_param_count, type_param_names);

                        // 设置 impl 信息
                        ModuleStructSymbol* added_struct = module_symbol_table_find_struct(table, struct_name);
                        if (added_struct && impl_count > 0) {
                            added_struct->impl_count = impl_count;
                            added_struct->impl_names = (char**)malloc(sizeof(char*) * impl_count);
                            for (int i = 0; i < impl_count; i++) {
                                added_struct->impl_names[i] = impl_names[i];
                            }
                        } else {
                            // 释放临时分配的 impl_names
                            for (int i = 0; i < impl_count; i++) {
                                free(impl_names[i]);
                            }
                        }

                        // 释放临时分配的内存
                        for (int i = 0; i < type_param_count; i++) {
                            free(type_param_names[i]);
                        }
                        for (int i = 0; i < field_count; i++) {
                            free(fields[i].name);
                        }
                        for (int i = 0; i < method_count; i++) {
                            free(methods[i].name);
                            free(methods[i].return_struct_name);
                            if (methods[i].return_type_info) {
                                type_free(methods[i].return_type_info);
                            }
                            free(methods[i].return_type_param_name);
                            for (int gi = 0; gi < methods[i].return_generic_count; gi++) {
                                free(methods[i].return_generic_param_names[gi]);
                            }
                            free(methods[i].return_generic_param_names);
                            free(methods[i].param_types);
                            // 释放泛型参数名
                            if (methods[i].param_generic_names) {
                                for (int pi = 0; pi < methods[i].param_count; pi++) {
                                    free(methods[i].param_generic_names[pi]);
                                }
                                free(methods[i].param_generic_names);
                            }
                        }

                        while (*after_struct && *after_struct != '}') after_struct++;
                        if (*after_struct == '}') after_struct++;
                    }
                }

                p = after_struct;
                continue;
            }

            // 检查是否是 cstruct 定义
            if (strncmp(after_export, "cstruct", 7) == 0 && (after_export[7] == ' ' || after_export[7] == '\t' || after_export[7] == '\n' || after_export[7] == '\r' || after_export[7] == '\0')) {
                char* after_cstruct = after_export + 7;
                while (*after_cstruct && (*after_cstruct == ' ' || *after_cstruct == '\t')) after_cstruct++;

                const char* name_start = after_cstruct;
                while (*after_cstruct && (isalnum((unsigned char)*after_cstruct) || *after_cstruct == '_')) after_cstruct++;
                int name_len = (int)(after_cstruct - name_start);

                if (name_len > 0 && name_len < 64) {
                    char cstruct_name[64];
                    strncpy(cstruct_name, name_start, name_len);
                    cstruct_name[name_len] = '\0';

                    while (*after_cstruct && (*after_cstruct == ' ' || *after_cstruct == '\t' || *after_cstruct == '\n' || *after_cstruct == '\r')) after_cstruct++;

                    if (*after_cstruct == '{') {
                        after_cstruct++;

                        ModuleStructField fields[MOD_MAX_FIELDS];
                        ModuleStructMethod methods[MOD_MAX_METHODS];
                        int field_count = 0;
                        int method_count = 0;

                        // cstruct 只解析字段，不解析方法（cstruct 用于 C 布局，不支持方法）
                        while (*after_cstruct && *after_cstruct != '}' && field_count < MOD_MAX_FIELDS) {
                            while (*after_cstruct && (*after_cstruct == ' ' || *after_cstruct == '\t' || *after_cstruct == '\n' || *after_cstruct == '\r')) after_cstruct++;
                            if (!*after_cstruct || *after_cstruct == '}') break;

                            if (*after_cstruct == '/' && *(after_cstruct+1) == '/') {
                                while (*after_cstruct && *after_cstruct != '\n') after_cstruct++;
                                continue;
                            }

                            // cstruct 不支持方法，跳过 func 定义
                            if (strncmp(after_cstruct, "func", 4) == 0 && (after_cstruct[4] == ' ' || after_cstruct[4] == '\t')) {
                                // 跳过整个 func 定义
                                char* after_func = after_cstruct + 4;
                                while (*after_func && (*after_func == ' ' || *after_func == '\t')) after_func++;
                                // 跳过方法名
                                while (*after_func && (isalnum((unsigned char)*after_func) || *after_func == '_')) after_func++;
                                // 跳过参数列表
                                while (*after_func && (*after_func == ' ' || *after_func == '\t')) after_func++;
                                if (*after_func == '(') {
                                    after_func++;
                                    int paren_depth = 1;
                                    while (*after_func && paren_depth > 0) {
                                        if (*after_func == '(') paren_depth++;
                                        else if (*after_func == ')') paren_depth--;
                                        after_func++;
                                    }
                                }
                                // 跳过返回类型
                                while (*after_func && (*after_func == ' ' || *after_func == '\t' || *after_func == '\n' || *after_func == '\r')) after_func++;
                                if (*after_func == ':') {
                                    after_func++;
                                    while (*after_func && (*after_func == ' ' || *after_func == '\t')) after_func++;
                                    while (*after_func && (isalnum((unsigned char)*after_func) || *after_func == '_')) after_func++;
                                }
                                // 跳过方法体
                                while (*after_func && (*after_func == ' ' || *after_func == '\t' || *after_func == '\n' || *after_func == '\r')) after_func++;
                                if (*after_func == '{') {
                                    after_func++;
                                    int brace_depth = 1;
                                    while (*after_func && brace_depth > 0) {
                                        if (*after_func == '{') brace_depth++;
                                        else if (*after_func == '}') brace_depth--;
                                        after_func++;
                                    }
                                }
                                after_cstruct = after_func;
                                continue;
                            }

                            // 解析字段
                            const char* type_start = after_cstruct;
                            while (*after_cstruct && (isalnum((unsigned char)*after_cstruct) || *after_cstruct == '_')) after_cstruct++;
                            int type_len = (int)(after_cstruct - type_start);

                            if (type_len > 0 && type_len < 64) {
                                char type_str[64];
                                strncpy(type_str, type_start, type_len);
                                type_str[type_len] = '\0';

                                // 检查是否是 Ptr[T] 类型
                                TypeKind element_type = TYPE_PTR;  // 默认元素类型
                                while (*after_cstruct && (*after_cstruct == ' ' || *after_cstruct == '\t')) after_cstruct++;
                                if (*after_cstruct == '[') {
                                    // 解析 Ptr[T] 中的 T
                                    after_cstruct++;
                                    while (*after_cstruct && (*after_cstruct == ' ' || *after_cstruct == '\t')) after_cstruct++;
                                    const char* elem_type_start = after_cstruct;
                                    while (*after_cstruct && (isalnum((unsigned char)*after_cstruct) || *after_cstruct == '_')) after_cstruct++;
                                    int elem_type_len = (int)(after_cstruct - elem_type_start);
                                    if (elem_type_len > 0 && elem_type_len < 64) {
                                        char elem_type_str[64];
                                        strncpy(elem_type_str, elem_type_start, elem_type_len);
                                        elem_type_str[elem_type_len] = '\0';
                                        element_type = parse_base_type(elem_type_str);
                                    }
                                    // 跳过到 ] 为止的内容
                                    while (*after_cstruct && *after_cstruct != ']') after_cstruct++;
                                    if (*after_cstruct == ']') after_cstruct++;
                                }

                                while (*after_cstruct && (*after_cstruct == ' ' || *after_cstruct == '\t')) after_cstruct++;

                                const char* field_start = after_cstruct;
                                while (*after_cstruct && (isalnum((unsigned char)*after_cstruct) || *after_cstruct == '_')) after_cstruct++;
                                int field_len = (int)(after_cstruct - field_start);

                                if (field_len > 0 && field_len < 64 && field_count < MOD_MAX_FIELDS) {
                                    TypeKind ft = parse_base_type(type_str);
                                    fields[field_count].name = (char*)malloc(field_len + 1);
                                    strncpy(fields[field_count].name, field_start, field_len);
                                    fields[field_count].name[field_len] = '\0';
                                    fields[field_count].type = ft;
                                    fields[field_count].struct_name = NULL;
                                    if (ft == TYPE_ANY) {
                                        if (is_known_clib(type_str, clib_names, clib_name_count)) {
                                            fields[field_count].type = TYPE_CLIB;
                                            fields[field_count].struct_name = strdup(type_str);
                                        } else if (is_known_struct(type_str, struct_names, struct_name_count)) {
                                            fields[field_count].type = TYPE_STRUCT;
                                            fields[field_count].struct_name = strdup(type_str);
                                        }
                                    }
                                    fields[field_count].element_type = element_type;
                                    if (field_count >= MOD_MAX_FIELDS) {
                                        fprintf(stderr, "[错误] cstruct字段数量超过上限 %d\n", MOD_MAX_FIELDS);
                                    } else {
                                        field_count++;
                                    }
                                }
                            }

                            while (*after_cstruct && *after_cstruct != '\n' && *after_cstruct != '}' && *after_cstruct != ';') after_cstruct++;
                            if (*after_cstruct == ';') after_cstruct++;
                        }

                        // 添加 cstruct 到符号表（包括字段和方法）
                        module_symbol_table_add_struct(table, cstruct_name, field_count, fields, method_count, methods, 1, 0, NULL);

                        // 释放临时分配的内存
                        for (int i = 0; i < field_count; i++) {
                            free(fields[i].name);
                        }
                        for (int i = 0; i < method_count; i++) {
                            free(methods[i].name);
                            free(methods[i].return_struct_name);
                            if (methods[i].return_type_info) {
                                type_free(methods[i].return_type_info);
                            }
                            free(methods[i].return_type_param_name);
                            for (int gi = 0; gi < methods[i].return_generic_count; gi++) {
                                free(methods[i].return_generic_param_names[gi]);
                            }
                            free(methods[i].return_generic_param_names);
                            free(methods[i].param_types);
                            // 释放泛型参数名
                            if (methods[i].param_generic_names) {
                                for (int pi = 0; pi < methods[i].param_count; pi++) {
                                    free(methods[i].param_generic_names[pi]);
                                }
                                free(methods[i].param_generic_names);
                            }
                        }

                        while (*after_cstruct && *after_cstruct != '}') after_cstruct++;
                        if (*after_cstruct == '}') after_cstruct++;
                    }
                }

                p = after_cstruct;
                continue;
            }

            // 检查是否是 face 定义
            if (strncmp(after_export, "face", 4) == 0 && (after_export[4] == ' ' || after_export[4] == '\t' || after_export[4] == '\n' || after_export[4] == '\r' || after_export[4] == '\0')) {
                char* after_face = after_export + 4;
                while (*after_face && (*after_face == ' ' || *after_face == '\t')) after_face++;

                const char* name_start = after_face;
                while (*after_face && (isalnum((unsigned char)*after_face) || *after_face == '_')) after_face++;
                int name_len = (int)(after_face - name_start);

                if (name_len > 0 && name_len < 64) {
                    char face_name[64];
                    strncpy(face_name, name_start, name_len);
                    face_name[name_len] = '\0';

                    // 跳过泛型参数 [T] 或 [K, V]，并计算参数数量
                    int face_type_param_count = 0;
                    while (*after_face && (*after_face == ' ' || *after_face == '\t')) after_face++;
                    if (*after_face == '[') {
                        after_face++; // skip '['
                        while (*after_face && *after_face != ']') {
                            while (*after_face && (*after_face == ' ' || *after_face == '\t')) after_face++;
                            if (*after_face && (isalnum((unsigned char)*after_face) || *after_face == '_')) {
                                face_type_param_count++;
                                while (*after_face && (isalnum((unsigned char)*after_face) || *after_face == '_')) after_face++;
                            }
                            while (*after_face && (*after_face == ' ' || *after_face == '\t')) after_face++;
                            if (*after_face == ',') after_face++;
                        }
                        if (*after_face == ']') after_face++; // skip ']'
                    }

                    while (*after_face && (*after_face == ' ' || *after_face == '\t' || *after_face == '\n' || *after_face == '\r')) after_face++;

                    if (*after_face == '{') {
                        after_face++;

                        ModuleFaceMethodSymbol methods[MOD_MAX_METHODS];
                        int method_count = 0;

                        while (*after_face && *after_face != '}' && method_count < MOD_MAX_METHODS) {
                            while (*after_face && (*after_face == ' ' || *after_face == '\t' || *after_face == '\n' || *after_face == '\r')) after_face++;
                            if (!*after_face || *after_face == '}') break;

                            if (*after_face == '/' && *(after_face+1) == '/') {
                                while (*after_face && *after_face != '\n') after_face++;
                                continue;
                            }

                            // 检查是否是方法声明 (func 关键字)
                            if (strncmp(after_face, "func", 4) == 0 && (after_face[4] == ' ' || after_face[4] == '\t')) {
                                char* after_func = after_face + 4;
                                while (*after_func && (*after_func == ' ' || *after_func == '\t')) after_func++;

                                // 读取方法名
                                const char* method_name_start = after_func;
                                while (*after_func && (isalnum((unsigned char)*after_func) || *after_func == '_')) after_func++;
                                int method_name_len = (int)(after_func - method_name_start);

                                if (method_name_len > 0 && method_name_len < 64) {
                                    // 跳过参数列表并计算参数数量
                                    while (*after_func && (*after_func == ' ' || *after_func == '\t')) after_func++;
                                    int param_count = 0;
                                    if (*after_func == '(') {
                                        after_func++;
                                        int paren_depth = 1;
                                        int has_param = 0;
                                        while (*after_func && paren_depth > 0) {
                                            if (*after_func == '(') paren_depth++;
                                            else if (*after_func == ')') paren_depth--;
                                            else if (paren_depth == 1) {
                                                if (*after_func == ',') param_count++;
                                                else if (!isspace((unsigned char)*after_func)) has_param = 1;
                                            }
                                            after_func++;
                                        }
                                        if (has_param) param_count++;
                                    }

                                    // 跳过空白
                                    while (*after_func && (*after_func == ' ' || *after_func == '\t' || *after_func == '\n' || *after_func == '\r')) after_func++;

                                    // 解析返回类型
                                    TypeKind method_return_type = TYPE_ANY;
                                    TypeInfo* method_return_type_info = NULL;
                                    char method_return_struct[64] = {0};

                                    if (*after_func == ':') {
                                        after_func++;
                                        while (*after_func && (*after_func == ' ' || *after_func == '\t')) after_func++;

                                        const char* ret_type_start = after_func;
                                        while (*after_func && (isalnum((unsigned char)*after_func) || *after_func == '_')) after_func++;
                                        int ret_type_len = (int)(after_func - ret_type_start);

                                        // 继续读取泛型参数部分（如 [string,float]）
                                        while (*after_func && (*after_func == ' ' || *after_func == '\t')) after_func++;
                                        if (*after_func == '[') {
                                            int depth = 0;
                                            while (*after_func) {
                                                if (*after_func == '[') depth++;
                                                else if (*after_func == ']') { depth--; if (depth == 0) { after_func++; break; } }
                                                after_func++;
                                            }
                                            ret_type_len = (int)(after_func - ret_type_start);
                                        }

                                        if (ret_type_len > 0 && ret_type_len < 128) {
                                            char ret_type_str[128];
                                            strncpy(ret_type_str, ret_type_start, ret_type_len);
                                            ret_type_str[ret_type_len] = '\0';

                                            // 先尝试从本地别名表解析
                                            TypeInfo* alias_ti = FIND_LOCAL_ALIAS(ret_type_str);
                                            if (alias_ti) {
                                                method_return_type_info = type_copy(alias_ti);
                                                method_return_type = method_return_type_info->kind;
                                            } else {
                                                method_return_type_info = parse_type_from_string(ret_type_str);
                                                if (method_return_type_info) {
                                                    method_return_type = method_return_type_info->kind;
                                                } else {
                                                    method_return_type = parse_base_type(ret_type_str);
                                                }
                                            }

                                            char base_str[64];
                                            int base_len = ret_type_len < 64 ? ret_type_len : 63;
                                            strncpy(base_str, ret_type_str, base_len);
                                            base_str[base_len] = '\0';
                                            char* bracket = strchr(base_str, '[');
                                            if (bracket) *bracket = '\0';

                                            if (method_return_type == TYPE_ANY || method_return_type == TYPE_STRUCT) {
                                                if (is_known_clib(base_str, clib_names, clib_name_count)) {
                                                    method_return_type = TYPE_CLIB;
                                                    if (method_return_type_info) method_return_type_info->kind = TYPE_CLIB;
                                                    size_t copy_len = strlen(base_str);
                                                    if (copy_len > sizeof(method_return_struct) - 1) copy_len = sizeof(method_return_struct) - 1;
                                                    memcpy(method_return_struct, base_str, copy_len);
                                                    method_return_struct[copy_len] = '\0';
                                                } else if (is_known_face(base_str, face_names, face_name_count)) {
                                                    method_return_type = TYPE_FACE;
                                                    if (method_return_type_info) method_return_type_info->kind = TYPE_FACE;
                                                    size_t copy_len = strlen(base_str);
                                                    if (copy_len > sizeof(method_return_struct) - 1) copy_len = sizeof(method_return_struct) - 1;
                                                    memcpy(method_return_struct, base_str, copy_len);
                                                    method_return_struct[copy_len] = '\0';
                                                } else if (is_known_cstruct(base_str, cstruct_names, cstruct_name_count)) {
                                                    method_return_type = TYPE_CSTRUCT;
                                                    if (method_return_type_info) method_return_type_info->kind = TYPE_CSTRUCT;
                                                    size_t copy_len = strlen(base_str);
                                                    if (copy_len > sizeof(method_return_struct) - 1) copy_len = sizeof(method_return_struct) - 1;
                                                    memcpy(method_return_struct, base_str, copy_len);
                                                    method_return_struct[copy_len] = '\0';
                                                } else if (is_known_struct(base_str, struct_names, struct_name_count)) {
                                                    method_return_type = TYPE_STRUCT;
                                                    size_t copy_len = strlen(base_str);
                                                    if (copy_len > sizeof(method_return_struct) - 1) copy_len = sizeof(method_return_struct) - 1;
                                                    memcpy(method_return_struct, base_str, copy_len);
                                                    method_return_struct[copy_len] = '\0';
                                                }
                                            }
                                        }
                                    }

                                    // 添加方法
                                    methods[method_count].name = (char*)malloc(method_name_len + 1);
                                    strncpy(methods[method_count].name, method_name_start, method_name_len);
                                    methods[method_count].name[method_name_len] = '\0';
                                    methods[method_count].return_type = method_return_type;
                                    methods[method_count].return_struct_name = method_return_struct[0] ? strdup(method_return_struct) : NULL;
                                    methods[method_count].param_count = param_count;
                                    // 释放临时 method_return_type_info（face 方法符号不存储此字段）
                                    if (method_return_type_info) { type_free(method_return_type_info); method_return_type_info = NULL; }
                                    if (method_count >= MOD_MAX_METHODS) {
                                        fprintf(stderr, "[错误] face方法数量超过上限 %d\n", MOD_MAX_METHODS);
                                    } else {
                                        method_count++;
                                    }
                                }

                                after_face = after_func;
                                continue;
                            }

                            // 跳过其他内容
                            while (*after_face && *after_face != '\n' && *after_face != '}') after_face++;
                        }

                        // 添加 face 到符号表
                        module_symbol_table_add_face(table, face_name, method_count, methods, face_type_param_count);

                        // 释放临时分配的内存（add_face 已复制数据）
                        for (int i = 0; i < method_count; i++) {
                            free(methods[i].name);
                            free(methods[i].return_struct_name);
                        }

                        while (*after_face && *after_face != '}') after_face++;
                        if (*after_face == '}') after_face++;
                    }
                }

                p = after_face;
                continue;
            }

            // 检查是否是 enum 定义
            if (strncmp(after_export, "enum", 4) == 0 && (after_export[4] == ' ' || after_export[4] == '\t' || after_export[4] == '\n' || after_export[4] == '\r' || after_export[4] == '\0')) {
                char* after_enum = after_export + 4;
                while (*after_enum && (*after_enum == ' ' || *after_enum == '\t')) after_enum++;

                const char* name_start = after_enum;
                while (*after_enum && (isalnum((unsigned char)*after_enum) || *after_enum == '_')) after_enum++;
                int name_len = (int)(after_enum - name_start);

                if (name_len > 0 && name_len < 64) {
                    char enum_name[64];
                    strncpy(enum_name, name_start, name_len);
                    enum_name[name_len] = '\0';

                    // 解析成员名
                    char* member_names_buf[MOD_MAX_MEMBERS];
                    int member_count = 0;
                    while (*after_enum && *after_enum != '{') after_enum++;
                    if (*after_enum == '{') {
                        after_enum++;
                        while (*after_enum && *after_enum != '}' && member_count < MOD_MAX_MEMBERS) {
                            while (*after_enum && (*after_enum == ' ' || *after_enum == '\t' || *after_enum == '\n' || *after_enum == '\r')) after_enum++;
                            if (*after_enum == '}' || *after_enum == '\0') break;
                            if (*after_enum == '/' && *(after_enum+1) == '/') {
                                while (*after_enum && *after_enum != '\n') after_enum++;
                                continue;
                            }
                            const char* m_start = after_enum;
                            while (*after_enum && (isalnum((unsigned char)*after_enum) || *after_enum == '_')) after_enum++;
                            int m_len = (int)(after_enum - m_start);
                            if (m_len > 0 && m_len < 64) {
                                member_names_buf[member_count] = (char*)malloc(m_len + 1);
                                strncpy(member_names_buf[member_count], m_start, m_len);
                                member_names_buf[member_count][m_len] = '\0';
                                member_count++;
                            }
                            while (*after_enum && *after_enum != ',' && *after_enum != '}' && *after_enum != '\n') after_enum++;
                            if (*after_enum == ',') after_enum++;
                        }
                        if (*after_enum == '}') after_enum++;
                    }

                    module_symbol_table_add_enum(table, enum_name, member_count, member_names_buf);
                    for (int mi = 0; mi < member_count; mi++) free(member_names_buf[mi]);
                }

                p = after_enum;
                continue;
            }

            // 检查是否是 func 定义
            if (strncmp(after_export, "func", 4) == 0 && (after_export[4] == ' ' || after_export[4] == '\t' || after_export[4] == '\n' || after_export[4] == '\r' || after_export[4] == '\0')) {
                char* after_func = after_export + 4;
                while (*after_func && (*after_func == ' ' || *after_func == '\t')) after_func++;

                const char* name_start = after_func;
                while (*after_func && (isalnum((unsigned char)*after_func) || *after_func == '_')) after_func++;
                int name_len = (int)(after_func - name_start);

                if (name_len > 0 && name_len < 64) {
                    char func_name[64];
                    strncpy(func_name, name_start, name_len);
                    func_name[name_len] = '\0';

                    // 解析泛型参数 [T] 或 [K, V] 或 [T: Face]
                    int func_type_param_count = 0;
                    while (*after_func && (*after_func == ' ' || *after_func == '\t')) after_func++;
                    if (*after_func == '[') {
                        after_func++; // skip '['
                        while (*after_func && *after_func != ']') {
                            while (*after_func && (*after_func == ' ' || *after_func == '\t')) after_func++;
                            if (*after_func && (isalnum((unsigned char)*after_func) || *after_func == '_')) {
                                func_type_param_count++;
                                while (*after_func && (isalnum((unsigned char)*after_func) || *after_func == '_')) after_func++;
                            }
                            while (*after_func && (*after_func == ' ' || *after_func == '\t')) after_func++;
                            // 跳过约束语法 T: FaceName
                            if (*after_func == ':') {
                                after_func++; // skip ':'
                                while (*after_func && (*after_func == ' ' || *after_func == '\t')) after_func++;
                                while (*after_func && (isalnum((unsigned char)*after_func) || *after_func == '_')) after_func++;
                            }
                            if (*after_func == ',') after_func++;
                        }
                        if (*after_func == ']') after_func++; // skip ']'
                    }

                    while (*after_func && (*after_func == ' ' || *after_func == '\t')) after_func++;
                    if (*after_func == '(') {
                        after_func++;
                        int paren_depth = 1;
                        while (*after_func && paren_depth > 0) {
                            if (*after_func == '(') paren_depth++;
                            else if (*after_func == ')') paren_depth--;
                            after_func++;
                        }

                        while (*after_func && (*after_func == ' ' || *after_func == '\t' || *after_func == '\n' || *after_func == '\r')) after_func++;

                        TypeKind return_type = TYPE_ANY;
                        TypeInfo* return_type_info = NULL;
                        char return_struct_name[64] = {0};

                        if (*after_func == ':') {
                            after_func++;
                            while (*after_func && (*after_func == ' ' || *after_func == '\t')) after_func++;

                            const char* type_start = after_func;
                            // 先读取标识符部分（如 Dict、Array、int 等）
                            while (*after_func && (isalnum((unsigned char)*after_func) || *after_func == '_')) after_func++;
                            int type_len = (int)(after_func - type_start);

                            // 继续读取泛型参数部分（如 [string,int]）
                            while (*after_func && (*after_func == ' ' || *after_func == '\t')) after_func++;
                            if (*after_func == '[') {
                                int depth = 0;
                                while (*after_func) {
                                    if (*after_func == '[') depth++;
                                    else if (*after_func == ']') { depth--; if (depth == 0) { after_func++; break; } }
                                    after_func++;
                                }
                                // type_len 现在包含泛型参数部分
                                type_len = (int)(after_func - type_start);
                            }

                            if (type_len > 0 && type_len < 128) {
                                char type_str[128];
                                strncpy(type_str, type_start, type_len);
                                type_str[type_len] = '\0';

                                // 先尝试从本地别名表解析
                                TypeInfo* alias_ti = FIND_LOCAL_ALIAS(type_str);
                                if (alias_ti) {
                                    return_type_info = type_copy(alias_ti);
                                    return_type = return_type_info->kind;
                                } else {
                                    // 解析完整类型信息（支持 Dict[K,V]、Array[T] 等泛型）
                                    return_type_info = parse_type_from_string(type_str);
                                    if (return_type_info) {
                                        return_type = return_type_info->kind;
                                    } else {
                                        // 回退：仅解析基本类型
                                        return_type = parse_base_type(type_str);
                                    }
                                }

                                // 用标识符部分（不含泛型参数）检查已知类型
                                char base_str[64];
                                int base_len = type_len < 64 ? type_len : 63;
                                strncpy(base_str, type_str, base_len);
                                base_str[base_len] = '\0';
                                // 截取到 '[' 之前
                                char* bracket = strchr(base_str, '[');
                                if (bracket) *bracket = '\0';

                                // 如果 return_type_info 返回 TYPE_STRUCT，但实际是 clib/face/cstruct，
                                // 需要修正 kind（parse_simple_type_str 对未知标识符默认返回 TYPE_STRUCT）
                                if (return_type == TYPE_ANY || return_type == TYPE_STRUCT) {
                                    if (is_known_clib(base_str, clib_names, clib_name_count)) {
                                        return_type = TYPE_CLIB;
                                        if (return_type_info) {
                                            return_type_info->kind = TYPE_CLIB;
                                        }
                                        size_t copy_len = strlen(base_str);
                                        if (copy_len > sizeof(return_struct_name) - 1) copy_len = sizeof(return_struct_name) - 1;
                                        memcpy(return_struct_name, base_str, copy_len);
                                        return_struct_name[copy_len] = '\0';
                                    } else if (is_known_face(base_str, face_names, face_name_count)) {
                                        return_type = TYPE_FACE;
                                        if (return_type_info) {
                                            return_type_info->kind = TYPE_FACE;
                                        }
                                        size_t copy_len = strlen(base_str);
                                        if (copy_len > sizeof(return_struct_name) - 1) copy_len = sizeof(return_struct_name) - 1;
                                        memcpy(return_struct_name, base_str, copy_len);
                                        return_struct_name[copy_len] = '\0';
                                    } else if (is_known_cstruct(base_str, cstruct_names, cstruct_name_count)) {
                                        return_type = TYPE_CSTRUCT;
                                        if (return_type_info) {
                                            return_type_info->kind = TYPE_CSTRUCT;
                                        }
                                        size_t copy_len = strlen(base_str);
                                        if (copy_len > sizeof(return_struct_name) - 1) copy_len = sizeof(return_struct_name) - 1;
                                        memcpy(return_struct_name, base_str, copy_len);
                                        return_struct_name[copy_len] = '\0';
                                    } else if (is_known_struct(base_str, struct_names, struct_name_count)) {
                                        return_type = TYPE_STRUCT;
                                        size_t copy_len = strlen(base_str);
                                        if (copy_len > sizeof(return_struct_name) - 1) copy_len = sizeof(return_struct_name) - 1;
                                        memcpy(return_struct_name, base_str, copy_len);
                                        return_struct_name[copy_len] = '\0';
                                    }
                                }
                            }
                        }

                        module_symbol_table_add_func(table, func_name, return_type,
                            return_struct_name[0] ? return_struct_name : NULL, func_type_param_count, return_type_info);
                        if (return_type_info) { type_free(return_type_info); return_type_info = NULL; }
                    }
                }

                p = after_func;
                continue;
            }

            // 检查是否是 var 定义
            if (strncmp(after_export, "var", 3) == 0 && (after_export[3] == ' ' || after_export[3] == '\t' || after_export[3] == '\n' || after_export[3] == '\r' || after_export[3] == '\0')) {
                char* after_var = after_export + 3;
                while (*after_var && (*after_var == ' ' || *after_var == '\t')) after_var++;

                const char* name_start = after_var;
                while (*after_var && (isalnum((unsigned char)*after_var) || *after_var == '_')) after_var++;
                int name_len = (int)(after_var - name_start);

                if (name_len > 0 && name_len < 64) {
                    char var_name[64];
                    strncpy(var_name, name_start, name_len);
                    var_name[name_len] = '\0';

                    while (*after_var && (*after_var == ' ' || *after_var == '\t')) after_var++;

                    TypeKind var_type = TYPE_ANY;
                    char var_struct_name[64] = {0};

                    // 检查是否有类型注解
                    if (*after_var == ':') {
                        after_var++;
                        while (*after_var && (*after_var == ' ' || *after_var == '\t')) after_var++;

                        const char* type_start = after_var;
                        while (*after_var && (isalnum((unsigned char)*after_var) || *after_var == '_')) after_var++;
                        int type_len = (int)(after_var - type_start);

                        if (type_len > 0 && type_len < 64) {
                            char type_str[64];
                            strncpy(type_str, type_start, type_len);
                            type_str[type_len] = '\0';

                            // 先检查是否是基本类型
                            var_type = parse_base_type(type_str);

                            // 如果不是基本类型，检查是否是已知的 clib/face/struct/cstruct
                            if (var_type == TYPE_ANY) {
                                if (is_known_clib(type_str, clib_names, clib_name_count)) {
                                    var_type = TYPE_CLIB;
                                    size_t copy_len = strlen(type_str);
                                    if (copy_len > sizeof(var_struct_name) - 1) copy_len = sizeof(var_struct_name) - 1;
                                    memcpy(var_struct_name, type_str, copy_len);
                                    var_struct_name[copy_len] = '\0';
                                } else if (is_known_face(type_str, face_names, face_name_count)) {
                                    var_type = TYPE_FACE;
                                    size_t copy_len = strlen(type_str);
                                    if (copy_len > sizeof(var_struct_name) - 1) copy_len = sizeof(var_struct_name) - 1;
                                    memcpy(var_struct_name, type_str, copy_len);
                                    var_struct_name[copy_len] = '\0';
                                } else if (is_known_cstruct(type_str, cstruct_names, cstruct_name_count)) {
                                    var_type = TYPE_CSTRUCT;
                                    size_t copy_len = strlen(type_str);
                                    if (copy_len > sizeof(var_struct_name) - 1) copy_len = sizeof(var_struct_name) - 1;
                                    memcpy(var_struct_name, type_str, copy_len);
                                    var_struct_name[copy_len] = '\0';
                                } else if (is_known_struct(type_str, struct_names, struct_name_count)) {
                                    var_type = TYPE_STRUCT;
                                    size_t copy_len = strlen(type_str);
                                    if (copy_len > sizeof(var_struct_name) - 1) copy_len = sizeof(var_struct_name) - 1;
                                    memcpy(var_struct_name, type_str, copy_len);
                                    var_struct_name[copy_len] = '\0';
                                }
                            }
                        }
                    } else {
                        // 没有类型注解，尝试从初始化值推断类型
                        // 跳过 = 号
                        if (*after_var == '=') {
                            after_var++;
                            while (*after_var && (*after_var == ' ' || *after_var == '\t')) after_var++;

                            // 检查是否是数字字面量
                            if (*after_var == '-' || (*after_var >= '0' && *after_var <= '9')) {
                                // 检查是否是浮点数
                                char* num_end = after_var;
                                int is_float = 0;
                                if (*num_end == '-') num_end++;
                                while (*num_end && (*num_end >= '0' && *num_end <= '9')) num_end++;
                                if (*num_end == '.') {
                                    is_float = 1;
                                    num_end++;
                                    while (*num_end && (*num_end >= '0' && *num_end <= '9')) num_end++;
                                }
                                if (is_float) {
                                    var_type = TYPE_FLOAT;
                                } else {
                                    var_type = TYPE_INT;
                                }
                            } else if (*after_var == '"') {
                                // 字符串字面量
                                var_type = TYPE_STRING;
                            } else if (strncmp(after_var, "true", 4) == 0 || strncmp(after_var, "false", 5) == 0) {
                                // 布尔字面量
                                var_type = TYPE_BOOL;
                            } else if (strncmp(after_var, "null", 4) == 0) {
                                // null
                                var_type = TYPE_NULL;
                            }
                            // 其他情况保持 TYPE_ANY
                        }
                    }

                    module_symbol_table_add_var(table, var_name, var_type,
                        var_struct_name[0] ? var_struct_name : NULL);
                }

                p = after_var;
                continue;
            }

            // 检查是否是 alias 定义
            if (strncmp(after_export, "alias", 5) == 0 && (after_export[5] == ' ' || after_export[5] == '\t' || after_export[5] == '\n' || after_export[5] == '\r' || after_export[5] == '\0')) {
                char* after_alias = after_export + 5;
                while (*after_alias && (*after_alias == ' ' || *after_alias == '\t')) after_alias++;

                const char* name_start = after_alias;
                while (*after_alias && (isalnum((unsigned char)*after_alias) || *after_alias == '_')) after_alias++;
                int name_len = (int)(after_alias - name_start);

                if (name_len > 0 && name_len < 64) {
                    char alias_name[64];
                    memcpy(alias_name, name_start, name_len);
                    alias_name[name_len] = '\0';

                    // 跳过空白和 =
                    while (*after_alias && (*after_alias == ' ' || *after_alias == '\t')) after_alias++;
                    if (*after_alias == '=') {
                        after_alias++;
                        while (*after_alias && (*after_alias == ' ' || *after_alias == '\t')) after_alias++;

                        // 收集完整类型字符串（支持 Array[int]、Dict[string, int] 等）
                        char type_str[256] = {0};
                        int ti = 0;
                        while (*after_alias && ti < 255 && *after_alias != '\n' && *after_alias != '\r' &&
                               *after_alias != ';' && *after_alias != '{' && *after_alias != '/') {
                            if (!(*after_alias == '_' || isalnum((unsigned char)*after_alias) ||
                                  *after_alias == '[' || *after_alias == ']' ||
                                  *after_alias == ',' || *after_alias == ':' ||
                                  *after_alias == '(' || *after_alias == ')' ||
                                  *after_alias == ' ' || *after_alias == '\t')) break;
                            // 跳过连续空格，压缩为单个空格
                            if ((*after_alias == ' ' || *after_alias == '\t') && ti > 0 && (type_str[ti-1] == ' ' || type_str[ti-1] == '\t')) {
                                after_alias++;
                                continue;
                            }
                            type_str[ti++] = (*after_alias == '\t') ? ' ' : *after_alias;
                            after_alias++;
                        }
                        type_str[ti] = '\0';

                        if (ti > 0) {
                            TypeInfo* ti_ptr = NULL;
                            // 先检查是否是本地别名引用（如 alias B = A）
                            if (!strchr(type_str, '[') && !strchr(type_str, ',')) {
                                ti_ptr = FIND_LOCAL_ALIAS(type_str);
                                if (ti_ptr) ti_ptr = type_copy(ti_ptr);
                            }
                            if (!ti_ptr) {
                                ti_ptr = parse_type_from_string(type_str);
                            }
                            if (ti_ptr) {
                                // 如果是简单 STRUCT 类型，检查是否实际是 face/cstruct
                                if (ti_ptr->kind == TYPE_STRUCT && ti_ptr->struct_name) {
                                    if (is_known_face(ti_ptr->struct_name, face_names, face_name_count)) {
                                        ti_ptr->kind = TYPE_FACE;
                                    } else if (is_known_cstruct(ti_ptr->struct_name, cstruct_names, cstruct_name_count)) {
                                        ti_ptr->kind = TYPE_CSTRUCT;
                                    } else if (!is_known_struct(ti_ptr->struct_name, struct_names, struct_name_count)) {
                                        // 不是本模块的 struct/face/cstruct，保留为 TYPE_STRUCT（可能来自 use）
                                    }
                                }
                                module_symbol_table_add_alias(table, alias_name, ti_ptr);
                                // 添加到本地解析表
                                if (local_alias_count >= MOD_MAX_ALIASES) {
                                    fprintf(stderr, "[错误] 别名数量超过上限 %d\n", MOD_MAX_ALIASES);
                                } else {
                                    local_alias_names[local_alias_count] = strdup(alias_name);
                                    local_alias_types[local_alias_count] = type_copy(ti_ptr);
                                    local_alias_count++;
                                }
                                type_free(ti_ptr);
                            }
                        }
                    }
                }

                p = after_alias;
                continue;
            }
        }

        p++;
    }

    // 释放本地别名表
    for (int i = 0; i < local_alias_count; i++) {
        free(local_alias_names[i]);
        type_free(local_alias_types[i]);
    }
    #undef FIND_LOCAL_ALIAS

    // 释放 struct 名称数组
    for (int i = 0; i < struct_name_count; i++) {
        free(struct_names[i]);
    }
    // 释放 cstruct 名称数组
    for (int i = 0; i < cstruct_name_count; i++) {
        free(cstruct_names[i]);
    }
    // 释放 face 名称数组
    for (int i = 0; i < face_name_count; i++) {
        free(face_names[i]);
    }

    free(source);
    return 0;
}

// 公开接口，从深度 0 开始扫描
int module_symbol_table_scan(ModuleSymbolTable* table, const char* current_file) {
    return module_symbol_table_scan_depth(table, current_file, 0);
}
