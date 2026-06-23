        case AST_BLOCK: {
            // 第零轮预扫描：提前注册所有 struct 和 face 定义
            // 确保函数体中引用 struct/face 时能通过 struct_def_find/face_def_find 找到
            for (int i = 0; i < ast->u.block.count; i++) {
                Ast* stmt = ast->u.block.items[i];
                if (stmt->kind == AST_STRUCT_DEF) {
                    if (!struct_def_find(stmt->u.struct_def.name)) {
                        ObjStructDef* early_def = struct_def_new(stmt->u.struct_def.name, stmt->u.struct_def.field_count, stmt->u.struct_def.method_count);
                        if (early_def) {
                            early_def->impl_count = stmt->u.struct_def.impl_count;
                            early_def->type_param_count = stmt->u.struct_def.type_param_count;
                            if (early_def->type_param_count > 0 && stmt->u.struct_def.type_params) {
                                early_def->type_param_names = (char**)malloc(sizeof(char*) * early_def->type_param_count);
                                for (int j = 0; j < early_def->type_param_count; j++) {
                                    early_def->type_param_names[j] = strdup(stmt->u.struct_def.type_params[j]);
                                }
                            }
                            if (early_def->impl_count > 0) {
                                early_def->impl_names = (char**)malloc(sizeof(char*) * early_def->impl_count);
                                for (int j = 0; j < early_def->impl_count; j++) {
                                    early_def->impl_names[j] = strdup(stmt->u.struct_def.impl_names[j]);
                                }
                            }
                            for (int j = 0; j < stmt->u.struct_def.method_count; j++) {
                                early_def->methods[j].name = strdup(stmt->u.struct_def.methods[j]->u.func.name);
                            }
                            struct_def_register(early_def);
                        }
                    }
                } else if (stmt->kind == AST_FACE_DEF) {
                    if (!face_def_find(stmt->u.face_def.name)) {
                        ObjFaceDef* early_fdef = face_def_new(stmt->u.face_def.name, stmt->u.face_def.method_count);
                        if (early_fdef) {
                            face_def_set_type_params(early_fdef, stmt->u.face_def.type_param_count,
                                stmt->u.face_def.type_params, stmt->u.face_def.type_param_constraints);
                            for (int j = 0; j < stmt->u.face_def.method_count; j++) {
                                early_fdef->methods[j].name = strdup(stmt->u.face_def.method_names[j]);
                                early_fdef->methods[j].param_count = stmt->u.face_def.method_param_counts[j];
                                if (stmt->u.face_def.method_return_types && stmt->u.face_def.method_return_types[j]) {
                                    early_fdef->methods[j].return_type = type_copy(stmt->u.face_def.method_return_types[j]);
                                    resolve_generic_in_type(early_fdef->methods[j].return_type,
                                        stmt->u.face_def.type_params, stmt->u.face_def.type_param_constraints,
                                        stmt->u.face_def.type_param_count);
                                } else {
                                    early_fdef->methods[j].return_type = NULL;
                                }
                            }
                            face_def_register(early_fdef);
                        }
                    }
                }
            }

            // 预注册所有 cstruct 定义（确保函数参数类型可以正确解析）
            for (int i = 0; i < ast->u.block.count; i++) {
                Ast* stmt = ast->u.block.items[i];
                if (stmt->kind == AST_CSTRUCT_DEF) {
                    if (s->current) {
                        Symbol* existing = scope_resolve_local(s->current, stmt->u.cstruct_def.name);
                        if (!existing) {
                            SymKind kind = (s->current->parent == NULL) ? (s->is_module ? SYM_MODULE : SYM_GLOBAL) : SYM_LOCAL;
                            Symbol* sym = scope_define(s->current, stmt->u.cstruct_def.name, kind);
                            if (sym) {
                                TypeInfo* cstruct_type = type_new(TYPE_CSTRUCT);
                                cstruct_type->struct_name = strdup(stmt->u.cstruct_def.name);
                                sym->type = cstruct_type;
                                stmt->u.cstruct_def.ref.kind = sym->kind;
                                stmt->u.cstruct_def.ref.index = sym->index;
                                stmt->u.cstruct_def.ref.name = strdup(sym->name);
                                stmt->u.cstruct_def.ref.type_kind = TYPE_CSTRUCT;
                            }
                        }
                    }
                }
            }

            // 预注册所有 clib 定义（确保变量类型声明可以正确解析）
            for (int i = 0; i < ast->u.block.count; i++) {
                Ast* stmt = ast->u.block.items[i];
                if (stmt->kind == AST_CLIB_DEF) {
                    if (s->current) {
                        Symbol* existing = scope_resolve_local(s->current, stmt->u.clib_def.name);
                        if (!existing) {
                            Symbol* sym = scope_define(s->current, stmt->u.clib_def.name, SYM_CLIB);
                            if (sym) {
                                TypeInfo* clib_type = type_new(TYPE_CLIB);
                                clib_type->struct_name = strdup(stmt->u.clib_def.name);
                                sym->type = clib_type;
                                stmt->u.clib_def.ref.kind = sym->kind;
                                stmt->u.clib_def.ref.index = sym->index;
                                stmt->u.clib_def.ref.name = strdup(sym->name);
                                stmt->u.clib_def.ref.type_kind = TYPE_CLIB;
                            }
                        }
                    }
                }
            }

            // 预注册所有 cfunc 定义
            for (int i = 0; i < ast->u.block.count; i++) {
                Ast* stmt = ast->u.block.items[i];
                if (stmt->kind == AST_CFUNC_DECL) {
                    if (s->current) {
                        Symbol* existing = scope_resolve_local(s->current, stmt->u.cfunc_decl.name);
                        if (!existing) {
                            Symbol* sym = scope_define(s->current, stmt->u.cfunc_decl.name, SYM_CFUNC);
                            if (sym) {
                                TypeInfo* cfunc_type = type_new(TYPE_CFUNC);
                                cfunc_type->struct_name = strdup(stmt->u.cfunc_decl.name);
                                sym->type = cfunc_type;
                                stmt->u.cfunc_decl.ref.kind = sym->kind;
                                stmt->u.cfunc_decl.ref.index = sym->index;
                                stmt->u.cfunc_decl.ref.name = strdup(sym->name);
                                stmt->u.cfunc_decl.ref.type_kind = TYPE_CFUNC;
                            }
                        }
                    }
                }
            }

            // 第一轮：收集所有命名函数定义
            for (int i = 0; i < ast->u.block.count; i++) {
                Ast* stmt = ast->u.block.items[i];
                if (stmt->kind == AST_FUNC_DEF) {
                    if (s->current) {
                        Symbol* existing = scope_resolve_local(s->current, stmt->u.func.name);
                        if (!existing) {
                            SymKind kind = (s->current->parent == NULL) ? SYM_GLOBAL_FUNC : SYM_LOCAL;
                            Symbol* sym = scope_define(s->current, stmt->u.func.name, kind);
                            if (sym) {
                                stmt->u.func.ref.kind = sym->kind;
                                if (kind == SYM_LOCAL) {
                                    sym->index = allocate_local_index(s);
                                }
                                stmt->u.func.ref.index = sym->index;
                                free(stmt->u.func.ref.name);
                                stmt->u.func.ref.name = strdup(sym->name);
                            }
                        } else {
                            stmt->u.func.ref.kind = existing->kind;
                            stmt->u.func.ref.index = existing->index;
                            free(stmt->u.func.ref.name);
                            stmt->u.func.ref.name = strdup(existing->name);
                        }
                    }
                }
            }
            // 第二轮：转换所有函数的参数类型（struct -> cstruct/face）
            for (int i = 0; i < ast->u.block.count; i++) {
                Ast* stmt = ast->u.block.items[i];
                if (stmt->kind == AST_FUNC_DEF) {
                    // 转换参数类型：struct -> cstruct/face
                    for (int j = 0; j < stmt->u.func.pcnt; j++) {
                        TypeInfo* pt = stmt->u.func.param_types[j];
                        if (pt && pt->kind == TYPE_STRUCT && pt->struct_name) {
                            if (face_def_find(pt->struct_name)) {
                                pt->kind = TYPE_FACE;
                            } else {
                                // 先尝试在当前作用域查找
                                Symbol* struct_def = scope_resolve_local(s->current, pt->struct_name);
                                // 如果找不到，尝试在父作用域查找
                                if (!struct_def && s->current) {
                                    struct_def = scope_resolve(s->current, pt->struct_name);
                                }
                                if (struct_def && struct_def->type && struct_def->type->kind == TYPE_CSTRUCT) {
                                    pt->kind = TYPE_CSTRUCT;
                                }
                            }
                        }
                    }
                    // 转换返回类型
                    if (stmt->u.func.return_type && stmt->u.func.return_type->kind == TYPE_STRUCT &&
                        stmt->u.func.return_type->struct_name) {
                        if (face_def_find(stmt->u.func.return_type->struct_name)) {
                            stmt->u.func.return_type->kind = TYPE_FACE;
                        } else {
                            // 先尝试在当前作用域查找
                            Symbol* struct_def = scope_resolve_local(s->current, stmt->u.func.return_type->struct_name);
                            // 如果找不到，尝试在父作用域查找
                            if (!struct_def && s->current) {
                                struct_def = scope_resolve(s->current, stmt->u.func.return_type->struct_name);
                            }
                            if (struct_def && struct_def->type && struct_def->type->kind == TYPE_CSTRUCT) {
                                stmt->u.func.return_type->kind = TYPE_CSTRUCT;
                            } else if (struct_def && struct_def->type && struct_def->type->kind == TYPE_CLIB) {
                                stmt->u.func.return_type->kind = TYPE_CLIB;
                            }
                        }
                    }
                }
            }

            // 第三轮：设置所有函数的类型（此时所有函数名已注册，可以正确解析相互引用）
            for (int i = 0; i < ast->u.block.count; i++) {
                Ast* stmt = ast->u.block.items[i];
                if (stmt->kind == AST_FUNC_DEF) {
                    if (s->current) {
                        Symbol* sym = scope_resolve_local(s->current, stmt->u.func.name);
                        if (sym && !sym->type) {
                            // 预扫描时设置函数类型（用于类型检查）
                            TypeInfo* return_type = (stmt->u.func.return_type && stmt->u.func.return_type->kind != TYPE_INFER)
                                ? type_copy(stmt->u.func.return_type) : NULL;
                            TypeInfo** param_types = NULL;
                            if (stmt->u.func.pcnt > 0) {
                                param_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * stmt->u.func.pcnt);
                                for (int j = 0; j < stmt->u.func.pcnt; j++) {
                                    param_types[j] = (stmt->u.func.param_types[j] && stmt->u.func.param_types[j]->kind != TYPE_INFER)
                                        ? type_copy(stmt->u.func.param_types[j]) : NULL;
                                }
                            }
                            sym->type = type_function(return_type, param_types, stmt->u.func.pcnt);
                            if (param_types) {
                                for (int j = 0; j < stmt->u.func.pcnt; j++) {
                                    if (param_types[j]) type_free(param_types[j]);
                                }
                                free(param_types);
                            }
                        }
                    }
                }
            }
            // 然后正常处理所有语句
            visit_list(s, &ast->u.block);
            break;
        }