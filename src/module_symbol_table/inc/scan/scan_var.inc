            // 检查是否是 var/const 定义
            {
                int is_const_decl = 0;
                char* after_var = NULL;
                if (strncmp(after_export, "const", 5) == 0 && (after_export[5] == ' ' || after_export[5] == '\t' || after_export[5] == '\n' || after_export[5] == '\r' || after_export[5] == '\0')) {
                    is_const_decl = 1;
                    after_var = after_export + 5;
                } else if (strncmp(after_export, "var", 3) == 0 && (after_export[3] == ' ' || after_export[3] == '\t' || after_export[3] == '\n' || after_export[3] == '\r' || after_export[3] == '\0')) {
                    after_var = after_export + 3;
                }
                if (after_var) {
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
                                } else {
                                    // 尝试解析跨模块引用: module.member
                                    // 格式: export const X = core.YYY
                                    const char* mod_start = after_var;
                                    const char* mod_end = after_var;
                                    while (*mod_end && (isalnum((unsigned char)*mod_end) || *mod_end == '_')) mod_end++;
                                    int mod_len = (int)(mod_end - mod_start);
                                    
                                    if (mod_len > 0 && mod_len < 64 && *mod_end == '.') {
                                        char mod_alias[64];
                                        memcpy(mod_alias, mod_start, mod_len);
                                        mod_alias[mod_len] = '\0';
                                        
                                        const char* member_start = mod_end + 1;
                                        const char* member_end = member_start;
                                        while (*member_end && (isalnum((unsigned char)*member_end) || *member_end == '_')) member_end++;
                                        int member_len = (int)(member_end - member_start);
                                        
                                        if (member_len > 0 && member_len < 64) {
                                            char member_name[64];
                                            memcpy(member_name, member_start, member_len);
                                            member_name[member_len] = '\0';
                                            
                                            // 在源码中反向查找 import "path" as mod_alias 来解析模块路径
                                            char* import_search = source;
                                            char resolved_path[1024] = {0};
                                            while (import_search < after_var) {
                                                if (strncmp(import_search, "import", 6) == 0 && (import_search[6] == ' ' || import_search[6] == '\t' || import_search[6] == '\"')) {
                                                    import_search += 6;
                                                    while (*import_search && (*import_search == ' ' || *import_search == '\t')) import_search++;
                                                    if (*import_search == '"') {
                                                        import_search++;
                                                        const char* path_start = import_search;
                                                        while (*import_search && *import_search != '"') import_search++;
                                                        int path_len = (int)(import_search - path_start);
                                                        import_search++; // skip closing "
                                                        
                                                        while (*import_search && (*import_search == ' ' || *import_search == '\t')) import_search++;
                                                        if (strncmp(import_search, "as", 2) == 0 && (import_search[2] == ' ' || import_search[2] == '\t')) {
                                                            import_search += 2;
                                                            while (*import_search && (*import_search == ' ' || *import_search == '\t')) import_search++;
                                                            const char* as_start = import_search;
                                                            while (*import_search && (isalnum((unsigned char)*import_search) || *import_search == '_')) import_search++;
                                                            int as_len = (int)(import_search - as_start);
                                                            if (as_len == mod_len && strncmp(as_start, mod_alias, mod_len) == 0) {
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
                                            
                                            // 扫描依赖模块，查找变量类型
                                            if (resolved_path[0]) {
                                                ModuleSymbolTable* dep_table = module_symbol_table_create(resolved_path);
                                                if (dep_table) {
                                                    if (module_symbol_table_scan(dep_table, normalized_current) == 0) {
                                                        // 查找变量
                                                        ModuleVarSymbol* dep_var = module_symbol_table_find_var(dep_table, member_name);
                                                        if (dep_var) {
                                                            var_type = dep_var->type;
                                                            if ((dep_var->type == TYPE_STRUCT || dep_var->type == TYPE_CSTRUCT) && dep_var->struct_name) {
                                                                size_t copy_len = strlen(dep_var->struct_name);
                                                                if (copy_len > sizeof(var_struct_name) - 1) copy_len = sizeof(var_struct_name) - 1;
                                                                memcpy(var_struct_name, dep_var->struct_name, copy_len);
                                                                var_struct_name[copy_len] = '\0';
                                                            }
                                                        }
                                                        // 查找 enum 成员（enum 成员也可被引用为常量）
                                                        if (var_type == TYPE_ANY) {
                                                            for (int ei = 0; ei < dep_table->enum_count; ei++) {
                                                                ModuleEnumSymbol* es = &dep_table->enums[ei];
                                                                for (int mi = 0; mi < es->member_count; mi++) {
                                                                    if (strcmp(es->member_names[mi], member_name) == 0) {
                                                                        var_type = TYPE_INT;
                                                                        break;
                                                                    }
                                                                }
                                                                if (var_type != TYPE_ANY) break;
                                                            }
                                                        }
                                                    }
                                                    module_symbol_table_destroy(dep_table);
                                                }
                                            }
                                        }
                                    }
                                    // 其他情况保持 TYPE_ANY
                                }
                            }
                        }

                        module_symbol_table_add_var(table, var_name, var_type,
                            var_struct_name[0] ? var_struct_name : NULL, is_const_decl);
                    }

                    p = after_var;
                    continue;
                }
            }
