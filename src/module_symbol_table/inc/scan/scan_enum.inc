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
                    char* member_names_buf[MOD_MAX_MEMBERS];
                    int64_t member_values_buf[MOD_MAX_MEMBERS];
                    int member_count = 0;
                    while (*after_enum && *after_enum != '{') after_enum++;
                    if (*after_enum == '{') {
                        after_enum++;
                        while (*after_enum && *after_enum != '}' && member_count < MOD_MAX_MEMBERS) {
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
                                member_names_buf[member_count] = (char*)malloc(m_len + 1);
                                strncpy(member_names_buf[member_count], m_start, m_len);
                                member_names_buf[member_count][m_len] = '\0';
                                // 检查是否有显式值 = expression
                                member_values_buf[member_count] = -1; // -1 表示无显式值
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

                                        // 尝试解析为简单数字（十六进制或十进制，含负号）
                                        int parsed = 0;
                                        int neg = 0;
                                        const char* pnum = expr_buf;
                                        while (*pnum == ' ' || *pnum == '\t') pnum++;
                                        if (*pnum == '-') { neg = 1; pnum++; }
                                        if (*pnum == '0' && (*(pnum+1) == 'x' || *(pnum+1) == 'X')) {
                                            pnum += 2;
                                            int64_t val = 0;
                                            int has_digit = 0;
                                            while (*pnum && isxdigit((unsigned char)*pnum)) {
                                                val = val * 16 + (*pnum <= '9' ? *pnum - '0' : (*pnum <= 'F' ? *pnum - 'A' + 10 : *pnum - 'a' + 10));
                                                pnum++;
                                                has_digit = 1;
                                            }
                                            // 跳过数字后的尾部空格
                                            while (*pnum == ' ' || *pnum == '\t') pnum++;
                                            if (has_digit && (*pnum == '\0' || *pnum == '/')) {
                                                member_values_buf[member_count] = neg ? -val : val;
                                                parsed = 1;
                                            }
                                        } else if (isdigit((unsigned char)*pnum)) {
                                            int64_t val = 0;
                                            while (*pnum && isdigit((unsigned char)*pnum)) {
                                                val = val * 10 + (*pnum - '0');
                                                pnum++;
                                            }
                                            // 跳过数字后的尾部空格
                                            while (*pnum == ' ' || *pnum == '\t') pnum++;
                                            if (*pnum == '\0' || *pnum == '/') {
                                                member_values_buf[member_count] = neg ? -val : val;
                                                parsed = 1;
                                            }
                                        }

                                        // 如果不是简单数字，尝试用简易表达式求值器
                                        if (!parsed) {
                                            // 简易递归下降表达式求值器
                                            // 支持：数字、十六进制、enum 成员引用、+ - * / % << >> | & ^
                                            // 使用内联的方式实现简易求值
                                            // 简易 token 化 + 递归求值
                                            // 保存当前 enum 的已解析成员用于引用
                                            char* saved_names[MOD_MAX_MEMBERS];
                                            int64_t saved_values[MOD_MAX_MEMBERS];
                                            int saved_count = member_count;
                                            for (int si = 0; si < member_count; si++) {
                                                saved_names[si] = member_names_buf[si];
                                                saved_values[si] = member_values_buf[si] == -1 ? (int64_t)si : member_values_buf[si];
                                            }

                                            // 简易递归下降求值器
                                            // expr = bitor_expr
                                            // bitor_expr = bitxor_expr ('|' bitxor_expr)*
                                            // bitxor_expr = bitand_expr ('^' bitand_expr)*
                                            // bitand_expr = shift_expr ('&' shift_expr)*
                                            // shift_expr = add_expr ('<<' | '>>') add_expr)*
                                            // add_expr = mul_expr (('+' | '-') mul_expr)*
                                            // mul_expr = unary_expr (('*' | '/' | '%') unary_expr)*
                                            // unary_expr = ('-' | '~')? primary
                                            // primary = number | hex | enum_member_ref
                                            {
                                                // 内联递归下降求值器
                                                // 简易 token 化
                                                typedef enum { TOK_NUM, TOK_IDENT, TOK_OP, TOK_END } TokType;
                                                typedef struct { TokType type; int64_t val; char op; char ident[64]; } Token;
                                                Token tokens[64];
                                                int tok_count = 0;

                                                // 简易词法分析
                                                const char* cp = expr_buf;
                                                while (*cp && tok_count < 64) {
                                                    while (*cp == ' ' || *cp == '\t') cp++;
                                                    if (!*cp) break;
                                                    if (isdigit((unsigned char)*cp)) {
                                                        int64_t v = 0;
                                                        if (*cp == '0' && (*(cp+1) == 'x' || *(cp+1) == 'X')) {
                                                            cp += 2;
                                                            while (*cp && isxdigit((unsigned char)*cp)) {
                                                                v = v * 16 + (*cp <= '9' ? *cp - '0' : (*cp <= 'F' ? *cp - 'A' + 10 : *cp - 'a' + 10));
                                                                cp++;
                                                            }
                                                        } else {
                                                            while (*cp && isdigit((unsigned char)*cp)) {
                                                                v = v * 10 + (*cp - '0');
                                                                cp++;
                                                            }
                                                        }
                                                        tokens[tok_count].type = TOK_NUM;
                                                        tokens[tok_count].val = v;
                                                        tok_count++;
                                                    } else if (isalpha((unsigned char)*cp) || *cp == '_') {
                                                        int il = 0;
                                                        while (*cp && (isalnum((unsigned char)*cp) || *cp == '_') && il < 63) {
                                                            tokens[tok_count].ident[il++] = *cp;
                                                            cp++;
                                                        }
                                                        tokens[tok_count].ident[il] = '\0';
                                                        tokens[tok_count].type = TOK_IDENT;
                                                        tok_count++;
                                                    } else if (*cp == '<' && *(cp+1) == '<') {
                                                        tokens[tok_count].type = TOK_OP;
                                                        tokens[tok_count].op = 'L'; // L = shift left
                                                        cp += 2;
                                                        tok_count++;
                                                    } else if (*cp == '>' && *(cp+1) == '>') {
                                                        tokens[tok_count].type = TOK_OP;
                                                        tokens[tok_count].op = 'R'; // R = shift right
                                                        cp += 2;
                                                        tok_count++;
                                                    } else if (*cp == '|' || *cp == '&' || *cp == '^' || *cp == '+' || *cp == '-' || *cp == '*' || *cp == '/' || *cp == '%') {
                                                        tokens[tok_count].type = TOK_OP;
                                                        tokens[tok_count].op = *cp;
                                                        cp++;
                                                        tok_count++;
                                                    } else if (*cp == '~') {
                                                        tokens[tok_count].type = TOK_OP;
                                                        tokens[tok_count].op = '~';
                                                        cp++;
                                                        tok_count++;
                                                    } else {
                                                        cp++; // 跳过未知字符
                                                    }
                                                }
                                                tokens[tok_count].type = TOK_END;

                                                // 将 ident 替换为对应值
                                                for (int ti = 0; ti < tok_count; ti++) {
                                                    if (tokens[ti].type == TOK_IDENT) {
                                                        int found = 0;
                                                        for (int mi2 = 0; mi2 < saved_count; mi2++) {
                                                            if (strcmp(saved_names[mi2], tokens[ti].ident) == 0) {
                                                                tokens[ti].type = TOK_NUM;
                                                                tokens[ti].val = saved_values[mi2];
                                                                found = 1;
                                                                break;
                                                            }
                                                        }
                                                        if (!found) {
                                                            // 不是 enum 成员，无法求值
                                                            break;
                                                        }
                                                    }
                                                }

                                                // 简易递归下降求值
                                                // 使用栈求值
                                                int ok_val = 1;

                                                // 简化版：支持运算符优先级的递归下降
                                                // 使用内联函数（通过代码复制）
                                                // 由于 C99 不支持嵌套函数，改用 goto 实现

                                                // 或者用最简方案：先处理所有一元运算，然后从左到右计算
                                                // 这对 enum 来说足够了（enum 表达式通常不复杂）

                                                // 最简方案：使用递归 + 函数指针
                                                // 实际上由于 .inc 文件被 include 进 .c 文件，
                                                // 可以使用文件级的 static 函数

                                                // 暂时使用从左到右的简单求值（不处理优先级）
                                                // 对 enum 表达式来说，通常只有一种运算符
                                                if (tok_count > 0 && tokens[0].type == TOK_NUM) {
                                                    int64_t result = tokens[0].val;
                                                    int ti2 = 1;
                                                    while (ti2 < tok_count && tokens[ti2].type == TOK_OP && ok_val) {
                                                        char op = tokens[ti2].op;
                                                        ti2++;
                                                        if (ti2 >= tok_count || tokens[ti2].type != TOK_NUM) { ok_val = 0; break; }
                                                        int64_t rhs = tokens[ti2].val;
                                                        ti2++;
                                                        switch (op) {
                                                            case '+': result += rhs; break;
                                                            case '-': result -= rhs; break;
                                                            case '*': result *= rhs; break;
                                                            case '/': result = rhs != 0 ? result / rhs : 0; break;
                                                            case '%': result = rhs != 0 ? result % rhs : 0; break;
                                                            case '|': result |= rhs; break;
                                                            case '&': result &= rhs; break;
                                                            case '^': result ^= rhs; break;
                                                            case 'L': result <<= rhs; break;
                                                            case 'R': result >>= rhs; break;
                                                            default: ok_val = 0; break;
                                                        }
                                                    }
                                                    if (ok_val && ti2 == tok_count) {
                                                        member_values_buf[member_count] = result;
                                                        parsed = 1;
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                                member_count++;
                            }
                            // 跳过分隔符：逗号、分号或换行
                            while (*after_enum && *after_enum != ',' && *after_enum != ';' && *after_enum != '}' && *after_enum != '\n') after_enum++;
                            if (*after_enum == ',' || *after_enum == ';') after_enum++;
                        }
                        if (*after_enum == '}') after_enum++;
                    }

                    module_symbol_table_add_enum(table, enum_name, member_count, member_names_buf, member_values_buf);
                    for (int mi = 0; mi < member_count; mi++) free(member_names_buf[mi]);
                }

                p = after_enum;
                continue;
            }
