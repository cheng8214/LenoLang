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
                                // 添加到本地解析表（动态追加，无固定上限）
                                mod_alias_push(&local_alias_names, &local_alias_types, &local_alias_count, &local_alias_cap,
                                    alias_name, type_copy(ti_ptr));
                                type_free(ti_ptr);
                            }
                        }
                    }
                }

                p = after_alias;
                continue;
            }
