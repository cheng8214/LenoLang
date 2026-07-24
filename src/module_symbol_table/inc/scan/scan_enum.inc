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
                            const char* m_start = after_enum;
                            while (*after_enum && (isalnum((unsigned char)*after_enum) || *after_enum == '_')) after_enum++;
                            int m_len = (int)(after_enum - m_start);
                            if (m_len > 0 && m_len < 64) {
                                member_names_buf[member_count] = (char*)malloc(m_len + 1);
                                strncpy(member_names_buf[member_count], m_start, m_len);
                                member_names_buf[member_count][m_len] = '\0';
                                // 检查是否有显式值 = number
                                member_values_buf[member_count] = -1; // -1 表示无显式值
                                while (*after_enum && (*after_enum == ' ' || *after_enum == '\t')) after_enum++;
                                if (*after_enum == '=') {
                                    after_enum++;
                                    while (*after_enum && (*after_enum == ' ' || *after_enum == '\t')) after_enum++;
                                    // 解析整数值（支持负数和十六进制）
                                    int neg = 0;
                                    if (*after_enum == '-') { neg = 1; after_enum++; }
                                    int64_t val = 0;
                                    // 支持 0x 前缀
                                    if (*after_enum == '0' && (*(after_enum+1) == 'x' || *(after_enum+1) == 'X')) {
                                        after_enum += 2;
                                        while (*after_enum && isxdigit((unsigned char)*after_enum)) {
                                            val = val * 16 + (*after_enum <= '9' ? *after_enum - '0' : (*after_enum <= 'F' ? *after_enum - 'A' + 10 : *after_enum - 'a' + 10));
                                            after_enum++;
                                        }
                                    } else {
                                        while (*after_enum && isdigit((unsigned char)*after_enum)) {
                                            val = val * 10 + (*after_enum - '0');
                                            after_enum++;
                                        }
                                    }
                                    member_values_buf[member_count] = neg ? -val : val;
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
