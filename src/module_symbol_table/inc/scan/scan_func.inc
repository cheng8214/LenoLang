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
