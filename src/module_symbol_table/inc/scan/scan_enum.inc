            // 检查是否是 enum 定义
            if (strncmp(after_export, "enum", 4) == 0 && (after_export[4] == ' ' || after_export[4] == '\t' || after_export[4] == '\n' || after_export[4] == '\r' || after_export[4] == '\0')) {
                char* after_enum = after_export + 4;
                while (*after_enum && (*after_enum == ' ' || *after_enum == '\t')) after_enum++;

                const char* name_start = after_enum;
                while (*after_enum && (isalnum((unsigned char)*after_enum) || *after_enum == '_')) after_enum++;
                int name_len = (int)(after_enum - name_start);

                if (name_len > 0 && name_len < 64) {
                    char enum_name[64];
                    strncpy(enum_name, name_start, name_len);
                    enum_name[name_len] = '\0';

                    // 解析成员名和显式值
                    // （动态数组，无固定上限：修复超过 128 个成员被静默截断的问题）
                    char** member_names_buf = NULL;
                    int64_t* member_values_buf = NULL;
                    int member_count = 0;
                    int member_capacity = 0;
                    // 下一个「无显式值」成员的自动递增值（首个为 0，之后跟随前一个成员 +1）
                    int64_t next_auto = 0;
                    while (*after_enum && *after_enum != '{') after_enum++;
                    if (*after_enum == '{') {
                        after_enum++;
                        while (*after_enum && *after_enum != '}') {
                            while (*after_enum && (*after_enum == ' ' || *after_enum == '\t' || *after_enum == '\n' || *after_enum == '\r')) after_enum++;
                            if (*after_enum == '}' || *after_enum == '\0') break;
                            if (*after_enum == '/' && *(after_enum+1) == '/') {
                                while (*after_enum && *after_enum != '\n') after_enum++;
                                continue;
                            }
                            // 跳过多行注释 /* ... */
                            if (*after_enum == '/' && *(after_enum+1) == '*') {
                                after_enum += 2;
                                while (*after_enum && !(*after_enum == '*' && *(after_enum+1) == '/')) after_enum++;
                                if (*after_enum) after_enum += 2;
                                continue;
                            }
                            const char* m_start = after_enum;
                            while (*after_enum && (isalnum((unsigned char)*after_enum) || *after_enum == '_')) after_enum++;
                            int m_len = (int)(after_enum - m_start);
                            if (m_len > 0 && m_len < 64) {
                                // 动态扩容成员数组（容量不足时翻倍）
                                int member_slot_ok = 0;
                                if (member_count < member_capacity) {
                                    member_slot_ok = 1;
                                } else {
                                    int new_cap = member_capacity == 0 ? 16 : member_capacity * 2;
                                    char** grown_names = (char**)realloc(member_names_buf, sizeof(char*) * new_cap);
                                    if (grown_names) {
                                        int64_t* grown_values = (int64_t*)realloc(member_values_buf, sizeof(int64_t) * new_cap);
                                        if (grown_values) {
                                            member_names_buf = grown_names;
                                            member_values_buf = grown_values;
                                            member_capacity = new_cap;
                                            member_slot_ok = 1;
                                        }
                                    }
                                }
                                if (member_slot_ok) {
                                member_names_buf[member_count] = (char*)malloc(m_len + 1);
                                strncpy(member_names_buf[member_count], m_start, m_len);
                                member_names_buf[member_count][m_len] = '\0';
                                // 检查是否有显式值 = expression
                                // 注意：不能再用 -1 之类的哨兵表示「无显式值」—— 枚举成员值本身
                                // 可以是负数（enum E { A = -1 } 是极常见写法），哨兵会被当成
                                // 「自动递增」而把合法值改写成下标。这里用 has_explicit 显式标记，
                                // 并在本函数末尾统一决定最终取值（见下面的 next_auto）。
                                member_values_buf[member_count] = 0;
                                int has_explicit = 0;
                                while (*after_enum && (*after_enum == ' ' || *after_enum == '\t')) after_enum++;
                                if (*after_enum == '=') {
                                    after_enum++;
                                    while (*after_enum && (*after_enum == ' ' || *after_enum == '\t')) after_enum++;

                                    // 解析常量表达式值（支持数字、十六进制、负数、
                                    // 移位 << >>、位运算 | & ^、算术 + - * / %、
                                    // enum 成员引用）
                                    // 表达式终止于逗号、分号、换行、} 或行注释 //
                                    const char* expr_start = after_enum;
                                    // 找到表达式结束位置
                                    while (*after_enum && *after_enum != ',' && *after_enum != ';' && *after_enum != '\n' && *after_enum != '}') {
                                        // 遇到行注释 // 时，表达式结束
                                        if (*after_enum == '/' && *(after_enum+1) == '/') break;
                                        after_enum++;
                                    }
                                    int expr_len = (int)(after_enum - expr_start);
                                    if (expr_len > 0 && expr_len < 256) {
                                        char expr_buf[256];
                                        // 去除尾部空白
                                        int real_len = expr_len;
                                        while (real_len > 0 && (expr_start[real_len-1] == ' ' || expr_start[real_len-1] == '\t' || expr_start[real_len-1] == '\r')) real_len--;
                                        strncpy(expr_buf, expr_start, real_len);
                                        expr_buf[real_len] = '\0';

                                        // 常量表达式求值：统一走与解析器同优先级的递归下降求值器
                                        // （定义见 inc/sym_table_enum_expr.inc）。
                                        // 历史：此处曾有两段自制逻辑 —— 先「试简单数字」，失败再退回
                                        // 「从左到右」求值（不处理优先级，括号被当未知字符丢弃），与
                                        // 解析器对同一表达式给出不同值（P = 2 + 3 * 4：解析器 14、
                                        // 扫描器 20），同一程序里成员值随访问写法而变。两段均已删除。
                                        {
                                            int64_t ev_val = 0;
                                            if (sym_enum_eval_expr(expr_buf, member_names_buf,
                                                                   member_values_buf, member_count,
                                                                   &ev_val)) {
                                                member_values_buf[member_count] = ev_val;
                                                has_explicit = 1;
                                            }
                                            // 求值失败：保持 has_explicit = 0（按「无显式值」自动递增）。
                                            // 此处不报错 —— 扫描器同时服务 LSP 与跨模块查询；模块自身
                                            // 编译时解析器（parse_enum_stmt）会对不可求值的表达式报错。
                                        }
                                    }
                                }        // 结束 if (*after_enum == '=')：显式值表达式已解析完毕
                                // 统一决定最终取值：有显式值 → 保留原值并按它推进 next_auto；
                                // 无显式值 → 取自动递增值。**这一段必须对每个成员都执行** ——
                                // 此前它被误关在 `if (*after_enum == '=')` 内部，导致不写 `=` 的
                                // 成员根本不进成员表（跨模块 use 报「枚举没有成员」的根因）。
                                if (has_explicit) {
                                    // 显式值：不再改写它（负值也必须原样保留），后续自动值从它 +1 起
                                    next_auto = member_values_buf[member_count] + 1;
                                } else {
                                    // 无显式值：前一个成员 +1（首个成员为 0）
                                    member_values_buf[member_count] = next_auto;
                                    next_auto += 1;
                                }
                                member_count++;
                            }
                        }
                        // 跳过分隔符：逗号、分号或换行
                            while (*after_enum && *after_enum != ',' && *after_enum != ';' && *after_enum != '}' && *after_enum != '\n') after_enum++;
                            if (*after_enum == ',' || *after_enum == ';') after_enum++;
                        }
                        if (*after_enum == '}') after_enum++;
                    }

                    module_symbol_table_add_enum(table, enum_name, member_count, member_names_buf, member_values_buf);
                    for (int mi = 0; mi < member_count; mi++) free(member_names_buf[mi]);
                    // 释放动态数组本体
                    free(member_names_buf);
                    free(member_values_buf);
                }

                p = after_enum;
                continue;
            }
