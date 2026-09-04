            // 检查是否是 cstruct 定义
            if (strncmp(after_export, "cstruct", 7) == 0 && (after_export[7] == ' ' || after_export[7] == '\t' || after_export[7] == '\n' || after_export[7] == '\r' || after_export[7] == '\0')) {
                char* after_cstruct = after_export + 7;
                while (*after_cstruct && (*after_cstruct == ' ' || *after_cstruct == '\t')) after_cstruct++;

                const char* name_start = after_cstruct;
                while (*after_cstruct && (isalnum((unsigned char)*after_cstruct) || *after_cstruct == '_')) after_cstruct++;
                int name_len = (int)(after_cstruct - name_start);

                if (name_len > 0 && name_len < 64) {
                    char cstruct_name[64];
                    strncpy(cstruct_name, name_start, name_len);
                    cstruct_name[name_len] = '\0';

                    while (*after_cstruct && (*after_cstruct == ' ' || *after_cstruct == '\t' || *after_cstruct == '\n' || *after_cstruct == '\r')) after_cstruct++;

                    if (*after_cstruct == '{') {
                        after_cstruct++;

                        // 字段数组按需动态增长（无固定上限，修复超过 128 个字段被静默丢弃的问题）
                        // cstruct 只解析字段，不解析方法（cstruct 用于 C 布局，不支持方法）
                        ModuleStructField* fields = NULL;
                        int field_capacity = 0;
                        int field_count = 0;

                        while (*after_cstruct && *after_cstruct != '}') {
                            while (*after_cstruct && (*after_cstruct == ' ' || *after_cstruct == '\t' || *after_cstruct == '\n' || *after_cstruct == '\r')) after_cstruct++;
                            if (!*after_cstruct || *after_cstruct == '}') break;

                            if (*after_cstruct == '/' && *(after_cstruct+1) == '/') {
                                while (*after_cstruct && *after_cstruct != '\n') after_cstruct++;
                                continue;
                            }
                            // 跳过多行注释 /* ... */
                            if (*after_cstruct == '/' && *(after_cstruct+1) == '*') {
                                after_cstruct += 2;
                                while (*after_cstruct && !(*after_cstruct == '*' && *(after_cstruct+1) == '/')) after_cstruct++;
                                if (*after_cstruct) after_cstruct += 2;
                                continue;
                            }

                            // cstruct 不支持方法，跳过 func 定义
                            if (strncmp(after_cstruct, "func", 4) == 0 && (after_cstruct[4] == ' ' || after_cstruct[4] == '\t')) {
                                // 跳过整个 func 定义
                                char* after_func = after_cstruct + 4;
                                while (*after_func && (*after_func == ' ' || *after_func == '\t')) after_func++;
                                // 跳过方法名
                                while (*after_func && (isalnum((unsigned char)*after_func) || *after_func == '_')) after_func++;
                                // 跳过参数列表
                                while (*after_func && (*after_func == ' ' || *after_func == '\t')) after_func++;
                                if (*after_func == '(') {
                                    after_func++;
                                    int paren_depth = 1;
                                    while (*after_func && paren_depth > 0) {
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
                                        after_func++;
                                    }
                                }
                                // 跳过返回类型
                                while (*after_func && (*after_func == ' ' || *after_func == '\t' || *after_func == '\n' || *after_func == '\r')) after_func++;
                                if (*after_func == ':') {
                                    after_func++;
                                    while (*after_func && (*after_func == ' ' || *after_func == '\t')) after_func++;
                                    while (*after_func && (isalnum((unsigned char)*after_func) || *after_func == '_')) after_func++;
                                }
                                // 跳过方法体
                                while (*after_func && (*after_func == ' ' || *after_func == '\t' || *after_func == '\n' || *after_func == '\r')) after_func++;
                                if (*after_func == '{') {
                                    after_func++;
                                    int brace_depth = 1;
                                    while (*after_func && brace_depth > 0) {
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
                                        // 跳过字符串字面量（双引号和单引号，避免字符串中的 { } / // 等影响深度计数和注释检测）
                                        if (*after_func == '"' || *after_func == '\'') {
                                            char quote = *after_func;
                                            after_func++;
                                            while (*after_func && *after_func != quote) {
                                                if (*after_func == '\\' && *(after_func+1)) after_func += 2;
                                                else after_func++;
                                            }
                                            if (*after_func) after_func++;
                                            continue;
                                        }
                                        if (*after_func == '{') brace_depth++;
                                        else if (*after_func == '}') brace_depth--;
                                        after_func++;
                                    }
                                }
                                after_cstruct = after_func;
                                continue;
                            }

                            // 解析字段
                            const char* type_start = after_cstruct;
                            while (*after_cstruct && (isalnum((unsigned char)*after_cstruct) || *after_cstruct == '_')) after_cstruct++;
                            int type_len = (int)(after_cstruct - type_start);

                            if (type_len > 0 && type_len < 64) {
                                char type_str[64];
                                strncpy(type_str, type_start, type_len);
                                type_str[type_len] = '\0';

                                // 检查是否是 Ptr[T] / Array[T] / Dict[K,V] 类型
                                TypeKind element_type = TYPE_PTR;  // 默认元素类型
                                char* element_struct_name = NULL;   // 元素类型的结构体/face/clib 名称
                                while (*after_cstruct && (*after_cstruct == ' ' || *after_cstruct == '\t')) after_cstruct++;
                                if (*after_cstruct == '[') {
                                    // 解析 [T] 中的 T
                                    after_cstruct++;
                                    while (*after_cstruct && (*after_cstruct == ' ' || *after_cstruct == '\t')) after_cstruct++;
                                    const char* elem_type_start = after_cstruct;
                                    while (*after_cstruct && (isalnum((unsigned char)*after_cstruct) || *after_cstruct == '_')) after_cstruct++;
                                    int elem_type_len = (int)(after_cstruct - elem_type_start);
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
                                    while (*after_cstruct && *after_cstruct != ']') after_cstruct++;
                                    if (*after_cstruct == ']') after_cstruct++;
                                }

                                while (*after_cstruct && (*after_cstruct == ' ' || *after_cstruct == '\t')) after_cstruct++;

                                const char* field_start = after_cstruct;
                                while (*after_cstruct && (isalnum((unsigned char)*after_cstruct) || *after_cstruct == '_')) after_cstruct++;
                                int field_len = (int)(after_cstruct - field_start);

                                if (field_len > 0 && field_len < 64) {
                                    // 动态扩容字段数组（容量不足时翻倍，无固定上限）
                                    int field_slot_ok = 0;
                                    if (field_count < field_capacity) {
                                        field_slot_ok = 1;
                                    } else {
                                        int new_cap = field_capacity == 0 ? 16 : field_capacity * 2;
                                        ModuleStructField* new_fields = (ModuleStructField*)realloc(fields, sizeof(ModuleStructField) * new_cap);
                                        if (new_fields) {
                                            fields = new_fields;
                                            field_capacity = new_cap;
                                            field_slot_ok = 1;
                                        }
                                    }
                                    if (field_slot_ok) {
                                        TypeKind ft = parse_base_type(type_str);
                                        fields[field_count].name = (char*)malloc(field_len + 1);
                                        strncpy(fields[field_count].name, field_start, field_len);
                                        fields[field_count].name[field_len] = '\0';
                                        fields[field_count].type = ft;
                                        fields[field_count].struct_name = NULL;
                                        // 即时计算字段定义行号
                                        {
                                            int fl = 1;
                                            for (const char* c = source; c < field_start; c++) {
                                                if (*c == '\n') fl++;
                                            }
                                            fields[field_count].line = fl;
                                        }
                                        if (ft == TYPE_ANY) {
                                            if (is_known_clib(type_str, clib_names, clib_name_count)) {
                                                fields[field_count].type = TYPE_CLIB;
                                                fields[field_count].struct_name = strdup(type_str);
                                            } else if (is_known_struct(type_str, struct_names, struct_name_count)) {
                                                fields[field_count].type = TYPE_STRUCT;
                                                fields[field_count].struct_name = strdup(type_str);
                                            }
                                        }
                                        fields[field_count].element_type = element_type;
                                        fields[field_count].element_struct_name = element_struct_name;
                                        fields[field_count].nullable = 0;  // cstruct 字段不支持 nullable，显式初始化
                                        field_count++;
                                    }
                                    // 内存不足（realloc 失败）时跳过该字段，继续扫描
                                }
                            }

                            // 跳过字段默认值（可能包含嵌套的 {} 或 []，如 Dict val = {}）
	                            // 先跳过到 = 或行尾/分号/}
	                            // 注意：遇到 // 注释时直接跳到行尾，避免注释中的 } 导致扫描错位
	                            while (*after_cstruct && *after_cstruct != '\n' && *after_cstruct != ';' && *after_cstruct != '=' && *after_cstruct != '}') {
	                                if (*after_cstruct == '/' && *(after_cstruct+1) == '/') {
	                                    while (*after_cstruct && *after_cstruct != '\n') after_cstruct++;
	                                    break;
	                                }
	                                if (*after_cstruct == '/' && *(after_cstruct+1) == '*') {
	                                    after_cstruct += 2;
	                                    while (*after_cstruct && !(*after_cstruct == '*' && *(after_cstruct+1) == '/')) after_cstruct++;
	                                    if (*after_cstruct) after_cstruct += 2;
	                                    continue;
	                                }
	                                after_cstruct++;
	                            }
	                            if (*after_cstruct == '=') {
	                                after_cstruct++; // 跳过 =
	                                // 跳过空白
	                                while (*after_cstruct && (*after_cstruct == ' ' || *after_cstruct == '\t')) after_cstruct++;
	                                // 处理默认值中的嵌套括号
	                                if (*after_cstruct == '{') {
	                                    int depth = 1;
	                                    after_cstruct++;
	                                    while (*after_cstruct && depth > 0) {
	                                        if (*after_cstruct == '/' && *(after_cstruct+1) == '/') {
	                                            while (*after_cstruct && *after_cstruct != '\n') after_cstruct++;
	                                            continue;
	                                        }
	                                        if (*after_cstruct == '/' && *(after_cstruct+1) == '*') {
	                                            after_cstruct += 2;
	                                            while (*after_cstruct && !(*after_cstruct == '*' && *(after_cstruct+1) == '/')) after_cstruct++;
	                                            if (*after_cstruct) after_cstruct += 2;
	                                            continue;
	                                        }
	                                        if (*after_cstruct == '{') depth++;
	                                        else if (*after_cstruct == '}') depth--;
	                                        after_cstruct++;
	                                    }
	                                } else if (*after_cstruct == '[') {
	                                    int depth = 1;
	                                    after_cstruct++;
	                                    while (*after_cstruct && depth > 0) {
	                                        if (*after_cstruct == '/' && *(after_cstruct+1) == '/') {
	                                            while (*after_cstruct && *after_cstruct != '\n') after_cstruct++;
	                                            continue;
	                                        }
	                                        if (*after_cstruct == '/' && *(after_cstruct+1) == '*') {
	                                            after_cstruct += 2;
	                                            while (*after_cstruct && !(*after_cstruct == '*' && *(after_cstruct+1) == '/')) after_cstruct++;
	                                            if (*after_cstruct) after_cstruct += 2;
	                                            continue;
	                                        }
	                                        if (*after_cstruct == '[') depth++;
	                                        else if (*after_cstruct == ']') depth--;
	                                        after_cstruct++;
	                                    }
	                                } else {
	                                    // 简单值（数字、字符串等），跳到 ; 或行尾或 }
	                                    while (*after_cstruct && *after_cstruct != '\n' && *after_cstruct != ';' && *after_cstruct != '}') {
	                                        if (*after_cstruct == '/' && *(after_cstruct+1) == '/') {
	                                            while (*after_cstruct && *after_cstruct != '\n') after_cstruct++;
	                                            break;
	                                        }
	                                        if (*after_cstruct == '/' && *(after_cstruct+1) == '*') {
	                                            after_cstruct += 2;
	                                            while (*after_cstruct && !(*after_cstruct == '*' && *(after_cstruct+1) == '/')) after_cstruct++;
	                                            if (*after_cstruct) after_cstruct += 2;
	                                            continue;
	                                        }
	                                        after_cstruct++;
	                                    }
	                                }
	                            }
	                            // 跳过分号（无论是否有默认值）
	                            if (*after_cstruct == ';') after_cstruct++;
                        }

                        // 添加 cstruct 到符号表（cstruct 无方法，method_count 恒为 0）
                        module_symbol_table_add_struct(table, cstruct_name, field_count, fields, 0, NULL, 1, 0, NULL);

                        // 释放临时分配的内存
                        for (int i = 0; i < field_count; i++) {
                            free(fields[i].name);
                        }
                        free(fields);

                        while (*after_cstruct && *after_cstruct != '}') after_cstruct++;
                        if (*after_cstruct == '}') after_cstruct++;
                    }
                }

                p = after_cstruct;
                continue;
            }
