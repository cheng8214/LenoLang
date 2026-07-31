// 获取补全项
LspCompletionItem* lsp_get_completions(const char* content, LspPosition pos, int* count, const char* file_path) {
    *count = 0;
    
    if (!content) return NULL;
    
    // 解析导入语句，建立别名映射
    int import_count = 0;
    ImportAlias* import_aliases = parse_imports(content, &import_count);
    
    // 获取前缀
    char* prefix = get_word_before_cursor(content, pos);

    // 检查是否是字符串字面量实例方法调用（如 "". 或 "hello".")
    bool is_string_literal = (prefix && strcmp(prefix, "__STRING_LITERAL__") == 0);

    // 检查是否是 "new " 后面（需要补全 struct 名称）
    bool is_new_prefix = false;
    {
        int offset = lsp_position_to_offset(content, pos);
        if (offset > 4) {
            int start = offset - 1;
            while (start >= 0 && isspace((unsigned char)content[start])) start--;
            if (start >= 3 &&
                (content[start-3] == 'n' || content[start-3] == 'N') &&
                (content[start-2] == 'e' || content[start-2] == 'E') &&
                (content[start-1] == 'w' || content[start-1] == 'W') &&
                (start == 3 || !isalnum((unsigned char)content[start-4]))) {
                is_new_prefix = true;
            }
        }
    }

    // 检查是否是模块前缀（如 "maths." 或 "color_module.Color."）
    char* module_alias = NULL;
    char* member_name = NULL;
    bool is_module_prefix = parse_module_prefix(prefix, &module_alias, &member_name);

    char* use_module = NULL;
    char* use_prefix = NULL;
    int use_context = detect_use_context(content, pos, &use_module, &use_prefix);

    // 分配补全数组
    int capacity = 256;
    LspCompletionItem* items = (LspCompletionItem*)malloc(
        sizeof(LspCompletionItem) * capacity);
    if (!items) {
        free(prefix);
        free(module_alias);
        free(member_name);
        free(use_module);
        free(use_prefix);
        free_import_aliases(import_aliases, import_count);
        return NULL;
    }
    
    if (is_new_prefix) {
        // "new " 后补全：列出所有 struct 名称和模块别名
        CompilerContext ctx;
        compiler_context_init(&ctx);
        compiler_analyze_with_filename(&ctx, content, file_path);

        if (ctx.root_scope) {
            for (int i = 0; i < ctx.root_scope->sym_cnt; i++) {
                Symbol* sym = ctx.root_scope->syms[i];
                if (sym->type && sym->type->kind == TYPE_STRUCT) {
                    char detail[256];
                    snprintf(detail, sizeof(detail), "struct %s", sym->name);
                    add_completion_item(&items, count, &capacity,
                                       sym->name,
                                       LSP_COMP_STRUCT,
                                       detail,
                                       prefix);
                }
            }
        }
        compiler_context_cleanup(&ctx);

        // 也列出模块别名（用于 new module.StructName()）
        for (int i = 0; i < import_count; i++) {
            char detail[256];
            snprintf(detail, sizeof(detail), "module %s", import_aliases[i].alias);
            add_completion_item(&items, count, &capacity,
                               import_aliases[i].alias,
                               LSP_COMP_MODULE,
                               detail,
                               prefix);
        }
    } else if (use_context == 1) {
        for (int i = 0; i < import_count; i++) {
            char detail[256];
            snprintf(detail, sizeof(detail), "module %s", import_aliases[i].alias);
            add_completion_item(&items, count, &capacity,
                               import_aliases[i].alias,
                               LSP_COMP_MODULE,
                               detail,
                               use_prefix);
        }
    } else if (use_context == 2) {
        const char* mod_path = find_module_path_by_alias(import_aliases, import_count, use_module);
        bool is_leno_mod = (mod_path != NULL);

        if (is_leno_mod) {
            add_module_symbol_completions(content, &items, count, &capacity,
                                          use_module, NULL,
                                          import_aliases, import_count,
                                          file_path, use_prefix);
        }

        const char* actual_mod = find_module_by_alias(import_aliases, import_count, use_module);
        int mtd_count = 0;
        ModuleMethodMeta* metas = native_get_module_method_metas(actual_mod, &mtd_count);
        if (metas && mtd_count > 0) {
            for (int i = 0; i < mtd_count; i++) {
                char detail[512];
                const char* ret_str = type_kind_to_string(metas[i].return_type);
                if (metas[i].arity == 0) {
                    snprintf(detail, sizeof(detail), "%s.%s() -> %s", use_module, metas[i].method_name, ret_str);
                } else if (metas[i].arity < 0) {
                    snprintf(detail, sizeof(detail), "%s.%s(...) -> %s", use_module, metas[i].method_name, ret_str);
                } else {
                    char params[256] = "";
                    int off = 0;
                    for (int pp = 0; pp < metas[i].arity && pp < MAX_METHOD_PARAMS; pp++) {
                        const char* pt = type_kind_to_string(metas[i].param_types[pp]);
                        if (pp > 0) off += snprintf(params + off, sizeof(params) - off, ", ");
                        off += snprintf(params + off, sizeof(params) - off, "%s", pt);
                    }
                    snprintf(detail, sizeof(detail), "%s.%s(%s) -> %s", use_module, metas[i].method_name, params, ret_str);
                }
                add_completion_item(&items, count, &capacity,
                                   metas[i].method_name,
                                   LSP_COMP_METHOD,
                                   detail,
                                   use_prefix);
            }
            native_free_module_method_metas(metas);
        }

        // 添加模块常量
        {
            int const_count = 0;
            char** consts = native_get_module_consts(actual_mod, &const_count);
            if (consts && const_count > 0) {
                for (int i = 0; i < const_count; i++) {
                    bool found = false;
                    int val = native_find_module_const(actual_mod, consts[i], &found);
                    char detail[256];
                    if (found) {
                        snprintf(detail, sizeof(detail), "%s.%s = %d", use_module, consts[i], val);
                    } else {
                        snprintf(detail, sizeof(detail), "%s.%s", use_module, consts[i]);
                    }
                    add_completion_item(&items, count, &capacity,
                                       consts[i],
                                       LSP_COMP_CONSTANT,
                                       detail,
                                       use_prefix);
                }
                native_free_module_const_list(consts, const_count);
            }
        }
    } else if (is_string_literal) {
        // 字符串字面量实例方法补全
        const char* display_name = "\"\"";  // 显示为 ""
        int method_count = 0;
        char** methods = native_get_instance_methods("string", &method_count);
        
        if (methods && method_count > 0) {
            for (int i = 0; i < method_count; i++) {
                int arity = native_get_instance_method_arity("string", methods[i]);
                TypeKind return_type = native_get_instance_method_return_type("string", methods[i], NULL);
                
                const char* return_type_str = type_kind_to_string(return_type);
                
                char detail[256];
                if (arity == 0) {
                    snprintf(detail, sizeof(detail), "%s.%s() -> %s", display_name, methods[i], return_type_str);
                } else if (arity < 0) {
                    snprintf(detail, sizeof(detail), "%s.%s(...) -> %s", display_name, methods[i], return_type_str);
                } else {
                    snprintf(detail, sizeof(detail), "%s.%s(...) -> %s", display_name, methods[i], return_type_str);
                }
                
                add_completion_item(&items, count, &capacity,
                                   methods[i],
                                   LSP_COMP_METHOD,
                                   detail,
                                   NULL);
            }
            native_free_instance_method_list(methods, method_count);
        }
    } else if (is_module_prefix) {
        if (strcmp(module_alias, "self") == 0) {
            int cursor_offset = lsp_position_to_offset(content, pos);
            char* enclosing_struct = find_enclosing_struct_name(content, cursor_offset);

            if (enclosing_struct) {
                add_current_file_struct_completions(content, &items, count, &capacity, enclosing_struct, "self");

                CompilerContext ctx;
                compiler_context_init(&ctx);
                bool analyze_ok = compiler_analyze_with_filename(&ctx, content, file_path);

                if (analyze_ok || ctx.root_scope) {
                    add_symbol_table_struct_completions(enclosing_struct, "self", &ctx, &items, count, &capacity);
                }
                compiler_context_cleanup(&ctx);

                free(enclosing_struct);
            }
        } else {
        const char* module_path = find_module_path_by_alias(import_aliases, import_count, module_alias);
        bool is_leno_module = (module_path != NULL);

        if (is_leno_module) {
            add_module_symbol_completions(content, &items, count, &capacity,
                                          module_alias, member_name,
                                          import_aliases, import_count,
                                          file_path, NULL);
        } else {
            add_current_file_enum_completions(content, &items, count, &capacity, module_alias);
        }

        // 然后尝试内置模块方法补全
        const char* actual_module = find_module_by_alias(import_aliases, import_count, module_alias);

        // 从编译器获取该模块的所有方法元数据（包含参数类型）
        int method_count = 0;
        ModuleMethodMeta* metas = native_get_module_method_metas(actual_module, &method_count);
        bool has_methods = (metas && method_count > 0);

        if (has_methods) {
            // 是模块方法调用
            for (int i = 0; i < method_count; i++) {
                // 构建详细的签名和文档
                char detail[512];
                char documentation[1024];

                // 获取返回类型字符串
                const char* return_type_str = type_kind_to_string(metas[i].return_type);

                // 构建参数列表
                char params[256] = "";
                if (metas[i].arity == 0) {
                    strcpy(params, "");
                } else if (metas[i].arity < 0) {
                    strcpy(params, "...");
                } else {
                    int offset = 0;
                    for (int p = 0; p < metas[i].arity && p < MAX_METHOD_PARAMS; p++) {
                        const char* param_type = type_kind_to_string(metas[i].param_types[p]);
                        if (p > 0) offset += snprintf(params + offset, sizeof(params) - offset, ", ");
                        offset += snprintf(params + offset, sizeof(params) - offset, "%s", param_type);
                    }
                }

                // 构建显示用的签名 (detail)
                if (metas[i].arity == 0) {
                    snprintf(detail, sizeof(detail), "%s.%s() -> %s", module_alias, metas[i].method_name, return_type_str);
                } else if (metas[i].arity < 0) {
                    snprintf(detail, sizeof(detail), "%s.%s(...) -> %s", module_alias, metas[i].method_name, return_type_str);
                } else {
                    snprintf(detail, sizeof(detail), "%s.%s(%s) -> %s", module_alias, metas[i].method_name, params, return_type_str);
                }

                // 构建文档
                snprintf(documentation, sizeof(documentation), "```leno\n%s\n```", detail);

                add_completion_item(&items, count, &capacity,
                                   metas[i].method_name,
                                   LSP_COMP_METHOD,
                                   detail,
                                   NULL);
            }
            native_free_module_method_metas(metas);
        }

        // 添加模块常量
        {
            int const_count = 0;
            char** consts = native_get_module_consts(actual_module, &const_count);
            if (consts && const_count > 0) {
                for (int i = 0; i < const_count; i++) {
                    bool found = false;
                    int val = native_find_module_const(actual_module, consts[i], &found);
                    char detail[256];
                    if (found) {
                        snprintf(detail, sizeof(detail), "%s.%s = %d", module_alias, consts[i], val);
                    } else {
                        snprintf(detail, sizeof(detail), "%s.%s", module_alias, consts[i]);
                    }
                    add_completion_item(&items, count, &capacity,
                                       consts[i],
                                       LSP_COMP_CONSTANT,
                                       detail,
                                       NULL);
                }
                native_free_module_const_list(consts, const_count);
            }
        }

        if (!has_methods && !is_leno_module) {
            char* var_type = get_variable_type(content, module_alias, file_path);

            if (var_type) {
                bool is_cstruct_type = (strstr(var_type, "cstruct") != NULL);
                bool is_face_type = (strstr(var_type, "face") != NULL);
                if (is_cstruct_type || strstr(var_type, "struct")) {
                    const char* keyword = is_cstruct_type ? "cstruct" : "struct";
                    const char* struct_name = strstr(var_type, keyword);
                    if (struct_name) {
                        struct_name += strlen(keyword);
                        while (*struct_name && isspace((unsigned char)*struct_name)) struct_name++;

                        if (is_cstruct_type && strcmp(module_alias, struct_name) == 0) {
                            for (int i = 0; cstruct_static_methods[i]; i++) {
                                char detail[256];
                                snprintf(detail, sizeof(detail), "%s.%s()", module_alias, cstruct_static_methods[i]);
                                add_completion_item(&items, count, &capacity,
                                                   cstruct_static_methods[i],
                                                   LSP_COMP_METHOD,
                                                   detail,
                                                   NULL);
                            }
                        } else {
                            int items_before = *count;
                            add_current_file_struct_completions(content, &items, count, &capacity, struct_name, module_alias);

                            if (*count == items_before) {
                                CompilerContext ctx;
                                compiler_context_init(&ctx);
                                bool analyze_ok = compiler_analyze_with_filename(&ctx, content, file_path);

                                if (!analyze_ok && !ctx.root_scope) {
                                    char* content_copy = strdup(content);
                                    if (content_copy) {
                                        char* search_pattern = malloc(strlen(module_alias) + 2);
                                        if (search_pattern) {
                                            sprintf(search_pattern, "%s.", module_alias);
                                            char* var_pos = strstr(content_copy, search_pattern);
                                            free(search_pattern);
                                            
                                            if (var_pos) {
                                                char* line_start = content_copy;
                                                char* p = content_copy;
                                                while (p < var_pos) {
                                                    if (*p == '\n') line_start = p + 1;
                                                    p++;
                                                }
                                                char* line_end = strchr(var_pos, '\n');
                                                if (line_end) {
                                                    memmove(line_start, line_end + 1, strlen(line_end + 1) + 1);
                                                    
                                                    compiler_context_cleanup(&ctx);
                                                    compiler_context_init(&ctx);
                                                    analyze_ok = compiler_analyze_with_filename(&ctx, content_copy, file_path);
                                                }
                                            }
                                        }
                                        free(content_copy);
                                    }
                                }
                                
                                if (analyze_ok || ctx.root_scope) {
                                    add_symbol_table_struct_completions(struct_name, module_alias, &ctx, &items, count, &capacity);

                                    for (int i = 0; i < import_count; i++) {
                                        const char* mp = find_module_path_by_alias(import_aliases, import_count, import_aliases[i].alias);
                                        if (mp) {
                                            char* module_source = read_module_file(mp, file_path);
                                            if (module_source) {
                                                add_module_struct_methods(module_source, struct_name, module_alias, &items, count, &capacity);
                                                free(module_source);
                                                break;
                                            }
                                        }
                                    }
                                    
                                    compiler_context_cleanup(&ctx);
                                }
                            }

                            if (*count == items_before) {
                                for (int i = 0; i < import_count; i++) {
                                    const char* mp = find_module_path_by_alias(import_aliases, import_count, import_aliases[i].alias);
                                    if (mp) {
                                        char* module_source = read_module_file(mp, file_path);
                                        if (module_source) {
                                            add_current_file_struct_completions(module_source, &items, count, &capacity, struct_name, module_alias);
                                            free(module_source);
                                            if (*count > items_before) {
                                                break;
                                            }
                                        }
                                    }
                                }
                            }

                            if (is_cstruct_type) {
                                for (int i = 0; cstruct_instance_methods[i]; i++) {
                                    char detail[256];
                                    snprintf(detail, sizeof(detail), "%s.%s()", module_alias, cstruct_instance_methods[i]);
                                    add_completion_item(&items, count, &capacity,
                                                       cstruct_instance_methods[i],
                                                       LSP_COMP_METHOD,
                                                       detail,
                                                       NULL);
                                }
                            } else {
                                char detail[256];
                                snprintf(detail, sizeof(detail), "%s.copy()", module_alias);
                                add_completion_item(&items, count, &capacity,
                                                   "copy",
                                                   LSP_COMP_METHOD,
                                                   detail,
                                                   NULL);
                            }
                        }
                    }
                } else if (is_face_type) {
                    const char* face_name = strstr(var_type, "face");
                    face_name += 4;
                    while (*face_name && isspace((unsigned char)*face_name)) face_name++;

                    int items_before = *count;
                    find_face_methods_in_content(content, face_name, &items, count, &capacity, module_alias);

                    if (*count == items_before) {
                        for (int i = 0; i < import_count; i++) {
                            const char* mp = find_module_path_by_alias(import_aliases, import_count, import_aliases[i].alias);
                            if (mp) {
                                char* module_source = read_module_file(mp, file_path);
                                if (module_source) {
                                    find_face_methods_in_content(module_source, face_name, &items, count, &capacity, module_alias);
                                    free(module_source);
                                    if (*count > items_before) break;
                                }
                            }
                        }
                    }
                } else {
                    // 获取该类型的实例方法
                    int inst_method_count = 0;
                    char** methods = native_get_instance_methods(var_type, &inst_method_count);

                    if (methods && inst_method_count > 0) {
                        for (int i = 0; i < inst_method_count; i++) {
                            // 获取方法元数据
                            int arity = native_get_instance_method_arity(var_type, methods[i]);
                            TypeKind return_type = native_get_instance_method_return_type(var_type, methods[i], NULL);

                            const char* return_type_str = type_kind_to_string(return_type);

                            char detail[256];
                            if (arity == 0) {
                                snprintf(detail, sizeof(detail), "%s.%s() -> %s", module_alias, methods[i], return_type_str);
                            } else if (arity < 0) {
                                snprintf(detail, sizeof(detail), "%s.%s(...) -> %s", module_alias, methods[i], return_type_str);
                            } else {
                                snprintf(detail, sizeof(detail), "%s.%s(...) -> %s", module_alias, methods[i], return_type_str);
                            }

                            add_completion_item(&items, count, &capacity,
                                               methods[i],
                                               LSP_COMP_METHOD,
                                               detail,
                                               NULL);
                        }
                        native_free_instance_method_list(methods, inst_method_count);
                    }
                }
                free(var_type);
            }
        }
        }
    } else if (module_alias) {
        char* var_type = get_variable_type(content, module_alias, file_path);

        if (var_type) {
            bool is_cstruct_var = (strstr(var_type, "cstruct") != NULL);
            bool is_face_var = (strstr(var_type, "face") != NULL);
            if (is_cstruct_var || strstr(var_type, "struct")) {
                const char* keyword = is_cstruct_var ? "cstruct" : "struct";
                const char* struct_name = strstr(var_type, keyword);
                if (struct_name) {
                    struct_name += strlen(keyword);
                    while (*struct_name && isspace((unsigned char)*struct_name)) struct_name++;

                    if (is_cstruct_var && strcmp(module_alias, struct_name) == 0) {
                        for (int i = 0; cstruct_static_methods[i]; i++) {
                            char detail[256];
                            snprintf(detail, sizeof(detail), "%s.%s()", module_alias, cstruct_static_methods[i]);
                            add_completion_item(&items, count, &capacity,
                                               cstruct_static_methods[i],
                                               LSP_COMP_METHOD,
                                               detail,
                                               NULL);
                        }
                    } else {
                        add_current_file_struct_completions(content, &items, count, &capacity, struct_name, module_alias);

                        if (is_cstruct_var) {
                            for (int i = 0; cstruct_instance_methods[i]; i++) {
                                char detail[256];
                                snprintf(detail, sizeof(detail), "%s.%s()", module_alias, cstruct_instance_methods[i]);
                                add_completion_item(&items, count, &capacity,
                                                   cstruct_instance_methods[i],
                                                   LSP_COMP_METHOD,
                                                   detail,
                                                   NULL);
                            }
                        } else {
                            char detail[256];
                            snprintf(detail, sizeof(detail), "%s.copy()", module_alias);
                            add_completion_item(&items, count, &capacity,
                                               "copy",
                                               LSP_COMP_METHOD,
                                               detail,
                                               NULL);
                        }
                    }
                }
            } else if (is_face_var) {
                const char* face_name = strstr(var_type, "face");
                face_name += 4;
                while (*face_name && isspace((unsigned char)*face_name)) face_name++;

                int items_before = *count;
                find_face_methods_in_content(content, face_name, &items, count, &capacity, module_alias);

                if (*count == items_before) {
                    for (int i = 0; i < import_count; i++) {
                        const char* mp = find_module_path_by_alias(import_aliases, import_count, import_aliases[i].alias);
                        if (mp) {
                            char* module_source = read_module_file(mp, file_path);
                            if (module_source) {
                                find_face_methods_in_content(module_source, face_name, &items, count, &capacity, module_alias);
                                free(module_source);
                                if (*count > items_before) break;
                            }
                        }
                    }
                }
            } else {
                // 获取该类型的实例方法
                int method_count = 0;
                char** methods = native_get_instance_methods(var_type, &method_count);

                if (methods && method_count > 0) {
                    for (int i = 0; i < method_count; i++) {
                        // 获取方法元数据
                        int arity = native_get_instance_method_arity(var_type, methods[i]);
                        TypeKind return_type = native_get_instance_method_return_type(var_type, methods[i], NULL);

                        const char* return_type_str = type_kind_to_string(return_type);

                        char detail[256];
                        if (arity == 0) {
                            snprintf(detail, sizeof(detail), "%s.%s() -> %s", module_alias, methods[i], return_type_str);
                        } else if (arity < 0) {
                            snprintf(detail, sizeof(detail), "%s.%s(...) -> %s", module_alias, methods[i], return_type_str);
                        } else {
                            snprintf(detail, sizeof(detail), "%s.%s(...) -> %s", module_alias, methods[i], return_type_str);
                        }

                        add_completion_item(&items, count, &capacity,
                                           methods[i],
                                           LSP_COMP_METHOD,
                                           detail,
                                           NULL);
                    }
                    native_free_instance_method_list(methods, method_count);
                }
            }
            free(var_type);
        }
    } else {
        int type_ctx_offset = lsp_position_to_offset(content, pos);
        if (is_type_annotation_context(content, type_ctx_offset)) {
            for (int i = 0; leno_types[i]; i++) {
                add_completion_item(&items, count, &capacity, leno_types[i],
                                   LSP_COMP_CLASS, "type", prefix);
            }

            add_completion_item(&items, count, &capacity, "var",
                               LSP_COMP_KEYWORD, "type inference", prefix);

            CompilerContext ctx;
            compiler_context_init(&ctx);
            compiler_analyze_with_filename(&ctx, content, file_path);
            if (ctx.root_scope) {
                for (int i = 0; i < ctx.root_scope->sym_cnt; i++) {
                    Symbol* sym = ctx.root_scope->syms[i];
                    if (sym->kind == SYM_TYPE || sym->kind == SYM_STRUCT ||
                        sym->kind == SYM_CSTRUCT || sym->kind == SYM_ENUM ||
                        sym->kind == SYM_CLIB || sym->kind == SYM_CFUNC) {
                        const char* type_label = "type";
                        int comp_kind = LSP_COMP_CLASS;
                        if (sym->type) {
                            switch (sym->type->kind) {
                                case TYPE_STRUCT: type_label = "struct"; comp_kind = LSP_COMP_STRUCT; break;
                                case TYPE_CSTRUCT: type_label = "cstruct"; comp_kind = LSP_COMP_STRUCT; break;
                                case TYPE_ENUM: type_label = "enum"; comp_kind = LSP_COMP_ENUM; break;
                                case TYPE_FACE: type_label = "face"; comp_kind = LSP_COMP_INTERFACE; break;
                                case TYPE_CLIB: type_label = "clib"; comp_kind = LSP_COMP_CLASS; break;
                                case TYPE_CFUNC: type_label = "cfunc"; comp_kind = LSP_COMP_CLASS; break;
                                default: break;
                            }
                        }
                        char detail[256];
                        snprintf(detail, sizeof(detail), "%s %s", type_label, sym->name);
                        add_completion_item(&items, count, &capacity,
                                           sym->name, comp_kind, detail, prefix);
                    }
                }
            }
            compiler_context_cleanup(&ctx);

            for (int i = 0; i < import_count; i++) {
                const char* mp = find_module_path_by_alias(import_aliases, import_count, import_aliases[i].alias);
                if (mp) {
                    char* module_source = read_module_file(mp, file_path);
                    if (module_source) {
                        CompilerContext mctx;
                        compiler_context_init(&mctx);
                        bool analyzed = compiler_analyze_with_filename(&mctx, module_source, file_path);
                        if (analyzed || mctx.root_scope) {
                            for (int j = 0; j < mctx.root_scope->sym_cnt; j++) {
                                Symbol* sym = mctx.root_scope->syms[j];
                                if (sym->kind == SYM_TYPE || sym->kind == SYM_STRUCT ||
                                    sym->kind == SYM_CSTRUCT || sym->kind == SYM_ENUM) {
                                    const char* type_label = "type";
                                    int comp_kind = LSP_COMP_CLASS;
                                    if (sym->type) {
                                        switch (sym->type->kind) {
                                            case TYPE_STRUCT: type_label = "struct"; comp_kind = LSP_COMP_STRUCT; break;
                                            case TYPE_CSTRUCT: type_label = "cstruct"; comp_kind = LSP_COMP_STRUCT; break;
                                            case TYPE_ENUM: type_label = "enum"; comp_kind = LSP_COMP_ENUM; break;
                                            case TYPE_FACE: type_label = "face"; comp_kind = LSP_COMP_INTERFACE; break;
                                            default: break;
                                        }
                                    }
                                    char detail[256];
                                    snprintf(detail, sizeof(detail), "%s.%s: %s", import_aliases[i].alias, sym->name, type_label);
                                    add_completion_item(&items, count, &capacity,
                                                       sym->name, comp_kind, detail, prefix);
                                }
                            }
                        }
                        compiler_context_cleanup(&mctx);
                        free(module_source);
                    }
                }
            }

            for (int i = 0; i < import_count; i++) {
                char detail[256];
                snprintf(detail, sizeof(detail), "module %s", import_aliases[i].alias);
                add_completion_item(&items, count, &capacity,
                                   import_aliases[i].alias,
                                   LSP_COMP_MODULE,
                                   detail,
                                   prefix);
            }
        } else {
        // 正常补全：关键字、类型、内置函数、模块方法、用户符号
        
        // 1. 添加关键字
        for (int i = 0; leno_keywords[i]; i++) {
            add_completion_item(&items, count, &capacity, leno_keywords[i], 
                               LSP_COMP_KEYWORD, "keyword", prefix);
        }

        // 1.5 如果在结构体方法体内，添加 self 关键字补全
        {
            int cursor_offset = lsp_position_to_offset(content, pos);
            char* enclosing_struct = find_enclosing_struct_name(content, cursor_offset);
            if (enclosing_struct) {
                add_completion_item(&items, count, &capacity, "self",
                                   LSP_COMP_KEYWORD, enclosing_struct, prefix);
                free(enclosing_struct);
            }
        }
        
        // 2. 添加类型
        for (int i = 0; leno_types[i]; i++) {
            add_completion_item(&items, count, &capacity, leno_types[i],
                               LSP_COMP_CLASS, "type", prefix);
        }
        
        // 3. 添加内置函数（从元数据表获取）
        for (int i = 0; builtin_functions[i].name != NULL; i++) {
            add_completion_item(&items, count, &capacity, builtin_functions[i].name,
                               LSP_COMP_FUNCTION, builtin_functions[i].signature, prefix);
        }
        
        // 4. 添加所有模块的方法（动态获取）
        // 从编译器获取所有可用模块
        int module_count = 0;
        char** modules = native_get_all_modules(&module_count);
        if (modules && module_count > 0) {
            for (int m = 0; m < module_count; m++) {
                // 模块方法
                int method_count = 0;
                char** methods = native_get_module_methods(modules[m], &method_count);
                if (methods && method_count > 0) {
                    for (int i = 0; i < method_count; i++) {
                        char full_name[256];
                        snprintf(full_name, sizeof(full_name), "%s.%s", modules[m], methods[i]);
                        char signature[256];
                        snprintf(signature, sizeof(signature), "%s.%s(...)", modules[m], methods[i]);
                        add_completion_item(&items, count, &capacity, full_name,
                                           LSP_COMP_METHOD, signature, prefix);
                    }
                    native_free_module_method_list(methods, method_count);
                }
                // 模块常量
                int const_count = 0;
                char** consts = native_get_module_consts(modules[m], &const_count);
                if (consts && const_count > 0) {
                    for (int i = 0; i < const_count; i++) {
                        bool found = false;
                        int val = native_find_module_const(modules[m], consts[i], &found);
                        char full_name[256];
                        snprintf(full_name, sizeof(full_name), "%s.%s", modules[m], consts[i]);
                        char detail[256];
                        if (found) {
                            snprintf(detail, sizeof(detail), "%s.%s = %d", modules[m], consts[i], val);
                        } else {
                            snprintf(detail, sizeof(detail), "%s.%s", modules[m], consts[i]);
                        }
                        add_completion_item(&items, count, &capacity, full_name,
                                           LSP_COMP_CONSTANT, detail, prefix);
                    }
                    native_free_module_const_list(consts, const_count);
                }
            }
            native_free_module_list(modules, module_count);
        }
        
        // 5. 从编译器符号表添加用户定义的符号
        add_symbols_from_compiler(content, &items, count, &capacity, prefix, file_path);
        }
    }
    
    free(prefix);
    free(module_alias);
    free(member_name);
    free(use_module);
    free(use_prefix);
    free_import_aliases(import_aliases, import_count);

    return items;
}

// 释放补全项
void lsp_free_completions(LspCompletionItem* items, int count) {
    if (!items) return;
    
    for (int i = 0; i < count; i++) {
        free(items[i].label);
        free(items[i].detail);
        free(items[i].documentation);
        free(items[i].insertText);
    }
    
    free(items);
}
