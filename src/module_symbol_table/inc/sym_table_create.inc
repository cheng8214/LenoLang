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
    table->cfuncs = NULL;
    table->cfunc_count = 0;
    table->cfunc_capacity = 0;
    table->dep_paths = NULL;
    table->dep_count = 0;
    table->dep_capacity = 0;

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
        free(table->funcs[i].param_text);
        if (table->funcs[i].return_type_info) {
            type_free(table->funcs[i].return_type_info);
        }
        // 释放默认参数文本
        if (table->funcs[i].param_default_texts) {
            for (int j = 0; j < table->funcs[i].param_count; j++) {
                free(table->funcs[i].param_default_texts[j]);
            }
            free(table->funcs[i].param_default_texts);
        }
    }
    free(table->funcs);

    // 释放 struct 符号
    for (int i = 0; i < table->struct_count; i++) {
        free(table->structs[i].name);
        for (int j = 0; j < table->structs[i].field_count; j++) {
            free(table->structs[i].fields[j].name);
            free(table->structs[i].fields[j].struct_name);
            free(table->structs[i].fields[j].element_struct_name);
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
        // 释放关联常量名数组
        for (int j = 0; j < table->structs[i].const_count; j++) {
            free(table->structs[i].const_names[j]);
            if (table->structs[i].const_value_strs && table->structs[i].const_value_strs[j]) {
                free(table->structs[i].const_value_strs[j]);
            }
        }
        free(table->structs[i].const_names);
        free(table->structs[i].const_value_strs);
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
            free(table->clibs[i].funcs[j].param_element_types);
            for (int k = 0; k < table->clibs[i].funcs[j].param_count; k++) {
                free(table->clibs[i].funcs[j].param_struct_names[k]);
            }
            free(table->clibs[i].funcs[j].param_types);
            free(table->clibs[i].funcs[j].param_struct_names);
        }
        free(table->clibs[i].funcs);
    }
    free(table->clibs);

    // 释放 cfunc 符号
    for (int i = 0; i < table->cfunc_count; i++) {
        free(table->cfuncs[i].name);
        free(table->cfuncs[i].param_types);
        free(table->cfuncs[i].param_element_types);
        free(table->cfuncs[i].return_struct_name);
        if (table->cfuncs[i].param_struct_names) {
            for (int j = 0; j < table->cfuncs[i].param_count; j++) {
                free(table->cfuncs[i].param_struct_names[j]);
            }
            free(table->cfuncs[i].param_struct_names);
        }
        if (table->cfuncs[i].param_names) {
            for (int j = 0; j < table->cfuncs[i].param_count; j++) {
                free(table->cfuncs[i].param_names[j]);
            }
            free(table->cfuncs[i].param_names);
        }
    }
    free(table->cfuncs);

    // 释放依赖路径数组
    for (int i = 0; i < table->dep_count; i++) {
        free(table->dep_paths[i]);
    }
    free(table->dep_paths);

    free(table);
}
