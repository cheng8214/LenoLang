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
        // 格式: use module.Type           — 单个导入
        //       use module.(A, B, C)      — 批量导入
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
                
                // 判断批量模式: use module.(A, B, C) 还是 use module.Type
                int batch_mode = (*p == '(');
                if (batch_mode) p++; // skip '('
                
                // 循环处理每个类型名（单模式只循环一次，批量模式循环到 ')'）
                while (1) {
                    if (batch_mode) {
                        while (*p && (*p == ' ' || *p == '\t')) p++;
                        if (*p == ')') { p++; break; } // 批量模式结束
                    }
                    
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
                                        if (e_sym && !module_symbol_table_find_enum(table, type_name)) {
                                            // 将 enum 完整信息（成员名+成员值）添加到当前模块的符号表
                                            module_symbol_table_add_enum(table, type_name,
                                                e_sym->member_count, e_sym->member_names, e_sym->member_values);
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
                                            // 递归传导 alias 底层类型依赖到当前模块符号表
                                            // 如 EventHandler = func(Event):bool，需要把 Event 也传导过来
                                            if (a_sym->type_info) {
                                                import_alias_type_deps_to_sym_table(table, dep_table, a_sym->type_info,
                                                    struct_names, &struct_name_count,
                                                    cstruct_names, &cstruct_name_count,
                                                    enum_names, &enum_name_count,
                                                    face_names, &face_name_count,
                                                    local_alias_names, local_alias_types, &local_alias_count);
                                            }
                                        }
                                    }
                                }
                                module_symbol_table_destroy(dep_table);
                            }
                        }
                    }
                    
                    if (!batch_mode) break; // 单模式：只处理一次
                    
                    // 批量模式：跳过逗号和空白，继续下一个类型名
                    while (*p && (*p == ' ' || *p == '\t')) p++;
                    if (*p == ',') p++;
                    while (*p && (*p == ' ' || *p == '\t')) p++;
                    // 允许尾逗号
                    if (*p == ')') { p++; break; }
                }
            } else {
                // 没有 '.' 的情况，跳过本行
            }
            continue;
        }
