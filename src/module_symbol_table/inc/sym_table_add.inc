// 添加函数符号
void module_symbol_table_add_func(ModuleSymbolTable* table, const char* name, TypeKind return_type, const char* return_struct_name, int type_param_count, TypeInfo* return_type_info, const char* param_text) {
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
    func->param_text = param_text ? strdup(param_text) : NULL;
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
        st->fields = (ModuleStructField*)calloc(field_count, sizeof(ModuleStructField));
        for (int i = 0; i < field_count; i++) {
            st->fields[i].name = strdup(fields[i].name);
            st->fields[i].type = fields[i].type;
            st->fields[i].struct_name = fields[i].struct_name ? strdup(fields[i].struct_name) : NULL;
            st->fields[i].element_type = fields[i].element_type;
            st->fields[i].element_struct_name = fields[i].element_struct_name ? strdup(fields[i].element_struct_name) : NULL;
            st->fields[i].nullable = fields[i].nullable;
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
void module_symbol_table_add_enum(ModuleSymbolTable* table, const char* name, int member_count, char** member_names, int64_t* member_values) {
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
        en->member_values = (int64_t*)malloc(sizeof(int64_t) * member_count);
        for (int i = 0; i < member_count; i++) {
            en->member_names[i] = strdup(member_names[i]);
            // 如果提供了显式值则使用，否则自动递增
            en->member_values[i] = (member_values && member_values[i] != -1) ? member_values[i] : (int64_t)i;
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
void module_symbol_table_add_var(ModuleSymbolTable* table, const char* name, TypeKind type, const char* struct_name, int is_const) {
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
    var->is_const = is_const;
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
        cl->funcs[i].return_element_type = funcs[i].return_element_type;
        cl->funcs[i].return_struct_name = funcs[i].return_struct_name ? strdup(funcs[i].return_struct_name) : NULL;
        cl->funcs[i].param_count = funcs[i].param_count;
        cl->funcs[i].param_types = (TypeKind*)malloc(sizeof(TypeKind) * funcs[i].param_count);
        cl->funcs[i].param_element_types = (TypeKind*)malloc(sizeof(TypeKind) * funcs[i].param_count);
        cl->funcs[i].param_struct_names = (char**)calloc(funcs[i].param_count, sizeof(char*));
        for (int j = 0; j < funcs[i].param_count; j++) {
            cl->funcs[i].param_types[j] = funcs[i].param_types[j];
            cl->funcs[i].param_element_types[j] = funcs[i].param_element_types[j];
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
