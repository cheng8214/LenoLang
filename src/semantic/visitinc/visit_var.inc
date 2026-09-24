        case AST_VAR_DECL: {
            // 检查变量声明类型是否使用了 C 布局类型（i32/u8/f32 等）
            // C 布局类型只能在 clib 声明、cstruct 字段、Ptr[T] 中使用
            if (ast->u.var_decl.type && is_c_layout_type(ast->u.var_decl.type->kind)) {
                char msg[BUFFER_MEDIUM];
                const char* type_str = type_kind_to_string(ast->u.var_decl.type->kind);
                snprintf(msg, sizeof(msg), "C 布局类型 '%s' 不能用于变量声明，请使用 Leno 类型（如 int/float/string）", type_str);
                error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
            }
            // 检查 any 类型：any 只能用于函数参数和返回类型，不能用于变量声明
            // 变量声明请使用 var（类型推断）或指定具体类型
            if (ast->u.var_decl.type && ast->u.var_decl.type->kind == TYPE_ANY) {
                error_add_at(ERR_SEMANTIC, ast->line, ast->column, "'any' 类型只能用于函数参数和返回类型，变量声明请使用 'var' 或指定具体类型");
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
                error_add_at(ERR_DUPLICATE_VAR, ast->line, ast->column, msg);
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
                // ★ 记「确定为 null」（编译期专用，见 Symbol.is_null_value 的说明）：
                //   声明时初始化为 `null` 字面量 ⇒ 只要此后再没有写操作，用到它时值就确定为 null
                //   （`var a = null; a + 1` / `int? a = null; a + 1` ⇒ 编译错误）。
                //   只对局部量置位：全局/模块级变量的写入可能发生在别的函数里。
                if (sym) {
                    sym->is_null_value = (sym->kind == SYM_LOCAL &&
                                          ast->u.var_decl.init->kind == AST_NULL) ? 1 : 0;
                }
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
                        error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
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
                            if (strcmp(type_name, "double") == 0) {
                                snprintf(msg, sizeof(msg), "未定义的类型: %s（Leno 中使用 float 代替 double，Leno 的 float 是 64 位双精度浮点数）", type_name);
                            } else {
                                snprintf(msg, sizeof(msg), "未定义的类型: %s（请检查是否已通过 use 语句导入该类型，如 use module.%s）", type_name, type_name);
                            }
                            error_add_at(ERR_UNDEFINED_VAR, ast->line, ast->column, msg);
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
                            if (strcmp(type_name, "double") == 0) {
                                snprintf(msg, sizeof(msg), "未定义的类型: %s（Leno 中使用 float 代替 double，Leno 的 float 是 64 位双精度浮点数）", type_name);
                            } else {
                                snprintf(msg, sizeof(msg), "未定义的类型: %s（请检查是否已通过 use 语句导入该类型，如 use module.%s）", type_name, type_name);
                            }
                            error_add_at(ERR_UNDEFINED_VAR, ast->line, ast->column, msg);
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
                                error_add_at(ERR_UNDEFINED_VAR, ast->line, ast->column, msg);
                                declared_type_undefined = 1;
                            }
                        } else if (struct_def && struct_def->type->kind == TYPE_CSTRUCT) {
                            // 如果实际是 cstruct，更新类型为 TYPE_CSTRUCT
                            ast->u.var_decl.type->kind = TYPE_CSTRUCT;
                        }
                    }
                }

                // ★ 递归检查泛型实参/子类型中的未定义类型（B2 修复）：
                //   上面的检查只看顶层类型名（`Array[intt]` 顶层是 Array，合法），
                //   `intt` 藏在 generic_args 里原先无人查 ⇒ 拖到类型不匹配时才报出
                //   误导性的"期望 Array[struct intt]"。这里补上 ⇒ 直接报"未定义的类型"。
                if (!declared_type_undefined && ast->u.var_decl.type &&
                    ast->u.var_decl.type->kind != TYPE_INFER) {
                    if (semantic_check_undefined_subtypes(s, ast->u.var_decl.type, ast->line, ast->column)) {
                        declared_type_undefined = 1;   // 跳过后续级联类型不匹配检查
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
                            // 1024（不是 BUFFER_MEDIUM）：报文尾部还要追加"转换/收窄"提示，
                            // 缓冲太小时提示会被截断（实测「…（如」✗）
                            char msg[1024];
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
                                { size_t _ml = strlen(msg); snprintf(msg + _ml, sizeof(msg) - _ml, "%s", hint); }
                            }
                            error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, msg);
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
                        } else if (init_type->kind == TYPE_MULTI_RET && init_type->param_count > 0) {
                            // 多返回值在非解构上下文中退化为第一个返回值类型
                            // 例如 var ms = measure() 其中 measure 返回 [float, float]
                            // ms 的类型应为 float（codegen 会弹出多余返回值）
                            sym->type = type_copy(init_type->param_types[0]);
                            type_free(init_type);
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
                            error_add_at(ERR_UNDEFINED_VAR, ast->line, ast->column, msg);
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
                error_add_at(ERR_UNDEFINED_VAR, ast->line, ast->column, msg);
            } else if (sym->is_initialized == 2) {
                // 变量正在初始化中，不允许引用自身
                char msg[BUFFER_MEDIUM];
                snprintf(msg, sizeof(msg), "变量 '%s' 在初始化前使用", ast->u.var.name);
                error_add_at(ERR_UNDEFINED_VAR, ast->line, ast->column, msg);
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
                error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
                break;
            }
            
            // 为每个左侧目标解析符号或访问索引表达式
            for (int i = 0; i < left_count; i++) {
                Ast* target = ast->u.assign.targets[i];
                
                if (target->kind == AST_INDEX) {
                    // const 检查必须在 visit 之前，因为 visit 会改变 obj 节点类型
                    if (target->u.index.obj && target->u.index.obj->kind == AST_VAR) {
                        const char* obj_name = target->u.index.obj->u.var.name;
                        SymRef tmp_ref;
                        memset(&tmp_ref, 0, sizeof(tmp_ref));
                        Symbol* sym = resolve_variable_with_upvalue(s, obj_name, &tmp_ref);
                        if (sym && sym->is_const) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "不能对 const 变量 '%s' 的元素重新赋值", obj_name);
                            error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
                        }
                    }
                    // 检查模块变量是否为 const（如 m.arr[0] = 99）
                    if (target->u.index.obj && target->u.index.obj->kind == AST_MODULE_ACCESS) {
                        const char* mod_name = target->u.index.obj->u.module_access.module_name;
                        const char* mem_name = target->u.index.obj->u.module_access.member_name;
                        ImportedModuleInfo* mod_info = find_imported_module(s, mod_name);
                        if (mod_info && mod_info->sym_table) {
                            ModuleVarSymbol* var_sym = module_symbol_table_find_var(mod_info->sym_table, mem_name);
                            if (var_sym && var_sym->is_const) {
                                char msg[BUFFER_MEDIUM];
                                snprintf(msg, sizeof(msg), "不能对 const 变量 '%s.%s' 的元素重新赋值", mod_name, mem_name);
                                error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
                            }
                        }
                    }

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
                    // ★ const 检查（B1 修复）：struct 关联常量在 visit 中会被折叠成字面量
                    //   （AST_NUM 等）——折叠后赋值目标不再是 lvalue，codegen 会静默丢弃
                    //   写入（实测 `Cfg.MAX = 20` 无报错且读回仍为旧值）⇒ 报错拦截。
                    visit(s, target);
                    if (target->kind == AST_NUM || target->kind == AST_STRING ||
                        target->kind == AST_BOOL || target->kind == AST_NULL) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "不能对常量重新赋值");
                        error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
                    }
                } else if (target->kind == AST_MODULE_ACCESS) {
                    // 模块访问（可能是 struct/cstruct 字段访问）：访问节点进行语义分析
                    // 这会触发转换为 AST_FIELD_ACCESS 或 AST_INDEX
                    // ★ const 检查（B1 修复）：同上——`Cfg.MAX = 20` 这类目标在 visit 中
                    //   被折叠成字面量 ⇒ 非法赋值，visit 后检测并报错（保留原名提示）。
                    char orig_buf[BUFFER_SMALL];
                    snprintf(orig_buf, sizeof(orig_buf), "%s.%s",
                             target->u.module_access.module_name,
                             target->u.module_access.member_name);
                    visit(s, target);
                    if (target->kind == AST_NUM || target->kind == AST_STRING ||
                        target->kind == AST_BOOL || target->kind == AST_NULL) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "不能对常量 '%s' 重新赋值", orig_buf);
                        error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
                    }
                } else if (target->kind == AST_VAR && ast->u.assign.names[i]) {
                    // 简单变量：解析符号
                    Symbol* sym = resolve_variable_with_upvalue(s, ast->u.assign.names[i], &ast->u.assign.refs[i]);
                    if (!sym) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "未定义的变量: %s", ast->u.assign.names[i]);
                        const char* hint = get_similar_name_hint(s->current, ast->u.assign.names[i]);
                        if (hint[0]) strncat(msg, hint, sizeof(msg) - strlen(msg) - 1);
                        error_add_at(ERR_UNDEFINED_VAR, ast->line, ast->column, msg);
                    } else {
                        // 检查是否对 const 变量赋值
                        if (sym->is_const) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "不能对 const 变量 '%s' 重新赋值", ast->u.assign.names[i]);
                            error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
                        }
                        // ★ 清「确定为 null」标志（T11）：这个变量被写过了 ⇒ 之后不能再断定它是 null。
                        //   ⚠ 所有写变量的地方都要清（`a = v` / `a += v` / for 的迭代变量…）——
                        //     漏一处就会把"本来能跑的程序"判成编译错误（误报比漏报严重得多）。
                        //     目前写点：本处、AST_COMPOUND_ASSIGN（本文件）、
                        //     `var` 声明（新符号，天然为 0）、for 迭代变量（作用域内新符号，天然为 0）。
                        sym->is_null_value = 0;
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
                                error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, msg);
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
                        type_utils_check_array_index_assignment(obj_type, value_type, ast->line, ast->column);
                    } else if (obj_type && obj_type->kind == TYPE_DICT) {
                        // 字典索引赋值
                        if (target->u.index.obj->kind == AST_VAR) {
                            const char* var_name = target->u.index.obj->u.var.name;
                            Symbol* sym = scope_resolve(s->current, var_name);
                            if (sym) {
                                type_utils_check_dict_index_assignment(sym, value_type, ast->line, ast->column);
                            }
                        }
                    } else if (obj_type && (obj_type->kind == TYPE_STRUCT || obj_type->kind == TYPE_CSTRUCT) &&
                               target->u.index.index && target->u.index.index->kind == AST_STRING) {
                        // ---- struct / cstruct 的"索引形式"字段赋值（`pa["x"] = v`）此前**完全不检查** ----
                        // 这条路径与 `pa.x = v`（AST_FIELD_ACCESS）是**两个**分支：解析器对
                        // `AST_INDEX` 左值建的是 AST_ASSIGN + INDEX 目标（parser_expr.c:1138），
                        // 而这里只认 ARRAY / DICT ⇒ `pa["x"] = v` 什么都不检查。
                        // 实测：`pa["x"] = "oops"`（int 字段赋 string）**零诊断、exit=0**；
                        // cstruct 同样（`v["x"] = "oops"` 也静默 —— 上面第一版只补了 struct，
                        // 漏了 cstruct，属"同义写法要逐个对"的又一例）。
                        // 走**唯一实现** infer_field_type（三级：作用域 → 全局定义表 → 导入模块符号表）。
                        const char* field_name = target->u.index.index->u.string.value;
                        TypeInfo* field_type = infer_field_type(s, obj_type, field_name, NULL);
                        if (field_type && value_type && !type_is_compatible(field_type, value_type)) {
                            char msg[BUFFER_MEDIUM];
                            format_detailed_type_error_ex(msg, sizeof(msg),
                                field_type, value_type,
                                "字段赋值类型不匹配", field_name);
                            error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, msg);
                        }
                        if (field_type) type_free(field_type);
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
                                    // 泛型 struct 实例：字段类型里的形参（Array[T] 的 T）
                                    // 要用实例的泛型实参替换后再比较，否则前向定义的
                                    // 泛型 struct（字段类型还是"struct T"占位）会误报
                                    TypeInfo* check_type = field_type;
                                    int check_owned = 0;
                                    if (obj_type->generic_count > 0 && obj_type->generic_args &&
                                        struct_sym->struct_type_param_count > 0 && struct_sym->struct_type_params) {
                                        check_type = type_copy(field_type);
                                        for (int gi = 0; gi < struct_sym->struct_type_param_count && gi < obj_type->generic_count; gi++) {
                                            TypeInfo* substituted = semantic_substitute_generic_param(check_type,
                                                struct_sym->struct_type_params[gi], obj_type->generic_args[gi]);
                                            type_free(check_type);
                                            check_type = substituted;
                                        }
                                        check_owned = 1;
                                    }
                                    if (check_type && !type_is_compatible(check_type, value_type)) {
                                        char msg[BUFFER_MEDIUM];
                                        format_detailed_type_error_ex(msg, sizeof(msg),
                                            check_type, value_type,
                                            "字段赋值类型不匹配", field_name);
                                        error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, msg);
                                    }
                                    if (check_owned) type_free(check_type);
                                    break;
                                }
                            }
                        } else if (value_type) {
                            // ---- 上面那两级查不到时，走**唯一实现**的字段类型推断（三级）----
                            // 此前这里是"查不到 ⇒ 什么都不做"，即**静默跳过类型检查**：实测
                            //   `var pa = new a.Pair()`（只 import、**不** use 类型名）之后
                            //   `pa.x = "oops"`（int 字段赋 string）**零诊断**、程序照跑还打印 X=oops；
                            //   而**同样代码加一句 `use a.Pair`** 会正确报「字段赋值类型不匹配 'x'」。
                            //   （与 ⑦"泛型方法经 alias 导入时静默跳过类型检查"同类：检查在一条路径上
                            //     悄悄不执行。）
                            const char* field_name = target->u.field_access.field_name;
                            TypeInfo* field_type = infer_field_type(s, obj_type, field_name, NULL);
                            if (field_type) {
                                if (!type_is_compatible(field_type, value_type)) {
                                    char msg[BUFFER_MEDIUM];
                                    format_detailed_type_error_ex(msg, sizeof(msg),
                                        field_type, value_type,
                                        "字段赋值类型不匹配", field_name);
                                    error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, msg);
                                }
                                type_free(field_type);
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
                    error_add_at(ERR_UNDEFINED_VAR, ast->line, ast->column, msg);
                }
            } else {
                // 检查是否对 const 变量复合赋值
                if (sym->is_const) {
                    char msg[BUFFER_MEDIUM];
                    snprintf(msg, sizeof(msg), "不能对 const 变量 '%s' 重新赋值", ast->u.compound_assign.name);
                    error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
                }
                // ★ 清「确定为 null」标志（T11，写点见普通赋值那处的说明）。
                //   这一处同时**报错**：`int? a = null; a += 1` 编译期就能确定是 null
                //   （报错口径与 `a + 1` 一致，见 semantic_type.c 的 report_known_null_name）。
                if (sym->is_null_value) {
                    report_known_null_name(s, ast->u.compound_assign.name, ast->line, ast->column);
                }
                sym->is_null_value = 0;
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
                        error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, msg);
                    }
                                        type_free(value_type);
                }
            }
            break;
        }

        case AST_DESTRUCT_DECL: {
            // 确定变量符号类型
            SymKind kind;
            if (s->current && s->current->parent == NULL) {
                kind = s->is_module ? SYM_MODULE : SYM_GLOBAL;
            } else {
                kind = SYM_LOCAL;
            }

            // 先注册所有变量到符号表
            for (int i = 0; i < ast->u.destruct_decl.slot_count; i++) {
                // 检查是否已被预注册（模块变量前向引用预扫描）
                Symbol* sym = scope_resolve_local(s->current, ast->u.destruct_decl.names[i]);
                if (sym && (sym->kind == SYM_GLOBAL || sym->kind == SYM_MODULE) &&
                    (kind == SYM_GLOBAL || kind == SYM_MODULE)) {
                    // 已被预注册，更新类型和 ref
                    ast->u.destruct_decl.refs[i].kind = sym->kind;
                    ast->u.destruct_decl.refs[i].index = sym->index;
                    if (!ast->u.destruct_decl.refs[i].name) {
                        ast->u.destruct_decl.refs[i].name = strdup(sym->name);
                    }
                } else {
                    sym = scope_define(s->current, ast->u.destruct_decl.names[i], kind);
                }
                if (sym) {
                    if (ast->u.destruct_decl.is_const) {
                        sym->is_const = 1;
                    }
                    if (kind == SYM_LOCAL) {
                        sym->index = allocate_local_index(s);
                    }
                    // 设置变量类型为槽位类型
                    if (sym->type) type_free(sym->type);
                    sym->type = type_copy(ast->u.destruct_decl.slot_types[i]);
                    // 如果不是预注册路径已填充 ref，这里填充
                    if (!ast->u.destruct_decl.refs[i].name) {
                        ast->u.destruct_decl.refs[i].kind = sym->kind;
                        ast->u.destruct_decl.refs[i].index = sym->index;
                        ast->u.destruct_decl.refs[i].name = strdup(sym->name);
                    }
                    // 检查循环作用域
                    Scope* check_scope = s->current;
                    while (check_scope && !check_scope->is_func) {
                        if (check_scope->is_loop) {
                            sym->is_in_loop = 1;
                            break;
                        }
                        check_scope = check_scope->parent;
                    }
                    // 递归解析 alias
                    resolve_alias_in_type(s, &ast->u.destruct_decl.slot_types[i], ast->line);
                } else {
                    char msg[BUFFER_MEDIUM];
                    snprintf(msg, sizeof(msg), "变量 '%s' 重复定义", ast->u.destruct_decl.names[i]);
                    error_add_at(ERR_DUPLICATE_VAR, ast->line, ast->column, msg);
                }
            }

            // 访问初始值表达式
            if (ast->u.destruct_decl.init) {
                visit(s, ast->u.destruct_decl.init);

                // ============================================================
                // 编译时类型检查：检查解构槽位类型与数据源元素类型是否兼容
                //
                // ★ 类型检查规则（any 槽位优先）：
                //   1. any 槽位（TYPE_ANY/TYPE_INFER）：总是兼容，跳过检查，不生成 CAST
                //      - var[any, any](a, b) = [1, "x"]      ✓ Array[any] + any 槽位
                //      - var[any](a) = [1, 2, 3]              ✓ Array[int] + any 槽位
                //   2. 具体类型槽位 + 具体元素类型：走 type_is_compatible 检查
                //      - var[int, float](a, b) = [1, 2]       ✓ int 兼容 int，int 兼容 float
                //      - var[string](a) = [1, 2]              ✗ string 与 int 不兼容
                //   3. 具体类型槽位 + any 元素类型：报错（不能从 any 转具体类型）
                //      - var[int](a) = [1, "x"]              ✗ Array[any] + int 槽位
                //   4. 源不是 Array/Dict 且非 any/infer：报错
                //      - var[int](a) = 42                     ✗ 不是数组
                //   5. 源是 any/infer（如无返回类型注解的函数）：报错
                //      - var[any](a) = no_return_type_func()  ✗ 无法确认是 Array
                // ============================================================
                TypeInfo* init_type = infer_expr_type(s, ast->u.destruct_decl.init);
                if (init_type) {
                    if (init_type->kind == TYPE_MULTI_RET) {
                        // 多返回值解构：var[T1, T2, ...](a, b, ...) = multi_ret_func()
                        int ret_count = init_type->param_count;
                        for (int i = 0; i < ast->u.destruct_decl.slot_count; i++) {
                            TypeInfo* slot_type = ast->u.destruct_decl.slot_types[i];
                            if (!slot_type) continue;
                            TypeKind slot_kind = slot_type->kind;
                            // any/infer 槽位：总是兼容，跳过检查
                            if (slot_kind == TYPE_ANY || slot_kind == TYPE_INFER) {
                                // ★ 自动推断：从返回值类型回填 TYPE_INFER 槽位
                                if (slot_kind == TYPE_INFER && i < ret_count) {
                                    type_free(ast->u.destruct_decl.slot_types[i]);
                                    ast->u.destruct_decl.slot_types[i] = type_copy(init_type->param_types[i]);
                                    // 同步更新符号类型
                                    Symbol* sym = scope_resolve(s->current, ast->u.destruct_decl.names[i]);
                                    if (sym) {
                                        if (sym->type) type_free(sym->type);
                                        sym->type = type_copy(init_type->param_types[i]);
                                    }
                                }
                                continue;
                            }
                            // 检查槽位是否超出返回值数量
                            if (i >= ret_count) {
                                char msg[BUFFER_MEDIUM];
                                snprintf(msg, sizeof(msg),
                                    "解构槽位 %d ('%s') 超出返回值数量 %d",
                                    i, ast->u.destruct_decl.names[i], ret_count);
                                error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, msg);
                            } else if (!type_is_compatible(slot_type, init_type->param_types[i])) {
                                char msg[BUFFER_MEDIUM];
                                snprintf(msg, sizeof(msg),
                                    "解构槽位 %d ('%s') 类型 %s 与返回值类型 %s 不兼容",
                                    i, ast->u.destruct_decl.names[i],
                                    type_kind_to_string(slot_kind),
                                    type_kind_to_string(init_type->param_types[i]->kind));
                                error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, msg);
                            }
                        }
                    } else if (ast->u.destruct_decl.is_dict) {
                        // 字典解构：检查 init 是否为 Dict 类型
                        if (init_type->kind == TYPE_DICT) {
                            TypeKind val_kind = init_type->value_type ?
                                init_type->value_type->kind : TYPE_ANY;
                            int val_is_any = (val_kind == TYPE_ANY || val_kind == TYPE_INFER);
                            // 检查每个槽位
                            for (int i = 0; i < ast->u.destruct_decl.slot_count; i++) {
                                TypeInfo* slot_type = ast->u.destruct_decl.slot_types[i];
                                if (!slot_type) continue;
                                TypeKind slot_kind = slot_type->kind;
                                // any 槽位：总是兼容，跳过检查
                                if (slot_kind == TYPE_ANY || slot_kind == TYPE_INFER) continue;
                                // 具体类型槽位：需要源值类型也是具体类型
                                if (val_is_any) {
                                    char msg[BUFFER_MEDIUM];
                                    snprintf(msg, sizeof(msg),
                                        "解构声明槽位 %d ('%s') 类型 %s 与字典值类型 %s 不兼容（源值类型为 any，无法转换为具体类型）",
                                        i, ast->u.destruct_decl.names[i],
                                        type_kind_to_string(slot_kind),
                                        type_kind_to_string(val_kind));
                                    error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, msg);
                                } else if (!type_is_compatible(slot_type, init_type->value_type)) {
                                    char msg[BUFFER_MEDIUM];
                                    snprintf(msg, sizeof(msg),
                                        "解构声明槽位 %d ('%s') 类型 %s 与字典值类型 %s 不兼容",
                                        i, ast->u.destruct_decl.names[i],
                                        type_kind_to_string(slot_kind),
                                        type_kind_to_string(val_kind));
                                    error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, msg);
                                }
                            }
                        } else if (init_type->kind == TYPE_ANY || init_type->kind == TYPE_INFER) {
                            // 返回类型为 any 或 infer（如无返回类型注解的函数），不允许解构
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg),
                                "字典解构要求初始化值为具体类型的 Dict（如 Dict[string, int]），"
                                "实际类型为 %s，无法保证类型安全",
                                type_kind_to_string(init_type->kind));
                            error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, msg);
                        } else if (init_type->kind != TYPE_NULL) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg),
                                "字典解构要求初始化值为 Dict 类型，实际为 %s",
                                type_kind_to_string(init_type->kind));
                            error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, msg);
                        }
                    } else {
                        // 数组解构：检查 init 是否为 Array 类型
                        if (init_type->kind == TYPE_ARRAY) {
                            TypeKind elem_kind = init_type->element_type ?
                                init_type->element_type->kind : TYPE_ANY;
                            int elem_is_any = (elem_kind == TYPE_ANY || elem_kind == TYPE_INFER);
                            // 检查每个槽位
                            for (int i = 0; i < ast->u.destruct_decl.slot_count; i++) {
                                TypeInfo* slot_type = ast->u.destruct_decl.slot_types[i];
                                if (!slot_type) continue;
                                TypeKind slot_kind = slot_type->kind;
                                // any/infer 槽位：总是兼容，跳过检查
                                if (slot_kind == TYPE_ANY || slot_kind == TYPE_INFER) {
                                    // ★ 自动推断：从数组元素类型回填 TYPE_INFER 槽位
                                    if (slot_kind == TYPE_INFER && !elem_is_any && init_type->element_type) {
                                        type_free(ast->u.destruct_decl.slot_types[i]);
                                        ast->u.destruct_decl.slot_types[i] = type_copy(init_type->element_type);
                                        // 同步更新符号类型
                                        Symbol* sym = scope_resolve(s->current, ast->u.destruct_decl.names[i]);
                                        if (sym) {
                                            if (sym->type) type_free(sym->type);
                                            sym->type = type_copy(init_type->element_type);
                                        }
                                    }
                                    continue;
                                }
                                // 具体类型槽位：需要源元素类型也是具体类型
                                if (elem_is_any) {
                                    char msg[BUFFER_MEDIUM];
                                    snprintf(msg, sizeof(msg),
                                        "解构声明槽位 %d ('%s') 类型 %s 与数组元素类型 %s 不兼容（源元素类型为 any，无法转换为具体类型）",
                                        i, ast->u.destruct_decl.names[i],
                                        type_kind_to_string(slot_kind),
                                        type_kind_to_string(elem_kind));
                                    error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, msg);
                                } else if (!type_is_compatible(slot_type, init_type->element_type)) {
                                    char msg[BUFFER_MEDIUM];
                                    snprintf(msg, sizeof(msg),
                                        "解构声明槽位 %d ('%s') 类型 %s 与数组元素类型 %s 不兼容",
                                        i, ast->u.destruct_decl.names[i],
                                        type_kind_to_string(slot_kind),
                                        type_kind_to_string(elem_kind));
                                    error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, msg);
                                }
                            }
                        } else if (init_type->kind == TYPE_ANY || init_type->kind == TYPE_INFER) {
                            // 返回类型为 any 或 infer（如无返回类型注解的函数），不允许解构
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg),
                                "数组解构要求初始化值为具体类型的 Array（如 Array[int]），"
                                "实际类型为 %s，无法保证类型安全",
                                type_kind_to_string(init_type->kind));
                            error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, msg);
                        } else if (init_type->kind != TYPE_NULL) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg),
                                "数组解构要求初始化值为 Array 类型，实际为 %s",
                                type_kind_to_string(init_type->kind));
                            error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, msg);
                        }
                    }
                    type_free(init_type);
                }
            }
            break;
        }