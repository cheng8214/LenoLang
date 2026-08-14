    // 只读取一次文件
    char* source = read_module_file(table->module_path, current_file);
    if (!source) return -1;

    // 构建当前模块的规范化绝对路径，供后续递归扫描使用
    // 这样子模块的相对路径可以基于正确的目录解析
    char normalized_current[MAX_PATH_LEN] = {0};
    // 判断 module_path 是否已经是绝对路径
    int module_path_is_absolute = 0;
    if (table->module_path) {
    #ifdef _WIN32
        if (table->module_path[1] == ':' && (table->module_path[2] == '\\' || table->module_path[2] == '/'))
            module_path_is_absolute = 1;
    #else
        if (table->module_path[0] == '/')
            module_path_is_absolute = 1;
    #endif
    }
    if (module_path_is_absolute) {
        // 已经是绝对路径，直接使用
        strncpy(normalized_current, table->module_path, MAX_PATH_LEN - 1);
        normalized_current[MAX_PATH_LEN - 1] = '\0';
    } else if (current_file) {
        strncpy(normalized_current, current_file, MAX_PATH_LEN - 1);
        normalized_current[MAX_PATH_LEN - 1] = '\0';
        char* last_sep = strrchr(normalized_current, '\\');
        if (!last_sep) last_sep = strrchr(normalized_current, '/');
        if (last_sep) {
            *(last_sep + 1) = '\0';
            strncat(normalized_current, table->module_path, MAX_PATH_LEN - strlen(normalized_current) - 1);
        } else {
            strncpy(normalized_current, table->module_path, MAX_PATH_LEN - 1);
        }
    } else {
        strncpy(normalized_current, table->module_path, MAX_PATH_LEN - 1);
    }
    // 规范化路径（解析 ../ 等）
    normalize_path(normalized_current, MAX_PATH_LEN);

    // 第一遍：收集所有 struct/cstruct/clib/face/enum 名称
    char* struct_names[MOD_MAX_TYPES];
    int struct_name_count = 0;
    char* cstruct_names[MOD_MAX_TYPES];
    int cstruct_name_count = 0;
    char* face_names[MOD_MAX_TYPES];
    int face_name_count = 0;
    char* clib_names[MOD_MAX_CLIBS];
    int clib_name_count = 0;
    char* enum_names[MOD_MAX_ENUMS];
    int enum_name_count = 0;
    char* p = source;

    while (*p && struct_name_count < MOD_MAX_TYPES) {
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

        // 查找 export struct 或 export cstruct
        if (strncmp(p, "export", 6) == 0 && (p[6] == ' ' || p[6] == '\t' || p[6] == '\n' || p[6] == '\r' || p[6] == '\0')) {
            char* after_export = p + 6;
            while (*after_export && (*after_export == ' ' || *after_export == '\t' || *after_export == '\n' || *after_export == '\r')) after_export++;

            // 检查 export struct
            if (strncmp(after_export, "struct", 6) == 0 && (after_export[6] == ' ' || after_export[6] == '\t' || after_export[6] == '\n' || after_export[6] == '\r' || after_export[6] == '\0')) {
                char* after_struct = after_export + 6;
                while (*after_struct && (*after_struct == ' ' || *after_struct == '\t')) after_struct++;

                const char* name_start = after_struct;
                while (*after_struct && (isalnum((unsigned char)*after_struct) || *after_struct == '_')) after_struct++;
                int name_len = (int)(after_struct - name_start);

                if (name_len > 0 && name_len < 64) {
                    if (struct_name_count >= MOD_MAX_TYPES) {
                        fprintf(stderr, "[错误] struct数量超过上限 %d\n", MOD_MAX_TYPES);
                    } else {
                        struct_names[struct_name_count] = (char*)malloc(name_len + 1);
                        strncpy(struct_names[struct_name_count], name_start, name_len);
                        struct_names[struct_name_count][name_len] = '\0';
                        struct_name_count++;
                    }
                }

                p = after_struct;
                continue;
            }

            // 检查 export cstruct
            if (strncmp(after_export, "cstruct", 7) == 0 && (after_export[7] == ' ' || after_export[7] == '\t' || after_export[7] == '\n' || after_export[7] == '\r' || after_export[7] == '\0')) {
                char* after_cstruct = after_export + 7;
                while (*after_cstruct && (*after_cstruct == ' ' || *after_cstruct == '\t')) after_cstruct++;

                const char* name_start = after_cstruct;
                while (*after_cstruct && (isalnum((unsigned char)*after_cstruct) || *after_cstruct == '_')) after_cstruct++;
                int name_len = (int)(after_cstruct - name_start);

                if (name_len > 0 && name_len < 64) {
                    if (cstruct_name_count >= MOD_MAX_TYPES) {
                        fprintf(stderr, "[错误] cstruct数量超过上限 %d\n", MOD_MAX_TYPES);
                    } else {
                        cstruct_names[cstruct_name_count] = (char*)malloc(name_len + 1);
                        strncpy(cstruct_names[cstruct_name_count], name_start, name_len);
                        cstruct_names[cstruct_name_count][name_len] = '\0';
                        cstruct_name_count++;
                    }
                }

                p = after_cstruct;
                continue;
            }

            // 检查 export face
            if (strncmp(after_export, "face", 4) == 0 && (after_export[4] == ' ' || after_export[4] == '\t' || after_export[4] == '\n' || after_export[4] == '\r' || after_export[4] == '\0')) {
                char* after_face = after_export + 4;
                while (*after_face && (*after_face == ' ' || *after_face == '\t')) after_face++;

                const char* name_start = after_face;
                while (*after_face && (isalnum((unsigned char)*after_face) || *after_face == '_')) after_face++;
                int name_len = (int)(after_face - name_start);

                if (name_len > 0 && name_len < 64) {
                    if (face_name_count >= MOD_MAX_TYPES) {
                        fprintf(stderr, "[错误] face数量超过上限 %d\n", MOD_MAX_TYPES);
                    } else {
                        face_names[face_name_count] = (char*)malloc(name_len + 1);
                        strncpy(face_names[face_name_count], name_start, name_len);
                        face_names[face_name_count][name_len] = '\0';
                        face_name_count++;
                    }
                }

                p = after_face;
                continue;
            }

            // 检查 export clib
            if (strncmp(after_export, "clib", 4) == 0 && (after_export[4] == ' ' || after_export[4] == '\t' || after_export[4] == '\n' || after_export[4] == '\r' || after_export[4] == '\0')) {
                char* after_clib = after_export + 4;
                while (*after_clib && (*after_clib == ' ' || *after_clib == '\t')) after_clib++;

                const char* name_start = after_clib;
                while (*after_clib && (isalnum((unsigned char)*after_clib) || *after_clib == '_')) after_clib++;
                int name_len = (int)(after_clib - name_start);

                if (name_len > 0 && name_len < 64) {
                    if (clib_name_count >= MOD_MAX_CLIBS) {
                        fprintf(stderr, "[错误] clib数量超过上限 %d\n", MOD_MAX_CLIBS);
                    } else {
                        clib_names[clib_name_count] = (char*)malloc(name_len + 1);
                        strncpy(clib_names[clib_name_count], name_start, name_len);
                        clib_names[clib_name_count][name_len] = '\0';
                        clib_name_count++;
                    }
                }

                p = after_clib;
                continue;
            }

            // 检查 export cfunc（C 回调函数签名）
            if (strncmp(after_export, "cfunc", 5) == 0 && (after_export[5] == ' ' || after_export[5] == '\t' || after_export[5] == '\n' || after_export[5] == '\r' || after_export[5] == '\0')) {
                char* after_cfunc = after_export + 5;
                while (*after_cfunc && (*after_cfunc == ' ' || *after_cfunc == '\t')) after_cfunc++;

                // 解析 cfunc 名称
                const char* name_start = after_cfunc;
                while (*after_cfunc && (isalnum((unsigned char)*after_cfunc) || *after_cfunc == '_')) after_cfunc++;
                int name_len = (int)(after_cfunc - name_start);

                if (name_len > 0 && name_len < 64) {
                    char cfunc_name_buf[64];
                    strncpy(cfunc_name_buf, name_start, name_len);
                    cfunc_name_buf[name_len] = '\0';

                    // 检查是否已经存在
                    if (!module_symbol_table_find_cfunc(table, cfunc_name_buf)) {
                        // 解析参数列表 (...)（支持 Ptr[T] 语法）
                        TypeKind param_types[MOD_MAX_PARAMS];
                        TypeKind param_element_types[MOD_MAX_PARAMS];
                        char* param_structs[MOD_MAX_PARAMS];
                        char* param_names[MOD_MAX_PARAMS];
                        int param_count = 0;
                        for (int pi = 0; pi < MOD_MAX_PARAMS; pi++) {
                            param_element_types[pi] = TYPE_PTR;
                            param_structs[pi] = NULL;
                            param_names[pi] = NULL;
                        }

                        // 跳过空白到 (
                        while (*after_cfunc && (*after_cfunc == ' ' || *after_cfunc == '\t')) after_cfunc++;
                        if (*after_cfunc == '(') {
                            after_cfunc++;
                            while (*after_cfunc && *after_cfunc != ')' && param_count < MOD_MAX_PARAMS) {
                                while (*after_cfunc && (*after_cfunc == ' ' || *after_cfunc == '\t' || *after_cfunc == ',' || *after_cfunc == '\n' || *after_cfunc == '\r')) after_cfunc++;
                                if (*after_cfunc == ')' || *after_cfunc == '\0') break;

                                // 解析参数类型（支持 Ptr[T] 语法）
                                const char* pt_start = after_cfunc;
                                while (*after_cfunc && (isalnum((unsigned char)*after_cfunc) || *after_cfunc == '_')) after_cfunc++;
                                if (*after_cfunc == '[') {
                                    after_cfunc++;
                                    while (*after_cfunc && *after_cfunc != ']') after_cfunc++;
                                    if (*after_cfunc == ']') after_cfunc++;
                                }
                                int pt_len = (int)(after_cfunc - pt_start);
                                if (pt_len > 0 && pt_len < 128) {
                                    char pt_buf[128]; strncpy(pt_buf, pt_start, pt_len); pt_buf[pt_len] = '\0';
                                    char* pbracket = strchr(pt_buf, '[');
                                    if (pbracket) {
                                        *pbracket = '\0';
                                        TypeKind base = parse_base_type(pt_buf);
                                        if (base == TYPE_PTR) {
                                            param_types[param_count] = TYPE_PTR_GENERIC;
                                            char* pelem_start = pbracket + 1;
                                            char* pelem_end = strchr(pelem_start, ']');
                                            if (pelem_end) *pelem_end = '\0';
                                            param_element_types[param_count] = parse_base_type(pelem_start);
                                        } else {
                                            param_types[param_count] = base;
                                        }
                                    } else {
                                        param_types[param_count] = parse_base_type(pt_buf);
                                    }
                                    param_structs[param_count] = NULL;
                                    param_count++;
                                }
                                // 跳过空白
                                while (*after_cfunc && (*after_cfunc == ' ' || *after_cfunc == '\t')) after_cfunc++;
                                // 解析参数名
                                const char* pname_start = after_cfunc;
                                while (*after_cfunc && (isalnum((unsigned char)*after_cfunc) || *after_cfunc == '_')) after_cfunc++;
                                int pname_len = (int)(after_cfunc - pname_start);
                                if (pname_len > 0 && pname_len < 64 && param_count > 0) {
                                    param_names[param_count - 1] = (char*)malloc(pname_len + 1);
                                    strncpy(param_names[param_count - 1], pname_start, pname_len);
                                    param_names[param_count - 1][pname_len] = '\0';
                                }
                            }
                            if (*after_cfunc == ')') after_cfunc++;
                        }

                        // 跳过空白到 :
                        while (*after_cfunc && (*after_cfunc == ' ' || *after_cfunc == '\t')) after_cfunc++;
                        if (*after_cfunc == ':') {
                            after_cfunc++;
                            while (*after_cfunc && (*after_cfunc == ' ' || *after_cfunc == '\t')) after_cfunc++;

                            // 解析返回类型（支持 Ptr[T] 语法）
                            const char* ret_start = after_cfunc;
                            while (*after_cfunc && (isalnum((unsigned char)*after_cfunc) || *after_cfunc == '_')) after_cfunc++;
                            if (*after_cfunc == '[') {
                                after_cfunc++;
                                while (*after_cfunc && *after_cfunc != ']') after_cfunc++;
                                if (*after_cfunc == ']') after_cfunc++;
                            }
                            int ret_len = (int)(after_cfunc - ret_start);
                            TypeKind ret_type = TYPE_I32;
                            TypeKind ret_element_type = TYPE_PTR;
                            if (ret_len > 0 && ret_len < 128) {
                                char ret_buf[128]; strncpy(ret_buf, ret_start, ret_len); ret_buf[ret_len] = '\0';
                                char* rbracket = strchr(ret_buf, '[');
                                if (rbracket) {
                                    *rbracket = '\0';
                                    ret_type = parse_base_type(ret_buf);
                                    if (ret_type == TYPE_PTR) {
                                        ret_type = TYPE_PTR_GENERIC;
                                        char* elem_start = rbracket + 1;
                                        char* elem_end = strchr(elem_start, ']');
                                        if (elem_end) *elem_end = '\0';
                                        ret_element_type = parse_base_type(elem_start);
                                    }
                                } else {
                                    ret_type = parse_base_type(ret_buf);
                                }
                            }

                            // 存储 cfunc 符号
                            module_symbol_table_add_cfunc(table, cfunc_name_buf,
                                param_count, param_types, param_element_types,
                                param_structs, param_names,
                                ret_type, ret_element_type, NULL);

                            // 释放临时数据
                            for (int pi = 0; pi < param_count; pi++) {
                                free(param_names[pi]);
                            }
                        }
                    }
                }

                p = after_cfunc;
                continue;
            }

            // 检查 export enum
            if (strncmp(after_export, "enum", 4) == 0 && (after_export[4] == ' ' || after_export[4] == '\t' || after_export[4] == '\n' || after_export[4] == '\r' || after_export[4] == '\0')) {
                char* after_enum = after_export + 4;
                while (*after_enum && (*after_enum == ' ' || *after_enum == '\t')) after_enum++;

                const char* name_start = after_enum;
                while (*after_enum && (isalnum((unsigned char)*after_enum) || *after_enum == '_')) after_enum++;
                int name_len = (int)(after_enum - name_start);

                if (name_len > 0 && name_len < 64) {
                    if (enum_name_count >= MOD_MAX_ENUMS) {
                        fprintf(stderr, "[错误] enum数量超过上限 %d\n", MOD_MAX_ENUMS);
                    } else {
                        enum_names[enum_name_count] = (char*)malloc(name_len + 1);
                        strncpy(enum_names[enum_name_count], name_start, name_len);
                        enum_names[enum_name_count][name_len] = '\0';
                        enum_name_count++;
                    }
                }

                p = after_enum;
                continue;
            }
        }

        // 非 export 行也收集 clib 并解析函数签名（模块内的 clib 定义不带 export）
        if (strncmp(p, "clib", 4) == 0 && (p[4] == ' ' || p[4] == '\t' || p[4] == '\n' || p[4] == '\r')) {
            char* after_clib = p + 4;
            while (*after_clib && (*after_clib == ' ' || *after_clib == '\t')) after_clib++;

            const char* name_start = after_clib;
            while (*after_clib && (isalnum((unsigned char)*after_clib) || *after_clib == '_')) after_clib++;
            int name_len = (int)(after_clib - name_start);

            if (name_len > 0 && name_len < 64) {
                char clib_name_buf[64];
                strncpy(clib_name_buf, name_start, name_len);
                clib_name_buf[name_len] = '\0';

                // 收集名称
                if (clib_name_count >= MOD_MAX_CLIBS) {
                    fprintf(stderr, "[错误] clib数量超过上限 %d\n", MOD_MAX_CLIBS);
                } else {
                    int already = 0;
                    for (int ci = 0; ci < clib_name_count; ci++) {
                        if (strcmp(clib_names[ci], clib_name_buf) == 0) { already = 1; break; }
                    }
                    if (!already) {
                        clib_names[clib_name_count] = strdup(clib_name_buf);
                        clib_name_count++;
                    }
                }

                // 查找 { 并解析函数签名
                while (*after_clib && *after_clib != '{' && *after_clib != '\n') after_clib++;
                if (*after_clib == '{') {
                    after_clib++;
                    ModuleClibFuncSymbol funcs[MOD_MAX_CLIB_FUNCS];
                    int func_count = 0;

                    while (*after_clib && *after_clib != '}' && func_count < MOD_MAX_CLIB_FUNCS) {
                        while (*after_clib && (*after_clib == ' ' || *after_clib == '\t' || *after_clib == '\n' || *after_clib == '\r')) after_clib++;
                        if (*after_clib == '}' || *after_clib == '\0') break;
                        if (*after_clib == '/' && *(after_clib+1) == '/') {
                            while (*after_clib && *after_clib != '\n') after_clib++;
                            continue;
                        }

                        // 解析返回类型（支持 Ptr[T] 语法）
                        const char* ret_start = after_clib;
                        while (*after_clib && (isalnum((unsigned char)*after_clib) || *after_clib == '_')) after_clib++;
                        // 支持 Ptr[T] 语法：跳过 [T] 部分
                        if (*after_clib == '[') {
                            after_clib++;
                            while (*after_clib && *after_clib != ']') after_clib++;
                            if (*after_clib == ']') after_clib++;
                        }
                        int ret_len = (int)(after_clib - ret_start);

                        TypeKind ret_type = TYPE_I32; // 默认
                        TypeKind ret_element_type = TYPE_PTR; // Ptr[T] 中的 T，默认无
                        if (ret_len > 0 && ret_len < 128) {
                            char ret_buf[128]; strncpy(ret_buf, ret_start, ret_len); ret_buf[ret_len] = '\0';
                            // 检查是否是 Ptr[T] 语法
                            char* bracket = strchr(ret_buf, '[');
                            if (bracket) {
                                *bracket = '\0'; // 截断为 "Ptr"
                                ret_type = parse_base_type(ret_buf);
                                if (ret_type == TYPE_PTR) {
                                    ret_type = TYPE_PTR_GENERIC;
                                    // 提取 [T] 中的元素类型
                                    char* elem_start = bracket + 1;
                                    char* elem_end = strchr(elem_start, ']');
                                    if (elem_end) *elem_end = '\0';
                                    ret_element_type = parse_base_type(elem_start);
                                }
                            } else {
                                ret_type = parse_base_type(ret_buf);
                            }
                        }
                        if (ret_len <= 0) break;

                        while (*after_clib && (*after_clib == ' ' || *after_clib == '\t')) after_clib++;

                        // 解析函数名
                        const char* func_name_start = after_clib;
                        while (*after_clib && (isalnum((unsigned char)*after_clib) || *after_clib == '_')) after_clib++;
                        int func_name_len = (int)(after_clib - func_name_start);
                        if (func_name_len <= 0) break;

                        funcs[func_count].name = (char*)malloc(func_name_len + 1);
                        strncpy(funcs[func_count].name, func_name_start, func_name_len);
                        funcs[func_count].name[func_name_len] = '\0';
                        funcs[func_count].return_type = ret_type;
                        funcs[func_count].return_element_type = ret_element_type;
                        funcs[func_count].return_struct_name = NULL;

                        while (*after_clib && (*after_clib == ' ' || *after_clib == '\t')) after_clib++;

                        // 解析参数列表 (...)（支持 Ptr[T] 语法）
                        TypeKind param_types[MOD_MAX_PARAMS]; char* param_structs[MOD_MAX_PARAMS];
                        TypeKind param_element_types[MOD_MAX_PARAMS]; // Ptr[T] 中的 T
                        int param_count = 0;
                        // 初始化 param_element_types
                        for (int pi = 0; pi < MOD_MAX_PARAMS; pi++) param_element_types[pi] = TYPE_PTR;
                        if (*after_clib == '(') {
                            after_clib++;
                            while (*after_clib && *after_clib != ')') {
                                while (*after_clib && (*after_clib == ' ' || *after_clib == '\t' || *after_clib == ',' || *after_clib == '\n' || *after_clib == '\r')) after_clib++;
                                if (*after_clib == ')' || *after_clib == '\0') break;

                                const char* pt_start = after_clib;
                                while (*after_clib && (isalnum((unsigned char)*after_clib) || *after_clib == '_')) after_clib++;
                                // 支持 Ptr[T] 语法：跳过 [T] 部分
                                if (*after_clib == '[') {
                                    after_clib++;
                                    while (*after_clib && *after_clib != ']') after_clib++;
                                    if (*after_clib == ']') after_clib++;
                                }
                                int pt_len = (int)(after_clib - pt_start);
                                if (pt_len > 0 && pt_len < 128) {
                                    if (param_count >= MOD_MAX_PARAMS) {
                                        fprintf(stderr, "[错误] clib函数参数数量超过上限 %d\n", MOD_MAX_PARAMS);
                                    } else {
                                        char pt_buf[128]; strncpy(pt_buf, pt_start, pt_len); pt_buf[pt_len] = '\0';
                                        // 检查是否是 Ptr[T] 语法
                                        char* pbracket = strchr(pt_buf, '[');
                                        if (pbracket) {
                                            *pbracket = '\0';
                                            TypeKind base = parse_base_type(pt_buf);
                                            if (base == TYPE_PTR) {
                                                param_types[param_count] = TYPE_PTR_GENERIC;
                                                char* pelem_start = pbracket + 1;
                                                char* pelem_end = strchr(pelem_start, ']');
                                                if (pelem_end) *pelem_end = '\0';
                                                param_element_types[param_count] = parse_base_type(pelem_start);
                                            } else {
                                                param_types[param_count] = base;
                                            }
                                        } else {
                                            param_types[param_count] = parse_base_type(pt_buf);
                                        }
                                        param_structs[param_count] = NULL;
                                        param_count++;
                                    }
                                }
                                while (*after_clib && (*after_clib == ' ' || *after_clib == '\t')) after_clib++;
                                // 跳过参数名
                                while (*after_clib && (isalnum((unsigned char)*after_clib) || *after_clib == '_')) after_clib++;
                            }
                            if (*after_clib == ')') after_clib++;
                        }

                        funcs[func_count].param_count = param_count;
                        funcs[func_count].param_types = (TypeKind*)malloc(sizeof(TypeKind) * param_count);
                        funcs[func_count].param_element_types = (TypeKind*)malloc(sizeof(TypeKind) * param_count);
                        funcs[func_count].param_struct_names = (char**)calloc(param_count, sizeof(char*));
                        for (int pi = 0; pi < param_count; pi++) {
                            funcs[func_count].param_types[pi] = param_types[pi];
                            funcs[func_count].param_element_types[pi] = param_element_types[pi];
                            funcs[func_count].param_struct_names[pi] = param_structs[pi]
                                ? strdup(param_structs[pi]) : NULL;
                        }
                        func_count++;

                        // 跳到下一行
                        while (*after_clib && *after_clib != '\n' && *after_clib != '}') after_clib++;
                        if (*after_clib == '\n') after_clib++;
                    }
                    if (*after_clib == '}') after_clib++;

                    // 存储 clib 符号
                    module_symbol_table_add_clib(table, clib_name_buf, func_count, funcs);

                    // 释放临时数据
                    for (int fi = 0; fi < func_count; fi++) {
                        free(funcs[fi].name);
                        for (int pi = 0; pi < funcs[fi].param_count; pi++) {
                            free(funcs[fi].param_struct_names[pi]);
                        }
                        free(funcs[fi].param_types);
                        free(funcs[fi].param_element_types);
                        free(funcs[fi].param_struct_names);
                    }
                }
            }

            p = after_clib;
            continue;
        }

        // 非 export 行也收集 cfunc 并解析签名（模块内的 cfunc 定义不带 export）
        if (strncmp(p, "cfunc", 5) == 0 && (p[5] == ' ' || p[5] == '\t' || p[5] == '\n' || p[5] == '\r')) {
            char* after_cfunc = p + 5;
            while (*after_cfunc && (*after_cfunc == ' ' || *after_cfunc == '\t')) after_cfunc++;

            // 解析 cfunc 名称
            const char* name_start = after_cfunc;
            while (*after_cfunc && (isalnum((unsigned char)*after_cfunc) || *after_cfunc == '_')) after_cfunc++;
            int name_len = (int)(after_cfunc - name_start);

            if (name_len > 0 && name_len < 64) {
                char cfunc_name_buf[64];
                strncpy(cfunc_name_buf, name_start, name_len);
                cfunc_name_buf[name_len] = '\0';

                // 检查是否已经存在
                if (!module_symbol_table_find_cfunc(table, cfunc_name_buf)) {
                    // 解析参数列表 (...)（支持 Ptr[T] 语法）
                    TypeKind param_types[MOD_MAX_PARAMS];
                    TypeKind param_element_types[MOD_MAX_PARAMS];
                    char* param_structs[MOD_MAX_PARAMS];
                    char* param_names[MOD_MAX_PARAMS];
                    int param_count = 0;
                    for (int pi = 0; pi < MOD_MAX_PARAMS; pi++) {
                        param_element_types[pi] = TYPE_PTR;
                        param_structs[pi] = NULL;
                        param_names[pi] = NULL;
                    }

                    // 跳过空白到 (
                    while (*after_cfunc && (*after_cfunc == ' ' || *after_cfunc == '\t')) after_cfunc++;
                    if (*after_cfunc == '(') {
                        after_cfunc++;
                        while (*after_cfunc && *after_cfunc != ')' && param_count < MOD_MAX_PARAMS) {
                            while (*after_cfunc && (*after_cfunc == ' ' || *after_cfunc == '\t' || *after_cfunc == ',' || *after_cfunc == '\n' || *after_cfunc == '\r')) after_cfunc++;
                            if (*after_cfunc == ')' || *after_cfunc == '\0') break;

                            // 解析参数类型（支持 Ptr[T] 语法）
                            const char* pt_start = after_cfunc;
                            while (*after_cfunc && (isalnum((unsigned char)*after_cfunc) || *after_cfunc == '_')) after_cfunc++;
                            if (*after_cfunc == '[') {
                                after_cfunc++;
                                while (*after_cfunc && *after_cfunc != ']') after_cfunc++;
                                if (*after_cfunc == ']') after_cfunc++;
                            }
                            int pt_len = (int)(after_cfunc - pt_start);
                            if (pt_len > 0 && pt_len < 128) {
                                char pt_buf[128]; strncpy(pt_buf, pt_start, pt_len); pt_buf[pt_len] = '\0';
                                char* pbracket = strchr(pt_buf, '[');
                                if (pbracket) {
                                    *pbracket = '\0';
                                    TypeKind base = parse_base_type(pt_buf);
                                    if (base == TYPE_PTR) {
                                        param_types[param_count] = TYPE_PTR_GENERIC;
                                        char* pelem_start = pbracket + 1;
                                        char* pelem_end = strchr(pelem_start, ']');
                                        if (pelem_end) *pelem_end = '\0';
                                        param_element_types[param_count] = parse_base_type(pelem_start);
                                    } else {
                                        param_types[param_count] = base;
                                    }
                                } else {
                                    param_types[param_count] = parse_base_type(pt_buf);
                                }
                                param_structs[param_count] = NULL;
                                param_count++;
                            }
                            // 跳过空白
                            while (*after_cfunc && (*after_cfunc == ' ' || *after_cfunc == '\t')) after_cfunc++;
                            // 解析参数名
                            const char* pname_start = after_cfunc;
                            while (*after_cfunc && (isalnum((unsigned char)*after_cfunc) || *after_cfunc == '_')) after_cfunc++;
                            int pname_len = (int)(after_cfunc - pname_start);
                            if (pname_len > 0 && pname_len < 64 && param_count > 0) {
                                param_names[param_count - 1] = (char*)malloc(pname_len + 1);
                                strncpy(param_names[param_count - 1], pname_start, pname_len);
                                param_names[param_count - 1][pname_len] = '\0';
                            }
                        }
                        if (*after_cfunc == ')') after_cfunc++;
                    }

                    // 跳过空白到 :
                    while (*after_cfunc && (*after_cfunc == ' ' || *after_cfunc == '\t')) after_cfunc++;
                    if (*after_cfunc == ':') {
                        after_cfunc++;
                        while (*after_cfunc && (*after_cfunc == ' ' || *after_cfunc == '\t')) after_cfunc++;

                        // 解析返回类型（支持 Ptr[T] 语法）
                        const char* ret_start = after_cfunc;
                        while (*after_cfunc && (isalnum((unsigned char)*after_cfunc) || *after_cfunc == '_')) after_cfunc++;
                        if (*after_cfunc == '[') {
                            after_cfunc++;
                            while (*after_cfunc && *after_cfunc != ']') after_cfunc++;
                            if (*after_cfunc == ']') after_cfunc++;
                        }
                        int ret_len = (int)(after_cfunc - ret_start);
                        TypeKind ret_type = TYPE_I32;
                        TypeKind ret_element_type = TYPE_PTR;
                        if (ret_len > 0 && ret_len < 128) {
                            char ret_buf[128]; strncpy(ret_buf, ret_start, ret_len); ret_buf[ret_len] = '\0';
                            char* rbracket = strchr(ret_buf, '[');
                            if (rbracket) {
                                *rbracket = '\0';
                                ret_type = parse_base_type(ret_buf);
                                if (ret_type == TYPE_PTR) {
                                    ret_type = TYPE_PTR_GENERIC;
                                    char* elem_start = rbracket + 1;
                                    char* elem_end = strchr(elem_start, ']');
                                    if (elem_end) *elem_end = '\0';
                                    ret_element_type = parse_base_type(elem_start);
                                }
                            } else {
                                ret_type = parse_base_type(ret_buf);
                            }
                        }

                        // 存储 cfunc 符号
                        module_symbol_table_add_cfunc(table, cfunc_name_buf,
                            param_count, param_types, param_element_types,
                            param_structs, param_names,
                            ret_type, ret_element_type, NULL);

                        // 释放临时数据
                        for (int pi = 0; pi < param_count; pi++) {
                            free(param_names[pi]);
                        }
                    }
                }
            }

            p = after_cfunc;
            continue;
        }

        p++;
    }

    // 递归扫描 import 子文件，收集子文件中的 clib 定义
    // 主文件只扫描到 export clib，但子文件中的 clib 也需要收集到模块符号表
    p = source;
    while (*p) {
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
        if (!*p) break;
        if (*p == '/' && *(p+1) == '/') { while (*p && *p != '\n') p++; continue; }
        if (*p == '/' && *(p+1) == '*') { p += 2; while (*p && !(*p == '*' && *(p+1) == '/')) p++; if (*p) p += 2; continue; }
        if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\' && *(p+1)) p += 2; else p++; } if (*p) p++; continue; }

        // 查找 import 语句
        if (strncmp(p, "import", 6) == 0 && (p[6] == ' ' || p[6] == '\t')) {
            char* after_import = p + 6;
            while (*after_import && (*after_import == ' ' || *after_import == '\t')) after_import++;
            if (*after_import == '"') {
                after_import++;
                const char* path_start = after_import;
                while (*after_import && *after_import != '"') after_import++;
                int path_len = (int)(after_import - path_start);
                if (path_len > 0 && path_len < MAX_PATH_LEN) {
                    char import_path[MAX_PATH_LEN];
                    strncpy(import_path, path_start, path_len);
                    import_path[path_len] = '\0';
                    // 解析子文件的绝对路径
                    char sub_full_path[MAX_PATH_LEN] = {0};
                    if (normalized_current[0]) {
                        snprintf(sub_full_path, MAX_PATH_LEN, "%s", normalized_current);
                        char* last_sep = strrchr(sub_full_path, '\\');
                        if (!last_sep) last_sep = strrchr(sub_full_path, '/');
                        if (last_sep) {
                            *(last_sep + 1) = '\0';
                            strncat(sub_full_path, import_path, MAX_PATH_LEN - strlen(sub_full_path) - 1);
                        } else {
                            snprintf(sub_full_path, MAX_PATH_LEN, "%s", import_path);
                        }
                    } else {
                        snprintf(sub_full_path, MAX_PATH_LEN, "%s", import_path);
                    }
                    normalize_path(sub_full_path, MAX_PATH_LEN);
                    
                    // 记录 import 依赖路径（用于 .lenosymc 缓存失效判定）
                    module_symbol_table_add_dep(table, sub_full_path);
                    
                    // 读取子文件并扫描其中的 clib 定义
                    char* sub_source = read_module_file(import_path, sub_full_path);
                    if (sub_source) {
                        char* sp = sub_source;
                        while (*sp) {
                            while (*sp && (*sp == ' ' || *sp == '\t' || *sp == '\n' || *sp == '\r')) sp++;
                            if (!*sp) break;
                            if (*sp == '/' && *(sp+1) == '/') { while (*sp && *sp != '\n') sp++; continue; }
                            if (*sp == '/' && *(sp+1) == '*') { sp += 2; while (*sp && !(*sp == '*' && *(sp+1) == '/')) sp++; if (*sp) sp += 2; continue; }
                            if (*sp == '"') { sp++; while (*sp && *sp != '"') { if (*sp == '\\' && *(sp+1)) sp += 2; else sp++; } if (*sp) sp++; continue; }

                            // 查找 export clib
                            if (strncmp(sp, "export", 6) == 0 && (sp[6] == ' ' || sp[6] == '\t' || sp[6] == '\n' || sp[6] == '\r')) {
                                char* after_exp = sp + 6;
                                while (*after_exp && (*after_exp == ' ' || *after_exp == '\t' || *after_exp == '\n' || *after_exp == '\r')) after_exp++;
                                if (strncmp(after_exp, "clib", 4) == 0 && (after_exp[4] == ' ' || after_exp[4] == '\t')) {
                                    char* after_clib = after_exp + 4;
                                    while (*after_clib && (*after_clib == ' ' || *after_clib == '\t')) after_clib++;
                                    const char* name_start = after_clib;
                                    while (*after_clib && (isalnum((unsigned char)*after_clib) || *after_clib == '_')) after_clib++;
                                    int name_len = (int)(after_clib - name_start);
                                    if (name_len > 0 && name_len < 64) {
                                        char clib_name_buf[64];
                                        strncpy(clib_name_buf, name_start, name_len);
                                        clib_name_buf[name_len] = '\0';
                                        // 检查是否已经收集过这个 clib
                                        int already_exists = 0;
                                        for (int ei = 0; ei < table->clib_count; ei++) {
                                            if (strcmp(table->clibs[ei].name, clib_name_buf) == 0) {
                                                already_exists = 1;
                                                break;
                                            }
                                        }
                                        if (!already_exists) {
                                            // 解析 clib 函数签名（与主文件相同的逻辑）
                                            // 跳过到 { 开始
                                            while (*after_clib && *after_clib != '{') after_clib++;
                                            if (*after_clib == '{') after_clib++;
                                            // 解析函数列表
                                            ModuleClibFuncSymbol funcs[MOD_MAX_CLIB_FUNCS];
                                            int func_count = 0;
                                            while (*after_clib && *after_clib != '}' && func_count < MOD_MAX_CLIB_FUNCS) {
                                                while (*after_clib && (*after_clib == ' ' || *after_clib == '\t' || *after_clib == '\n' || *after_clib == '\r')) after_clib++;
                                                if (*after_clib == '}') break;
                                                // 解析返回类型
                                                const char* rt_start = after_clib;
                                                while (*after_clib && *after_clib != ' ' && *after_clib != '\t') after_clib++;
                                                int rt_len = (int)(after_clib - rt_start);
                                                while (*after_clib && (*after_clib == ' ' || *after_clib == '\t')) after_clib++;
                                                // 解析函数名
                                                const char* fn_start = after_clib;
                                                while (*after_clib && *after_clib != '(') after_clib++;
                                                int fn_len = (int)(after_clib - fn_start);
                                                if (fn_len > 0 && rt_len > 0) {
                                                    funcs[func_count].name = (char*)malloc(fn_len + 1);
                                                    strncpy(funcs[func_count].name, fn_start, fn_len);
                                                    funcs[func_count].name[fn_len] = '\0';
                                                    // 解析返回类型
                                                    char rt_buf[128];
                                                    strncpy(rt_buf, rt_start, rt_len < 127 ? rt_len : 127);
                                                    rt_buf[rt_len < 127 ? rt_len : 127] = '\0';
                                                    char* rbracket = strchr(rt_buf, '[');
                                                    if (rbracket) {
                                                        *rbracket = '\0';
                                                        TypeKind base = parse_base_type(rt_buf);
                                                        if (base == TYPE_PTR) {
                                                            funcs[func_count].return_type = TYPE_PTR_GENERIC;
                                                            char* elem_start = rbracket + 1;
                                                            char* elem_end = strchr(elem_start, ']');
                                                            if (elem_end) *elem_end = '\0';
                                                            funcs[func_count].return_element_type = parse_base_type(elem_start);
                                                        } else {
                                                            funcs[func_count].return_type = base;
                                                            funcs[func_count].return_element_type = TYPE_UNKNOWN;
                                                        }
                                                    } else {
                                                        funcs[func_count].return_type = parse_base_type(rt_buf);
                                                        funcs[func_count].return_element_type = TYPE_UNKNOWN;
                                                    }
                                                    funcs[func_count].return_struct_name = NULL;
                                                    // 解析参数
                                                    if (*after_clib == '(') after_clib++;
                                                    TypeKind param_types[MOD_MAX_PARAMS];
                                                    TypeKind param_element_types[MOD_MAX_PARAMS];
                                                    char* param_structs[MOD_MAX_PARAMS];
                                                    int param_count = 0;
                                                    while (*after_clib && *after_clib != ')' && param_count < MOD_MAX_PARAMS) {
                                                        while (*after_clib && (*after_clib == ' ' || *after_clib == '\t')) after_clib++;
                                                        const char* pt_start = after_clib;
                                                        while (*after_clib && *after_clib != ',' && *after_clib != ')' && *after_clib != ' ' && *after_clib != '\t') after_clib++;
                                                        int pt_len = (int)(after_clib - pt_start);
                                                        if (pt_len > 0 && pt_len < 128) {
                                                            char pt_buf[128]; strncpy(pt_buf, pt_start, pt_len); pt_buf[pt_len] = '\0';
                                                            char* pbracket = strchr(pt_buf, '[');
                                                            if (pbracket) {
                                                                *pbracket = '\0';
                                                                TypeKind base = parse_base_type(pt_buf);
                                                                if (base == TYPE_PTR) {
                                                                    param_types[param_count] = TYPE_PTR_GENERIC;
                                                                    char* pelem_start = pbracket + 1;
                                                                    char* pelem_end = strchr(pelem_start, ']');
                                                                    if (pelem_end) *pelem_end = '\0';
                                                                    param_element_types[param_count] = parse_base_type(pelem_start);
                                                                } else {
                                                                    param_types[param_count] = base;
                                                                    param_element_types[param_count] = TYPE_UNKNOWN;
                                                                }
                                                            } else {
                                                                param_types[param_count] = parse_base_type(pt_buf);
                                                                param_element_types[param_count] = TYPE_UNKNOWN;
                                                            }
                                                            param_structs[param_count] = NULL;
                                                            param_count++;
                                                        }
                                                        while (*after_clib && (*after_clib == ' ' || *after_clib == '\t')) after_clib++;
                                                        // 跳过参数名
                                                        while (*after_clib && *after_clib != ',' && *after_clib != ')') after_clib++;
                                                        if (*after_clib == ',') after_clib++;
                                                    }
                                                    if (*after_clib == ')') after_clib++;
                                                    funcs[func_count].param_count = param_count;
                                                    funcs[func_count].param_types = (TypeKind*)malloc(sizeof(TypeKind) * param_count);
                                                    funcs[func_count].param_element_types = (TypeKind*)malloc(sizeof(TypeKind) * param_count);
                                                    funcs[func_count].param_struct_names = (char**)calloc(param_count, sizeof(char*));
                                                    for (int pi = 0; pi < param_count; pi++) {
                                                        funcs[func_count].param_types[pi] = param_types[pi];
                                                        funcs[func_count].param_element_types[pi] = param_element_types[pi];
                                                        funcs[func_count].param_struct_names[pi] = param_structs[pi]
                                                            ? strdup(param_structs[pi]) : NULL;
                                                    }
                                                    func_count++;
                                                }
                                                while (*after_clib && *after_clib != '\n' && *after_clib != '}') after_clib++;
                                                if (*after_clib == '\n') after_clib++;
                                            }
                                            if (*after_clib == '}') after_clib++;
                                            // 存储 clib 符号
                                            module_symbol_table_add_clib(table, clib_name_buf, func_count, funcs);
                                            // 释放临时数据
                                            for (int fi = 0; fi < func_count; fi++) {
                                                free(funcs[fi].name);
                                                for (int pi = 0; pi < funcs[fi].param_count; pi++) {
                                                    free(funcs[fi].param_struct_names[pi]);
                                                }
                                                free(funcs[fi].param_types);
                                                free(funcs[fi].param_element_types);
                                            }
                                        }
                                    }
                                }
                            }

                            // 查找 export cfunc（子文件中的 cfunc 定义）
                            if (strncmp(sp, "export", 6) == 0 && (sp[6] == ' ' || sp[6] == '\t' || sp[6] == '\n' || sp[6] == '\r')) {
                                char* after_exp = sp + 6;
                                while (*after_exp && (*after_exp == ' ' || *after_exp == '\t' || *after_exp == '\n' || *after_exp == '\r')) after_exp++;
                                if (strncmp(after_exp, "cfunc", 5) == 0 && (after_exp[5] == ' ' || after_exp[5] == '\t' || after_exp[5] == '\n' || after_exp[5] == '\r' || after_exp[5] == '\0')) {
                                    char* after_cfunc = after_exp + 5;
                                    while (*after_cfunc && (*after_cfunc == ' ' || *after_cfunc == '\t')) after_cfunc++;

                                    const char* name_start = after_cfunc;
                                    while (*after_cfunc && (isalnum((unsigned char)*after_cfunc) || *after_cfunc == '_')) after_cfunc++;
                                    int name_len = (int)(after_cfunc - name_start);

                                    if (name_len > 0 && name_len < 64) {
                                        char cfunc_name_buf[64];
                                        strncpy(cfunc_name_buf, name_start, name_len);
                                        cfunc_name_buf[name_len] = '\0';

                                        if (!module_symbol_table_find_cfunc(table, cfunc_name_buf)) {
                                            // 解析参数列表
                                            TypeKind param_types[MOD_MAX_PARAMS];
                                            TypeKind param_element_types[MOD_MAX_PARAMS];
                                            char* param_structs[MOD_MAX_PARAMS];
                                            char* param_names[MOD_MAX_PARAMS];
                                            int param_count = 0;
                                            for (int pi = 0; pi < MOD_MAX_PARAMS; pi++) {
                                                param_element_types[pi] = TYPE_PTR;
                                                param_structs[pi] = NULL;
                                                param_names[pi] = NULL;
                                            }

                                            while (*after_cfunc && (*after_cfunc == ' ' || *after_cfunc == '\t')) after_cfunc++;
                                            if (*after_cfunc == '(') {
                                                after_cfunc++;
                                                while (*after_cfunc && *after_cfunc != ')' && param_count < MOD_MAX_PARAMS) {
                                                    while (*after_cfunc && (*after_cfunc == ' ' || *after_cfunc == '\t' || *after_cfunc == ',' || *after_cfunc == '\n' || *after_cfunc == '\r')) after_cfunc++;
                                                    if (*after_cfunc == ')' || *after_cfunc == '\0') break;

                                                    const char* pt_start = after_cfunc;
                                                    while (*after_cfunc && (isalnum((unsigned char)*after_cfunc) || *after_cfunc == '_')) after_cfunc++;
                                                    if (*after_cfunc == '[') {
                                                        after_cfunc++;
                                                        while (*after_cfunc && *after_cfunc != ']') after_cfunc++;
                                                        if (*after_cfunc == ']') after_cfunc++;
                                                    }
                                                    int pt_len = (int)(after_cfunc - pt_start);
                                                    if (pt_len > 0 && pt_len < 128) {
                                                        char pt_buf[128]; strncpy(pt_buf, pt_start, pt_len); pt_buf[pt_len] = '\0';
                                                        char* pbracket = strchr(pt_buf, '[');
                                                        if (pbracket) {
                                                            *pbracket = '\0';
                                                            TypeKind base = parse_base_type(pt_buf);
                                                            if (base == TYPE_PTR) {
                                                                param_types[param_count] = TYPE_PTR_GENERIC;
                                                                char* pelem_start = pbracket + 1;
                                                                char* pelem_end = strchr(pelem_start, ']');
                                                                if (pelem_end) *pelem_end = '\0';
                                                                param_element_types[param_count] = parse_base_type(pelem_start);
                                                            } else {
                                                                param_types[param_count] = base;
                                                            }
                                                        } else {
                                                            param_types[param_count] = parse_base_type(pt_buf);
                                                        }
                                                        param_structs[param_count] = NULL;
                                                        param_count++;
                                                    }
                                                    while (*after_cfunc && (*after_cfunc == ' ' || *after_cfunc == '\t')) after_cfunc++;
                                                    // 解析参数名
                                                    const char* pname_start = after_cfunc;
                                                    while (*after_cfunc && (isalnum((unsigned char)*after_cfunc) || *after_cfunc == '_')) after_cfunc++;
                                                    int pname_len = (int)(after_cfunc - pname_start);
                                                    if (pname_len > 0 && pname_len < 64 && param_count > 0) {
                                                        param_names[param_count - 1] = (char*)malloc(pname_len + 1);
                                                        strncpy(param_names[param_count - 1], pname_start, pname_len);
                                                        param_names[param_count - 1][pname_len] = '\0';
                                                    }
                                                }
                                                if (*after_cfunc == ')') after_cfunc++;
                                            }

                                            while (*after_cfunc && (*after_cfunc == ' ' || *after_cfunc == '\t')) after_cfunc++;
                                            if (*after_cfunc == ':') {
                                                after_cfunc++;
                                                while (*after_cfunc && (*after_cfunc == ' ' || *after_cfunc == '\t')) after_cfunc++;

                                                const char* ret_start = after_cfunc;
                                                while (*after_cfunc && (isalnum((unsigned char)*after_cfunc) || *after_cfunc == '_')) after_cfunc++;
                                                if (*after_cfunc == '[') {
                                                    after_cfunc++;
                                                    while (*after_cfunc && *after_cfunc != ']') after_cfunc++;
                                                    if (*after_cfunc == ']') after_cfunc++;
                                                }
                                                int ret_len = (int)(after_cfunc - ret_start);
                                                TypeKind ret_type = TYPE_I32;
                                                TypeKind ret_element_type = TYPE_PTR;
                                                if (ret_len > 0 && ret_len < 128) {
                                                    char ret_buf[128]; strncpy(ret_buf, ret_start, ret_len); ret_buf[ret_len] = '\0';
                                                    char* rbracket = strchr(ret_buf, '[');
                                                    if (rbracket) {
                                                        *rbracket = '\0';
                                                        ret_type = parse_base_type(ret_buf);
                                                        if (ret_type == TYPE_PTR) {
                                                            ret_type = TYPE_PTR_GENERIC;
                                                            char* elem_start = rbracket + 1;
                                                            char* elem_end = strchr(elem_start, ']');
                                                            if (elem_end) *elem_end = '\0';
                                                            ret_element_type = parse_base_type(elem_start);
                                                        }
                                                    } else {
                                                        ret_type = parse_base_type(ret_buf);
                                                    }
                                                }

                                                module_symbol_table_add_cfunc(table, cfunc_name_buf,
                                                    param_count, param_types, param_element_types,
                                                    param_structs, param_names,
                                                    ret_type, ret_element_type, NULL);

                                                for (int pi = 0; pi < param_count; pi++) {
                                                    free(param_names[pi]);
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            // 查找非 export cfunc（子文件中的裸 cfunc 定义）
                            if (strncmp(sp, "cfunc", 5) == 0 && (sp[5] == ' ' || sp[5] == '\t' || sp[5] == '\n' || sp[5] == '\r')) {
                                char* after_cfunc = sp + 5;
                                while (*after_cfunc && (*after_cfunc == ' ' || *after_cfunc == '\t')) after_cfunc++;

                                const char* name_start = after_cfunc;
                                while (*after_cfunc && (isalnum((unsigned char)*after_cfunc) || *after_cfunc == '_')) after_cfunc++;
                                int name_len = (int)(after_cfunc - name_start);

                                if (name_len > 0 && name_len < 64) {
                                    char cfunc_name_buf[64];
                                    strncpy(cfunc_name_buf, name_start, name_len);
                                    cfunc_name_buf[name_len] = '\0';

                                    if (!module_symbol_table_find_cfunc(table, cfunc_name_buf)) {
                                        // 解析参数列表
                                        TypeKind param_types[MOD_MAX_PARAMS];
                                        TypeKind param_element_types[MOD_MAX_PARAMS];
                                        char* param_structs[MOD_MAX_PARAMS];
                                        char* param_names[MOD_MAX_PARAMS];
                                        int param_count = 0;
                                        for (int pi = 0; pi < MOD_MAX_PARAMS; pi++) {
                                            param_element_types[pi] = TYPE_PTR;
                                            param_structs[pi] = NULL;
                                            param_names[pi] = NULL;
                                        }

                                        while (*after_cfunc && (*after_cfunc == ' ' || *after_cfunc == '\t')) after_cfunc++;
                                        if (*after_cfunc == '(') {
                                            after_cfunc++;
                                            while (*after_cfunc && *after_cfunc != ')' && param_count < MOD_MAX_PARAMS) {
                                                while (*after_cfunc && (*after_cfunc == ' ' || *after_cfunc == '\t' || *after_cfunc == ',' || *after_cfunc == '\n' || *after_cfunc == '\r')) after_cfunc++;
                                                if (*after_cfunc == ')' || *after_cfunc == '\0') break;

                                                const char* pt_start = after_cfunc;
                                                while (*after_cfunc && (isalnum((unsigned char)*after_cfunc) || *after_cfunc == '_')) after_cfunc++;
                                                if (*after_cfunc == '[') {
                                                    after_cfunc++;
                                                    while (*after_cfunc && *after_cfunc != ']') after_cfunc++;
                                                    if (*after_cfunc == ']') after_cfunc++;
                                                }
                                                int pt_len = (int)(after_cfunc - pt_start);
                                                if (pt_len > 0 && pt_len < 128) {
                                                    char pt_buf[128]; strncpy(pt_buf, pt_start, pt_len); pt_buf[pt_len] = '\0';
                                                    char* pbracket = strchr(pt_buf, '[');
                                                    if (pbracket) {
                                                        *pbracket = '\0';
                                                        TypeKind base = parse_base_type(pt_buf);
                                                        if (base == TYPE_PTR) {
                                                            param_types[param_count] = TYPE_PTR_GENERIC;
                                                            char* pelem_start = pbracket + 1;
                                                            char* pelem_end = strchr(pelem_start, ']');
                                                            if (pelem_end) *pelem_end = '\0';
                                                            param_element_types[param_count] = parse_base_type(pelem_start);
                                                        } else {
                                                            param_types[param_count] = base;
                                                        }
                                                    } else {
                                                        param_types[param_count] = parse_base_type(pt_buf);
                                                    }
                                                    param_structs[param_count] = NULL;
                                                    param_count++;
                                                }
                                                while (*after_cfunc && (*after_cfunc == ' ' || *after_cfunc == '\t')) after_cfunc++;
                                                // 解析参数名
                                                const char* pname_start = after_cfunc;
                                                while (*after_cfunc && (isalnum((unsigned char)*after_cfunc) || *after_cfunc == '_')) after_cfunc++;
                                                int pname_len = (int)(after_cfunc - pname_start);
                                                if (pname_len > 0 && pname_len < 64 && param_count > 0) {
                                                    param_names[param_count - 1] = (char*)malloc(pname_len + 1);
                                                    strncpy(param_names[param_count - 1], pname_start, pname_len);
                                                    param_names[param_count - 1][pname_len] = '\0';
                                                }
                                            }
                                            if (*after_cfunc == ')') after_cfunc++;
                                        }

                                        while (*after_cfunc && (*after_cfunc == ' ' || *after_cfunc == '\t')) after_cfunc++;
                                        if (*after_cfunc == ':') {
                                            after_cfunc++;
                                            while (*after_cfunc && (*after_cfunc == ' ' || *after_cfunc == '\t')) after_cfunc++;

                                            const char* ret_start = after_cfunc;
                                            while (*after_cfunc && (isalnum((unsigned char)*after_cfunc) || *after_cfunc == '_')) after_cfunc++;
                                            if (*after_cfunc == '[') {
                                                after_cfunc++;
                                                while (*after_cfunc && *after_cfunc != ']') after_cfunc++;
                                                if (*after_cfunc == ']') after_cfunc++;
                                            }
                                            int ret_len = (int)(after_cfunc - ret_start);
                                            TypeKind ret_type = TYPE_I32;
                                            TypeKind ret_element_type = TYPE_PTR;
                                            if (ret_len > 0 && ret_len < 128) {
                                                char ret_buf[128]; strncpy(ret_buf, ret_start, ret_len); ret_buf[ret_len] = '\0';
                                                char* rbracket = strchr(ret_buf, '[');
                                                if (rbracket) {
                                                    *rbracket = '\0';
                                                    ret_type = parse_base_type(ret_buf);
                                                    if (ret_type == TYPE_PTR) {
                                                        ret_type = TYPE_PTR_GENERIC;
                                                        char* elem_start = rbracket + 1;
                                                        char* elem_end = strchr(elem_start, ']');
                                                        if (elem_end) *elem_end = '\0';
                                                        ret_element_type = parse_base_type(elem_start);
                                                    }
                                                } else {
                                                    ret_type = parse_base_type(ret_buf);
                                                }
                                            }

                                            module_symbol_table_add_cfunc(table, cfunc_name_buf,
                                                param_count, param_types, param_element_types,
                                                param_structs, param_names,
                                                ret_type, ret_element_type, NULL);

                                            for (int pi = 0; pi < param_count; pi++) {
                                                free(param_names[pi]);
                                            }
                                        }
                                    }
                                }
                            }
                            sp++;
                        }
                        free(sub_source);
                    }
                }
            }
        }
        p++;
    }
