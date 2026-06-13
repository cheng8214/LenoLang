#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "include/module_symbol_table.h"
#include "include/module_loader.h"

// 初始容量
#define INITIAL_CAPACITY 16

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
    }
    free(table->funcs);

    // 释放 struct 符号
    for (int i = 0; i < table->struct_count; i++) {
        free(table->structs[i].name);
        for (int j = 0; j < table->structs[i].field_count; j++) {
            free(table->structs[i].fields[j].name);
        }
        free(table->structs[i].fields);
        // 释放方法数组
        for (int j = 0; j < table->structs[i].method_count; j++) {
            free(table->structs[i].methods[j].name);
            free(table->structs[i].methods[j].return_struct_name);
            free(table->structs[i].methods[j].param_types);
        }
        free(table->structs[i].methods);
        // 释放 impl 名称数组
        for (int j = 0; j < table->structs[i].impl_count; j++) {
            free(table->structs[i].impl_names[j]);
        }
        free(table->structs[i].impl_names);
    }
    free(table->structs);

    // 释放 enum 符号
    for (int i = 0; i < table->enum_count; i++) {
        free(table->enums[i].name);
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

    free(table);
}

// 添加函数符号
void module_symbol_table_add_func(ModuleSymbolTable* table, const char* name, TypeKind return_type, const char* return_struct_name) {
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
    func->return_struct_name = return_struct_name ? strdup(return_struct_name) : NULL;
}

