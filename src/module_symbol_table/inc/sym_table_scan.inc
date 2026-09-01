static int module_symbol_table_scan_depth(ModuleSymbolTable* table, const char* current_file, int depth) {
    if (depth > MODULE_SCAN_MAX_DEPTH) return -1;
    if (!table || !table->module_path) return -1;

    // Pass 1: 读取文件、规范化路径、收集所有 struct/cstruct/clib/face/enum 名称
    #include "scan/scan_pass1.inc"

    // Pass 2: 初始化本地别名表、进入主循环、跳过空白/注释、处理 use 导入
    #include "scan/scan_pass2_init.inc"

        // 查找 export 关键字
        if (strncmp(p, "export", 6) == 0 && (p[6] == ' ' || p[6] == '\t' || p[6] == '\n' || p[6] == '\r' || p[6] == '\0')) {
            char* after_export = p + 6;
            while (*after_export && (*after_export == ' ' || *after_export == '\t' || *after_export == '\n' || *after_export == '\r')) after_export++;

            // 跳过可选的 packed / align(N) 前缀（cstruct 布局属性）
            for (int attr_pass = 0; attr_pass < 2; attr_pass++) {
                if (strncmp(after_export, "packed", 6) == 0 && (after_export[6] == ' ' || after_export[6] == '\t' || after_export[6] == '\n' || after_export[6] == '\r' || after_export[6] == '\0')) {
                    after_export += 6;
                    while (*after_export && (*after_export == ' ' || *after_export == '\t' || *after_export == '\n' || *after_export == '\r')) after_export++;
                } else if (strncmp(after_export, "align", 5) == 0 && (after_export[5] == ' ' || after_export[5] == '\t' || after_export[5] == '(')) {
                    after_export += 5;
                    while (*after_export && (*after_export == ' ' || *after_export == '\t')) after_export++;
                    if (*after_export == '(') {
                        after_export++;
                        while (*after_export && *after_export != ')') after_export++;
                        if (*after_export == ')') after_export++;
                        while (*after_export && (*after_export == ' ' || *after_export == '\t' || *after_export == '\n' || *after_export == '\r')) after_export++;
                    }
                } else {
                    break;
                }
            }

            #include "scan/scan_struct.inc"
            #include "scan/scan_cstruct.inc"
            #include "scan/scan_face.inc"
            #include "scan/scan_enum.inc"
            #include "scan/scan_func.inc"
            #include "scan/scan_var.inc"
            #include "scan/scan_alias.inc"
        }

        if (*p == '\n') cur_line++;
        p++;
    }

    // ---- 自校验：符号收集完整性交叉验证（防"静默截断"类 bug 复发） ----
    // 对每个 struct/face 符号，在源码中重新定位其定义体，
    // 统计体内 "func " 关键字出现次数（跳过注释与字符串），与方法数交叉验证。
    // 不一致说明扫描器收集不完整（历史上曾因固定上限数组静默截断方法表，
    // 导致错误爆发在远处的调用点），此处立即报错，把问题定位在定义处。
    {
        const char* struct_kinds[2] = { "struct", "cstruct" };
        for (int si = 0; si < table->struct_count; si++) {
            ModuleStructSymbol* sym = &table->structs[si];
            // 在源码中定位定义体（use 导入的符号不在本源码中，跳过校验）
            const char* def_open = NULL;
            for (int kw = 0; kw < 2 && !def_open; kw++) {
                size_t kw_len = strlen(struct_kinds[kw]);
                const char* q = source;
                while (*q) {
                    // 跳过注释与字符串
                    if (*q == '/' && *(q+1) == '/') { while (*q && *q != '\n') q++; continue; }
                    if (*q == '/' && *(q+1) == '*') { q += 2; while (*q && !(*q == '*' && *(q+1) == '/')) q++; if (*q) q += 2; continue; }
                    if (*q == '"') { q++; while (*q && *q != '"') { if (*q == '\\' && *(q+1)) q += 2; else q++; } if (*q) q++; continue; }
                    // 匹配关键字（前后边界均为非标识符字符）
                    if (strncmp(q, struct_kinds[kw], kw_len) == 0
                        && (q == source || !(isalnum((unsigned char)q[-1]) || q[-1] == '_'))
                        && (q[kw_len] == ' ' || q[kw_len] == '\t' || q[kw_len] == '\n' || q[kw_len] == '\r')) {
                        const char* after = q + kw_len;
                        while (*after && (*after == ' ' || *after == '\t' || *after == '\n' || *after == '\r')) after++;
                        size_t name_len = strlen(sym->name);
                        if (strncmp(after, sym->name, name_len) == 0
                            && !(isalnum((unsigned char)after[name_len]) || after[name_len] == '_')) {
                            // 名称匹配，向后查找定义体的 '{'（允许中间出现泛型参数 <T> 等）
                            const char* brace = after + name_len;
                            int scan_limit = 256;
                            while (*brace && *brace != '{' && *brace != ';' && scan_limit-- > 0) brace++;
                            if (*brace == '{') def_open = brace;
                        }
                    }
                    if (*q) q++;
                }
            }
            if (!def_open) continue;  // 定义不在本源码中（use 导入等），跳过

            // 统计定义体内 "func " 关键字出现次数（跳过注释与字符串；
            // 只统计 struct 体第一层（depth==1）的方法声明，
            // 排除方法体内的局部函数——Leno 支持 func 作为局部定义）
            int depth = 1;
            const char* q = def_open + 1;
            int source_funcs = 0;
            while (*q && depth > 0) {
                if (*q == '/' && *(q+1) == '/') { while (*q && *q != '\n') q++; continue; }
                if (*q == '/' && *(q+1) == '*') { q += 2; while (*q && !(*q == '*' && *(q+1) == '/')) q++; if (*q) q += 2; continue; }
                if (*q == '"') { q++; while (*q && *q != '"') { if (*q == '\\' && *(q+1)) q += 2; else q++; } if (*q) q++; continue; }
                if (*q == '{') depth++;
                else if (*q == '}') depth--;
                else if (depth == 1
                    && strncmp(q, "func", 4) == 0
                    && (q == source || !(isalnum((unsigned char)q[-1]) || q[-1] == '_'))
                    && (q[4] == ' ' || q[4] == '\t' || q[4] == '\n' || q[4] == '\r')) {
                    source_funcs++;
                }
                q++;
            }
            if (source_funcs != sym->method_count) {
                char check_msg[256];
                snprintf(check_msg, sizeof(check_msg),
                    "内部错误: struct '%s' 方法收集不完整（源码中 %d 个 func 定义，符号表仅收录 %d 个），请报告编译器 bug",
                    sym->name, source_funcs, sym->method_count);
                error_add(ERR_SEMANTIC, mod_source_line(source, def_open), check_msg);
            }
        }
        // face 方法数校验（face 体内方法同样以 "func " 声明）
        for (int fi = 0; fi < table->face_count; fi++) {
            ModuleFaceSymbol* sym = &table->faces[fi];
            const char* def_open = NULL;
            const char* q = source;
            while (*q) {
                if (*q == '/' && *(q+1) == '/') { while (*q && *q != '\n') q++; continue; }
                if (*q == '/' && *(q+1) == '*') { q += 2; while (*q && !(*q == '*' && *(q+1) == '/')) q++; if (*q) q += 2; continue; }
                if (*q == '"') { q++; while (*q && *q != '"') { if (*q == '\\' && *(q+1)) q += 2; else q++; } if (*q) q++; continue; }
                if (strncmp(q, "face", 4) == 0
                    && (q == source || !(isalnum((unsigned char)q[-1]) || q[-1] == '_'))
                    && (q[4] == ' ' || q[4] == '\t' || q[4] == '\n' || q[4] == '\r')) {
                    const char* after = q + 4;
                    while (*after && (*after == ' ' || *after == '\t' || *after == '\n' || *after == '\r')) after++;
                    size_t name_len = strlen(sym->name);
                    if (strncmp(after, sym->name, name_len) == 0
                        && !(isalnum((unsigned char)after[name_len]) || after[name_len] == '_')) {
                        const char* brace = after + name_len;
                        int scan_limit = 256;
                        while (*brace && *brace != '{' && *brace != ';' && scan_limit-- > 0) brace++;
                        if (*brace == '{') def_open = brace;
                    }
                }
                if (*q) q++;
            }
            if (!def_open) continue;

            int depth = 1;
            q = def_open + 1;
            int source_funcs = 0;
            while (*q && depth > 0) {
                if (*q == '/' && *(q+1) == '/') { while (*q && *q != '\n') q++; continue; }
                if (*q == '/' && *(q+1) == '*') { q += 2; while (*q && !(*q == '*' && *(q+1) == '/')) q++; if (*q) q += 2; continue; }
                if (*q == '"') { q++; while (*q && *q != '"') { if (*q == '\\' && *(q+1)) q += 2; else q++; } if (*q) q++; continue; }
                if (*q == '{') depth++;
                else if (*q == '}') depth--;
                else if (depth == 1
                    && strncmp(q, "func", 4) == 0
                    && (q == source || !(isalnum((unsigned char)q[-1]) || q[-1] == '_'))
                    && (q[4] == ' ' || q[4] == '\t' || q[4] == '\n' || q[4] == '\r')) {
                    source_funcs++;
                }
                q++;
            }
            if (source_funcs != sym->method_count) {
                char check_msg[256];
                snprintf(check_msg, sizeof(check_msg),
                    "内部错误: face '%s' 方法收集不完整（源码中 %d 个 func 定义，符号表仅收录 %d 个），请报告编译器 bug",
                    sym->name, source_funcs, sym->method_count);
                error_add(ERR_SEMANTIC, mod_source_line(source, def_open), check_msg);
            }
        }
    }

    // 清理局部别名表、名称数组，返回结果
    #include "scan/scan_cleanup.inc"
