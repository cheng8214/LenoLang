        case AST_TYPE_CHECK:
            if (ast->u.type_check.expr) {
                visit(s, ast->u.type_check.expr);
            }
            if (ast->u.type_check.type && ast->u.type_check.type->kind == TYPE_STRUCT && ast->u.type_check.type->struct_name) {
                Symbol* sym = scope_resolve(s->current, ast->u.type_check.type->struct_name);
                if (sym && sym->type && sym->type->kind == TYPE_FACE) {
                    ast->u.type_check.type->kind = TYPE_FACE;
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
                }
            }
            break;