        case AST_TYPE_CHECK:
            if (ast->u.type_check.expr) {
                visit(s, ast->u.type_check.expr);
            }
            if (ast->u.type_check.type && ast->u.type_check.type->kind == TYPE_STRUCT && ast->u.type_check.type->struct_name) {
                Symbol* sym = scope_resolve(s->current, ast->u.type_check.type->struct_name);
                if (sym && sym->type && sym->type->kind == TYPE_FACE) {
                    ast->u.type_check.type->kind = TYPE_FACE;
                } else {
                    // 当前作用域未找到，尝试在导入模块的符号表中查找 face
                    for (int mi = 0; mi < s->imported_module_count; mi++) {
                        if (s->imported_modules[mi].sym_table) {
                            ModuleFaceSymbol* fsym = module_symbol_table_find_face(
                                s->imported_modules[mi].sym_table, ast->u.type_check.type->struct_name);
                            if (fsym) {
                                ast->u.type_check.type->kind = TYPE_FACE;
                                break;
                            }
                        }
                    }
                }
            }
            break;

        case AST_AS_CAST:
            if (ast->u.type_check.expr) {
                visit(s, ast->u.type_check.expr);
            }
            if (ast->u.type_check.type && ast->u.type_check.type->kind == TYPE_STRUCT && ast->u.type_check.type->struct_name) {
                Symbol* sym = scope_resolve(s->current, ast->u.type_check.type->struct_name);
                if (sym && sym->type && sym->type->kind == TYPE_FACE) {
                    ast->u.type_check.type->kind = TYPE_FACE;
                } else {
                    // 当前作用域未找到，尝试在导入模块的符号表中查找 face
                    for (int mi = 0; mi < s->imported_module_count; mi++) {
                        if (s->imported_modules[mi].sym_table) {
                            ModuleFaceSymbol* fsym = module_symbol_table_find_face(
                                s->imported_modules[mi].sym_table, ast->u.type_check.type->struct_name);
                            if (fsym) {
                                ast->u.type_check.type->kind = TYPE_FACE;
                                break;
                            }
                        }
                    }
                }
            }
            break;