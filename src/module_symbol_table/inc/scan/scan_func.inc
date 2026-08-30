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
                        const char* param_start = after_func;
                        int paren_depth = 1;
                        while (*after_func && paren_depth > 0) {
                            if (*after_func == '(') paren_depth++;
                            else if (*after_func == ')') paren_depth--;
                            after_func++;
                        }
                        
                        // 提取参数文本（在 ) 之前的内容）
                        char param_text[256] = {0};
                        int param_len = (int)(after_func - 1 - param_start);
                        if (param_len > 0 && param_len < (int)sizeof(param_text)) {
                            memcpy(param_text, param_start, param_len);
                            param_text[param_len] = '\0';
                            // 去除尾部空白
                            while (param_len > 0 && isspace((unsigned char)param_text[param_len - 1])) {
                                param_text[--param_len] = '\0';
                            }
                        }

                        // 解析参数列表：统计参数数量、默认值数量和默认值文本
                        int parsed_param_count = 0;
                        int parsed_default_count = 0;
                        char* parsed_default_texts[32] = {0};
                        if (param_text[0] != '\0') {
                            const char* seg_start = param_text;
                            const char* p2 = param_text;
                            int depth = 0;
                            int in_str = 0;
                            char str_q = 0;
                            while (1) {
                                char c = *p2;
                                if (c == '\0') {
                                    int seg_len2 = (int)(p2 - seg_start);
                                    if (seg_len2 > 0 && parsed_param_count < 32) {
                                        // 在参数段中查找 = 号（不在字符串/括号内）
                                        int d2 = 0, s2 = 0; char sq2 = 0;
                                        for (int k = 0; k < seg_len2; k++) {
                                            char ch = seg_start[k];
                                            if (s2) {
                                                if (ch == '\\') { k++; continue; }
                                                if (ch == sq2) s2 = 0;
                                                continue;
                                            }
                                            if (ch == '"' || ch == '\'') { s2 = 1; sq2 = ch; continue; }
                                            if (ch == '(' || ch == '[' || ch == '{') { d2++; continue; }
                                            if (ch == ')' || ch == ']' || ch == '}') { d2--; continue; }
                                            if (ch == '=' && d2 == 0) {
                                                // 找到默认值
                                                const char* vs = seg_start + k + 1;
                                                while (vs < p2 && isspace((unsigned char)*vs)) vs++;
                                                const char* ve = p2 - 1;
                                                while (ve >= vs && isspace((unsigned char)*ve)) ve--;
                                                int vl = (int)(ve - vs + 1);
                                                if (vl > 0) {
                                                    parsed_default_texts[parsed_param_count] = (char*)malloc(vl + 1);
                                                    memcpy(parsed_default_texts[parsed_param_count], vs, vl);
                                                    parsed_default_texts[parsed_param_count][vl] = '\0';
                                                    parsed_default_count++;
                                                }
                                                break;
                                            }
                                        }
                                        parsed_param_count++;
                                    }
                                    break;
                                }
                                if (in_str) {
                                    if (c == '\\') { p2 += 2; continue; }
                                    if (c == str_q) in_str = 0;
                                    p2++;
                                    continue;
                                }
                                if (c == '"' || c == '\'') { in_str = 1; str_q = c; p2++; continue; }
                                if (c == '(' || c == '[' || c == '{') { depth++; p2++; continue; }
                                if (c == ')' || c == ']' || c == '}') { depth--; p2++; continue; }
                                if (c == ',' && depth == 0) {
                                    int seg_len2 = (int)(p2 - seg_start);
                                    if (seg_len2 > 0 && parsed_param_count < 32) {
                                        int d2 = 0, s2 = 0; char sq2 = 0;
                                        for (int k = 0; k < seg_len2; k++) {
                                            char ch = seg_start[k];
                                            if (s2) {
                                                if (ch == '\\') { k++; continue; }
                                                if (ch == sq2) s2 = 0;
                                                continue;
                                            }
                                            if (ch == '"' || ch == '\'') { s2 = 1; sq2 = ch; continue; }
                                            if (ch == '(' || ch == '[' || ch == '{') { d2++; continue; }
                                            if (ch == ')' || ch == ']' || ch == '}') { d2--; continue; }
                                            if (ch == '=' && d2 == 0) {
                                                const char* vs = seg_start + k + 1;
                                                while (vs < p2 && isspace((unsigned char)*vs)) vs++;
                                                const char* ve = p2 - 1;
                                                while (ve >= vs && isspace((unsigned char)*ve)) ve--;
                                                int vl = (int)(ve - vs + 1);
                                                if (vl > 0) {
                                                    parsed_default_texts[parsed_param_count] = (char*)malloc(vl + 1);
                                                    memcpy(parsed_default_texts[parsed_param_count], vs, vl);
                                                    parsed_default_texts[parsed_param_count][vl] = '\0';
                                                    parsed_default_count++;
                                                }
                                                break;
                                            }
                                        }
                                        parsed_param_count++;
                                    }
                                    seg_start = p2 + 1;
                                    p2++;
                                    continue;
                                }
                                p2++;
                            }
                        }

                        while (*after_func && (*after_func == ' ' || *after_func == '\t' || *after_func == '\n' || *after_func == '\r')) after_func++;

                        TypeKind return_type = TYPE_ANY;
                        TypeInfo* return_type_info = NULL;
                        char return_struct_name[64] = {0};

                        if (*after_func == ':') {
                            after_func++;
                            while (*after_func && (*after_func == ' ' || *after_func == '\t')) after_func++;

                            const char* type_start = after_func;

                            // 检测多返回值类型: [T1, T2, ...] 或 {"k": T1, ...}
                            if (*after_func == '[' || *after_func == '{') {
                                // 读取整个多返回值类型表达式
                                int depth = 0;
                                while (*after_func) {
                                    if (*after_func == '[' || *after_func == '{') depth++;
                                    else if (*after_func == ']' || *after_func == '}') { depth--; if (depth == 0) { after_func++; break; } }
                                    after_func++;
                                }
                                int mr_len = (int)(after_func - type_start);
                                if (mr_len > 0 && mr_len < 128) {
                                    char mr_str[128];
                                    strncpy(mr_str, type_start, mr_len);
                                    mr_str[mr_len] = '\0';
                                    // 解析多返回值类型
                                    return_type_info = parse_type_from_string(mr_str);
                                    if (return_type_info) {
                                        return_type = return_type_info->kind;
                                    } else {
                                        return_type = TYPE_MULTI_RET;
                                    }
                                }
                                // 跳过后面的可空标记和函数体
                                goto scan_func_body_skip;
                            }

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

                            // ★ 消费可空返回类型的尾部 '?'（如 HtmlNode?）
                            // 若不消费，模块函数符号表的返回类型会丢失可空标记，
                            // 导致跨模块调用时 var 推断成非可空，== null 判断误报
                            // "struct 值类型默认不为 null" 警告（与 scan_struct.inc 同源 bug）
                            int return_is_nullable = 0;
                            while (*after_func && (*after_func == ' ' || *after_func == '\t')) after_func++;
                            if (*after_func == '?') {
                                return_is_nullable = 1;
                                after_func++;
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

                                // 应用可空标记（可选链返回、struct 可空等）
                                if (return_is_nullable && return_type_info) {
                                    return_type_info->nullable = 1;
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

                        scan_func_body_skip:
                        // 即时计算函数定义行号（从 source 开头数到 p 的换行数 +1）
                        // p 指向 "export" 关键字开头
                        {
                            int func_def_line = 1;
                            for (const char* c = source; c < p; c++) {
                                if (*c == '\n') func_def_line++;
                            }
                            // 临时存储行号，通过全局变量传递（add_func 不接受 line 参数）
                            // 这里先调用 add_func，再通过 find_func 设置行号
                            module_symbol_table_add_func(table, func_name, return_type,
                                return_struct_name[0] ? return_struct_name : NULL, func_type_param_count, return_type_info,
                                param_text[0] ? param_text : NULL,
                                parsed_param_count, parsed_default_count, parsed_default_texts);
                            ModuleFuncSymbol* added_func = module_symbol_table_find_func(table, func_name);
                            if (added_func) {
                                added_func->line = func_def_line;
                            }
                        }
                        if (return_type_info) { type_free(return_type_info); return_type_info = NULL; }
                        // 释放临时的默认值文本（add_func 内部已 strdup）
                        for (int i = 0; i < 32; i++) {
                            free(parsed_default_texts[i]);
                        }
                    }
                }

                p = after_func;
                continue;
            }
