        case AST_VAR_DECL: {
            // 检查变量声明类型是否使用了 C 布局类型（i32/u8/f32 等）
            // C 布局类型只能在 clib 声明、cstruct 字段、Ptr[T] 中使用
            if (ast->u.var_decl.type && is_c_layout_type(ast->u.var_decl.type->kind)) {
                char msg[BUFFER_MEDIUM];
                const char* type_str = type_kind_to_string(ast->u.var_decl.type->kind);
                snprintf(msg, sizeof(msg), "C 布局类型 '%s' 不能用于变量声明，请使用 Leno 类型（如 int/float/string）", type_str);
                error_add(ERR_SEMANTIC, ast->line, msg);
            }
            // 检查 Ptr[T] 中的 T 是否是 C 布局类型（Ptr[u8] 是合法的）
            // 注意：Ptr[T] 本身是合法的，不检查

            // 在当前作用域定义变量
            SymKind kind;
            if (s->current && s->current->parent == NULL) {
                // 根作用域：模块模式下使用 SYM_MODULE，否则使用 SYM_GLOBAL
                kind = s->is_module ? SYM_MODULE : SYM_GLOBAL;
            } else {
                kind = SYM_LOCAL;
            }
            // 检查是否已被预注册（全局变量前向引用预扫描）
            Symbol* sym = scope_resolve_local(s->current, ast->u.var_decl.name);
            if (sym && (sym->kind == SYM_GLOBAL || sym->kind == SYM_MODULE) &&
                (kind == SYM_GLOBAL || kind == SYM_MODULE)) {
                // 已被预注册，更新类型和 ref
                ast->u.var_decl.ref.kind = sym->kind;
                ast->u.var_decl.ref.index = sym->index;
                if (!ast->u.var_decl.ref.name) {
                    ast->u.var_decl.ref.name = strdup(sym->name);
                }
            } else {
                sym = scope_define(s->current, ast->u.var_decl.name, kind);
            }
            if (sym) {
                // 设置 const 标志
                if (ast->u.var_decl.is_const) {
                    sym->is_const = 1;
                }
                // 局部变量使用allocate_local_index分配索引，与函数local_count保持一致
                if (kind == SYM_LOCAL) {
                    sym->index = allocate_local_index(s);
                }
                // 检查是否在循环作用域内
                Scope* check_scope = s->current;
                while (check_scope && !check_scope->is_func) {
                    if (check_scope->is_loop) {
                        sym->is_in_loop = 1;
                        break;
                    }
                    check_scope = check_scope->parent;
                }
                ast->u.var_decl.ref.kind = sym->kind;
                ast->u.var_decl.ref.index = sym->index;
                ast->u.var_decl.ref.name = strdup(sym->name);
            } else {
                // 重复定义错误 - 报告准确的行号
                char msg[BUFFER_MEDIUM];
                snprintf(msg, sizeof(msg), "变量 '%s' 重复定义", ast->u.var_decl.name);
                error_add(ERR_DUPLICATE_VAR, ast->line, msg);
                break;
            }

            if (ast->u.var_decl.init) {
                // 在访问初始化表达式前临时设置声明的类型，以便初始化表达式中引用自身时能获得正确类型
                // 保存原始 sym->type，visit 后恢复以避免后续代码覆盖时内存泄漏
                TypeInfo* saved_type = sym ? sym->type : NULL;
                if (sym && ast->u.var_decl.type && ast->u.var_decl.type->kind != TYPE_INFER) {
                    sym->type = type_copy(ast->u.var_decl.type);
                }
                // 标记变量正在初始化，防止初始化表达式中引用自身
                if (sym) sym->is_initialized = 2;
                visit(s, ast->u.var_decl.init);
                // 标记变量已初始化
                if (sym) sym->is_initialized = 1;
                // 恢复原始 type（后续类型推断会重新设置正确的类型）
                if (sym && ast->u.var_decl.type && ast->u.var_decl.type->kind != TYPE_INFER) {
                    type_free(sym->type);
                    sym->type = saved_type;
                }

                // 递归解析变量声明类型中的 alias（如 Array[EventHandler] 中 EventHandler = func(Event):bool）
                resolve_alias_in_type(s, &ast->u.var_decl.type, ast->line);

                // 检查 var 是否被用作类型参数（如 Array[var]、Dict[string, var]）
                // var 只能用于变量声明的顶层类型（var x = ...），不能作为复合类型的类型参数
                int infer_as_param = 0;
                if (ast->u.var_decl.type && ast->u.var_decl.type->kind != TYPE_INFER) {
                    TypeKind parent_kind = TYPE_UNKNOWN;
                    if (type_has_infer_as_param(ast->u.var_decl.type, &parent_kind)) {
                        char msg[BUFFER_MEDIUM];
                        const char* parent_name = type_kind_to_string(parent_kind);
                        snprintf(msg, sizeof(msg), "var 不能用作类型参数，请改用 any（如 %s[any]）", parent_name);
                        error_add(ERR_SEMANTIC, ast->line, msg);
                        infer_as_param = 1;
                    }
                }

                // 检查声明的类型是否存在（对于自定义 struct/cstruct/clib/alias 类型）
                int declared_type_undefined = 0;  // 标记声明类型是否未定义（用于跳过后续级联类型检查）
                if ((ast->u.var_decl.type->kind == TYPE_STRUCT || ast->u.var_decl.type->kind == TYPE_CLIB) &&
                    ast->u.var_decl.type->struct_name) {
                    const char* type_name = ast->u.var_decl.type->struct_name;
                    if (face_def_find(type_name)) {
                        ast->u.var_decl.type->kind = TYPE_FACE;
                    } else {
                        Symbol* struct_def = scope_resolve(s->current, type_name);
                        if (!struct_def) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "未定义的类型: %s（请检查是否已通过 use 语句导入该类型，如 use module.%s）", type_name, type_name);
                            error_add(ERR_UNDEFINED_VAR, ast->line, msg);
                            declared_type_undefined = 1;
                        } else if (struct_def->kind == SYM_TYPE && struct_def->type) {
                            // 类型是 alias（如 FSize = Dict[string,float]），解析为实际类型
                            type_free(ast->u.var_decl.type);
                            ast->u.var_decl.type = type_copy(struct_def->type);
                        } else if (struct_def->type && struct_def->type->kind == TYPE_CLIB) {
                            // 如果是 clib 类型，更新为 TYPE_CLIB
                            ast->u.var_decl.type->kind = TYPE_CLIB;
                        } else if (struct_def->type && struct_def->type->kind == TYPE_CSTRUCT) {
                            // 如果实际是 cstruct，更新类型为 TYPE_CSTRUCT
                            ast->u.var_decl.type->kind = TYPE_CSTRUCT;
                        } else if (!struct_def->type ||
                                   (struct_def->type->kind != TYPE_STRUCT &&
                                    struct_def->type->kind != TYPE_FACE)) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "未定义的类型: %s（请检查是否已通过 use 语句导入该类型，如 use module.%s）", type_name, type_name);
                            error_add(ERR_UNDEFINED_VAR, ast->line, msg);
                            declared_type_undefined = 1;
                        }
                    }
                } else if (ast->u.var_decl.type && ast->u.var_decl.type->kind == TYPE_STRUCT &&
                           ast->u.var_decl.type->struct_name) {
                    // 保持原有的 struct 检查路径（当 type->kind 明确是 TYPE_STRUCT 时）
                    const char* type_name = ast->u.var_decl.type->struct_name;
                    if (face_def_find(type_name)) {
                        ast->u.var_decl.type->kind = TYPE_FACE;
                    } else {
                        Symbol* struct_def = scope_resolve(s->current, type_name);
                        if (!struct_def || !struct_def->type ||
                            (struct_def->type->kind != TYPE_STRUCT &&
                             struct_def->type->kind != TYPE_CSTRUCT &&
                             struct_def->type->kind != TYPE_FACE)) {
                            // 检查是否是 alias
                            if (struct_def && struct_def->kind == SYM_TYPE && struct_def->type) {
                                type_free(ast->u.var_decl.type);
                                ast->u.var_decl.type = type_copy(struct_def->type);
                            } else {
                                char msg[BUFFER_MEDIUM];
                                snprintf(msg, sizeof(msg), "未定义的类型: %s（请检查是否已通过 use 语句导入该类型，如 use module.%s）", type_name, type_name);
                                error_add(ERR_UNDEFINED_VAR, ast->line, msg);
                                declared_type_undefined = 1;
                            }
                        } else if (struct_def && struct_def->type->kind == TYPE_CSTRUCT) {
                            // 如果实际是 cstruct，更新类型为 TYPE_CSTRUCT
                            ast->u.var_decl.type->kind = TYPE_CSTRUCT;
                        }
                    }
                }

                TypeInfo* init_type = infer_expr_type(s, ast->u.var_decl.init);
                if (init_type) {
                    if (ast->u.var_decl.type && ast->u.var_decl.type->kind != TYPE_INFER && !infer_as_param) {
                        // ★ 如果声明类型已报告未定义，跳过类型不匹配检查（避免级联错误），
                        //   并用初始化值的实际类型作为变量类型（让后续方法调用能正常解析）
                        if (declared_type_undefined) {
                            sym->type = type_copy(init_type);
                            // init_type 由下方统一的 type_free(init_type) 释放
                        } else if (!type_is_compatible(ast->u.var_decl.type, init_type)) {
                            char msg[BUFFER_MEDIUM];
                            char context[256];
                            snprintf(context, sizeof(context), "变量 '%s' 声明类型与初始化值类型不匹配",
                                     ast->u.var_decl.name);
                            format_detailed_type_error(msg, sizeof(msg),
                                ast->u.var_decl.type, init_type, context);
                            // 如果是 struct 未实现 face，追加提示
                            if (ast->u.var_decl.type->kind == TYPE_FACE && init_type->kind == TYPE_STRUCT
                                && ast->u.var_decl.type->struct_name && init_type->struct_name) {
                                char hint[256];
                                snprintf(hint, sizeof(hint), " (struct '%s' 未实现 face '%s'，请添加 impl: struct %s impl %s { ... })",
                                    init_type->struct_name, ast->u.var_decl.type->struct_name,
                                    init_type->struct_name, ast->u.var_decl.type->struct_name);
                                strncat(msg, hint, sizeof(msg) - strlen(msg) - 1);
                            }
                            error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                            sym->type = type_copy(ast->u.var_decl.type);
                        } else {
                            // 检查是否需要更新类型（如 Dict -> Dict[string, any]）
                            int need_update = 0;
                            if (ast->u.var_decl.type->kind == TYPE_ARRAY && 
                                !ast->u.var_decl.type->element_type &&
                                init_type->element_type) {
                                need_update = 1;
                            }
                            if (ast->u.var_decl.type->kind == TYPE_DICT && 
                                !ast->u.var_decl.type->key_type &&
                                init_type->key_type) {
                                need_update = 1;
                            }
                            if (need_update) {
                                sym->type = type_copy(init_type); // 使用具体的推断类型
                            } else {
                                sym->type = type_copy(ast->u.var_decl.type);
                            }
                        }
                        type_free(init_type);
                    } else {
                        // var 声明：如果初始化值是 null，将变量类型设为 any（允许后续赋任何值）
                        if (init_type->kind == TYPE_NULL) {
                            type_free(init_type);
                            sym->type = type_new(TYPE_ANY);
                        } else {
                            sym->type = init_type; // 使用推断的类型
                        }
                    }
                }

                if (ast->u.var_decl.init->kind == AST_DICT && sym) {
                    for (int i = 0; i < ast->u.var_decl.init->u.dict.count; i++) {
                        Ast* key_ast = ast->u.var_decl.init->u.dict.entries[i].key;
                        if (key_ast->kind == AST_STRING) {
                            symbol_add_dict_key(sym, key_ast->u.string.value);
                        }
                    }
                }

                // 如果变量类型是 struct 或 cstruct，复制 struct/cstruct 定义的字段信息
                if (sym && sym->type && (sym->type->kind == TYPE_STRUCT || sym->type->kind == TYPE_CSTRUCT)) {
                    // 从初始化表达式获取类型名称
                    const char* type_name = NULL;
                    if (ast->u.var_decl.init->kind == AST_STRUCT_INIT) {
                        type_name = ast->u.var_decl.init->u.struct_init.struct_name;
                    } else if (sym->type->struct_name) {
                        // 对于 cstruct，从类型信息中获取名称
                        type_name = sym->type->struct_name;
                    }
                    
                    if (type_name) {
                        Symbol* type_def_sym = scope_resolve(s->current, type_name);
                        if (type_def_sym && type_def_sym->struct_field_count > 0) {
                            // 设置符号的类型名称
                            sym->struct_type_name = strdup(type_name);
                            // 同时设置类型信息的 struct 名称（供字段索引优化使用）
                            if (!sym->type->struct_name) {
                                sym->type->struct_name = strdup(type_name);
                            }
                            sym->struct_field_count = type_def_sym->struct_field_count;
                            sym->struct_field_names = (char**)malloc(sizeof(char*) * type_def_sym->struct_field_count);
                            sym->struct_field_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * type_def_sym->struct_field_count);
                            for (int i = 0; i < type_def_sym->struct_field_count; i++) {
                                sym->struct_field_names[i] = strdup(type_def_sym->struct_field_names[i]);
                                sym->struct_field_types[i] = type_copy(type_def_sym->struct_field_types[i]);
                            }
                        }
                    }
                }
            } else {
                // 变量没有初始化，使用声明的类型或 ANY
                if (sym) sym->is_initialized = 1;
                if (ast->u.var_decl.type && ast->u.var_decl.type->kind != TYPE_INFER) {
                    // 递归解析变量声明类型中的 alias / struct→face（与有初始化的路径保持一致）
                    // 修复：无初始化值时也需修正 func(Widget,...) 等复合类型参数中的 struct→face
                    resolve_alias_in_type(s, &ast->u.var_decl.type, ast->line);
                    sym->type = type_copy(ast->u.var_decl.type);

                    // 检查自定义 struct/cstruct/alias 类型是否已定义
                    if (sym->type->kind == TYPE_STRUCT && sym->type->struct_name) {
                        Symbol* struct_def = scope_resolve(s->current, sym->type->struct_name);
                        if (struct_def && struct_def->kind == SYM_TYPE && struct_def->type) {
                            // 类型是 alias（如 FSize = Dict[string,float]），解析为实际类型
                            type_free(sym->type);
                            sym->type = type_copy(struct_def->type);
                        } else if (struct_def && struct_def->type && struct_def->type->kind == TYPE_CSTRUCT) {
                            // 如果实际是 cstruct，更新类型为 TYPE_CSTRUCT
                            sym->type->kind = TYPE_CSTRUCT;
                        } else if (!struct_def || !struct_def->type ||
                                   (struct_def->type->kind != TYPE_STRUCT &&
                                    struct_def->type->kind != TYPE_CSTRUCT &&
                                    struct_def->type->kind != TYPE_FACE)) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "未定义的类型: %s（请检查是否已通过 use 语句导入该类型，如 use module.%s）",
                                     sym->type->struct_name, sym->type->struct_name);
                            error_add(ERR_UNDEFINED_VAR, ast->line, msg);
                        }
                    }
                } else {
                    sym->type = type_new(TYPE_ANY);
                }
            }

            if (sym) {
                ast->u.var_decl.ref.type_kind = sym->type ? sym->type->kind : TYPE_ANY;
                if (sym->type && sym->type->struct_name) {
                    free(ast->u.var_decl.ref.struct_name);
                    ast->u.var_decl.ref.struct_name = strdup(sym->type->struct_name);
                }
            }
            break;
        }
        
        case AST_VAR: {
            Symbol* sym = resolve_variable_with_upvalue(s, ast->u.var.name, &ast->u.var.ref);
            if (!sym) {
                char msg[BUFFER_MEDIUM];
                snprintf(msg, sizeof(msg), "未定义的变量: %s", ast->u.var.name);
                const char* hint = get_similar_name_hint(s->current, ast->u.var.name);
                if (hint[0]) strncat(msg, hint, sizeof(msg) - strlen(msg) - 1);
                error_add(ERR_UNDEFINED_VAR, ast->line, msg);
            } else if (sym->is_initialized == 2) {
                // 变量正在初始化中，不允许引用自身
                char msg[BUFFER_MEDIUM];
                snprintf(msg, sizeof(msg), "变量 '%s' 在初始化前使用", ast->u.var.name);
                error_add(ERR_UNDEFINED_VAR, ast->line, msg);
                // 但仍然设置类型信息（从声明的类型获取），避免后续类型推断产生级联错误
                if (sym->type) {
                    ast->u.var.ref.type_kind = sym->type->kind;
                    if (sym->type->struct_name) {
                        free(ast->u.var.ref.struct_name);
                        ast->u.var.ref.struct_name = strdup(sym->type->struct_name);
                    }
                    if (!ast->cached_type) {
                        ast->cached_type = type_copy(sym->type);
                    }
                }
            } else {
                ast->u.var.ref.type_kind = sym->type ? sym->type->kind : TYPE_ANY;
                if (sym->type && sym->type->struct_name) {
                    free(ast->u.var.ref.struct_name);
                    ast->u.var.ref.struct_name = strdup(sym->type->struct_name);
                }
                if (sym->type && !ast->cached_type) {
                    ast->cached_type = type_copy(sym->type);
                }
            }
            break;
        }
        
        case AST_ASSIGN: {
            // 处理并行赋值：a, b = c, d 或 a, b = [c, d] 或 arr[0], arr[1] = x, y
            int left_count = ast->u.assign.name_count;
            
            // 先访问右侧值
            visit(s, ast->u.assign.value);
            
            // 检查右侧值数量
            // 只有当左侧有多个变量且右侧是数组时，才展开数组作为并行赋值
            int right_count = 1;  // 默认是单个值
            if (left_count > 1 && ast->u.assign.value->kind == AST_ARRAY) {
                right_count = ast->u.assign.value->u.array.count;
            }
            
            // 检查左右数量是否相等
            if (left_count != right_count) {
                char msg[BUFFER_MEDIUM];
                snprintf(msg, sizeof(msg), "并行赋值数量不匹配：左侧 %d 个变量，右侧 %d 个值",
                         left_count, right_count);
                error_add(ERR_SEMANTIC, ast->line, msg);
                break;
            }
            
            // 为每个左侧目标解析符号或访问索引表达式
            for (int i = 0; i < left_count; i++) {
                Ast* target = ast->u.assign.targets[i];
                
                if (target->kind == AST_INDEX) {
                    // 索引表达式：访问索引节点进行语义分析
                    visit(s, target);
                    
                    // 对于索引目标，获取索引对象变量的符号信息
                    // 这用于代码生成时计算临时槽位基址
                    if (target->u.index.obj && target->u.index.obj->kind == AST_VAR) {
                        const char* obj_name = target->u.index.obj->u.var.name;
                        Symbol* sym = resolve_variable_with_upvalue(s, obj_name, &ast->u.assign.refs[i]);
                        if (sym) {
                            ast->u.assign.refs[i].type_kind = sym->type ? sym->type->kind : TYPE_ANY;
                        }
                    }
                } else if (target->kind == AST_FIELD_ACCESS) {
                    // 字段访问：访问字段访问节点进行语义分析
                    visit(s, target);
                } else if (target->kind == AST_MODULE_ACCESS) {
                    // 模块访问（可能是 struct/cstruct 字段访问）：访问节点进行语义分析
                    // 这会触发转换为 AST_FIELD_ACCESS 或 AST_INDEX
                    visit(s, target);
                } else if (target->kind == AST_VAR && ast->u.assign.names[i]) {
                    // 简单变量：解析符号
                    Symbol* sym = resolve_variable_with_upvalue(s, ast->u.assign.names[i], &ast->u.assign.refs[i]);
                    if (!sym) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "未定义的变量: %s", ast->u.assign.names[i]);
                        const char* hint = get_similar_name_hint(s->current, ast->u.assign.names[i]);
                        if (hint[0]) strncat(msg, hint, sizeof(msg) - strlen(msg) - 1);
                        error_add(ERR_UNDEFINED_VAR, ast->line, msg);
                    } else {
                        // 检查是否对 const 变量赋值
                        if (sym->is_const) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "不能对 const 变量 '%s' 重新赋值", ast->u.assign.names[i]);
                            error_add(ERR_SEMANTIC, ast->line, msg);
                        }
                        ast->u.assign.refs[i].type_kind = sym->type ? sym->type->kind : TYPE_ANY;
                    }
                }
            }

            // 类型检查（对简单变量和索引赋值）
            for (int i = 0; i < left_count; i++) {
                Ast* target = ast->u.assign.targets[i];
                
                // 获取对应的右侧值类型
                TypeInfo* value_type = NULL;
                if (left_count > 1 && ast->u.assign.value->kind == AST_ARRAY && i < ast->u.assign.value->u.array.count) {
                    value_type = infer_expr_type(s, ast->u.assign.value->u.array.items[i]);
                } else {
                    value_type = infer_expr_type(s, ast->u.assign.value);
                }
                
                if (target->kind == AST_VAR && ast->u.assign.names[i]) {
                    // 简单变量的类型检查
                    Symbol* sym = scope_resolve(s->current, ast->u.assign.names[i]);
                    if (sym && sym->type) {
                        if (value_type) {
                            if (!type_is_compatible(sym->type, value_type)) {
                                char msg[BUFFER_MEDIUM];
                                format_detailed_type_error_ex(msg, sizeof(msg),
                                    sym->type, value_type,
                                    "无法将值赋值给变量", ast->u.assign.names[i]);
                                error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                            } else {
                                // var 声明的变量（类型为 any），第一次赋非 null 值时锁定类型
                                if (sym->type->kind == TYPE_ANY && value_type->kind != TYPE_NULL) {
                                    type_free(sym->type);
                                    sym->type = type_copy(value_type);
                                    ast->u.assign.refs[i].type_kind = sym->type->kind;
                                }
                            }
                        }
                    }
                } else if (target->kind == AST_INDEX) {
                    // 数组或字典索引赋值的类型检查
                    TypeInfo* obj_type = infer_expr_type(s, target->u.index.obj);
                    if (obj_type && obj_type->kind == TYPE_ARRAY) {
                        type_utils_check_array_index_assignment(obj_type, value_type, ast->line);
                    } else if (obj_type && obj_type->kind == TYPE_DICT) {
                        // 字典索引赋值
                        if (target->u.index.obj->kind == AST_VAR) {
                            const char* var_name = target->u.index.obj->u.var.name;
                            Symbol* sym = scope_resolve(s->current, var_name);
                            if (sym) {
                                type_utils_check_dict_index_assignment(sym, value_type, ast->line);
                            }
                        }
                    }
                    if (obj_type) type_free(obj_type);
                } else if (target->kind == AST_FIELD_ACCESS) {
                    // struct/cstruct 字段赋值的类型检查
                    TypeInfo* obj_type = infer_expr_type(s, target->u.field_access.obj);
                    if (obj_type && (obj_type->kind == TYPE_STRUCT || obj_type->kind == TYPE_CSTRUCT) && obj_type->struct_name) {
                        Symbol* struct_sym = scope_resolve(s->current, obj_type->struct_name);
                        if (!struct_sym) {
                            struct_sym = scope_resolve_tree_bfs(s->root_scope, obj_type->struct_name);
                        }
                        if (struct_sym && struct_sym->struct_field_names && struct_sym->struct_field_types && value_type) {
                            const char* field_name = target->u.field_access.field_name;
                            for (int fi = 0; fi < struct_sym->struct_field_count; fi++) {
                                if (strcmp(struct_sym->struct_field_names[fi], field_name) == 0) {
                                    TypeInfo* field_type = struct_sym->struct_field_types[fi];
                                    if (field_type && !type_is_compatible(field_type, value_type)) {
                                        char msg[BUFFER_MEDIUM];
                                        format_detailed_type_error_ex(msg, sizeof(msg),
                                            field_type, value_type,
                                            "字段赋值类型不匹配", field_name);
                                        error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                                    }
                                    break;
                                }
                            }
                        }
                    }
                    if (obj_type) type_free(obj_type);
                }
                
                if (value_type) type_free(value_type);
            }
            break;
        }
        
        case AST_COMPOUND_ASSIGN: {
            Symbol* sym = resolve_variable_with_upvalue(s, ast->u.compound_assign.name, &ast->u.compound_assign.ref);
            if (!sym) {
                // 检查是否是 struct 字段（通过 __self_field__ 标记）
                if (ast->u.compound_assign.ref.name && strcmp(ast->u.compound_assign.ref.name, "__self_field__") == 0) {
                    // struct 字段的复合赋值，标记为已处理
                    // 类型将在后续从 struct 定义推断
                    ast->u.compound_assign.ref.type_kind = TYPE_ANY;
                } else {
                    char msg[BUFFER_MEDIUM];
                    snprintf(msg, sizeof(msg), "未定义的变量: %s", ast->u.compound_assign.name);
                    const char* hint = get_similar_name_hint(s->current, ast->u.compound_assign.name);
                    if (hint[0]) strncat(msg, hint, sizeof(msg) - strlen(msg) - 1);
                    error_add(ERR_UNDEFINED_VAR, ast->line, msg);
                }
            } else {
                // 检查是否对 const 变量复合赋值
                if (sym->is_const) {
                    char msg[BUFFER_MEDIUM];
                    snprintf(msg, sizeof(msg), "不能对 const 变量 '%s' 重新赋值", ast->u.compound_assign.name);
                    error_add(ERR_SEMANTIC, ast->line, msg);
                }
                ast->u.compound_assign.ref.type_kind = sym->type ? sym->type->kind : TYPE_ANY;
            }
            visit(s, ast->u.compound_assign.value);

            // 复合赋值类型检查
            if (sym && sym->type) {
                TypeInfo* value_type = infer_expr_type(s, ast->u.compound_assign.value);
                if (value_type) {
                    if (!type_is_compatible(sym->type, value_type)) {
                        char msg[BUFFER_MEDIUM];
                        format_detailed_type_error_ex(msg, sizeof(msg),
                            sym->type, value_type,
                            "复合赋值类型不匹配", ast->u.compound_assign.name);
                        error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                    }
                    type_free(value_type);
                }
            }
            break;
        }