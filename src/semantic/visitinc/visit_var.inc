        case AST_VAR_DECL: {
            // 检查变量声明类型是否使用了 C 布局类型（i32/u8/f32 等）
            // C 布局类型只能在 clib 声明、cstruct 字段、Ptr[T] 中使用
            if (ast->u.var_decl.type && is_c_layout_type(ast->u.var_decl.type->kind)) {
                const char* type_str = type_kind_to_string(ast->u.var_decl.type->kind);
                error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                 "C 布局类型 '%s' 不能用于变量声明，请使用 Leno 类型（如 int/float/string）", type_str);
            }
            // 检查 any 类型：any 只能用于函数参数和返回类型，不能用于变量声明
            // 变量声明请使用 var（类型推断）或指定具体类型
            if (ast->u.var_decl.type && ast->u.var_decl.type->kind == TYPE_ANY) {
                error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column, "'any' 类型只能用于函数参数和返回类型，变量声明请使用 'var' 或指定具体类型");
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
                // ★ M4（2026-10-11）：变量名撞上**模块别名** ⇒ 遮蔽告警 ✓
                //   （与 struct 字段那条共用 semantic_check_alias_shadow ✓）
                semantic_check_alias_shadow(s, ast->u.var_decl.name, "局部变量",
                                            ast->line, ast->column);
            }
            if (sym) {
                // ★ 未使用局部量诊断（WARN_UNUSED_VAR）的**登记点**：只登记"用户代码里的局部量" ✓
                //   模块级/全局量不登记（可能被别的模块用到 ✓）；
                //   LSP 模式不登记（编到一半的代码天天有未用变量 ✗ 会刷屏 ✓）；
                //   形参 / 解构名 / for 头变量不走这条路径 ⇒ 天然豁免 ✓。
                //   ⚠ 三个字段在这里**显式赋值**：不依赖符号是 calloc 还是 malloc 出来的 ✓
                if (kind == SYM_LOCAL && !s->is_lsp_mode) {
                    sym->track_unused = 1;
                    sym->decl_line = ast->line;
                    sym->is_read = 0;
                }
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
                // 预扫描（semantic.c 的全局变量预注册）可能已经填过 ref.name；
                // 直接覆盖会漏掉那一份 strdup ⇒ 先释放再写。
                free(ast->u.var_decl.ref.name);
                ast->u.var_decl.ref.name = strdup(sym->name);
            } else {
                // 重复定义错误 - 报告准确的行号
                error_add_at_fmt(ERR_DUPLICATE_VAR, ast->line, ast->column,
                                 "变量 '%s' 重复定义", ast->u.var_decl.name);
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
                    // 记住初值表达式：`var p = x as P` 这类"可能是 null"的来源要靠它认出来
                    sym->decl_init = ast->u.var_decl.init;
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
                        const char* parent_name = type_kind_to_string(parent_kind);
                        error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                         "var 不能用作类型参数，请改用 any（如 %s[any]）", parent_name);
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
                        if (!struct_def && semantic_native_struct_known(type_name)) {
                            // native struct 兜底（v3.2.5）：`DirInfo d = dirs.stat(p)` / `DirEntry e`
                            //   这类标注合法 —— 名字不在符号表里，但字段表由 native 规格提供
                            //   （与字段解析、运行期 ObjStructDef 同源）⇒ 保持 TYPE_STRUCT + 名字即可
                        } else if (!struct_def) {
                            char hint[BUFFER_MEDIUM];
                            // ★ 2026-10-10：提示改由**共享实现**生成 —— 它会去已导入模块里
                            //   按名字找（命中就说清是哪个模块导出的 ✓），`double` 的专门
                            //   话术也归它 ✓（原来这八处各写一份 ✗）
                            semantic_undefined_type_hint(s, type_name, hint, sizeof(hint));
                            error_add_at_fmt(ERR_UNDEFINED_VAR, ast->line, ast->column,
                                             "未定义的类型: %s%s", type_name, hint);
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
                            char hint[BUFFER_MEDIUM];
                            semantic_undefined_type_hint(s, type_name, hint, sizeof(hint));
                            error_add_at_fmt(ERR_UNDEFINED_VAR, ast->line, ast->column,
                                             "未定义的类型: %s%s", type_name, hint);
                            declared_type_undefined = 1;
                        }
                    }
                } else if (ast->u.var_decl.type && ast->u.var_decl.type->kind == TYPE_FUNCTION) {
                    // ★ 2026-11-08：**函数类型声明**里的参数/返回类型也要校验。
                    //   此前 `func(int): Nope f = null` 里未定义的 `Nope` **零诊断** ✗
                    //   （只查 TYPE_STRUCT/CLIB 顶层名字 ⇒ TYPE_FUNCTION 整个被跳过）
                    TypeInfo* ft = ast->u.var_decl.type;
                    for (int pass = 0; pass < 2; pass++) {
                        int n = (pass == 0) ? ft->param_count : (ft->return_type ? 1 : 0);
                        for (int pi = 0; pi < n; pi++) {
                            TypeInfo* t = (pass == 0) ? (ft->param_types ? ft->param_types[pi] : NULL)
                                                      : ft->return_type;
                            if (!t || t->module_qualifier) continue;   // 限定名交给别名/use 路径 ✓
                            if (t->kind != TYPE_STRUCT && t->kind != TYPE_CLIB && t->kind != TYPE_FACE) continue;
                            if (!t->struct_name || !t->struct_name[0]) continue;
                            if (face_def_find(t->struct_name)) continue;
                            if (scope_resolve(s->current, t->struct_name)) continue;
                            if (semantic_native_struct_known(t->struct_name)) continue;
                            char hint[BUFFER_MEDIUM];
                            semantic_undefined_type_hint(s, t->struct_name, hint, sizeof(hint));
                            error_add_at_fmt(ERR_UNDEFINED_VAR, ast->line, ast->column,
                                             "函数类型声明里的类型 '%s' 未定义%s",
                                             t->struct_name, hint);
                            declared_type_undefined = 1;
                            break;
                        }
                        if (declared_type_undefined) break;
                    }
                } else if (ast->u.var_decl.type && ast->u.var_decl.type->kind == TYPE_STRUCT &&
                           ast->u.var_decl.type->struct_name) {
                    // 保持原有的 struct 检查路径（当 type->kind 明确是 TYPE_STRUCT 时）
                    const char* type_name = ast->u.var_decl.type->struct_name;
                    if (face_def_find(type_name)) {
                        ast->u.var_decl.type->kind = TYPE_FACE;
                    } else {
                        Symbol* struct_def = scope_resolve(s->current, type_name);
                        if (!struct_def && semantic_native_struct_known(type_name)) {
                            // native struct 兜底（同上一处，v3.2.5）
                        } else if (!struct_def || !struct_def->type ||
                            (struct_def->type->kind != TYPE_STRUCT &&
                             struct_def->type->kind != TYPE_CSTRUCT &&
                             struct_def->type->kind != TYPE_FACE)) {
                            // 检查是否是 alias
                            if (struct_def && struct_def->kind == SYM_TYPE && struct_def->type) {
                                type_free(ast->u.var_decl.type);
                                ast->u.var_decl.type = type_copy(struct_def->type);
                            } else {
                                char hint[BUFFER_MEDIUM];
                                semantic_undefined_type_hint(s, type_name, hint, sizeof(hint));
                                error_add_at_fmt(ERR_UNDEFINED_VAR, ast->line, ast->column,
                                                 "未定义的类型: %s%s", type_name, hint);
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

                // 预扫描给这个符号塞过占位类型（ANY / 声明的副本）⇒ 下面会用推断结果重设，
                // 先释放旧的，避免每次声明都漏一个 TypeInfo。
                if (sym && sym->type) { type_free(sym->type); sym->type = NULL; }
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
                            char context[256];
                            snprintf(context, sizeof(context), "变量 '%s' 声明类型与初始化值类型不匹配",
                                     ast->u.var_decl.name);
                            // 如果是 struct 未实现 face，追加提示（做成尾串 ⇒ 不必先拼整条消息 ✓）
                            char face_hint[256];
                            face_hint[0] = '\0';
                            if (ast->u.var_decl.type->kind == TYPE_FACE && init_type->kind == TYPE_STRUCT
                                && ast->u.var_decl.type->struct_name && init_type->struct_name) {
                                snprintf(face_hint, sizeof(face_hint), " (struct '%s' 未实现 face '%s'，请添加 impl: struct %s impl %s { ... })",
                                    init_type->struct_name, ast->u.var_decl.type->struct_name,
                                    init_type->struct_name, ast->u.var_decl.type->struct_name);
                            }
                            error_add_at_fmt(ERR_TYPE_MISMATCH, ast->line, ast->column, "%s%s",
                                format_detailed_type_error_text(ast->u.var_decl.type, init_type, context),
                                face_hint);
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
                // 同有初始化分支：先释放预扫描的占位类型
                if (sym && sym->type) { type_free(sym->type); sym->type = NULL; }
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
                        } else if (!struct_def && semantic_native_struct_known(sym->type->struct_name)) {
                            // native struct 兜底：**无初始化的声明**（`DirInfo d`）同样合法（v3.2.5）
                        } else if (!struct_def || !struct_def->type ||
                                   (struct_def->type->kind != TYPE_STRUCT &&
                                    struct_def->type->kind != TYPE_CSTRUCT &&
                                    struct_def->type->kind != TYPE_FACE)) {
                            char hint[BUFFER_MEDIUM];
                            semantic_undefined_type_hint(s, sym->type->struct_name, hint, sizeof(hint));
                            error_add_at_fmt(ERR_UNDEFINED_VAR, ast->line, ast->column,
                                             "未定义的类型: %s%s",
                                             sym->type->struct_name, hint);
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
            // ★ 未使用局部量诊断（WARN_UNUSED_VAR）：这里是"**读过**"的唯一置位点 ✓
            //   （若本分支同时被"赋值目标"复用，则赋值也会被算成读 ⇒ 只会**少报**、
            //    不会误报 ✓ 方向是安全的 —— 未使用变量的诊断宁漏勿误报 ✓）
            if (sym) sym->is_read = 1;
            // ★ 2026-11-07：**裸写枚举成员**（`enum Color { red }` 后直接写 `red`）
            //   此前语义能解析到成员符号，但 codegen 没有给这个全局槽赋值 ⇒ 运行期静默得到 null ✗
            //   （实测 `print(_str(red))` 输出 null，既不报错也不等于 0）。
            //   枚举成员必须带限定名访问（`Color.red`）⇒ 这里定向报错，不再放行到运行期。
            if (sym && sym->name && sym->is_enum_member &&
                sym->type && sym->type->struct_name) {
                error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                    "枚举成员 '%s' 不能直接当变量用（直接写会静默得到 null）—— 请写 `%s.%s`",
                    ast->u.var.name, sym->type->struct_name, ast->u.var.name);
            }
            if (!sym) {
                // ★ 2026-10-24（Q3）：`for k in d` 里那个未定义的 k（用户想遍历字典/数组）
                //   原来报「未定义的变量: k」+ 拼写建议「是否想输入 'd'？」——
                //   两条都指不到真因（真因：本语言遍历写 `for 集合 to 变量`，集合在前；
                //   `x in y` 是**条件循环**，x 必须是已定义的外部变量）。
                if (s->for_in_lhs == ast) {
                    // ★ 2026-10-24（Q3）：`for k in 集合` 里的 k **未定义** ——
                    //   原来报「未定义的变量: k」+ 拼写建议「是否想输入 'd'？」，两条都误导。
                    //   这里直接说清：这个形状不是遍历（for 是区间循环，`in` 是成员测试，
                    //   端点变 bool ⇒ 空区间 ⇒ 一次都不跑），并给出正确写法。
                    error_add_at_fmt(ERR_UNDEFINED_VAR, ast->line, ast->column,
                        "for 头的 `%s in 集合` 不是遍历写法（`in` 是成员测试运算符，"
                        "而 for 是区间循环 ⇒ 循环体不会执行），且 %s 未定义；"
                        "遍历请写 `for 集合 to %s`（**集合在前**）",
                        ast->u.var.name, ast->u.var.name, ast->u.var.name);
                } else if (s->for_in_lhs_name && ast->u.var.name &&
                           strcmp(s->for_in_lhs_name, ast->u.var.name) == 0) {
                    // ★ Q3：同一条 for 的循环体里再引用这个名字 ⇒ 上面已定向报过一次，
                    //   这里**静默**，不再刷「未定义变量 + 拼写建议」（级联噪音）。
                } else {
                    // ★ 2026-10-28（docs/待修_编译与语义问题.md §3）：裸写的名字其实是**本文件某个
                    //   struct 的方法** ⇒ 明说"它是方法 + 两条出路"，不再报"未定义的变量"了事
                    //   （那句话让人以为名字拼错了 —— 实测定位花了 20 分钟 ✗）。
                    //   · 方法体内直接写方法名：编译器自动改写成 `self["名字"]`（绑好接收者 ✓）
                    //   · 方法体外必须给接收者：`对象.名字`
                    const char* owner = semantic_method_owner(s, ast->u.var.name);
                    if (owner) {
                        error_add_at_fmt(ERR_UNDEFINED_VAR, ast->line, ast->column,
                            "未定义的变量: %s —— 它是 struct '%s' 的**成员方法**，不能脱离接收者当值用。"
                            "在方法体内直接写方法名即可（编译器自动绑定 self）；"
                            "方法体外请写 `对象.%s`",
                            ast->u.var.name, owner, ast->u.var.name);
                    } else {
                        const char* hint = get_similar_name_hint(s->current, ast->u.var.name);
                        error_add_at_fmt(ERR_UNDEFINED_VAR, ast->line, ast->column,
                                         "未定义的变量: %s%s", ast->u.var.name, hint);
                    }
                }
            } else if (sym->is_initialized == 2) {
                // 变量正在初始化中，不允许引用自身
                error_add_at_fmt(ERR_UNDEFINED_VAR, ast->line, ast->column,
                                 "变量 '%s' 在初始化前使用", ast->u.var.name);
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
                error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                 "并行赋值数量不匹配：左侧 %d 个变量，右侧 %d 个值",
                                 left_count, right_count);
                break;
            }
            
            // 为每个左侧目标解析符号或访问索引表达式
            for (int i = 0; i < left_count; i++) {
                Ast* target = ast->u.assign.targets[i];

                // ★ N1（2026-10-25）：`m.var = v` —— 跨模块写导出变量现在**支持**了
                //   （codegen 发 OP_INDEX_SET，VM 侧写穿到 module->globals 并同步 exports）。
                //   这里只拦两种写法：
                //     ① 目标是模块里的 **const** ⇒ 常量不可修改（给 setter / 改成 var）；
                //     ② 目标**不是导出的变量**（函数 / 类型 / 未导出 / 不存在）⇒ 写不了。
                //   类型检查在下面第二个循环（与其它赋值目标同一处，见 AST_MODULE_ACCESS 分支）。
                //   ⚠ wr_mi 为 NULL 说明是 `实例.字段`（parser 对字段访问也建 MODULE_ACCESS）⇒
                //     必须**放行**，交给后面的 AST_MODULE_ACCESS 分支按 struct 字段处理 ✓
                if (target->kind == AST_MODULE_ACCESS) {
                    ImportedModuleInfo* wr_mi = semantic_module_alias_info(
                        s, target->u.module_access.module_name);
                    if (wr_mi && wr_mi->sym_table) {
                        ModuleVarSymbol* wv = module_symbol_table_find_var(
                            wr_mi->sym_table, target->u.module_access.member_name);
                        if (!wv) {
                            error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                "模块 '%s' 中没有可赋值的变量 '%s' —— 跨模块只能写 `export var` 声明的变量",
                                target->u.module_access.module_name,
                                target->u.module_access.member_name);
                            continue;
                        }
                        if (wv->is_const) {
                            error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                "不能给模块 '%s' 的 const 变量 '%s' 赋值 —— 常量不可修改；"
                                "需要可变状态请把模块里的声明改成 var，或提供 setter",
                                target->u.module_access.module_name,
                                target->u.module_access.member_name);
                            continue;
                        }
                    }
                }

                if (target->kind == AST_INDEX) {
                    // const 检查必须在 visit 之前，因为 visit 会改变 obj 节点类型
                    if (target->u.index.obj && target->u.index.obj->kind == AST_VAR) {
                        const char* obj_name = target->u.index.obj->u.var.name;
                        SymRef tmp_ref;
                        memset(&tmp_ref, 0, sizeof(tmp_ref));
                        Symbol* sym = resolve_variable_with_upvalue(s, obj_name, &tmp_ref);
                        if (sym && sym->is_const) {
                            error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                             "不能对 const 变量 '%s' 的元素重新赋值", obj_name);
                        }
                        symref_dispose(&tmp_ref);
                    }
                    // 检查模块变量是否为 const（如 m.arr[0] = 99）
                    if (target->u.index.obj && target->u.index.obj->kind == AST_MODULE_ACCESS) {
                        const char* mod_name = target->u.index.obj->u.module_access.module_name;
                        const char* mem_name = target->u.index.obj->u.module_access.member_name;
                        ImportedModuleInfo* mod_info = find_imported_module(s, mod_name);
                        if (mod_info && mod_info->sym_table) {
                            ModuleVarSymbol* var_sym = module_symbol_table_find_var(mod_info->sym_table, mem_name);
                            if (var_sym && var_sym->is_const) {
                                error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                                 "不能对 const 变量 '%s.%s' 的元素重新赋值", mod_name, mem_name);
                            }
                        }
                    }

                    // ★ N14（2026-10-24）：更深的链 —— `const s = new S9(); s.arr[0] = 9`
                    //   以及**嵌套下标** `const n = [[1]]; n[0][0] = 9`。
                    //   上面两条只认"obj 是裸变量"和"obj 是模块成员"，其余形态（字段 / 嵌套下标 /
                    //   struct 字段形态的 MODULE_ACCESS）全落在这里，用**根**判据兜住。
                    //   ⚠ 只对上面没报过的形态判（否则同一行报两条）：
                    //     · obj 是 AST_VAR ⇒ 第一条已报
                    //     · obj 是"模块别名.成员" ⇒ 第二条已管
                    //     · 其余（FIELD_ACCESS / INDEX / 非模块的 MODULE_ACCESS）⇒ 这里判
                    {
                        Ast* iobj = target->u.index.obj;
                        int covered = 0;
                        if (iobj && iobj->kind == AST_VAR) {
                            covered = 1;
                        } else if (iobj && iobj->kind == AST_MODULE_ACCESS &&
                                   find_imported_module(s, iobj->u.module_access.module_name)) {
                            covered = 1;
                        }
                        if (!covered && iobj) {
                            char const_root[BUFFER_SMALL];
                            if (sem_const_root_name(s, iobj, const_root, (int)sizeof(const_root))) {
                                error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                                 "不能修改 const 变量 '%s' 的元素（const 是**整体不可变**）",
                                                 const_root);
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
                    // ★ N14（2026-10-24）：根是 const 的字段写也要拦（`const s = new S8(); s.v = 42`
                    //   此前这条路径根本没有 const 检查，改得动）。
                    char const_root[BUFFER_SMALL];
                    if (sem_const_root_name(s, target, const_root, (int)sizeof(const_root))) {
                        error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                         "不能修改 const 变量 '%s' 的字段（const 是**整体不可变**："
                                         "需要可变对象请声明为 var）", const_root);
                    } else {
                        visit(s, target);
                        if (target->kind == AST_NUM || target->kind == AST_STRING ||
                            target->kind == AST_BOOL || target->kind == AST_NULL) {
                            error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column, "不能对常量重新赋值");
                        }
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
                    // ★ N14（2026-10-24）：`const s = new S8(); s.v = 42` —— 根是 const 的
                    //   字段写此前**完全没检查**（AST_FIELD 写路径没有 const 分支）。
                    //   必须在 visit 之前判：visit 会把它改写成 AST_FIELD_ACCESS/AST_INDEX。
                    char const_root[BUFFER_SMALL];
                    if (sem_const_root_name(s, target, const_root, (int)sizeof(const_root))) {
                        error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                         "不能修改 const 变量 '%s' 的字段（const 是**整体不可变**："
                                         "需要可变对象请声明为 var）", const_root);
                    } else {
                        visit(s, target);
                        if (target->kind == AST_NUM || target->kind == AST_STRING ||
                            target->kind == AST_BOOL || target->kind == AST_NULL) {
                            error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                             "不能对常量 '%s' 重新赋值", orig_buf);
                        }
                    }
                } else if (target->kind == AST_VAR && ast->u.assign.names[i]) {
                    // 简单变量：解析符号
                    Symbol* sym = resolve_variable_with_upvalue(s, ast->u.assign.names[i], &ast->u.assign.refs[i]);
                    if (!sym) {
                        const char* hint = get_similar_name_hint(s->current, ast->u.assign.names[i]);
                        error_add_at_fmt(ERR_UNDEFINED_VAR, ast->line, ast->column,
                                         "未定义的变量: %s%s", ast->u.assign.names[i], hint);
                    } else {
                        // 检查是否对 const 变量赋值
                        if (sym->is_const) {
                            error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                             "不能对 const 变量 '%s' 重新赋值", ast->u.assign.names[i]);
                        }
                        // ★ 清「确定为 null」标志（T11）：这个变量被写过了 ⇒ 之后不能再断定它是 null。
                        //   ⚠ 所有写变量的地方都要清（`a = v` / `a += v` / for 的迭代变量…）——
                        //     漏一处就会把"本来能跑的程序"判成编译错误（误报比漏报严重得多）。
                        //     目前写点：本处、AST_COMPOUND_ASSIGN（本文件）、
                        //     `var` 声明（新符号，天然为 0）、for 迭代变量（作用域内新符号，天然为 0）。
                        sym->is_null_value = 0;
                        // 普通赋值不保证非空 ⇒ 清掉"已确定非空"（`x ?? = v` 建立的那个）
                        sym->is_nonnull = 0;
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
                                error_add_at_fmt(ERR_TYPE_MISMATCH, ast->line, ast->column, "%s",
                                    format_detailed_type_error_ex_text(sym->type, value_type,
                                        "无法将值赋值给变量", ast->u.assign.names[i]));
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
                        // ★ pri：`pa["secret"] = v` 的私有字段检查（2026-11-08）
                        if (obj_type->kind == TYPE_STRUCT) {
                            pri_check_field_access(s, target, obj_type, field_name);
                        }
                        if (field_type && value_type && !type_is_compatible(field_type, value_type)) {
                            error_add_at_fmt(ERR_TYPE_MISMATCH, ast->line, ast->column, "%s",
                                format_detailed_type_error_ex_text(field_type, value_type,
                                    "字段赋值类型不匹配", field_name));
                        }
                        if (field_type) type_free(field_type);
                    }
                    if (obj_type) type_free(obj_type);
                } else if (target->kind == AST_MODULE_ACCESS) {
                    // ★ N1 写侧（2026-10-25）：`m.x = v` 的**类型检查** —— 用模块符号表里
                    //   声明的类型（`ModuleVarSymbol.type_info` 优先，退化成扁平的 type/struct_name）。
                    //   ⚠ 只在"确实是模块别名 + 那个变量可写"时判；`实例.字段` 那种形态
                    //     wr_mi 为 NULL ⇒ 落到后面的 AST_FIELD_ACCESS 分支（不该由这里管）。
                    ImportedModuleInfo* wr_mi = semantic_module_alias_info(
                        s, target->u.module_access.module_name);
                    if (wr_mi && wr_mi->sym_table && value_type) {
                        ModuleVarSymbol* wv = module_symbol_table_find_var(
                            wr_mi->sym_table, target->u.module_access.member_name);
                        if (wv && !wv->is_const) {
                            TypeInfo* want = wv->type_info;
                            TypeInfo* tmp_want = NULL;
                            if (!want && wv->type != TYPE_ANY && wv->type != TYPE_UNKNOWN) {
                                tmp_want = type_new(wv->type);
                                if (wv->struct_name) tmp_want->struct_name = strdup(wv->struct_name);
                                want = tmp_want;
                            }
                            if (want && !type_is_compatible(want, value_type)) {
                                error_add_at_fmt(ERR_TYPE_MISMATCH, ast->line, ast->column, "%s",
                                    format_detailed_type_error_ex_text(want, value_type,
                                        "无法将值赋给模块变量",
                                        target->u.module_access.member_name));
                            }
                            if (tmp_want) type_free(tmp_want);
                        }
                    }
                } else if (target->kind == AST_FIELD_ACCESS) {
                    // struct/cstruct 字段赋值的类型检查
                    TypeInfo* obj_type = infer_expr_type(s, target->u.field_access.obj);
                    if (obj_type && (obj_type->kind == TYPE_STRUCT || obj_type->kind == TYPE_CSTRUCT) && obj_type->struct_name) {
                        // ★ pri：`pa.secret = v` 的检查交给 visit_field_access（那条赋值目标
                        //   也会被当作字段访问访问一遍 ⇒ 在这里再查一次只会多一条「(重复 2 次)」✗）
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
                                        error_add_at_fmt(ERR_TYPE_MISMATCH, ast->line, ast->column, "%s",
                                            format_detailed_type_error_ex_text(check_type, value_type,
                                                "字段赋值类型不匹配", field_name));
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
                                    error_add_at_fmt(ERR_TYPE_MISMATCH, ast->line, ast->column, "%s",
                                        format_detailed_type_error_ex_text(field_type, value_type,
                                            "字段赋值类型不匹配", field_name));
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
                    const char* hint = get_similar_name_hint(s->current, ast->u.compound_assign.name);
                    error_add_at_fmt(ERR_UNDEFINED_VAR, ast->line, ast->column,
                                     "未定义的变量: %s%s", ast->u.compound_assign.name, hint);
                }
            } else {
                // 检查是否对 const 变量复合赋值
                if (sym->is_const) {
                    error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                     "不能对 const 变量 '%s' 重新赋值", ast->u.compound_assign.name);
                }
                // ★ 清「确定为 null」标志（T11，写点见普通赋值那处的说明）。
                //   这一处同时**报错**：`int? a = null; a += 1` 编译期就能确定是 null
                //   （报错口径与 `a + 1` 一致，见 semantic_type.c 的 report_known_null_name）。
                //   ⚠ 空值合并赋值（B1）**必须豁免**：`int? a = null; a ?? = 3` 正是它的头号用法，
                //     报"确定是 null"等于把新特性唯一的入口封死。
                if (ast->u.compound_assign.op != TOK_COALESCE_ASSIGN && sym->is_null_value) {
                    report_known_null_name(s, ast->u.compound_assign.name, ast->line, ast->column);
                }
                sym->is_null_value = 0;
                // ★ 2026-11-03：`x ?? = v` 之后 x **一定非空** ⇒ 后续算术不用再报
                //   "可能为 null"（见 leno_vm.h 的 is_nonnull 说明）。
                //   其它复合赋值（+= 之类）不保证非空 ⇒ 清掉。
                sym->is_nonnull = (ast->u.compound_assign.op == TOK_COALESCE_ASSIGN) ? 1 : 0;
                // 记录"这个变量被 ??= 初始化过"（跨分支保留），供可空警告给更具体的修法
                if (ast->u.compound_assign.op == TOK_COALESCE_ASSIGN) sym->coalesce_inited = 1;
                ast->u.compound_assign.ref.type_kind = sym->type ? sym->type->kind : TYPE_ANY;

                // ★ 2026-11-02（B1）：空值合并赋值的目标若**永远不可能是 null** ⇒ 恒不生效，直接报错。
                //   ⚠ 只对**值类型**（int/float/bool/bigint/enum/C 布局标量）报：
                //     引用类（string / Array / Dict / struct / clib / Ptr / 函数）在 Leno 里
                //     本来就允许 `string s = null`（不是 `string? s`）⇒ 那些类型上 `??=` 完全合法，
                //     实测 `string s = null; s ??= "x"` 与 `_krnl ??= ffi.load(...)` 都必须能过。
                //   ⚠ 注释里一律写 "?? ="（中间加空格）而不是连写：连写会触发三字符组
                //     （trigraph）替换 —— 连写的三字符在翻译阶段就被换成 `#`，字符串里同样中招。
                if (ast->u.compound_assign.op == TOK_COALESCE_ASSIGN && sym->type) {
                    TypeKind vk = sym->type->kind;
                    int value_kind = (vk == TYPE_INT || vk == TYPE_FLOAT || vk == TYPE_BOOL ||
                                      vk == TYPE_BIGINT || vk == TYPE_ENUM || is_c_layout_type(vk));
                    if (value_kind && !type_is_nullable(sym->type)) {
                        error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                            // ⚠ 操作符拆成两段字面量写出：连写 `?` `?` `=` 会被三字符组替换成 '#'
                            "'%s' 不可空（类型 '%s'），`?" "?=` 恒不生效 —— 直接写 '%s = 值' 即可；"
                            "需要「可能为空」语义请把声明改成 '%s?'",
                            ast->u.compound_assign.name,
                            type_kind_to_string(sym->type->kind),
                            ast->u.compound_assign.name, ast->u.compound_assign.name);
                    }
                }
            }
            visit(s, ast->u.compound_assign.value);

            // ★ 2026-11-02（B1）：空值合并赋值也要"锁类型"。
            //   普通赋值 `var cb = null; cb = ffi.callback(...)` 会把 any 变量锁成右侧的具体类型
            //   （见本文件 AST_ASSIGN 的"第一次赋非 null 值时锁定类型"）。`cb ??= 同一个值`
            //   若不锁，变量就一直是 any ⇒ 后续传给 `Ptr` 形参会报"实际 any"（实测 sdl_tray /
            //   sdl_filedialog 的 C 回调句柄就是这么挂的）。这里对齐普通赋值的口径。
            if (sym && sym->type && ast->u.compound_assign.op == TOK_COALESCE_ASSIGN &&
                sym->type->kind == TYPE_ANY) {
                TypeInfo* cvt = infer_expr_type(s, ast->u.compound_assign.value);
                if (cvt && cvt->kind != TYPE_NULL && cvt->kind != TYPE_ANY &&
                    cvt->kind != TYPE_UNKNOWN && cvt->kind != TYPE_INFER) {
                    type_free(sym->type);
                    sym->type = type_copy(cvt);
                    ast->u.compound_assign.ref.type_kind = sym->type->kind;
                }
                if (cvt) type_free(cvt);
            }

            // 复合赋值类型检查
            if (sym && sym->type) {
                TypeInfo* value_type = infer_expr_type(s, ast->u.compound_assign.value);
                if (value_type) {
                    if (!type_is_compatible(sym->type, value_type)) {
                        error_add_at_fmt(ERR_TYPE_MISMATCH, ast->line, ast->column, "%s",
                            format_detailed_type_error_ex_text(sym->type, value_type,
                                "复合赋值类型不匹配", ast->u.compound_assign.name));
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
                    error_add_at_fmt(ERR_DUPLICATE_VAR, ast->line, ast->column,
                                     "变量 '%s' 重复定义", ast->u.destruct_decl.names[i]);
                }
            }

            // ★ 默认值表达式也要 visit（2026-10-22）
            //   不 visit 就不会做标识符绑定 ⇒ 里面的变量（如 `var{n = base + 1}` 的 base）
            //   运行时读到 **null**，而且会被"未使用变量"误报 ✗
            if (ast->u.destruct_decl.slot_defaults) {
                for (int i = 0; i < ast->u.destruct_decl.slot_count; i++) {
                    if (ast->u.destruct_decl.slot_defaults[i]) {
                        visit(s, ast->u.destruct_decl.slot_defaults[i]);
                    }
                }
            }

            // 访问初始值表达式
            if (ast->u.destruct_decl.init) {
                visit(s, ast->u.destruct_decl.init);

                // ★ 2026-11-08：右侧是**字面量**时先查形状。
                //   此前 `var{"a": int}(a) = {"b": 1}` 运行期静默 null ✗；
                //   `var[int,int,int](a,b,c) = [1,2]` 运行期才报「数组索引越界」✗
                {
                    Ast* lit = ast->u.destruct_decl.init;
                    if (lit && ast->u.destruct_decl.is_dict && lit->kind == AST_DICT) {
                        for (int i = 0; i < ast->u.destruct_decl.slot_count; i++) {
                            const char* key = (ast->u.destruct_decl.slot_keys
                                               ? ast->u.destruct_decl.slot_keys[i] : NULL);
                            if (!key) continue;
                            if (ast->u.destruct_decl.slot_defaults &&
                                ast->u.destruct_decl.slot_defaults[i]) continue;   // 有默认值 ⇒ 缺键合法
                            int found = 0;
                            for (int e = 0; e < lit->u.dict.count; e++) {
                                Ast* k = lit->u.dict.entries[e].key;
                                if (k && k->kind == AST_STRING && k->u.string.value &&
                                    strcmp(k->u.string.value, key) == 0) { found = 1; break; }
                            }
                            if (!found) {
                                error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                    "字典解构的键 '%s' 不在右侧字典字面量里 ⇒ 运行期会拿到 null；"
                                    "请补上键，或给槽位写默认值（`var{\"%s\": 类型 = 默认值}(…)`）",
                                    key, key);
                            }
                        }
                    } else if (lit && !ast->u.destruct_decl.is_dict && lit->kind == AST_ARRAY &&
                               lit->u.array.count < ast->u.destruct_decl.slot_count) {
                        int has_default = 0;
                        if (ast->u.destruct_decl.slot_defaults) {
                            for (int i = 0; i < ast->u.destruct_decl.slot_count; i++) {
                                if (ast->u.destruct_decl.slot_defaults[i]) { has_default = 1; break; }
                            }
                        }
                        if (!has_default) {
                            error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                "右侧数组字面量只有 %d 个元素，解构需要 %d 个 ⇒ 运行期会索引越界；"
                                "请补齐元素或减少槽位",
                                lit->u.array.count, ast->u.destruct_decl.slot_count);
                        }
                    }
                }

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
                                error_add_at_fmt(ERR_TYPE_MISMATCH, ast->line, ast->column,
                                    "解构槽位 %d ('%s') 超出返回值数量 %d",
                                    i, ast->u.destruct_decl.names[i], ret_count);
                            } else if (!type_is_compatible(slot_type, init_type->param_types[i])) {
                                error_add_at_fmt(ERR_TYPE_MISMATCH, ast->line, ast->column,
                                    "解构槽位 %d ('%s') 类型 %s 与返回值类型 %s 不兼容",
                                    i, ast->u.destruct_decl.names[i],
                                    type_kind_to_string(slot_kind),
                                    type_kind_to_string(init_type->param_types[i]->kind));
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
                                // ★ 2026-10-22：**有默认值的槽位豁免源类型检查**
                                //   缺键时用默认值（它保证类型），有键时值来自 any 字典 ——
                                //   与 `opts["k"] ?? 默认` 同一处境：运行时才是唯一判据。
                                //   SDL3 控件的 opts 就是 `Dict[any,any]`，不豁免的话
                                //   `var{"x": int = 0}(x) = opts` 永远编不过 ✗
                                if (ast->u.destruct_decl.slot_defaults &&
                                    ast->u.destruct_decl.slot_defaults[i]) {
                                    continue;
                                }
                                // 具体类型槽位：需要源值类型也是具体类型
                                if (val_is_any) {
                                    error_add_at_fmt(ERR_TYPE_MISMATCH, ast->line, ast->column,
                                        "解构声明槽位 %d ('%s') 类型 %s 与字典值类型 %s 不兼容（源值类型为 any，无法转换为具体类型）",
                                        i, ast->u.destruct_decl.names[i],
                                        type_kind_to_string(slot_kind),
                                        type_kind_to_string(val_kind));
                                } else if (!type_is_compatible(slot_type, init_type->value_type)) {
                                    error_add_at_fmt(ERR_TYPE_MISMATCH, ast->line, ast->column,
                                        "解构声明槽位 %d ('%s') 类型 %s 与字典值类型 %s 不兼容",
                                        i, ast->u.destruct_decl.names[i],
                                        type_kind_to_string(slot_kind),
                                        type_kind_to_string(val_kind));
                                }
                            }
                        } else if (init_type->kind == TYPE_ANY || init_type->kind == TYPE_INFER) {
                            // 返回类型为 any 或 infer（如无返回类型注解的函数），不允许解构
                            error_add_at_fmt(ERR_TYPE_MISMATCH, ast->line, ast->column,
                                "字典解构要求初始化值为具体类型的 Dict（如 Dict[string, int]），"
                                "实际类型为 %s，无法保证类型安全",
                                type_kind_to_string(init_type->kind));
                        } else if (init_type->kind != TYPE_NULL) {
                            error_add_at_fmt(ERR_TYPE_MISMATCH, ast->line, ast->column,
                                "字典解构要求初始化值为 Dict 类型，实际为 %s",
                                type_kind_to_string(init_type->kind));
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
                                    error_add_at_fmt(ERR_TYPE_MISMATCH, ast->line, ast->column,
                                        "解构声明槽位 %d ('%s') 类型 %s 与数组元素类型 %s 不兼容（源元素类型为 any，无法转换为具体类型）",
                                        i, ast->u.destruct_decl.names[i],
                                        type_kind_to_string(slot_kind),
                                        type_kind_to_string(elem_kind));
                                } else if (!type_is_compatible(slot_type, init_type->element_type)) {
                                    error_add_at_fmt(ERR_TYPE_MISMATCH, ast->line, ast->column,
                                        "解构声明槽位 %d ('%s') 类型 %s 与数组元素类型 %s 不兼容",
                                        i, ast->u.destruct_decl.names[i],
                                        type_kind_to_string(slot_kind),
                                        type_kind_to_string(elem_kind));
                                }
                            }
                        } else if (init_type->kind == TYPE_ANY || init_type->kind == TYPE_INFER) {
                            // 返回类型为 any 或 infer（如无返回类型注解的函数），不允许解构
                            // ★ 2026-10-24（round5 的 N4/Q3）：文案原来只说"要求 Array"，
                            //   完全指不到真因 —— 实测最常见的来源是**多返回值/闭包函数没写
                            //   返回签名**（`func f() { return 1, "hi" }` ⇒ any）。这里按节点
                            //   形态给出可操作提示（是函数调用才提，避免误导普通 any 变量）✓
                            int init_is_call = (ast->u.destruct_decl.init &&
                                                ast->u.destruct_decl.init->kind == AST_CALL) ||
                                               (ast->u.destruct_decl.init &&
                                                ast->u.destruct_decl.init->kind == AST_MODULE_CALL);
                            error_add_at_fmt(ERR_TYPE_MISMATCH, ast->line, ast->column,
                                "数组解构要求初始化值为具体类型的 Array（如 Array[int]），"
                                "实际类型为 %s，无法保证类型安全%s",
                                type_kind_to_string(init_type->kind),
                                init_is_call
                                  ? "（被调函数没有返回签名时返回值就是 any —— 请给它补上 "
                                    "`: [类型,…]`，例如 `func f(): [int, string] { return 1, \"hi\" }`）"
                                  : "");
                        } else if (init_type->kind != TYPE_NULL) {
                            error_add_at_fmt(ERR_TYPE_MISMATCH, ast->line, ast->column,
                                "数组解构要求初始化值为 Array 类型，实际为 %s",
                                type_kind_to_string(init_type->kind));
                        }
                    }
                    type_free(init_type);
                }
            }
            break;
        }