// 添加 struct 符号
void module_symbol_table_add_struct(ModuleSymbolTable* table, const char* name, int field_count, ModuleStructField* fields, int method_count, ModuleStructMethod* methods, int is_cstruct) {
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

    if (field_count > 0 && fields) {
        st->fields = (ModuleStructField*)malloc(sizeof(ModuleStructField) * field_count);
        for (int i = 0; i < field_count; i++) {
            st->fields[i].name = strdup(fields[i].name);
            st->fields[i].type = fields[i].type;
            st->fields[i].element_type = fields[i].element_type;
        }
    }

    if (method_count > 0 && methods) {
        st->methods = (ModuleStructMethod*)malloc(sizeof(ModuleStructMethod) * method_count);
        for (int i = 0; i < method_count; i++) {
            st->methods[i].name = strdup(methods[i].name);
            st->methods[i].return_type = methods[i].return_type;
            st->methods[i].return_struct_name = methods[i].return_struct_name ? strdup(methods[i].return_struct_name) : NULL;
            st->methods[i].param_count = methods[i].param_count;
            st->methods[i].param_types = NULL;
            if (methods[i].param_count > 0 && methods[i].param_types) {
                st->methods[i].param_types = (TypeKind*)malloc(sizeof(TypeKind) * methods[i].param_count);
                for (int j = 0; j < methods[i].param_count; j++) {
                    st->methods[i].param_types[j] = methods[i].param_types[j];
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
void module_symbol_table_add_enum(ModuleSymbolTable* table, const char* name) {
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
void module_symbol_table_add_face(ModuleSymbolTable* table, const char* name, int method_count, ModuleFaceMethodSymbol* methods) {
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

// 基本类型解析 - 只识别内置类型，其他返回 TYPE_ANY
static TypeKind parse_base_type(const char* type_str) {
    if (strcmp(type_str, "int") == 0) return TYPE_INT;
    if (strcmp(type_str, "float") == 0) return TYPE_FLOAT;
    if (strcmp(type_str, "string") == 0) return TYPE_STRING;
    if (strcmp(type_str, "bool") == 0) return TYPE_BOOL;
    if (strcmp(type_str, "Array") == 0) return TYPE_ARRAY;
    if (strcmp(type_str, "Dict") == 0) return TYPE_DICT;
    // "Bint" 已移除：对外统一用 int
    // if (strcmp(type_str, "Bint") == 0) return TYPE_BIGINT;
    if (strcmp(type_str, "null") == 0) return TYPE_NULL;
    if (strcmp(type_str, "File") == 0) return TYPE_FILE;
    if (strcmp(type_str, "GWin") == 0) return TYPE_WIN;
    if (strcmp(type_str, "GDraw") == 0) return TYPE_DRAW;
    if (strcmp(type_str, "GEvent") == 0) return TYPE_EVENT;
    if (strcmp(type_str, "GImage") == 0) return TYPE_IMAGE;
    if (strcmp(type_str, "GFont") == 0) return TYPE_FONT;
    if (strcmp(type_str, "Socket") == 0) return TYPE_SOCKET;
    if (strcmp(type_str, "any") == 0) return TYPE_ANY;
    if (strcmp(type_str, "Ptr") == 0) return TYPE_PTR;
    return TYPE_ANY;  // 未知类型，可能是自定义 struct
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

// 从模块文件读取源代码（需要外部提供或复制相关代码）
extern char* read_module_file(const char* file_path, const char* current_file);

// 扫描模块文件，提取所有符号信息
// 两遍扫描：第一遍收集所有 struct 名称，第二遍解析所有符号
int module_symbol_table_scan(ModuleSymbolTable* table, const char* current_file) {
    if (!table || !table->module_path) return -1;

    // 只读取一次文件
    char* source = read_module_file(table->module_path, current_file);
    if (!source) return -1;

    // 第一遍：收集所有 struct 名称
    char* struct_names[64];
    int struct_name_count = 0;
    char* p = source;

    while (*p && struct_name_count < 64) {
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
                    struct_names[struct_name_count] = (char*)malloc(name_len + 1);
                    strncpy(struct_names[struct_name_count], name_start, name_len);
                    struct_names[struct_name_count][name_len] = '\0';
                    struct_name_count++;
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
                    struct_names[struct_name_count] = (char*)malloc(name_len + 1);
                    strncpy(struct_names[struct_name_count], name_start, name_len);
                    struct_names[struct_name_count][name_len] = '\0';
                    struct_name_count++;
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
                    struct_names[struct_name_count] = (char*)malloc(name_len + 1);
                    strncpy(struct_names[struct_name_count], name_start, name_len);
                    struct_names[struct_name_count][name_len] = '\0';
                    struct_name_count++;
                }

                p = after_face;
                continue;
            }
        }

        p++;
    }

    // 第二遍：解析所有符号（struct 和函数）
    p = source;

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

                    while (*after_struct && (*after_struct == ' ' || *after_struct == '\t' || *after_struct == '\n' || *after_struct == '\r')) after_struct++;

                    // 解析 impl 声明
                    char* impl_names[64];
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

                        ModuleStructField fields[64];
                        ModuleStructMethod methods[64];
                        int field_count = 0;
                        int method_count = 0;

                        while (*after_struct && *after_struct != '}' && (field_count < 64 || method_count < 64)) {
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

                                if (method_name_len > 0 && method_name_len < 64 && method_count < 64) {
                                    char method_name[64];
                                    strncpy(method_name, method_name_start, method_name_len);
                                    method_name[method_name_len] = '\0';

                                    // 使用 struct_name::method_name 格式，避免与全局函数冲突
                                    char method_key[256];
                                    snprintf(method_key, sizeof(method_key), "%s::%s", struct_name, method_name);
                                    int method_key_len = (int)strlen(method_key);

                                    // 解析参数列表
                                    int param_count = 0;
                                    TypeKind param_types[64] = {0};
                                    char param_struct_names[64][64] = {{0}};

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

                                                    if (has_param && param_count < 64) {
                                                        param_types[param_count] = param_type;
                                                        if (param_struct_name[0]) {
                                                            strncpy(param_struct_names[param_count], param_struct_name, 63);
                                                            param_struct_names[param_count][63] = '\0';
                                                        }
                                                        param_count++;
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
                                        char method_return_struct[64] = {0};

                                        if (*after_func == ':') {
                                            after_func++;
                                            while (*after_func && (*after_func == ' ' || *after_func == '\t')) after_func++;

                                            const char* ret_type_start = after_func;
                                            while (*after_func && (isalnum((unsigned char)*after_func) || *after_func == '_')) after_func++;
                                            int ret_type_len = (int)(after_func - ret_type_start);

                                            if (ret_type_len > 0 && ret_type_len < 64) {
                                                char ret_type_str[64];
                                                strncpy(ret_type_str, ret_type_start, ret_type_len);
                                                ret_type_str[ret_type_len] = '\0';
                                                method_return_type = parse_base_type(ret_type_str);
                                                if (method_return_type == TYPE_ANY) {
                                                    // 检查是否是已知的 struct 类型
                                                    if (is_known_struct(ret_type_str, struct_names, struct_name_count)) {
                                                        method_return_type = TYPE_STRUCT;
                                                        size_t copy_len = strlen(ret_type_str);
                                                        if (copy_len > sizeof(method_return_struct) - 1) copy_len = sizeof(method_return_struct) - 1;
                                                        memcpy(method_return_struct, ret_type_str, copy_len);
                                                        method_return_struct[copy_len] = '\0';
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
                                        methods[method_count].return_struct_name = method_return_struct[0] ? strdup(method_return_struct) : NULL;
                                        methods[method_count].param_count = param_count;
                                        methods[method_count].param_types = NULL;
                                        if (param_count > 0) {
                                            methods[method_count].param_types = (TypeKind*)malloc(sizeof(TypeKind) * param_count);
                                            for (int pi = 0; pi < param_count; pi++) {
                                                methods[method_count].param_types[pi] = param_types[pi];
                                            }
                                        }
                                        method_count++;
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

                                if (field_len > 0 && field_len < 64 && field_count < 64) {
                                    fields[field_count].name = (char*)malloc(field_len + 1);
                                    strncpy(fields[field_count].name, field_start, field_len);
                                    fields[field_count].name[field_len] = '\0';
                                    fields[field_count].type = parse_base_type(type_str);
                                    fields[field_count].element_type = element_type;
                                    field_count++;
                                }
                            }

                            while (*after_struct && *after_struct != '\n' && *after_struct != '}') after_struct++;
                        }

                        // 添加 struct 到符号表（包括字段和方法）
                        module_symbol_table_add_struct(table, struct_name, field_count, fields, method_count, methods, 0);

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
                        for (int i = 0; i < field_count; i++) {
                            free(fields[i].name);
                        }
                        for (int i = 0; i < method_count; i++) {
                            free(methods[i].name);
                            free(methods[i].return_struct_name);
                            free(methods[i].param_types);
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

                        ModuleStructField fields[64];
                        ModuleStructMethod methods[64];
                        int field_count = 0;
                        int method_count = 0;

                        // cstruct 只解析字段，不解析方法（cstruct 用于 C 布局，不支持方法）
                        while (*after_cstruct && *after_cstruct != '}' && field_count < 64) {
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

                                if (field_len > 0 && field_len < 64 && field_count < 64) {
                                    fields[field_count].name = (char*)malloc(field_len + 1);
                                    strncpy(fields[field_count].name, field_start, field_len);
                                    fields[field_count].name[field_len] = '\0';
                                    fields[field_count].type = parse_base_type(type_str);
                                    fields[field_count].element_type = element_type;
                                    field_count++;
                                }
                            }

                            while (*after_cstruct && *after_cstruct != '\n' && *after_cstruct != '}') after_cstruct++;
                        }

                        // 添加 cstruct 到符号表（包括字段和方法）
                        module_symbol_table_add_struct(table, cstruct_name, field_count, fields, method_count, methods, 1);

                        // 释放临时分配的内存
                        for (int i = 0; i < field_count; i++) {
                            free(fields[i].name);
                        }
                        for (int i = 0; i < method_count; i++) {
                            free(methods[i].name);
                            free(methods[i].return_struct_name);
                            free(methods[i].param_types);
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

                    while (*after_face && (*after_face == ' ' || *after_face == '\t' || *after_face == '\n' || *after_face == '\r')) after_face++;

                    if (*after_face == '{') {
                        after_face++;

                        ModuleFaceMethodSymbol methods[64];
                        int method_count = 0;

                        while (*after_face && *after_face != '}' && method_count < 64) {
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
                                    char method_return_struct[64] = {0};

                                    if (*after_func == ':') {
                                        after_func++;
                                        while (*after_func && (*after_func == ' ' || *after_func == '\t')) after_func++;

                                        const char* ret_type_start = after_func;
                                        while (*after_func && (isalnum((unsigned char)*after_func) || *after_func == '_')) after_func++;
                                        int ret_type_len = (int)(after_func - ret_type_start);

                                        if (ret_type_len > 0 && ret_type_len < 64) {
                                            char ret_type_str[64];
                                            strncpy(ret_type_str, ret_type_start, ret_type_len);
                                            ret_type_str[ret_type_len] = '\0';
                                            method_return_type = parse_base_type(ret_type_str);
                                            if (method_return_type == TYPE_ANY) {
                                                if (is_known_struct(ret_type_str, struct_names, struct_name_count)) {
                                                    method_return_type = TYPE_STRUCT;
                                                    size_t copy_len = strlen(ret_type_str);
                                                    if (copy_len > sizeof(method_return_struct) - 1) copy_len = sizeof(method_return_struct) - 1;
                                                    memcpy(method_return_struct, ret_type_str, copy_len);
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
                                    method_count++;
                                }

                                after_face = after_func;
                                continue;
                            }

                            // 跳过其他内容
                            while (*after_face && *after_face != '\n' && *after_face != '}') after_face++;
                        }

                        // 添加 face 到符号表
                        module_symbol_table_add_face(table, face_name, method_count, methods);

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

                    // 添加 enum 到符号表
                    module_symbol_table_add_enum(table, enum_name);

                    // 跳过 enum 定义体
                    while (*after_enum && *after_enum != '{') after_enum++;
                    if (*after_enum == '{') {
                        after_enum++;
                        int brace_depth = 1;
                        while (*after_enum && brace_depth > 0) {
                            if (*after_enum == '{') brace_depth++;
                            else if (*after_enum == '}') brace_depth--;
                            after_enum++;
                        }
                    }
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
                        char return_struct_name[64] = {0};

                        if (*after_func == ':') {
                            after_func++;
                            while (*after_func && (*after_func == ' ' || *after_func == '\t')) after_func++;

                            const char* type_start = after_func;
                            while (*after_func && (isalnum((unsigned char)*after_func) || *after_func == '_')) after_func++;
                            int type_len = (int)(after_func - type_start);

                            if (type_len > 0 && type_len < 64) {
                                char type_str[64];
                                strncpy(type_str, type_start, type_len);
                                type_str[type_len] = '\0';

                                // 先检查是否是基本类型
                                return_type = parse_base_type(type_str);

                                // 如果不是基本类型，检查是否是已知的 struct
                                if (return_type == TYPE_ANY) {
                                    if (is_known_struct(type_str, struct_names, struct_name_count)) {
                                        return_type = TYPE_STRUCT;
                                        size_t copy_len = strlen(type_str);
                                        if (copy_len > sizeof(return_struct_name) - 1) copy_len = sizeof(return_struct_name) - 1;
                                        memcpy(return_struct_name, type_str, copy_len);
                                        return_struct_name[copy_len] = '\0';
                                    }
                                }
                            }
                        }

                        module_symbol_table_add_func(table, func_name, return_type,
                            return_struct_name[0] ? return_struct_name : NULL);
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

                            // 如果不是基本类型，检查是否是已知的 struct
                            if (var_type == TYPE_ANY) {
                                if (is_known_struct(type_str, struct_names, struct_name_count)) {
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
        }

        p++;
    }

    // 释放 struct 名称数组
    for (int i = 0; i < struct_name_count; i++) {
        free(struct_names[i]);
    }

    free(source);
    return 0;
}
