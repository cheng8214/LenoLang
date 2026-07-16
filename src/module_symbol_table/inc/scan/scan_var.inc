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
