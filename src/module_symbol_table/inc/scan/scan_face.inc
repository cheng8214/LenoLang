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
                            const char* fp_iter_start = after_face;   // 进度守卫基准
                            // 跳过空白（含换行，泛型参数列表可跨行）
                            while (*after_face && (*after_face == ' ' || *after_face == '\t' ||
                                                   *after_face == '\n' || *after_face == '\r')) after_face++;
                            if (*after_face && (isalnum((unsigned char)*after_face) || *after_face == '_')) {
                                face_type_param_count++;
                                while (*after_face && (isalnum((unsigned char)*after_face) || *after_face == '_')) after_face++;
                            }
                            while (*after_face && (*after_face == ' ' || *after_face == '\t' ||
                                                   *after_face == '\n' || *after_face == '\r')) after_face++;
                            if (*after_face == ',') after_face++;
                            // 本圈没推进就退出，避免死循环
                            if (after_face == fp_iter_start) break;
                        }
                        if (*after_face == ']') after_face++; // skip ']'
                    }

                    while (*after_face && (*after_face == ' ' || *after_face == '\t' || *after_face == '\n' || *after_face == '\r')) after_face++;

                    if (*after_face == '{') {
                        after_face++;

                        // 方法数组按需动态增长（无固定上限，修复超过 128 个方法被静默丢弃的问题）
                        ModuleFaceMethodSymbol* methods = NULL;
                        int method_capacity = 0;
                        int method_count = 0;

                        while (*after_face && *after_face != '}') {
                            while (*after_face && (*after_face == ' ' || *after_face == '\t' || *after_face == '\n' || *after_face == '\r')) after_face++;
                            if (!*after_face || *after_face == '}') break;

                            if (*after_face == '/' && *(after_face+1) == '/') {
                                while (*after_face && *after_face != '\n') after_face++;
                                continue;
                            }
                            // 跳过多行注释 /* ... */
                            if (*after_face == '/' && *(after_face+1) == '*') {
                                after_face += 2;
                                while (*after_face && !(*after_face == '*' && *(after_face+1) == '/')) after_face++;
                                if (*after_face) after_face += 2;
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
                                    // 跳过参数列表：既数个数，**也把参数原文抓下来**（⑰-2 要从中解析类型）
                                    while (*after_func && (*after_func == ' ' || *after_func == '\t')) after_func++;
                                    int param_count = 0;
                                    char param_text[512] = {0};
                                    int param_text_len = 0;
                                    if (*after_func == '(') {
                                        after_func++;
                                        int paren_depth = 1;
                                        int has_param = 0;
                                        while (*after_func && paren_depth > 0) {
                                            // 跳过注释
                                            if (*after_func == '/' && *(after_func+1) == '/') {
                                                while (*after_func && *after_func != '\n') after_func++;
                                                continue;
                                            }
                                            if (*after_func == '/' && *(after_func+1) == '*') {
                                                after_func += 2;
                                                while (*after_func && !(*after_func == '*' && *(after_func+1) == '/')) after_func++;
                                                if (*after_func) after_func += 2;
                                                continue;
                                            }
                                            if (*after_func == '(') paren_depth++;
                                            else if (*after_func == ')') paren_depth--;
                                            else if (paren_depth == 1) {
                                                if (*after_func == ',') param_count++;
                                                else if (!isspace((unsigned char)*after_func)) has_param = 1;
                                            }
                                            // ⑰-2：抓深度 1 的参数原文
                                            if (paren_depth >= 1 && param_text_len < (int)sizeof(param_text) - 1) {
                                                param_text[param_text_len++] = *after_func;
                                                param_text[param_text_len] = '\0';
                                            }
                                            after_func++;
                                        }
                                        if (has_param) param_count++;
                                    }

                                    // ---- ⑰-2：把参数原文解析成类型 ----
                                    // 此前 face 只存 `param_count` ⇒ **跨模块**的 face 方法实参只判得了个数、
                                    // 判不了类型（同文件能判：解析器的 face AST 有 method_param_types）。
                                    // 解析走**唯一实现** `mod_scan_params`（`scan_params.inc`）—— 本处原先
                                    // 内联的那份与它逐条对应，已合并（⑧ 那一族的教训：参数解析只留一份，
                                    // 四个名字表都在那一份里查）。
                                    TypeKind face_param_types[MOD_MAX_PARAMS];
                                    char face_param_structs[MOD_MAX_PARAMS][MOD_MAX_NAMES];
                                    int face_param_parsed = mod_scan_params(param_text, MOD_MAX_PARAMS,
                                        face_param_types, face_param_structs, NULL,
                                        clib_names, clib_name_count,
                                        face_names, face_name_count,
                                        cstruct_names, cstruct_name_count,
                                        struct_names, struct_name_count,
                                        NULL, 0);

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
                                                // 名字解析走**唯一政策点**（㉓）
                                                char agg_name[64] = {0};
                                                TypeKind ak = mod_resolve_param_type(base_str,
                                                    agg_name, (int)sizeof(agg_name), NULL, 0,
                                                    clib_names, clib_name_count,
                                                    face_names, face_name_count,
                                                    cstruct_names, cstruct_name_count,
                                                    struct_names, struct_name_count,
                                                    NULL, 0);
                                                if (ak != TYPE_ANY) {
                                                    method_return_type = ak;
                                                    // struct 分支原本不更新 kind（默认即 TYPE_STRUCT）⇒ 保持等价
                                                    if (ak != TYPE_STRUCT && method_return_type_info) {
                                                        method_return_type_info->kind = ak;
                                                    }
                                                    if (agg_name[0]) {
                                                        size_t copy_len = strlen(agg_name);
                                                        if (copy_len > sizeof(method_return_struct) - 1) copy_len = sizeof(method_return_struct) - 1;
                                                        memcpy(method_return_struct, agg_name, copy_len);
                                                        method_return_struct[copy_len] = '\0';
                                                    }
                                                }
                                            }
                                        }
                                    }

                                    // 释放临时 method_return_type_info（face 方法符号不存储此字段）
                                    if (method_return_type_info) { type_free(method_return_type_info); method_return_type_info = NULL; }
                                    // 添加方法（动态扩容，容量不足时翻倍）
                                    if (method_count < method_capacity) {
                                        // 容量足够
                                    } else {
                                        int new_cap = method_capacity == 0 ? 16 : method_capacity * 2;
                                        ModuleFaceMethodSymbol* new_methods = (ModuleFaceMethodSymbol*)realloc(methods, sizeof(ModuleFaceMethodSymbol) * new_cap);
                                        if (new_methods) {
                                            methods = new_methods;
                                            method_capacity = new_cap;
                                        }
                                    }
                                    if (method_count < method_capacity) {
                                        methods[method_count].name = (char*)malloc(method_name_len + 1);
                                        strncpy(methods[method_count].name, method_name_start, method_name_len);
                                        methods[method_count].name[method_name_len] = '\0';
                                        methods[method_count].return_type = method_return_type;
                                        methods[method_count].return_struct_name = method_return_struct[0] ? strdup(method_return_struct) : NULL;
                                        methods[method_count].param_count = param_count;
                                        // ⑰-2：参数类型（此前只存个数 ⇒ 跨模块 face 方法实参判不了类型）
                                        methods[method_count].param_types = NULL;
                                        methods[method_count].param_struct_names = NULL;
                                        if (param_count > 0) {
                                            methods[method_count].param_types = (TypeKind*)calloc(param_count, sizeof(TypeKind));
                                            methods[method_count].param_struct_names = (char**)calloc(param_count, sizeof(char*));
                                            int np = face_param_parsed < param_count ? face_param_parsed : param_count;
                                            for (int pi = 0; pi < np; pi++) {
                                                methods[method_count].param_types[pi] = face_param_types[pi];
                                                if (face_param_structs[pi][0]) {
                                                    methods[method_count].param_struct_names[pi] =
                                                        strdup(face_param_structs[pi]);
                                                }
                                            }
                                        }
                                        method_count++;
                                    }
                                    // 内存不足（realloc 失败）时跳过该方法，继续扫描
                                }

                                after_face = after_func;
                                continue;
                            }

                            // 跳过其他内容
                            while (*after_face && *after_face != '\n' && *after_face != '}') {
                                if (*after_face == '/' && *(after_face+1) == '/') {
                                    while (*after_face && *after_face != '\n') after_face++;
                                    break;
                                }
                                if (*after_face == '/' && *(after_face+1) == '*') {
                                    after_face += 2;
                                    while (*after_face && !(*after_face == '*' && *(after_face+1) == '/')) after_face++;
                                    if (*after_face) after_face += 2;
                                    continue;
                                }
                                after_face++;
                            }
                        }

                        // 添加 face 到符号表
                        module_symbol_table_add_face(table, face_name, method_count, methods, face_type_param_count);

                        // 释放临时分配的内存（add_face 已复制数据）
                        for (int i = 0; i < method_count; i++) {
                            free(methods[i].name);
                            free(methods[i].return_struct_name);
                            // ⑰-2：参数类型与聚合名（add_face 里做了深拷贝）
                            free(methods[i].param_types);
                            if (methods[i].param_struct_names) {
                                for (int pi = 0; pi < methods[i].param_count; pi++) {
                                    free(methods[i].param_struct_names[pi]);
                                }
                                free(methods[i].param_struct_names);
                            }
                        }
                        free(methods);

                        while (*after_face && *after_face != '}') after_face++;
                        if (*after_face == '}') after_face++;
                    }
                }

                p = after_face;
                continue;
            }
