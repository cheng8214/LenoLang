        case AST_FIELD_ACCESS:
            ast->u.field_access.field_index = -1;
            if (ast->u.field_access.obj) {
                visit(s, ast->u.field_access.obj);

                // 检查 clib 类型的函数调用
                TypeInfo* obj_type = infer_expr_type(s, ast->u.field_access.obj);
                if (obj_type && obj_type->kind == TYPE_CLIB && obj_type->struct_name) {
                    // 检查 clib 函数是否存在
                    Symbol* clib_sym = scope_resolve(s->current, obj_type->struct_name);
                    if (clib_sym && clib_sym->clib_func_count > 0) {
                        const char* func_name = ast->u.field_access.field_name;
                        int func_found = 0;
                        for (int i = 0; i < clib_sym->clib_func_count; i++) {
                            if (strcmp(clib_sym->clib_func_names[i], func_name) == 0) {
                                func_found = 1;
                                ast->u.field_access.field_index = i;
                                break;
                            }
                        }
                        if (!func_found) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "clib '%s' 没有声明函数 '%s'",
                                     obj_type->struct_name, func_name);
                            error_add(ERR_SEMANTIC, ast->line, msg);
                        }
                    }
                    type_free(obj_type);
                    break;
                }

                // 检查 struct/cstruct 字段是否存在，并设置字段索引
                if (obj_type && (obj_type->kind == TYPE_STRUCT || obj_type->kind == TYPE_CSTRUCT || obj_type->kind == TYPE_FACE)) {
                    const char* field_name = ast->u.field_access.field_name;
                    Symbol* struct_sym = NULL;
                    const char* var_name = NULL;

                    // face 类型限制：只能访问 face 中定义的方法，不能访问底层 struct 的字段
                    if (obj_type->kind == TYPE_FACE && obj_type->struct_name) {
                        // 使用 face_def_find 获取 face 的方法列表（比 scope_resolve 更可靠）
                        ObjFaceDef* fdef = face_def_find(obj_type->struct_name);
                        if (fdef && fdef->method_count > 0) {
                            // 检查字段是否是 face 定义的方法
                            int is_face_method = 0;
                            for (int i = 0; i < fdef->method_count; i++) {
                                if (strcmp(fdef->methods[i].name, field_name) == 0) {
                                    is_face_method = 1;
                                    break;
                                }
                            }
                            if (!is_face_method) {
                                char msg[BUFFER_MEDIUM];
                                snprintf(msg, sizeof(msg), "face '%s' 没有字段或方法 '%s'（使用 'as' 转型访问底层 struct）",
                                         obj_type->struct_name, field_name);
                                error_add(ERR_SEMANTIC, ast->line, msg);
                                type_free(obj_type);
                                break;
                            }
                        }
                    }

                    // 获取变量名（支持嵌套访问）
                    if (ast->u.field_access.obj->kind == AST_VAR) {
                        var_name = ast->u.field_access.obj->u.var.name;
                        struct_sym = scope_resolve(s->current, var_name);
                        if (struct_sym && struct_sym->struct_field_count == 0 && obj_type->struct_name) {
                            struct_sym = scope_resolve(s->current, obj_type->struct_name);
                        }
                    }

                    // 如果找不到符号，尝试通过类型名称查找 struct/cstruct 定义
                    if (!struct_sym && obj_type->struct_name) {
                    struct_sym = scope_resolve(s->current, obj_type->struct_name);
                }

                    // 检查是否是 self["field_name"] 形式（在 struct 方法中）
                    if (!struct_sym && ast->u.field_access.obj->kind == AST_INDEX) {
                        Ast* index_obj = ast->u.field_access.obj->u.index.obj;
                        Ast* index_index = ast->u.field_access.obj->u.index.index;
                        if (index_obj && index_obj->kind == AST_VAR &&
                            strcmp(index_obj->u.var.name, "self") == 0 &&
                            index_index && index_index->kind == AST_STRING) {
                            // 这是 self["field_name"] 形式，查找 self 的类型
                            Symbol* self_sym = scope_resolve(s->current, "self");
                            if (self_sym && self_sym->type && self_sym->type->struct_name) {
                                struct_sym = scope_resolve(s->current, self_sym->type->struct_name);
                            }
                        }
                    }

                    if (struct_sym && struct_sym->struct_field_count > 0) {
                        int field_found = 0;
                        for (int i = 0; i < struct_sym->struct_field_count; i++) {
                            if (strcmp(struct_sym->struct_field_names[i], field_name) == 0) {
                                field_found = 1;
                                ast->u.field_access.field_index = i;
                                break;
                            }
                        }

                        if (!field_found) {
                            char msg[BUFFER_MEDIUM];
                            const char* type_kind_name = (obj_type->kind == TYPE_CSTRUCT) ? "cstruct" : "struct";
                            snprintf(msg, sizeof(msg), "%s 没有字段 '%s'", type_kind_name, field_name);
                            error_add(ERR_SEMANTIC, ast->line, msg);
                        }

                        // 注意：单层字段访问（如 o.inner）不需要检查 null 默认值
                        // 因为获取 null 本身不会运行时出错
                        // 链式访问（如 o.inner.value）的 null 检查在 AST_INDEX 中处理
                    } else if (!struct_sym && obj_type->kind == TYPE_CSTRUCT && obj_type->struct_name) {
                        // 从全局 cstruct 定义表查找（跨模块导入的 cstruct）
                        ObjCStructDef* cdef = cstruct_def_find(obj_type->struct_name);
                        if (cdef) {
                            int idx = cstruct_get_field_index(cdef, field_name);
                            if (idx >= 0) {
                                ast->u.field_access.field_index = idx;
                            } else {
                                char msg[BUFFER_MEDIUM];
                                snprintf(msg, sizeof(msg), "cstruct 没有字段 '%s'", field_name);
                                error_add(ERR_SEMANTIC, ast->line, msg);
                            }
                        } else {
                            // 全局表可能还没注册（语义分析阶段模块尚未编译执行），从导入模块的符号表查找
                            int found = 0;
                            for (int mi = 0; mi < s->imported_module_count && !found; mi++) {
                                ImportedModuleInfo* mod = &s->imported_modules[mi];
                                if (mod->sym_table) {
                                    ModuleStructSymbol* ssym = module_symbol_table_find_struct(mod->sym_table, obj_type->struct_name);
                                    if (ssym && ssym->is_cstruct) {
                                        for (int fi = 0; fi < ssym->field_count; fi++) {
                                            if (strcmp(ssym->fields[fi].name, field_name) == 0) {
                                                ast->u.field_access.field_index = fi;
                                                found = 1;
                                                break;
                                            }
                                        }
                                    }
                                }
                            }
                            if (!found) {
                                char msg[BUFFER_MEDIUM];
                                snprintf(msg, sizeof(msg), "cstruct 没有字段 '%s'", field_name);
                                error_add(ERR_SEMANTIC, ast->line, msg);
                            }
                        }
                    }
                }
                if (obj_type) type_free(obj_type);
            }
            break;

        case AST_ADDRESS_OF:
            // 取地址运算符：&expr，仅支持 cstruct 字段
            if (ast->u.address_of.operand) {
                visit(s, ast->u.address_of.operand);

                // 验证操作数是字段访问
                if (ast->u.address_of.operand->kind != AST_FIELD_ACCESS) {
                    error_add(ERR_SEMANTIC, ast->line, "& 取地址运算符只能用于 cstruct 字段访问（如 &c.r）");
                    break;
                }

                // 验证对象类型是 cstruct
                Ast* field_access = ast->u.address_of.operand;
                TypeInfo* obj_type = infer_expr_type(s, field_access->u.field_access.obj);
                if (obj_type && obj_type->kind != TYPE_CSTRUCT) {
                    error_add(ERR_SEMANTIC, ast->line, "& 取地址运算符仅支持 cstruct 字段（普通 struct 字段无 C 稳定地址）");
                    type_free(obj_type);
                    break;
                }
                if (obj_type) type_free(obj_type);

                // 验证字段索引已解析
                if (field_access->u.field_access.field_index < 0) {
                    error_add(ERR_SEMANTIC, ast->line, "无法确定字段索引，& 取地址失败");
                    break;
                }
            }
            break;