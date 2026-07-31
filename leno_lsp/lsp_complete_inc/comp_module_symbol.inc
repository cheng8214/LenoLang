// 从导入的 .leno 模块获取补全项（使用编译器获取完整类型）
// module_alias: 模块别名（如 "color_module"）
// member_name: 可选的成员名（如 "Color"），用于补全 enum/struct 的成员
// current_file: 当前文件的完整路径，用于解析相对路径的模块
static void add_module_symbol_completions(const char* content, LspCompletionItem** items, int* count, int* capacity,
                                          const char* module_alias, const char* member_name,
                                          ImportAlias* import_aliases, int import_count,
                                          const char* current_file, const char* prefix) {
    (void)content;
    const char* module_path = find_module_path_by_alias(import_aliases, import_count, module_alias);
    if (!module_path) return;

    char* module_source = read_module_file(module_path, current_file);
    if (!module_source) {
        return;
    }

    CompilerContext ctx;
    compiler_context_init(&ctx);
    bool analyzed = compiler_analyze_with_filename(&ctx, module_source, current_file);

    if (!analyzed || !ctx.root_scope) {
        free(module_source);
        compiler_context_cleanup(&ctx);
        return;
    }

    if (member_name && *member_name) {
        Symbol* sym = scope_resolve_tree_bfs(ctx.root_scope, member_name);
        bool is_type_symbol = (sym && sym->kind == SYM_TYPE) ||
                              (sym && sym->kind == SYM_MODULE && sym->type &&
                               (sym->type->kind == TYPE_STRUCT || sym->type->kind == TYPE_ENUM ||
                                sym->type->kind == TYPE_FACE || sym->type->kind == TYPE_CLIB ||
                                sym->type->kind == TYPE_CFUNC));
        if (is_type_symbol) {
            if (sym->type && sym->type->kind == TYPE_ENUM) {
                char detail[256];
                snprintf(detail, sizeof(detail), "%s: enum", member_name);
                add_completion_item(items, count, capacity,
                                   member_name,
                                   LSP_COMP_ENUM,
                                   detail,
                                   prefix);
            } else if (sym->type && sym->type->kind == TYPE_STRUCT) {
                if (sym->struct_field_names && sym->struct_field_types) {
                    for (int i = 0; i < sym->struct_field_count; i++) {
                        const char* type_str = type_to_string(sym->struct_field_types[i]);
                        char detail[256];
                        snprintf(detail, sizeof(detail), "%s.%s: %s", member_name, sym->struct_field_names[i], type_str);

                        add_completion_item(items, count, capacity,
                                           sym->struct_field_names[i],
                                           LSP_COMP_FIELD,
                                           detail,
                                           prefix);
                    }
                }
                add_module_struct_methods(module_source, member_name, module_alias, items, count, capacity);
            }
        }
    } else {
        for (int i = 0; i < ctx.root_scope->sym_cnt; i++) {
            Symbol* sym = ctx.root_scope->syms[i];

            if (sym->scope != ctx.root_scope) {
                continue;
            }

            const char* type_str = type_to_string(sym->type);
            char detail[256];

            switch (sym->kind) {
                case SYM_GLOBAL_FUNC:
                    snprintf(detail, sizeof(detail), "%s.%s() -> %s", module_alias, sym->name, type_str);
                    add_completion_item(items, count, capacity,
                                       sym->name,
                                       LSP_COMP_FUNCTION,
                                       detail,
                                       prefix);
                    break;
                case SYM_TYPE:
                    if (sym->type && sym->type->kind == TYPE_STRUCT) {
                        snprintf(detail, sizeof(detail), "%s.%s: struct", module_alias, sym->name);
                        add_completion_item(items, count, capacity,
                                           sym->name,
                                           LSP_COMP_STRUCT,
                                           detail,
                                           prefix);
                    } else if (sym->type && sym->type->kind == TYPE_ENUM) {
                        snprintf(detail, sizeof(detail), "%s.%s: enum", module_alias, sym->name);
                        add_completion_item(items, count, capacity,
                                           sym->name,
                                           LSP_COMP_ENUM,
                                           detail,
                                           prefix);
                    } else if (sym->type && sym->type->kind == TYPE_FACE) {
                        snprintf(detail, sizeof(detail), "%s.%s: face", module_alias, sym->name);
                        add_completion_item(items, count, capacity,
                                           sym->name,
                                           LSP_COMP_INTERFACE,
                                           detail,
                                           prefix);
                    } else if (sym->type && sym->type->kind == TYPE_CLIB) {
                        snprintf(detail, sizeof(detail), "%s.%s: clib", module_alias, sym->name);
                        add_completion_item(items, count, capacity,
                                           sym->name,
                                           LSP_COMP_CLASS,
                                           detail,
                                           prefix);
                    }
                    break;
                case SYM_CLIB:
                    snprintf(detail, sizeof(detail), "%s.%s: clib", module_alias, sym->name);
                    add_completion_item(items, count, capacity,
                                       sym->name,
                                       LSP_COMP_CLASS,
                                       detail,
                                       prefix);
                    break;
                case SYM_GLOBAL:
                case SYM_MODULE:
                    snprintf(detail, sizeof(detail), "%s.%s: %s", module_alias, sym->name, type_str);
                    add_completion_item(items, count, capacity,
                                       sym->name,
                                       LSP_COMP_VARIABLE,
                                       detail,
                                       prefix);
                    break;
                default:
                    break;
            }
        }
    }

    free(module_source);
    compiler_context_cleanup(&ctx);
}
