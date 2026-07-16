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
