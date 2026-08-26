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
                        // 关联常量临时数组
                        char* const_names_tmp[64];
                        char* const_values_tmp[64];
                        int const_count_tmp = 0;

                        while (*after_struct && *after_struct != '}' && (field_count < MOD_MAX_FIELDS || method_count < MOD_MAX_METHODS)) {
                            while (*after_struct && (*after_struct == ' ' || *after_struct == '\t' || *after_struct == '\n' || *after_struct == '\r')) after_struct++;
                            if (!*after_struct || *after_struct == '}') break;

                            if (*after_struct == '/' && *(after_struct+1) == '/') {
                                while (*after_struct && *after_struct != '\n') after_struct++;
                                continue;
                            }

                            // 检查是否是关联常量声明 (const NAME = value)
                            if (strncmp(after_struct, "const", 5) == 0 && (after_struct[5] == ' ' || after_struct[5] == '\t')) {
                                char* after_const = after_struct + 5;
                                while (*after_const && (*after_const == ' ' || *after_const == '\t')) after_const++;

                                // 跳过可选的类型注解 (const int X = 1)
                                // 预读：如果标识符后面跟 = 或 ,，说明是类型推断形式
                                const char* peek_start = after_const;
                                while (*after_const && (isalnum((unsigned char)*after_const) || *after_const == '_')) after_const++;
                                // 跳过空白
                                while (*after_const && (*after_const == ' ' || *after_const == '\t')) after_const++;
                                if (*after_const != '=' && *after_const != ',') {
                                    // 显式类型形式：token 是类型，跳过类型后再读名称
                                    after_const = (char*)peek_start; // 回退
                                    // 跳过类型
                                    while (*after_const && (isalnum((unsigned char)*after_const) || *after_const == '_')) after_const++;
                                    // 跳过 [T] 泛型部分
                                    while (*after_const && (*after_const == ' ' || *after_const == '\t')) after_const++;
                                    if (*after_const == '[') {
                                        after_const++;
                                        int depth = 1;
                                        while (*after_const && depth > 0) {
                                            if (*after_const == '[') depth++;
                                            else if (*after_const == ']') depth--;
                                            after_const++;
                                        }
                                    }
                                    while (*after_const && (*after_const == ' ' || *after_const == '\t')) after_const++;
                                } else {
                                    // 类型推断形式：token 就是常量名
                                    after_const = (char*)peek_start;
                                }

                                // 支持逗号分隔多个常量：const A = 1, B = 2
                                do {
                                    // 跳过空白
                                    while (*after_const && (*after_const == ' ' || *after_const == '\t')) after_const++;

                                    // 读取常量名
                                    const char* cname_start = after_const;
                                    while (*after_const && (isalnum((unsigned char)*after_const) || *after_const == '_')) after_const++;
                                    int cname_len = (int)(after_const - cname_start);

                                    if (cname_len > 0 && cname_len < 64) {
                                        // 存入临时 const_names 数组
                                        if (const_count_tmp < 64) {
                                            const_names_tmp[const_count_tmp] = (char*)malloc(cname_len + 1);
                                            strncpy(const_names_tmp[const_count_tmp], cname_start, cname_len);
                                            const_names_tmp[const_count_tmp][cname_len] = '\0';
                                            // 捕获常量值文本
                                            const_values_tmp[const_count_tmp] = NULL;
                                            // 跳过空白后检查 '='
                                            char* after_name = after_const;
                                            while (*after_name && (*after_name == ' ' || *after_name == '\t')) after_name++;
                                            if (*after_name == '=') {
                                                after_name++;
                                                // 跳过空白
                                                while (*after_name && (*after_name == ' ' || *after_name == '\t')) after_name++;
                                                const char* val_start = after_name;
                                                // 值表达式到逗号或行尾或分号或}
                                                while (*after_name && *after_name != ',' && *after_name != ';' && *after_name != '\n' && *after_name != '}') after_name++;
                                                int val_len = (int)(after_name - val_start);
                                                // 去除尾部空白
                                                while (val_len > 0 && (val_start[val_len-1] == ' ' || val_start[val_len-1] == '\t' || val_start[val_len-1] == '\r')) val_len--;
                                                if (val_len > 0) {
                                                    const_values_tmp[const_count_tmp] = (char*)malloc(val_len + 1);
                                                    strncpy(const_values_tmp[const_count_tmp], val_start, val_len);
                                                    const_values_tmp[const_count_tmp][val_len] = '\0';
                                                }
                                                after_const = after_name;
                                            }
                                            const_count_tmp++;
                                        }
                                    }

                                    // 跳过空白
                                    while (*after_const && (*after_const == ' ' || *after_const == '\t')) after_const++;
                                } while (*after_const == ',' && (after_const++, 1));

                                after_struct = after_const;
                                // 跳过已处理的值表达式部分
                                // （值已在上面处理完毕）
                                // 跳过分号
                                if (*after_struct == ';') after_struct++;
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

                                            // ★ 消费可空返回类型的尾部 '?'（如 HtmlNode?）
                                            // 若不消费，方法体将无法被跳过，导致从第一个可空返回方法起
                                            // 之后的 struct 方法全部丢失（跨模块 use 时报"没有方法"）
                                            while (*after_func && (*after_func == ' ' || *after_func == '\t')) after_func++;
                                            if (*after_func == '?') {
                                                after_func++;
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
                            // 检查类型名后的 ? (如 Font?)
                            int is_nullable = 0;
                            if (*after_struct == '?') {
                                is_nullable = 1;
                                after_struct++;
                            }
                            int type_len = (int)(after_struct - type_start);

                            if (type_len > 0 && type_len < 64) {
                                char type_str[64];
                                // 如果类型以 ? 结尾，去掉 ?（已由上面的 is_nullable 标记）
                                int copy_len = is_nullable ? type_len - 1 : type_len;
                                strncpy(type_str, type_start, copy_len);
                                type_str[copy_len] = '\0';

                                // 检查可空类型后缀 Type?（泛型 ] 后的 ? 已在上面处理）

                                // 检查是否是 Ptr[T] / Array[T] / Dict[K,V] 类型
                                TypeKind element_type = TYPE_PTR;  // 默认元素类型
                                char* element_struct_name = NULL;   // 元素类型的结构体/face/clib 名称
                                while (*after_struct && (*after_struct == ' ' || *after_struct == '\t')) after_struct++;
                                if (*after_struct == '[') {
                                    // 解析 [T] 中的 T
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
                                        // 如果元素类型是未识别的自定义类型，检查是否是 struct/face/clib
                                        if (element_type == TYPE_ANY) {
                                            if (is_known_struct(elem_type_str, struct_names, struct_name_count)) {
                                                element_type = TYPE_STRUCT;
                                                element_struct_name = strdup(elem_type_str);
                                            } else if (is_known_face(elem_type_str, face_names, face_name_count)) {
                                                element_type = TYPE_FACE;
                                                element_struct_name = strdup(elem_type_str);
                                            } else if (is_known_clib(elem_type_str, clib_names, clib_name_count)) {
                                                element_type = TYPE_CLIB;
                                                element_struct_name = strdup(elem_type_str);
                                            }
                                        }
                                    }
                                    // 跳过到 ] 为止的内容
                                    while (*after_struct && *after_struct != ']') after_struct++;
                                    if (*after_struct == ']') after_struct++;
                                    // 检查 ] 后是否有 ? (如 Array[Font]?)
                                    while (*after_struct && (*after_struct == ' ' || *after_struct == '\t')) after_struct++;
                                    if (*after_struct == '?') {
                                        is_nullable = 1;
                                        after_struct++;
                                    }
                                }

                                while (*after_struct && (*after_struct == ' ' || *after_struct == '\t')) after_struct++;

                                // 支持逗号分隔多字段：int a, b, c
                                do {
                                    // 跳过字段名前的空白（逗号后可能有空白）
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
                                        // 如果是未识别的自定义类型，检查是否是 alias、clib 或 struct
                                        if (ft == TYPE_ANY) {
                                            // ★ 先检查本地 alias（如 Color = int）
                                            //   alias 在 scan_alias.inc 中注册到 local_alias 表，
                                            //   若匹配则直接用 alias 底层类型的 TypeKind
                                            TypeInfo* alias_ti = FIND_LOCAL_ALIAS(type_str);
                                            if (alias_ti) {
                                                fields[field_count].type = alias_ti->kind;
                                                // alias 可能指向 struct/face/clib（如 Node = TreeNode）
                                                if (alias_ti->kind == TYPE_STRUCT && alias_ti->struct_name) {
                                                    fields[field_count].struct_name = strdup(alias_ti->struct_name);
                                                }
                                            } else if (is_known_clib(type_str, clib_names, clib_name_count)) {
                                                fields[field_count].type = TYPE_CLIB;
                                                fields[field_count].struct_name = strdup(type_str);
                                            } else if (is_known_struct(type_str, struct_names, struct_name_count)) {
                                                fields[field_count].type = TYPE_STRUCT;
                                                fields[field_count].struct_name = strdup(type_str);
                                            }
                                        }
                                        fields[field_count].element_type = element_type;
                                        fields[field_count].element_struct_name = element_struct_name;
                                        fields[field_count].nullable = is_nullable;
                                        // 仅第一个字段转移 element_struct_name 所有权
                                        if (field_count > 0) {
                                            // 后续字段需要独立的 element_struct_name 副本
                                            if (element_struct_name) {
                                                fields[field_count].element_struct_name = strdup(element_struct_name);
                                            }
                                        } else {
                                            // 第一个字段直接使用 element_struct_name 指针
                                        }
                                        if (field_count >= MOD_MAX_FIELDS) {
                                            fprintf(stderr, "[错误] struct字段数量超过上限 %d\n", MOD_MAX_FIELDS);
                                        } else {
                                            field_count++;
                                        }
                                    }

                                    // 跳过空白
                                    while (*after_struct && (*after_struct == ' ' || *after_struct == '\t')) after_struct++;

                                    // 跳过字段默认值（逗号前的默认值：int a = 5, b = 3）
                                    if (*after_struct == '=') {
                                        after_struct++; // 跳过 =
                                        while (*after_struct && (*after_struct == ' ' || *after_struct == '\t')) after_struct++;
                                        // 处理默认值中的嵌套括号
                                        if (*after_struct == '{') {
                                            int depth = 1;
                                            after_struct++;
                                            while (*after_struct && depth > 0) {
                                                if (*after_struct == '{') depth++;
                                                else if (*after_struct == '}') depth--;
                                                after_struct++;
                                            }
                                        } else if (*after_struct == '[') {
                                            int depth = 1;
                                            after_struct++;
                                            while (*after_struct && depth > 0) {
                                                if (*after_struct == '[') depth++;
                                                else if (*after_struct == ']') depth--;
                                                after_struct++;
                                            }
                                        } else {
                                            // 简单值，跳到逗号或分号或行尾或 }
                                            while (*after_struct && *after_struct != ',' && *after_struct != ';' && *after_struct != '\n' && *after_struct != '}') after_struct++;
                                        }
                                        // 跳过空白
                                        while (*after_struct && (*after_struct == ' ' || *after_struct == '\t')) after_struct++;
                                    }
                                } while (*after_struct == ',' && (after_struct++, 1));
                            }

                            // 跳过字段默认值（可能包含嵌套的 {} 或 []，如 Dict val = {}）
	                            // 先跳过到 = 或行尾/分号/}
	                            while (*after_struct && *after_struct != '\n' && *after_struct != ';' && *after_struct != '=' && *after_struct != '}') after_struct++;
	                            if (*after_struct == '=') {
	                                after_struct++; // 跳过 =
	                                // 跳过空白
	                                while (*after_struct && (*after_struct == ' ' || *after_struct == '\t')) after_struct++;
	                                // 处理默认值中的嵌套括号
	                                if (*after_struct == '{') {
	                                    int depth = 1;
	                                    after_struct++;
	                                    while (*after_struct && depth > 0) {
	                                        if (*after_struct == '{') depth++;
	                                        else if (*after_struct == '}') depth--;
	                                        after_struct++;
	                                    }
	                                } else if (*after_struct == '[') {
	                                    int depth = 1;
	                                    after_struct++;
	                                    while (*after_struct && depth > 0) {
	                                        if (*after_struct == '[') depth++;
	                                        else if (*after_struct == ']') depth--;
	                                        after_struct++;
	                                    }
	                                } else {
	                                    // 简单值（数字、字符串等），跳到 ; 或行尾或 }
	                                    while (*after_struct && *after_struct != '\n' && *after_struct != ';' && *after_struct != '}') after_struct++;
	                                }
	                            }
	                            // 跳过分号（无论是否有默认值）
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

                        // 设置关联常量信息
                        if (added_struct && const_count_tmp > 0) {
                            added_struct->const_count = const_count_tmp;
                            added_struct->const_names = (char**)malloc(sizeof(char*) * const_count_tmp);
                            added_struct->const_value_strs = (char**)calloc(const_count_tmp, sizeof(char*));
                            for (int i = 0; i < const_count_tmp; i++) {
                                added_struct->const_names[i] = const_names_tmp[i];
                                const_names_tmp[i] = NULL; // 转移所有权
                                added_struct->const_value_strs[i] = const_values_tmp[i];
                                const_values_tmp[i] = NULL; // 转移所有权
                            }
                        } else {
                            // 释放临时分配的 const_names 和 const_values
                            for (int i = 0; i < const_count_tmp; i++) {
                                if (const_names_tmp[i]) free(const_names_tmp[i]);
                                if (const_values_tmp[i]) free(const_values_tmp[i]);
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
