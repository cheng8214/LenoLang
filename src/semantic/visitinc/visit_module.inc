        case AST_IMPORT: {
            if (strstr(ast->u.import.module_name, ".leno") != NULL) {
                const char* alias = ast->u.import.alias;
                char* module_name = NULL;
                // 在模块上下文中使用 SYM_MODULE，否则使用 SYM_GLOBAL
                SymKind module_sym_kind = s->is_module ? SYM_MODULE : SYM_GLOBAL;
                if (!alias) {
                    alias = strrchr(ast->u.import.module_name, '/');
                    if (!alias) alias = strrchr(ast->u.import.module_name, '\\');
                    if (alias) alias++;
                    else alias = ast->u.import.module_name;
                    
                    module_name = strdup(alias);
                    char* dot = strrchr(module_name, '.');
                    if (dot) *dot = '\0';
                    
                    Symbol* sym = scope_define(s->current, module_name, module_sym_kind);
                    if (sym) {
                        sym->type = type_new(TYPE_DICT);
                    }
                } else {
                    module_name = strdup(alias);
                    
                    Symbol* sym = scope_define(s->current, alias, module_sym_kind);
                    if (sym) {
                        sym->type = type_new(TYPE_DICT);
                    }
                }
                
                if (s->imported_module_count < 64 && ast->u.import.file_path) {
                    // ★ N7（2026-10-24）：**同一个别名 import 了两个不同模块** ⇒ 编译期报错。
                    //   此前完全静默：`find_imported_module` 线性查**第一命中** ⇒ 第二个 import
                    //   等于没写，`m.bump()` 报「模块 'm' 中没有方法 'bump'」，用户根本看不出
                    //   "别名早被前一个 import 占了"（md10c 实测 ✗）
                    //   判据：别名已存在且指向**另一个文件** ⇒ 冲突；同一文件的重复 import 合法 ✓
                    ImportedModuleInfo* dup_info = find_imported_module(s, module_name);
                    if (dup_info && dup_info->file_path &&
                        !semantic_same_module_file(dup_info->file_path, ast->u.import.file_path)) {
                        error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                            "模块别名 '%s' 已被前面的 import 占用（%s）—— 一个别名不能指向两个模块；"
                            "请给其中一个换别名（如 `as %s2`），或删掉多余的 import",
                            module_name, dup_info->file_path, module_name);
                        break;   // 不登记：避免"两个 import 抢一个别名"的静默覆盖
                    }

                    ImportedModuleInfo* info = &s->imported_modules[s->imported_module_count++];
                    info->alias = module_name;
                    info->file_path = strdup(ast->u.import.file_path);

                    // 取（必要时扫描）模块符号表 —— **走进程内记忆化**，与依赖侧（扫描器解析
                    // use / 变量类型依赖时走的 module_symbol_table_get_shared）**共用同一张表**。
                    // ⚠ 2026-09-18：此前这里是 create + scan，**绕过记忆化** ⇒ 同一个文件被反复
                    //   重扫：实测 47 模块的应用冷扫建表 **380 次**、`sdl_core.leno` 被建 **41 次**，
                    //   其中 327 次的 module_path 还是**裸文件名**（"没解析过"的痕迹）。
                    //   记忆化的键是**解析后的绝对路径** ⇒ 同一文件无论被裸名还是绝对路径引用，
                    //   都只扫一次；且与依赖侧同实例（不再出现"同一模块两张表"）。
                    //   返回的表由记忆化持有：调用方只读，destroy 对它已是 no-op
                    //   （module_symbol_table_destroy 的 shared 分支）。
                    // 扫描失败（文件不存在 / 读取失败）返回 NULL。
                    const char* current_file = error_get_filename();
                    info->sym_table = module_symbol_table_get_shared(ast->u.import.file_path, current_file);
                    if (!info->sym_table) {
                        // 注册错误并跳过后续注册：否则空符号表会让下游变量被推断为 any，掩盖根因
                        error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                         "找不到模块文件: %s", ast->u.import.file_path);
                        // 标记模块信息为加载失败，避免后续任何注册
                        info->load_failed = 1;
                        // 设置全局标志，抑制下游级联 any 错误
                        s->has_module_load_failure = 1;
                    } else {

                        // 注意：不再将模块中的 struct 定义注册到当前作用域
                        // struct 必须通过模块名访问，例如：new module.Point()
                        // 这样可以保持与 func 和 var 的一致性

                        // 但是，需要将模块中的 face 定义注册到全局表
                        // 这样模块中的 struct 可以实现这些 face
                        for (int fi = 0; fi < info->sym_table->face_count; fi++) {
                            ModuleFaceSymbol* face_sym = &info->sym_table->faces[fi];
                            if (!face_def_find(face_sym->name)) {
                                ObjFaceDef* fdef = face_def_new(face_sym->name, face_sym->method_count);
                                if (fdef) {
                                    for (int mi = 0; mi < face_sym->method_count; mi++) {
                                        fdef->methods[mi].name = strdup(face_sym->methods[mi].name);
                                        fdef->methods[mi].return_type = type_new(face_sym->methods[mi].return_type);
                                        fdef->methods[mi].param_count = face_sym->methods[mi].param_count;
                                    }
                                    face_def_register(fdef);
                                }
                            }
                        }

                        // 注意：不再自动将模块的 alias 注册到当前作用域
                        // alias 必须通过 use 语句显式导入才能使用
                        // （use 语句的处理在下方）

                        // 将模块中的 struct 定义注册到全局表
                        // 这样类型检查时可以验证 struct 是否实现了 face
                        for (int si = 0; si < info->sym_table->struct_count; si++) {
                            ModuleStructSymbol* struct_sym = &info->sym_table->structs[si];
                            if (struct_sym->is_cstruct) continue;
                            ObjStructDef* existing_def = struct_def_find(struct_sym->name);
                            // 若已有定义但缺少关联常量，而模块符号表有常量，则补充
                            if (existing_def && existing_def->const_count == 0 && struct_sym->const_count > 0) {
                                existing_def->const_count = struct_sym->const_count;
                                existing_def->const_names = (char**)malloc(sizeof(char*) * existing_def->const_count);
                                existing_def->const_values = (Value*)calloc(existing_def->const_count, sizeof(Value));
                                for (int ci = 0; ci < existing_def->const_count; ci++) {
                                    existing_def->const_names[ci] = strdup(struct_sym->const_names[ci]);
                                    existing_def->const_values[ci] = val_null();
                                    if (struct_sym->const_value_strs && struct_sym->const_value_strs[ci]) {
                                        const char* vs = struct_sym->const_value_strs[ci];
                                        while (*vs == ' ' || *vs == '\t') vs++;
                                        if (strcmp(vs, "true") == 0) {
                                            existing_def->const_values[ci] = val_bool(1);
                                        } else if (strcmp(vs, "false") == 0) {
                                            existing_def->const_values[ci] = val_bool(0);
                                        } else if (strcmp(vs, "null") == 0) {
                                            existing_def->const_values[ci] = val_null();
                                        } else if (vs[0] == '"' || vs[0] == '\'') {
                                            char q = vs[0];
                                            const char* end = vs + 1;
                                            while (*end && *end != q) end++;
                                            int slen = (int)(end - vs - 1);
                                            existing_def->const_values[ci] = val_obj((Object*)str_new(vs + 1, slen));
                                        } else if (vs[0] == '0' && (vs[1] == 'x' || vs[1] == 'X')) {
                                            existing_def->const_values[ci] = val_int((int64_t)strtoll(vs, NULL, 16));
                                        } else if (strchr(vs, '.') || strchr(vs, 'e') || strchr(vs, 'E')) {
                                            existing_def->const_values[ci] = val_float(atof(vs));
                                        } else {
                                            existing_def->const_values[ci] = val_int((int64_t)strtoll(vs, NULL, 10));
                                        }
                                    }
                                }
                                continue;
                            }
                            if (!existing_def) {
                                ObjStructDef* sdef = struct_def_new(struct_sym->name, struct_sym->field_count, struct_sym->method_count);
                                if (sdef) {
                                    // 设置泛型类型参数数量
                                    sdef->type_param_count = struct_sym->type_param_count;
                                    // 设置泛型类型参数名称
                                    if (struct_sym->type_param_count > 0 && struct_sym->type_param_names) {
                                        sdef->type_param_names = (char**)malloc(sizeof(char*) * struct_sym->type_param_count);
                                        for (int tpi = 0; tpi < struct_sym->type_param_count; tpi++) {
                                            sdef->type_param_names[tpi] = strdup(struct_sym->type_param_names[tpi]);
                                        }
                                    }

                                    // 设置 struct 方法名
                                    for (int mi = 0; mi < struct_sym->method_count; mi++) {
                                        const char* full_method_name = struct_sym->methods[mi].name;
                                        const char* method_name = full_method_name;
                                        const char* sep = strstr(full_method_name, "::");
                                        if (sep) {
                                            method_name = sep + 2;
                                        }
                                        sdef->methods[mi].name = strdup(method_name);
                                    }

                                    // 设置 impl 信息
                                    if (struct_sym->impl_count > 0) {
                                        sdef->impl_count = struct_sym->impl_count;
                                        sdef->impl_names = (char**)malloc(sizeof(char*) * struct_sym->impl_count);
                                        for (int ii = 0; ii < struct_sym->impl_count; ii++) {
                                            sdef->impl_names[ii] = strdup(struct_sym->impl_names[ii]);
                                        }
                                    }

                                    // 设置关联常量（从模块符号表复制名称和解析值）
                                    if (struct_sym->const_count > 0) {
                                        sdef->const_count = struct_sym->const_count;
                                        sdef->const_names = (char**)malloc(sizeof(char*) * sdef->const_count);
                                        sdef->const_values = (Value*)calloc(sdef->const_count, sizeof(Value));
                                        for (int ci = 0; ci < sdef->const_count; ci++) {
                                            sdef->const_names[ci] = strdup(struct_sym->const_names[ci]);
                                            sdef->const_values[ci] = val_null();
                                            // 解析常量值字符串
                                            if (struct_sym->const_value_strs && struct_sym->const_value_strs[ci]) {
                                                const char* vs = struct_sym->const_value_strs[ci];
                                                // 跳过前导空白
                                                while (*vs == ' ' || *vs == '\t') vs++;
                                                if (strcmp(vs, "true") == 0) {
                                                    sdef->const_values[ci] = val_bool(1);
                                                } else if (strcmp(vs, "false") == 0) {
                                                    sdef->const_values[ci] = val_bool(0);
                                                } else if (strcmp(vs, "null") == 0) {
                                                    sdef->const_values[ci] = val_null();
                                                } else if (vs[0] == '"' || vs[0] == '\'') {
                                                    // 字符串字面量：去除引号
                                                    char q = vs[0];
                                                    const char* end = vs + 1;
                                                    while (*end && *end != q) end++;
                                                    int slen = (int)(end - vs - 1);
                                                    sdef->const_values[ci] = val_obj((Object*)str_new(vs + 1, slen));
                                                } else if (vs[0] == '0' && (vs[1] == 'x' || vs[1] == 'X')) {
                                                    // 十六进制整数
                                                    sdef->const_values[ci] = val_int((int64_t)strtoll(vs, NULL, 16));
                                                } else if (strchr(vs, '.') || strchr(vs, 'e') || strchr(vs, 'E')) {
                                                    // 浮点数
                                                    sdef->const_values[ci] = val_float(atof(vs));
                                                } else {
                                                    // 十进制整数
                                                    sdef->const_values[ci] = val_int((int64_t)strtoll(vs, NULL, 10));
                                                }
                                            }
                                        }
                                    }

                                    struct_def_register(sdef);
                                }
                            }
                        }
                        } // end else（sym_table 非 NULL ⇒ 必然已扫描完成）
                } else {
                    free(module_name);
                }
            } else {
                // 原生模块：运行时通过 OP_LOAD_NATIVE_MODULE 加载
                // 编译期先校验模块名。原生模块清单本身是编译期确定的（native.c 的
                // module_init_table），名字写错必须在这里报错：否则字节码带着这个名字跑到
                // 运行期，native_init_module 找不到（此前是静默 no-op），直到调用该模块的
                // 方法才失败 —— 报错位置与真实原因不符，且从不调用就永不报错。
                if (!native_module_is_registered(ast->u.import.module_name)) {
                    error_add_at_fmt(ERR_UNDEFINED_VAR, ast->line, ast->column,
                             "未知的原生模块 '%s'（可用的原生模块: %s）",
                             ast->u.import.module_name, native_module_names_csv());
                    break;
                }

                // 记录原生模块导入，用于编译时检查
                const char* alias = ast->u.import.alias ? ast->u.import.alias : ast->u.import.module_name;
                if (s->imported_module_count < 64) {
                    ImportedModuleInfo* info = &s->imported_modules[s->imported_module_count++];
                    info->alias = strdup(alias);
                    info->file_path = NULL;  // 原生模块没有文件路径
                }

                // 注册模块别名（如果有显式别名）
                if (ast->u.import.alias) {
                    native_register_module_alias(ast->u.import.alias, ast->u.import.module_name);
                }
                
                // 将别名或模块名定义为一个全局符号，以便在模块调用时可以解析
                // 在模块上下文中使用 SYM_MODULE，否则使用 SYM_GLOBAL
                SymKind module_sym_kind = s->is_module ? SYM_MODULE : SYM_GLOBAL;
                Symbol* sym = scope_define(s->current, alias, module_sym_kind);
                if (sym) {
                    sym->type = type_new(TYPE_DICT);
                }
            }
            break;
        }

        case AST_USE: {
            // 类型/符号导入的**唯一实现**在 semantic_import_use_symbol —— 原来那 390 行分派体
            //   就长在这里；抽成函数是为了让**类型位置写限定名**（`m.Pt`，US1）复用同一条路径 ✓
            //   （两份分派会让"报错文案 / 可用性判据"悄悄漂移 ✗ —— 本仓纪律：单一事实来源 ✓）
            semantic_import_use_symbol(s, ast->u.use.module_name, ast->u.use.symbol_name,
                                       ast->line, ast->column);
            break;
        }

        case AST_MODULE_CALL: {
            visit_list(s, &ast->u.module_call.args);
            const char* actual_module = native_resolve_module_alias(ast->u.module_call.module_name);

            int is_native_module = native_is_module(actual_module);

            int is_imported_module = 0;
            ImportedModuleInfo* module_info = find_imported_module(s, ast->u.module_call.module_name);
            if (module_info) {
                is_imported_module = 1;
                // 模块加载失败时，跳过后续检查（错误已注册），避免级联 any 错误
                if (module_info->load_failed) break;
            }

            // 检查模块是否已导入：原生模块必须显式导入，用户模块也必须导入
            if (is_native_module && !is_imported_module) {
                error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                         "模块 '%s' 未导入，请先使用 'import %s' 导入模块",
                         ast->u.module_call.module_name, ast->u.module_call.module_name);
                break;
            }

            if (is_native_module) {
                // 原生模块调用，检查方法是否存在
                // 先检查方法是否存在（通过查找元信息）
                extern ModuleMethodMeta* native_find_module_method(const char* module_name, const char* method_name);
                ModuleMethodMeta* meta = native_find_module_method(actual_module, ast->u.module_call.method_name);
                
                if (!meta) {
                    // 方法不存在，尝试列出该模块的可用方法作为提示
                    extern ModuleMethodMeta* native_get_module_method_metas(const char* module_name, int* count);
                    int method_count = 0;
                    ModuleMethodMeta* all_methods = native_get_module_method_metas(actual_module, &method_count);
                    if (all_methods && method_count > 0) {
                        // 构建可用方法列表
                        char available[BUFFER_MEDIUM];
                        int pos = 0;
                        for (int mi = 0; mi < method_count && pos < BUFFER_MEDIUM - 50; mi++) {
                            if (mi > 0) pos += snprintf(available + pos, BUFFER_MEDIUM - pos, ", ");
                            pos += snprintf(available + pos, BUFFER_MEDIUM - pos, "%s", all_methods[mi].method_name);
                        }
                        error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                 "未找到模块方法: %s.%s（可用: %s）",
                                 actual_module, ast->u.module_call.method_name, available);
                        free(all_methods);
                    } else {
                        error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                 "未找到模块方法: %s.%s",
                                 actual_module, ast->u.module_call.method_name);
                    }
                } else {
                    // 方法存在，检查参数数量
                    int expected_arity = meta->arity;
                    int actual_arity = ast->u.module_call.args.count;

                    // ★★ 2026-10-10 新增：`printf` 名字骗人（WARN_PRINTF_NO_FORMAT）✓
                    //   与全局 `printf(...)` 走**同一个**共享实现（visit_expr.inc 那条是另一入口 ✓）
                    if (strcmp(actual_module, "io") == 0 &&
                        strcmp(ast->u.module_call.method_name, "printf") == 0) {
                        semantic_check_printf_style("io.printf", &ast->u.module_call.args,
                                                    ast->line, ast->column);
                    }

                    // ★★ 2026-10-10（A 组第 3 条后半）：`threads.start` 的**跨线程实参**拦截 ✓
                    //   把"运行期才报不能跨线程传递此类型"提前到编译期 ✓ 见共享实现 ✓
                    //   ★ 2026-10-12（TH1）口径已收窄：**只拦静态类型确定为 `cstruct`** 的实参 ✓
                    //     —— struct / face 已能跨线程（深克隆 + 接收侧按定义重建），原先拦它们是**误报** ✗
                    //     （见 docs/线程并发能力修复追踪.md 的 TH1 ✓）
                    if (strcmp(actual_module, "threads") == 0 &&
                        strcmp(ast->u.module_call.method_name, "start") == 0) {
                        semantic_check_cross_thread_args(s, &ast->u.module_call.args,
                                                         ast->line, ast->column);
                    }

                    // arity >= 0: 固定参数数量
                    if (expected_arity >= 0 && actual_arity != expected_arity) {
                        // `.map()` 那条提示做成尾串（原来是往 msg 尾部 strncat ✓ 等价）
                        const char* map_hint =
                            (strcmp(ast->u.module_call.method_name, "map") == 0 && actual_arity == 1 && expected_arity == 2)
                            ? "\n  提示: .map() 会传递 (item, index) 两个参数，\n"
                              "     函数签名需加第二个参数，如 func compress(string w, int _): string"
                            : "";
                        error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                 "%s.%s 参数数量不匹配: 期望 %d, 实际 %d%s",
                                 actual_module, ast->u.module_call.method_name, expected_arity, actual_arity,
                                 map_hint);
                    }
                    // arity < 0: 可变参数，检查是否少于 min_arity
                    else if (expected_arity < 0 && meta->min_arity >= 0 && actual_arity < meta->min_arity) {
                        error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                 "%s.%s 参数过少: 最少 %d 个, 实际 %d",
                                 actual_module, ast->u.module_call.method_name, meta->min_arity, actual_arity);
                    }
                    // arity < 0: 可变参数，检查是否超过 max_arity
                    else if (expected_arity < 0 && meta->max_arity >= 0 && actual_arity > meta->max_arity) {
                        error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                 "%s.%s 参数过多: 最多 %d 个, 实际 %d",
                                 actual_module, ast->u.module_call.method_name, meta->max_arity, actual_arity);
                    }
                    
                    // const 检查：模块方法调用不能修改 const 容器
                    {
                        const char* m_name = ast->u.module_call.method_name;
                        if (type_utils_is_array_element_mutator(m_name) ||
                            type_utils_is_dict_element_mutator(m_name)) {
                            if (actual_arity > 0) {
                                Ast* first_arg = ast->u.module_call.args.items[0];
                                if (first_arg && first_arg->kind == AST_VAR) {
                                    SymRef tmp_ref;
                                    memset(&tmp_ref, 0, sizeof(tmp_ref));
                                    Symbol* sym = resolve_variable_with_upvalue(s, first_arg->u.var.name, &tmp_ref);
                                    if (sym && sym->is_const) {
                                        error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                                         "不能对 const 变量 '%s' 调用修改方法 '%s'", first_arg->u.var.name, m_name);
                                    }
                                } else if (first_arg && first_arg->kind == AST_MODULE_ACCESS) {
                                    const char* mod_name = first_arg->u.module_access.module_name;
                                    const char* mem_name = first_arg->u.module_access.member_name;
                                    ImportedModuleInfo* mod_info = find_imported_module(s, mod_name);
                                    if (mod_info && mod_info->sym_table) {
                                        ModuleVarSymbol* var_sym = module_symbol_table_find_var(mod_info->sym_table, mem_name);
                                        if (var_sym && var_sym->is_const) {
                                            error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                                             "不能对 const 变量 '%s.%s' 调用修改方法 '%s'", mod_name, mem_name, m_name);
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // 检查参数类型
                    // 先找到第一个数组参数及其元素类型
                    TypeInfo* array_elem_type = NULL;
                    for (int j = 0; j < actual_arity; j++) {
                        TypeKind param_type = native_get_module_method_param_type(actual_module, ast->u.module_call.method_name, j);
                        if (param_type == TYPE_ARRAY) {
                            Ast* arg = ast->u.module_call.args.items[j];
                            TypeInfo* arg_type = infer_expr_type(s, arg);
                            if (arg_type && arg_type->kind == TYPE_ARRAY && arg_type->element_type) {
                                array_elem_type = arg_type->element_type;
                                break;
                            }
                        }
                    }
                    
                    for (int i = 0; i < actual_arity; i++) {
                        TypeKind expected_type = native_get_module_method_param_type(actual_module, ast->u.module_call.method_name, i);
                        Ast* arg = ast->u.module_call.args.items[i];
                        TypeInfo* arg_type = infer_expr_type(s, arg);
                        
                        // 如果注册表中是 TYPE_ANY，且已知数组元素类型，则用元素类型检查
                        if (expected_type == TYPE_ANY && array_elem_type) {
                            expected_type = array_elem_type->kind;
                        }
                        
                        // 处理数组元素修改方法（add/insert）：向空数组添加第一个元素时更新类型
                        const char* method_name = ast->u.module_call.method_name;
                        if (type_utils_is_array_element_mutator(method_name)) {
                        int elem_param_idx = type_utils_get_array_element_param_index(method_name, 1); // 1 = 模块调用
                        if (i == elem_param_idx) {
                            // 获取第一个参数（数组）
                            Ast* arr_arg = ast->u.module_call.args.items[0];
                            Symbol* arr_sym = type_utils_resolve_var_symbol(s, arr_arg);
                            if (type_utils_try_update_array_element_type(arr_sym, arg_type)) {
                                // 类型已更新，同时更新期望参数类型
                                expected_type = arg_type->kind;
                            }
                        }
                    }
                    
                    // ★ 泛型参数 → 具体类型 native 形参：**拦**（2026-10-02；理由见 semantic_reject_generic_arg_to_native）
                    //   ⚠ 容器元素 mutator（add/insert/set）跳过：那是"存入"（Value 带 tag、不越界）✓
                    if (!type_utils_is_array_element_mutator(method_name) &&
                        !type_utils_is_dict_element_mutator(method_name)) {
                        char callee[256];
                        snprintf(callee, sizeof(callee), "%s.%s", actual_module, method_name);
                        if (semantic_check_native_arg(s, ast, arg_type, expected_type, callee, i)) {
                            if (arg_type) type_free(arg_type);
                            continue;   // 已由需求机制接管 ⇒ 跳过常规比较（见该函数注释 ✓）
                        }
                    }

                    // ⚠ v3.2.7：**删掉了 `arg_type->kind != TYPE_ANY` 这层豁免**。
                    //   老写法是"实参静态类型是 any 就跳过检查" —— 可 any 到运行期可能是任何东西
                    //   （null / int / ...），而 native 侧普遍**无校验强转**（如
                    //   `(ObjString*)val_as_obj(args[0])`）⇒ 实测直接崩：
                    //       var a = _env("不存在的变量")   // 静态 any、运行期 null
                    //       regexs.find_all(a, "[0-9]+")  ⇒ 0xC0000005
                    //   语言在"字段赋值"处的口径本来就是**要求显式转换**
                    //   （"期望类型: int / 实际类型: any / 提示：使用 _int(value) 显式转换"）
                    //   ⇒ 这里对齐成同一口径：静态 any 传具体类型形参必须 `_str() / _int() / as`。
                    //   运行期还有 `native_check_module_params()` 兜底（动态路径无法静态判定）。
                    if (expected_type != TYPE_ANY && arg_type && arg_type->kind != expected_type) {
                        // ── 判据：**与赋值/其它通道共用同一个实现** `type_is_compatible`（2026-10-02 合并）──
                        //   为什么必须合并：这里原先自己抄了一份"判据链"（int→float、bigint、
                        //   C 布局 int、f32/f64、str8/str16、Ptr[T]→Ptr、cfunc、clib→Ptr…），而
                        //   `type_is_compatible` 里**本来就有**同样这些规则 ⇒ 同一规则两处写：
                        //   新增/修正一条要改两处，漏一处就出现"赋值能过、传参却报错"这类不一致
                        //   （本轮实测到一次：`clib → Ptr` 只在 type.c 里有时，ffi.call* 那 6 处照旧报错 ✗）。
                        //   口径保持不变的两点：
                        //     · **种类相同 ⇒ 直接兼容**：这里历来只按 Kind 判、不比结构体名（保持老行为 ✓）
                        //     · **静态 any 仍必须拒绝**（v3.2.7 定案：native 侧普遍无校验强转，any 到运行期
                        //       可能是 null ⇒ 必须显式 `_str() / _int() / as`）；`type_is_compatible` 对
                        //       "源是 any" 没有放行规则、尾部是 `return 0` ⇒ 自然拒绝 ✓
                        bool type_compatible = (arg_type->kind == expected_type);
                        if (!type_compatible) {
                            TypeInfo expected_ti;
                            memset(&expected_ti, 0, sizeof(expected_ti));
                            expected_ti.kind = expected_type;
                            type_compatible = (type_is_compatible(&expected_ti, arg_type) != 0);
                        }
                        if (!type_compatible) {
                            const char* hint = get_type_conversion_hint(expected_type, arg_type->kind);
                            if (hint) {
                                error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                         "%s.%s 参数 %d 类型错误: 期望 '%s'，但传入 '%s'\n%s",
                                         actual_module, ast->u.module_call.method_name, i + 1,
                                         type_kind_to_string(expected_type), type_to_string(arg_type), hint);
                            } else {
                                error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                         "%s.%s 参数 %d 类型错误: 期望 '%s'，但传入 '%s'",
                                         actual_module, ast->u.module_call.method_name, i + 1,
                                         type_kind_to_string(expected_type), type_to_string(arg_type));
                            }
                        }
                    }
                    
                    if (arg_type) type_free(arg_type);
                } // 关闭 for 循环
                } // 关闭 else 块（方法存在时的处理）
                
                // 如果返回类型是 TYPE_ANY，方法可能不存在，但不做错误报告
                // 因为某些原生方法可能返回 TYPE_ANY
            } else if (is_imported_module && module_info && module_info->file_path) {
                // 用户模块调用，检查方法是否存在
                const char* current_file = error_get_filename();
                
                // 首先检查是否是 struct 定义。
                // 注意：**不要**在这里再 create + scan 一遍模块 —— import 阶段已经把整张
                // 符号表扫好并存在 module_info->sym_table 里（下面查函数用的就是它）。
                // 此前每个模块调用点都白扫一遍整个模块文件（一个调用点 = 3 次全文件遍历）。
                int is_struct = 0;
                if (module_info->sym_table && !module_info->load_failed) {
                    ModuleStructSymbol* struct_sym = module_symbol_table_find_struct(
                        module_info->sym_table, ast->u.module_call.method_name);
                    if (struct_sym) {
                        is_struct = 1;
                        // 是 struct 定义，提示使用 new 关键字实例化
                        error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                "struct '%s.%s' 需要使用 new 关键字实例化，例如: new %s.%s()",
                                ast->u.module_call.module_name, struct_sym->name,
                                ast->u.module_call.module_name, struct_sym->name);
                    }
                }
                
                // 如果不是 struct，检查是否是函数，并推断返回类型
                if (!is_struct) {
                    int has_method = module_has_method(module_info->file_path, current_file, 
                                                       ast->u.module_call.method_name);
                    if (has_method == 0) {
                        error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                 "模块 '%s' 中没有方法 '%s'",
                                 ast->u.module_call.module_name, ast->u.module_call.method_name);
                        semantic_poison_expr(ast);   /* 根因已报 ⇒ 抑制下游级联 */
                    } else if (module_info->sym_table) {
                        // 从模块符号表推断返回类型
                        ModuleFuncSymbol* func_sym = module_symbol_table_find_func(
                            module_info->sym_table, ast->u.module_call.method_name);
                        if (func_sym) {
                            // 检查参数数量（支持默认参数）
                            int actual_cnt = ast->u.module_call.args.count;
                            int expected_cnt = func_sym->param_count;
                            int default_cnt = func_sym->default_count;
                            int required_cnt = expected_cnt - default_cnt;
                            if (actual_cnt < required_cnt || actual_cnt > expected_cnt) {
                                if (actual_cnt < required_cnt) {
                                    error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                             "%s.%s 参数数量不足: 至少需要 %d 个参数，实际传入 %d 个",
                                             ast->u.module_call.module_name, ast->u.module_call.method_name,
                                             required_cnt, actual_cnt);
                                } else {
                                    error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                             "%s.%s 参数数量过多: 最多接受 %d 个参数，实际传入 %d 个",
                                             ast->u.module_call.module_name, ast->u.module_call.method_name,
                                             expected_cnt, actual_cnt);
                                }
                            }
                            // ★ 参数**类型**检查（⑱）：此前只查个数 ⇒ 跨模块 `a.take("oops")`
                            //   （`take(int v)`）**零诊断**（同文件会报 —— 那条走 AST 里的 func_def）。
                            // 数据来源：**⑱ 转正后改读 `ModuleFuncSymbol.param_types`** —— 扫描器用
                            //   **唯一实现** `mod_scan_params`（`inc/sym_table_params.inc`）填的，
                            //   随 `.lenosymc` v29 往返。本处原先那段"就地解析 `param_text`"已删除
                            //   （那是"参数表 → 类型"的第三份实现，⑳ 随之收敛到只剩 scan_struct 那份）。
                            // 保守闸门（与 ⑯/⑰-2 同款）：判 4 个内置基本类型 + **有名字且不带模块限定名**的
                            //   聚合类型（struct/face/cstruct/clib，名字取自 param_struct_names）；
                            //   C 布局（i32/f64/str8…）、泛型、无名字的聚合、含 '.' 的限定名一律跳过
                            //   —— 宁漏不误报。两侧都是聚合时额外保守（kind 或名字不齐就跳过）。
                            if (func_sym->param_types && actual_cnt > 0) {
                                int judge_cnt = actual_cnt;
                                if (judge_cnt > (int)ast->u.module_call.args.count) {
                                    judge_cnt = (int)ast->u.module_call.args.count;
                                }
                                for (int pi = 0; pi < judge_cnt; pi++) {
                                    TypeKind ek = func_sym->param_types[pi];
                                    int is_basic = (ek == TYPE_INT || ek == TYPE_FLOAT ||
                                                    ek == TYPE_STRING || ek == TYPE_BOOL);
                                    const char* ek_name = (func_sym->param_struct_names)
                                                          ? func_sym->param_struct_names[pi] : NULL;
                                    int is_agg = (ek == TYPE_STRUCT || ek == TYPE_FACE ||
                                                  ek == TYPE_CSTRUCT || ek == TYPE_CLIB);
                                    if (is_agg && (!ek_name || strchr(ek_name, '.'))) is_agg = 0;
                                    if (!is_basic && !is_agg) continue;

                                    TypeInfo* expected_type = type_new(ek);
                                    if (is_agg && ek_name) expected_type->struct_name = strdup(ek_name);
                                    TypeInfo* arg_type = infer_expr_type(s, ast->u.module_call.args.items[pi]);
                                    if (arg_type && arg_type->kind != TYPE_ANY) {
                                        int skip = 0;
                                        if (is_agg) {
                                            int arg_agg = (arg_type->kind == TYPE_STRUCT ||
                                                           arg_type->kind == TYPE_FACE ||
                                                           arg_type->kind == TYPE_CSTRUCT ||
                                                           arg_type->kind == TYPE_CLIB);
                                            // 两侧都是聚合：保守（kind 或名字不齐就不判）
                                            if (arg_agg && (arg_type->kind != ek ||
                                                            !arg_type->struct_name ||
                                                            strchr(arg_type->struct_name, '.'))) {
                                                skip = 1;
                                            }
                                        }
                                        if (!skip && !type_is_compatible(expected_type, arg_type)) {
                                            char idx_str[16];
                                            snprintf(idx_str, sizeof(idx_str), "%d", pi + 1);
                                            error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column, "%s",
                                                format_type_error_text("%s3 第 %s4 个参数类型不匹配: 期望 %s1, 实际 %s2",
                                                    expected_type, arg_type,
                                                    ast->u.module_call.method_name, idx_str, NULL));
                                        }
                                    }
                                    if (arg_type) type_free(arg_type);
                                    type_free(expected_type);
                                }
                            }
                            // 推断返回类型
                            if (func_sym->return_type != TYPE_ANY && func_sym->return_type != TYPE_INFER) {
                                if (func_sym->return_type_info) {
                                    ast->cached_type = type_copy(func_sym->return_type_info);
                                    // ★ v31：把**调用点的类型实参按名代入**返回类型里的占位符。
                                    //   此前模块函数符号只记了类型形参**数量** ⇒ `gm.makePair[string, int]("x", 10)`
                                    //   的静态类型停在 `Pair[K, V]`（占位符）⇒ `string k = p.getKey()`
                                    //   报「期望 string，实际 struct K」（实测）。
                                    //   助手与 struct 方法侧**同一套**（`semantic_substitute_generic_param` ——
                                    //   它现为 `type_substitute` 的转发，唯一实现 ✓；两者都认 TYPE_GENERIC_PARAM
                                    //   与历史遗留的 "TYPE_STRUCT 占位形参" ✓ 而 AST 路径自 2026-10-03 起只产生前者 ✓）。
                                    //   形参名 ↔ 调用实参**按位置**对应（声明 `f[K, V]` 的顺序 = 实参顺序）。
                                    if (func_sym->type_param_count > 0 && func_sym->type_param_names &&
                                        ast->u.module_call.generic_type_args) {
                                        int gp_n = func_sym->type_param_count < ast->u.module_call.generic_type_count
                                                   ? func_sym->type_param_count
                                                   : ast->u.module_call.generic_type_count;
                                        for (int gi = 0; gi < gp_n; gi++) {
                                            if (!func_sym->type_param_names[gi] ||
                                                !ast->u.module_call.generic_type_args[gi]) continue;
                                            TypeInfo* sub = semantic_substitute_generic_param(
                                                ast->cached_type, func_sym->type_param_names[gi],
                                                ast->u.module_call.generic_type_args[gi]);
                                            type_free(ast->cached_type);
                                            ast->cached_type = sub;
                                        }
                                    }
                                } else {
                                    ast->cached_type = type_new(func_sym->return_type);
                                    if (func_sym->return_struct_name) {
                                        ast->cached_type->struct_name = strdup(func_sym->return_struct_name);
                                    }
                                }
                            }
                        }
                    }
                }
            } else {
                // 不是模块调用，可能是实例方法调用
                // 使用resolve_variable_with_upvalue处理变量，支持闭包
                SymRef ref;
                memset(&ref, 0, sizeof(ref));
                Symbol* obj_sym = resolve_variable_with_upvalue(s, ast->u.module_call.module_name, &ref);
                // ★★ 2026-10-10 新增：**把 struct 类型名当接收者用** ⇒ 编译期就拦（十有八九是忘写 new ✓）
                //   实测 `C.go(1)`（C 是脚本 struct ✓）原先一路走到**运行期**才报
                //     「类型 string 上不存在方法 'go'」✗ —— 完全指不到点上 ✗（写的人找不到北 ✓）。
                //   ⚠ 判据是 **SYM_STRUCT**（脚本 struct ✓）而不是 `SYM_TYPE` ✗ —— 我用探针
                //     实测过：脚本 struct 的类型符号 kind = SYM_STRUCT(8) ✓、`type->kind` = TYPE_STRUCT ✓
                //     （`SYM_TYPE` 是 enum / face 那一路 ✓；`SYM_CSTRUCT` / `SYM_CLIB` 是另一路 ✓）。
                //   ⇒ 只认脚本 struct ✓：cstruct / clib 的 `Vec.malloc()` 这类**静态写法是合法的** ✓
                //     不能误伤 ✓（见 07_collections.inc 里那条兜底注释 ✓）。全套 434 条用例过后无回归 ✓。
                if (obj_sym && obj_sym->kind == SYM_STRUCT) {
                    error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                             "'%s' 是 struct **类型名**，不能直接当接收者用（本语言没有静态方法 ✓）——"
                             "想调它的方法请先造实例：var x = new %s()，然后 x.%s(...)",
                             ast->u.module_call.module_name, ast->u.module_call.module_name,
                             ast->u.module_call.method_name);
                    break;
                }
                if (obj_sym) {
                    const char* method_name = ast->u.module_call.method_name;
                    int expected_arity = -1;
                    TypeKind return_type;

                    // 检查是否是 struct 方法调用
                    int is_struct_method = 0;
                    Ast* method_def = NULL;

                    // 使用 infer_expr_type 获取对象类型（更准确）
                    Ast* temp_var = ast_new(AST_VAR, ast->line);
                    temp_var->u.var.name = strdup(ast->u.module_call.module_name);
                    temp_var->u.var.ref.kind = ref.kind;
                    temp_var->u.var.ref.index = ref.index;
                    temp_var->u.var.ref.name = ref.name ? strdup(ref.name) : NULL;
                    temp_var->u.var.ref.type_kind = ref.type_kind;
                    temp_var->cached_type = NULL;  // 确保 cached_type 为 NULL
                    TypeInfo* obj_type = infer_expr_type(s, temp_var);

                    // 检查类型是否是 clib
                    int is_clib_call = 0;
                    if (!is_struct_method && obj_type && obj_type->kind == TYPE_CLIB && obj_type->struct_name) {
                        // 查找 clib 符号
                        Symbol* clib_sym = scope_resolve_tree_bfs(s->root_scope, obj_type->struct_name);
                        if (!clib_sym) {
                            clib_sym = scope_resolve(s->current, obj_type->struct_name);
                        }
                        if (clib_sym && clib_sym->clib_func_count > 0) {
                            for (int fi = 0; fi < clib_sym->clib_func_count; fi++) {
                                if (strcmp(clib_sym->clib_func_names[fi], method_name) == 0) {
                                    is_clib_call = 1;
                                    // 检查参数数量
                                    int expected_count = clib_sym->clib_func_param_counts[fi];
                                    int actual_count = ast->u.module_call.args.count;
                                    if (actual_count != expected_count) {
                                        error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                                 "clib '%s' 函数 '%s' 参数数量不匹配: 期望 %d, 实际 %d",
                                                 obj_type->struct_name, method_name, expected_count, actual_count);
                                    }
                                    // 检查每个参数类型
                                    for (int pi = 0; pi < actual_count && pi < expected_count; pi++) {
                                        TypeInfo* expected_param = clib_sym->clib_func_param_types[fi][pi];
                                        Ast* arg = ast->u.module_call.args.items[pi];
                                        TypeInfo* arg_type = infer_expr_type(s, arg);
                                        if (expected_param && arg_type && !type_is_compatible(expected_param, arg_type)) {
                                            // ★ 参数序号走 `%s5`（format_type_error 的第 5 槽 ✓）⇒ 一行收尾：
                                            //   原来这里要"先出模板、再 snprintf 把 %d 填掉"两层缓冲，
                                            //   因为 `%s1..%s4` 四个槽被（两个类型 + 结构名 + 方法名）占满了 ✗
                                            char pi_str[16];
                                            snprintf(pi_str, sizeof(pi_str), "%d", pi + 1);
                                            error_add_at_fmt(ERR_TYPE_MISMATCH, ast->line, ast->column, "%s",
                                                format_type_error_text(
                                                    "clib '%s3' 函数 '%s4' 参数 %s5 类型不匹配: 期望 '%s1', 实际 '%s2'",
                                                    expected_param, arg_type,
                                                    obj_type->struct_name, method_name, pi_str));
                                        }
                                        if (arg_type) type_free(arg_type);
                                    }
                                    break;
                                }
                            }
                        } else if (clib_sym) {
                            // 导入的 clib 类型：函数表为空，允许任意调用（运行时验证）
                            is_clib_call = 1;
                        }
                        if (!is_clib_call) {
                            // 检查是否是因为未 use 导入 clib 类型
                            Symbol* local_clib = scope_resolve(s->current, obj_type->struct_name);
                            if (!local_clib) {
                                error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                         "clib '%s' 函数 '%s' 未找到，请添加 use 语句导入 clib 类型（如 use module.%s）",
                                         obj_type->struct_name, method_name, obj_type->struct_name);
                            } else {
                                error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                         "clib '%s' 没有声明函数 '%s'",
                                         obj_type->struct_name, method_name);
                            }
                        } else {
                            // clib 调用验证通过，标记 cached_type 让 codegen 识别
                            ast->cached_type = type_new(TYPE_CLIB);
                            ast->cached_type->struct_name = strdup(obj_type->struct_name);
                            // 同时保存 lib 变量的 ref 信息
                            ast->u.module_call.lib_ref = ref;
                            if (ref.name) ast->u.module_call.lib_ref.name = strdup(ref.name);
                            // 计算并缓存 clib 函数的 Leno 返回类型（供 infer_expr_type 使用）
                            // clib_sym 可能有函数信息（同模块定义），也可能没有（跨模块导入）
                            int clib_ret_set = 0;
                            if (clib_sym && clib_sym->clib_func_count > 0) {
                                for (int fi = 0; fi < clib_sym->clib_func_count; fi++) {
                                    if (strcmp(clib_sym->clib_func_names[fi], method_name) == 0) {
                                        TypeInfo* ret_type = clib_sym->clib_func_return_types[fi];
                                        if (ret_type) {
                                            TypeKind rk = ret_type->kind;
                                            TypeKind leno_rk = c_layout_type_to_leno(rk);
                                            if (leno_rk != rk) {
                                                ast->u.module_call.clib_return_type = type_new(leno_rk);
                                            } else {
                                                switch (rk) {
                                                    case TYPE_PTR: {
                                                        TypeInfo* t = type_new(TYPE_PTR);
                                                        t->struct_name = strdup("Ptr");
                                                        ast->u.module_call.clib_return_type = t;
                                                        break;
                                                    }
                                                    case TYPE_PTR_GENERIC: {
                                                        if (ret_type->element_type) {
                                                            ast->u.module_call.clib_return_type = type_ptr_generic(type_copy(ret_type->element_type));
                                                        } else {
                                                            TypeInfo* t = type_new(TYPE_PTR);
                                                            t->struct_name = strdup("Ptr");
                                                            ast->u.module_call.clib_return_type = t;
                                                        }
                                                        break;
                                                    }
                                                    case TYPE_BOOL:
                                                        ast->u.module_call.clib_return_type = type_new(TYPE_BOOL);
                                                        break;
                                                    case TYPE_NULL:
                                                        ast->u.module_call.clib_return_type = type_new(TYPE_NULL);
                                                        break;
                                                    default:
                                                        ast->u.module_call.clib_return_type = type_copy(ret_type);
                                                        break;
                                                }
                                            }
                                        } else {
                                            ast->u.module_call.clib_return_type = type_new(TYPE_ANY);
                                        }
                                        clib_ret_set = 1;
                                        break;
                                    }
                                }
                            }
                            // 如果 clib_sym 没有函数信息（跨模块导入），从导入模块的符号表查找
                            if (!clib_ret_set) {
                                for (int mi = 0; mi < s->imported_module_count; mi++) {
                                    ImportedModuleInfo* info = &s->imported_modules[mi];
                                    if (info->sym_table) {
                                        ModuleClibSymbol* mod_clib = module_symbol_table_find_clib(info->sym_table, obj_type->struct_name);
                                        if (mod_clib && mod_clib->func_count > 0) {
                                            for (int fi = 0; fi < mod_clib->func_count; fi++) {
                                                if (strcmp(mod_clib->funcs[fi].name, method_name) == 0) {
                                                    TypeKind rk = mod_clib->funcs[fi].return_type;
                                                    TypeKind leno_rk = c_layout_type_to_leno(rk);
                                                    if (leno_rk != rk) {
                                                        ast->u.module_call.clib_return_type = type_new(leno_rk);
                                                    } else {
                                                        switch (rk) {
                                                            case TYPE_PTR: {
                                                                TypeInfo* t = type_new(TYPE_PTR);
                                                                t->struct_name = strdup("Ptr");
                                                                ast->u.module_call.clib_return_type = t;
                                                                break;
                                                            }
                                                            case TYPE_PTR_GENERIC: {
                                                                if (mod_clib->funcs[fi].return_element_type != TYPE_UNKNOWN) {
                                                                    ast->u.module_call.clib_return_type = type_ptr_generic(type_new(mod_clib->funcs[fi].return_element_type));
                                                                } else {
                                                                    TypeInfo* t = type_new(TYPE_PTR);
                                                                    t->struct_name = strdup("Ptr");
                                                                    ast->u.module_call.clib_return_type = t;
                                                                }
                                                                break;
                                                            }
                                                            case TYPE_BOOL:
                                                                ast->u.module_call.clib_return_type = type_new(TYPE_BOOL);
                                                                break;
                                                            case TYPE_NULL:
                                                                ast->u.module_call.clib_return_type = type_new(TYPE_NULL);
                                                                break;
                                                            default:
                                                                ast->u.module_call.clib_return_type = type_new(rk);
                                                                break;
                                                        }
                                                    }
                                                    clib_ret_set = 1;
                                                    break;
                                                }
                                            }
                                            if (clib_ret_set) break;
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // clib 调用不继续走 struct 检查
                    if (is_clib_call) {
                        // 释放临时对象
                        if (obj_type) type_free(obj_type);
                        free(temp_var->u.var.name);
                        if (temp_var->u.var.ref.name) free(temp_var->u.var.ref.name);
                        free(temp_var);
                        break;
                    }

                    // 检查类型是否是 struct 或 cstruct
                    if (obj_type && (obj_type->kind == TYPE_STRUCT || obj_type->kind == TYPE_CSTRUCT || obj_type->kind == TYPE_FACE) && obj_type->struct_name) {
                        if (obj_type->kind == TYPE_FACE) {
                            ObjFaceDef* fdef = face_def_find(obj_type->struct_name);
                            if (fdef) {
                                for (int fi = 0; fi < fdef->method_count; fi++) {
                                    if (strcmp(fdef->methods[fi].name, method_name) == 0) {
                                        is_struct_method = 1;
                                        break;
                                    }
                                }
                            } else {
                                // face 可能定义在导入的模块中，从模块符号表查找
                                ImportedModuleInfo* mod_info = find_imported_module(s, ast->u.module_call.module_name);
                                if (mod_info && mod_info->sym_table) {
                                    ModuleFaceSymbol* face_sym = module_symbol_table_find_face(mod_info->sym_table, obj_type->struct_name);
                                    if (face_sym) {
                                        for (int fi = 0; fi < face_sym->method_count; fi++) {
                                            if (strcmp(face_sym->methods[fi].name, method_name) == 0) {
                                                is_struct_method = 1;
                                                break;
                                            }
                                        }
                                    }
                                }
                                // 也检查其他导入的模块
                                if (!is_struct_method) {
                                    for (int mi = 0; mi < s->imported_module_count && !is_struct_method; mi++) {
                                        ImportedModuleInfo* m = &s->imported_modules[mi];
                                        if (m && m->sym_table) {
                                            ModuleFaceSymbol* face_sym = module_symbol_table_find_face(m->sym_table, obj_type->struct_name);
                                            if (face_sym) {
                                                for (int fi = 0; fi < face_sym->method_count; fi++) {
                                                    if (strcmp(face_sym->methods[fi].name, method_name) == 0) {
                                                        is_struct_method = 1;
                                                        break;
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        } else {
                            char method_key[256];
                            snprintf(method_key, sizeof(method_key), "%s::%s", obj_type->struct_name, method_name);
                            method_def = func_table_find(&s->func_table, method_key);
                            if (method_def && method_def->kind == AST_FUNC_DEF) {
                                is_struct_method = 1;
                            }

                            // ★ 前向定义的 struct（定义在使用点之后）方法：func_table 占位符
                            //   要到真实访问 struct 定义时才注册 ⇒ 这里补 root AST 兜底
                            //   （对齐 visit_expr.inc 的同名回退），否则前向方法调用误报"没有方法"
                            if (!is_struct_method && obj_type->kind == TYPE_STRUCT) {
                                for (int si = 0; si < s->root->u.block.count && !is_struct_method; si++) {
                                    Ast* stmt = s->root->u.block.items[si];
                                    Ast* sdef = NULL;
                                    if (stmt->kind == AST_STRUCT_DEF && stmt->u.struct_def.name &&
                                        strcmp(stmt->u.struct_def.name, obj_type->struct_name) == 0) {
                                        sdef = stmt;
                                    } else if (stmt->kind == AST_EXPORT && stmt->u.export.decl &&
                                               stmt->u.export.decl->kind == AST_STRUCT_DEF &&
                                               stmt->u.export.decl->u.struct_def.name &&
                                               strcmp(stmt->u.export.decl->u.struct_def.name, obj_type->struct_name) == 0) {
                                        sdef = stmt->u.export.decl;
                                    }
                                    if (sdef) {
                                        for (int mi = 0; mi < sdef->u.struct_def.method_count; mi++) {
                                            Ast* m = sdef->u.struct_def.methods[mi];
                                            if (m && m->kind == AST_FUNC_DEF &&
                                                strcmp(m->u.func.name, method_name) == 0) {
                                                is_struct_method = 1;
                                                break;
                                            }
                                        }
                                    }
                                }
                            }

                            if (!is_struct_method) {
                                int face_param_count = 0;
                                TypeKind face_return_type = TYPE_ANY;
                                if (struct_def_has_face_method(obj_type->struct_name, method_name, &face_param_count, &face_return_type)) {
                                    is_struct_method = 1;
                                }
                            }

                            if (!is_struct_method) {
                                for (int mi = 0; mi < s->imported_module_count && !is_struct_method; mi++) {
                                    ImportedModuleInfo* m = &s->imported_modules[mi];
                                    if (m && m->sym_table) {
                                        ModuleStructMethod* mod_method = module_symbol_table_find_struct_method(
                                            m->sym_table, obj_type->struct_name, method_name);
                                        if (mod_method) {
                                            is_struct_method = 1;
                                            break;
                                        }
                                    }
                                }
                            }

                            if (!is_struct_method) {
                                for (int mi = 0; mi < s->imported_module_count && !is_struct_method; mi++) {
                                    ImportedModuleInfo* m = &s->imported_modules[mi];
                                    if (m && m->sym_table) {
                                        ObjStructDef* sd = struct_def_find(obj_type->struct_name);
                                        if (sd && sd->impl_count > 0) {
                                            for (int ii = 0; ii < sd->impl_count && !is_struct_method; ii++) {
                                                ModuleFaceSymbol* face_sym = module_symbol_table_find_face(m->sym_table, sd->impl_names[ii]);
                                                if (face_sym) {
                                                    for (int fi = 0; fi < face_sym->method_count; fi++) {
                                                        if (strcmp(face_sym->methods[fi].name, method_name) == 0) {
                                                            is_struct_method = 1;
                                                            break;
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // 检查是否是函数类型字段调用（如 t.cb() 其中 cb 是 func():void 字段）
                    int is_func_type_field_call = 0;
                    if (!is_struct_method && obj_type && obj_type->struct_name &&
                        (obj_type->kind == TYPE_STRUCT || obj_type->kind == TYPE_CSTRUCT)) {
                        Symbol* struct_sym = scope_resolve(s->current, obj_type->struct_name);
                        if (!struct_sym) {
                            struct_sym = scope_resolve_tree_bfs(s->root_scope, obj_type->struct_name);
                        }
                        if (struct_sym && struct_sym->struct_field_names && struct_sym->struct_field_types) {
                            for (int fi = 0; fi < struct_sym->struct_field_count; fi++) {
                                if (strcmp(struct_sym->struct_field_names[fi], method_name) == 0 &&
                                    struct_sym->struct_field_types[fi]->kind == TYPE_FUNCTION) {
                                    is_func_type_field_call = 1;
                                    break;
                                }
                            }
                        }
                    }

                    if (is_struct_method) {
                        if (obj_type && obj_type->kind == TYPE_FACE && obj_type->struct_name) {
                            // ★ face 接收者也会被算作 "struct 方法"（打点实测：`an.feed(...)` 的
                            //   obj_kind=14=TYPE_FACE 而 is_struct_method=1）⇒ 会被带进下面那套 struct
                            //   逻辑（按 "Animal::feed" 找占位符 ⇒ 找不到 ⇒ **整块跳过**）⇒ 实参个数与
                            //   类型全不检查（同文件也静默）。这里分流给 face 的检查。
                            semantic_check_face_method_args(s, obj_type->struct_name, method_name,
                                                            &ast->u.module_call.args, ast->line, ast->column,
                                                            obj_type);   // ← 接收者类型（泛型 face 代入用）
                        }
                        // 自定义 struct 方法的参数类型检查
                        else if (obj_type && obj_type->kind == TYPE_STRUCT && obj_type->struct_name) {
                            Ast* struct_def_ast = NULL;
                            for (int si = 0; si < s->root->u.block.count; si++) {
                                Ast* stmt = s->root->u.block.items[si];
                                if (stmt->kind == AST_STRUCT_DEF && strcmp(stmt->u.struct_def.name, obj_type->struct_name) == 0) {
                                    struct_def_ast = stmt;
                                    break;
                                }
                            }
                            // ★ pri：私有方法的编译期检查（默认全公有 ⇒ 只有标了 pri 的才拦 ✓）
                            //   为什么在这里：用户 struct 的方法调用（`w.touch()`）在语义期走的是
                            //   **AST_MODULE_CALL** 这一条 ✓（放在"内建方法表"分支里实测走不到 ✗）
                            //   ⚠ 必须在 `struct_def_ast` 判断**之外** ✗：跨模块时它是 NULL，
                            //     放里面就等于对导入 struct 永不执行 ✓（实测踩过 ✓）
                            //   ⚠ 只覆盖**点号调用** ✓；`obj["方法名"]()` 是运行期按名分发（v1 不拦 ✓）
                            pri_check_method_access(s, ast, obj_type, method_name);
                            if (struct_def_ast) {
                                for (int mi = 0; mi < struct_def_ast->u.struct_def.method_count; mi++) {
                                    Ast* method_ast = struct_def_ast->u.struct_def.methods[mi];
                                    if (method_ast && method_ast->kind == AST_FUNC_DEF &&
                                        strcmp(method_ast->u.func.name, method_name) == 0) {
                                        int actual_arity = ast->u.module_call.args.count;
                                        // ★ 前向定义时方法 AST 未变换（self 未在 visit_type_def
                                        //   阶段前置）⇒ 按 params[0] 是否为 "self" 调整个数与下标
                                        int has_self = method_ast->u.func.pcnt > 0 &&
                                            method_ast->u.func.params &&
                                            method_ast->u.func.params[0] &&
                                            strcmp(method_ast->u.func.params[0], "self") == 0;
                                        int self_off = has_self ? 1 : 0;
                                        int method_param_count = method_ast->u.func.pcnt - self_off;
                                        int default_cnt = method_ast->u.func.default_count;
                                        int required_cnt = method_param_count - default_cnt;
                                        if (actual_arity < required_cnt || actual_arity > method_param_count) {
                                            // `.map()` 那条提示做成尾串（原来是往 msg 尾部 strncat ✓ 等价）
                                            const char* map_hint =
                                                (strcmp(method_name, "map") == 0 && actual_arity == 1 && method_param_count == 2)
                                                ? "\n  提示: .map() 会传递 (item, index) 两个参数，\n"
                                                  "     函数签名需加第二个参数，如 func compress(string w, int _): string"
                                                : "";
                                            if (actual_arity < required_cnt) {
                                                error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                                         "方法 '%s' 参数不足: 至少需要 %d 个, 实际 %d%s",
                                                         method_name, required_cnt, actual_arity, map_hint);
                                            } else {
                                                error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                                         "方法 '%s' 参数过多: 最多 %d 个, 实际 %d%s",
                                                         method_name, method_param_count, actual_arity, map_hint);
                                            }
                                        } else {
                                            // 构建泛型参数替换表（包含 struct 的泛型参数和方法的泛型参数）
                                            char** gp_names = NULL;
                                            TypeInfo** gp_types = NULL;
                                            int gp_count = 0;

                                            // struct 的泛型参数（如 Box[T] 的 T）
                                            if (obj_type->generic_count > 0 && obj_type->generic_args) {
                                                gp_count = obj_type->generic_count;
                                                gp_names = (char**)malloc(sizeof(char*) * (gp_count + method_ast->u.func.type_param_count));
                                                gp_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * (gp_count + method_ast->u.func.type_param_count));
                                                if (struct_def_ast->u.struct_def.type_params) {
                                                    for (int gi = 0; gi < gp_count; gi++) {
                                                        gp_names[gi] = struct_def_ast->u.struct_def.type_params[gi];
                                                        gp_types[gi] = type_copy(obj_type->generic_args[gi]);
                                                    }
                                                }
                                            } else {
                                                gp_names = (char**)malloc(sizeof(char*) * method_ast->u.func.type_param_count);
                                                gp_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * method_ast->u.func.type_param_count);
                                                gp_count = 0;
                                            }

                                            // 方法的泛型参数（如 map[U] 的 U）
                                            if (method_ast->u.func.type_param_count > 0 && method_ast->u.func.type_params) {
                                                for (int gi = 0; gi < method_ast->u.func.type_param_count; gi++) {
                                                    int idx = gp_count;
                                                    gp_names[idx] = method_ast->u.func.type_params[gi];
                                                    if (ast->u.module_call.generic_type_args && gi < ast->u.module_call.generic_type_count) {
                                                        gp_types[idx] = type_copy(ast->u.module_call.generic_type_args[gi]);
                                                    } else {
                                                        gp_types[idx] = type_new(TYPE_ANY);
                                                    }
                                                    gp_count++;
                                                }
                                            }

                                            // ★ 泛型需求校验（B 第二步，2026-10-02）：模块限定形态的 struct 方法调用，
                                            //   用**接收者（struct）的类型实参**逐条判（与同文件 visit_expr 那条对称 ✓）
                                            //   ⚠ 必须与上面**填充循环同条件**（type_params 非空）—— 否则 gp_names/gp_types
                                            //     是 malloc 未初始化的槽位，读进去就是垃圾指针 ✗（实测踩过）
                                            if (gp_count > 0 && obj_type && obj_type->kind == TYPE_STRUCT &&
                                                obj_type->generic_count > 0 && struct_def_ast &&
                                                struct_def_ast->u.struct_def.type_params) {
                                                int sc = obj_type->generic_count;
                                                for (int gi = 0; gi < sc && gi < gp_count; gi++) {
                                                    if (!gp_names[gi] || !gp_types[gi]) continue;
                                                    semantic_record_pending_req_check(s, obj_type->struct_name,
                                                                                        ast->u.module_call.method_name,
                                                                                        gp_names[gi], gp_types[gi], ast);
                                                }
                                            }

                                            for (int i = 0; i < actual_arity; i++) {
                                                // +1 跳过 self（前向定义时 self 还没前置，偏移为 0）
                                                TypeInfo* expected_type = method_ast->u.func.param_types[i + self_off];
                                                if (!expected_type) continue;

                                                TypeInfo* check_type = expected_type;
                                                if (gp_count > 0) {
                                                    for (int gi = 0; gi < gp_count; gi++) {
                                                        // ★ 用双形态替换（TYPE_GENERIC_PARAM + 历史遗留的
                                                        //   TYPE_STRUCT 占位形参都认）：前向定义时方法参数里的 T
                                                        //   曾是 "struct T" 占位，残留占位会把 int 实参误判为不匹配
                                                        //   （★ 2026-10-03 产生端根治后 AST 路径只产生前者 ✓，
                                                        //     此处双形态只兜缓存/外部符号表的旧内容 ✓）
                                                        TypeInfo* sub = semantic_substitute_generic_param(check_type, gp_names[gi], gp_types[gi]);
                                                        // 首轮的 check_type 是 AST 拥有的 param_types[i]，
                                                        // 不能释放；之后的中间副本由这里释放
                                                        if (check_type != expected_type) type_free(check_type);
                                                        check_type = sub;
                                                    }
                                                }

                                                TypeInfo* arg_type = infer_expr_type(s, ast->u.module_call.args.items[i]);
                                                if (arg_type && arg_type->kind != TYPE_ANY && check_type->kind != TYPE_ANY) {
                                                    // 替换后仍是"struct 形参名"占位 ⇒ 泛型参数，不比较
                                                    int is_generic = 0;
                                                    if (check_type->kind == TYPE_STRUCT && check_type->struct_name &&
                                                        struct_def_ast->u.struct_def.type_params) {
                                                        for (int tp = 0; tp < struct_def_ast->u.struct_def.type_param_count; tp++) {
                                                            if (strcmp(check_type->struct_name, struct_def_ast->u.struct_def.type_params[tp]) == 0) {
                                                                is_generic = 1; break;
                                                            }
                                                        }
                                                    }
                                                    if (!is_generic && !type_is_compatible(check_type, arg_type)) {
                                                        char idx_str[16];
                                                        snprintf(idx_str, sizeof(idx_str), "%d", i + 1);
                                                        error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column, "%s",
                                                            format_type_error_text("%s3 第 %s4 个参数类型不匹配: 期望 %s1, 实际 %s2",
                                                                check_type, arg_type,
                                                                method_name, idx_str, NULL));
                                                    }
                                                }
                                                if (arg_type) type_free(arg_type);
                                                if (check_type != expected_type) type_free(check_type);
                                            }

                                            if (gp_types) {
                                                for (int gi = 0; gi < gp_count; gi++) {
                                                    type_free(gp_types[gi]);
                                                }
                                                free(gp_types);
                                                free(gp_names);
                                            }
                                        }
                                        break;
                                    }
                                }
                            } else {
                                // ---- 跨模块 struct：本文件里没有它的 AST ⇒ 回落用**方法占位符** ----
                                // 上面那支（以及 visit_expr.inc 里的另一份拷贝）都只在本编译单元的
                                // root AST 里找 struct 声明 ⇒ 跨模块时整块检查被跳过。共享实现见
                                // semantic_type_utils.c 的 semantic_check_method_args_from_placeholder。
                                semantic_check_method_args_from_placeholder(
                                    s, obj_type->struct_name, method_name,
                                    &ast->u.module_call.args, ast->line, ast->column);
                            }
                        }

                        // 保存原始对象名（用于创建 self 参数）
                        char* original_obj_name = strdup(ast->u.module_call.module_name);
                        // 将 AST_MODULE_CALL 转换为 AST_CALL，callee 为 INDEX(self, "method")
                        free(ast->u.module_call.module_name);

                        // 创建 self 变量节点
                        Ast* self_var = ast_new(AST_VAR, ast->line);
                        self_var->u.var.name = original_obj_name;
                        if (ref.name) {
                            self_var->u.var.ref.kind = ref.kind;
                            self_var->u.var.ref.index = ref.index;
                            self_var->u.var.ref.name = strdup(ref.name);
                            self_var->u.var.ref.type_kind = ref.type_kind;
                            self_var->u.var.ref.struct_name = (ref.struct_name) ? strdup(ref.struct_name) : NULL;
                        } else if (obj_sym) {
                            self_var->u.var.ref.kind = obj_sym->kind;
                            self_var->u.var.ref.index = obj_sym->index;
                            self_var->u.var.ref.name = strdup(obj_sym->name);
                            self_var->u.var.ref.type_kind = obj_sym->type ? obj_sym->type->kind : TYPE_ANY;
                            self_var->u.var.ref.struct_name = (obj_sym->type && obj_sym->type->struct_name) ? strdup(obj_sym->type->struct_name) : NULL;
                        }
                        if (obj_type) {
                            self_var->cached_type = type_copy(obj_type);
                        }

                        // 创建方法名字符串节点
                        Ast* method_str = ast_new(AST_STRING, ast->line);
                        method_str->u.string.value = strdup(method_name);
                        method_str->u.string.len = (int)strlen(method_name);

                        // 创建 INDEX 节点：self["method"]
                        Ast* index_ast = ast_new(AST_INDEX, ast->line);
                        index_ast->u.index.obj = self_var;
                        index_ast->u.index.index = method_str;

                        // 将 self 参数添加到参数列表开头
                        AstList new_args;
                        ast_list_init(&new_args);
                        Ast* self_arg = ast_new(AST_VAR, ast->line);
                        self_arg->u.var.name = strdup(original_obj_name);
                        if (ref.name) {
                            self_arg->u.var.ref.kind = ref.kind;
                            self_arg->u.var.ref.index = ref.index;
                            self_arg->u.var.ref.name = strdup(ref.name);
                            self_arg->u.var.ref.type_kind = ref.type_kind;
                            self_arg->u.var.ref.struct_name = (ref.struct_name) ? strdup(ref.struct_name) : NULL;
                        } else if (obj_sym) {
                            self_arg->u.var.ref.kind = obj_sym->kind;
                            self_arg->u.var.ref.index = obj_sym->index;
                            self_arg->u.var.ref.name = strdup(obj_sym->name);
                            self_arg->u.var.ref.type_kind = obj_sym->type ? obj_sym->type->kind : TYPE_ANY;
                            self_arg->u.var.ref.struct_name = (obj_sym->type && obj_sym->type->struct_name) ? strdup(obj_sym->type->struct_name) : NULL;
                        }
                        ast_list_add(&new_args, self_arg);
                        for (int i = 0; i < ast->u.module_call.args.count; i++) {
                            ast_list_add(&new_args, ast->u.module_call.args.items[i]);
                        }
                        free(ast->u.module_call.args.items);

                        // 转换 AST 类型
                        ast->kind = AST_CALL;
                        ast->u.call.callee = index_ast;
                        ast->u.call.args = new_args;
                        ast->u.call.is_tail_call = 0;
                    }

                    // 释放临时对象
                    if (obj_type) type_free(obj_type);
                    free(temp_var->u.var.name);
                    if (temp_var->u.var.ref.name) free(temp_var->u.var.ref.name);
                    free(temp_var);

                    if (!is_struct_method) {
                        // 根据对象类型确定类型名称
                        const char* obj_type_name = NULL;
                        if (obj_sym->type) {
                            obj_type_name = native_get_type_name(obj_sym->type->kind);
                        }
                        // ★ 2026-10-30：本调用是否已经报过根因（或接收者是毒值）。
                        //   ✗ 此前 any 接收者会**连报两条**：这里「不能在 any 类型上调用方法」
                        //     + 下方回退链里「类型 'any' 没有方法」；而毒值（TYPE_POISON，
                        //     上游缺失方法的级联结果）更会报出一条本不该出现的消息 ✗
                        //   ⇒ 报过一次就抑制后续"没有方法"分支；毒值则完全静默 ✓
                        int call_diag_reported = 0;

                        // 根据对象类型名称和方法名查找方法
                        if (obj_type_name) {
                            return_type = native_get_instance_method_return_type(obj_type_name, method_name, &expected_arity);
                        } else if (obj_sym->type && obj_sym->type->kind == TYPE_GENERIC_PARAM) {
                            // 泛型约束类型参数（如 T: Comparable），交给下面的约束检查
                            expected_arity = -1;
                            return_type = TYPE_ANY;
                        } else {
                            if (obj_sym->type && obj_sym->type->kind == TYPE_POISON) {
                                /* 接收者是"已被报过根因"的毒值 ⇒ 静默，不再刷屏；
                                 * 调用结果继续是毒值 ⇒ 下游（声明/返回/再次调用）全部静默 ✓ */
                                call_diag_reported = 1;
                                semantic_poison_expr(ast);
                            } else if (obj_sym->type && obj_sym->type->struct_name) {
                                /* face / struct / cstruct 等**具名类型**：交给下方按类型报
                                 * 「类型 'face Speaker' 没有方法 'x'」（带相似名提示），
                                 * 这里不报误导性的"any" ✓ */
                            } else {
                                // any 类型不能调用方法，提示用户使用类型收窄或指定参数类型
                                error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                    "不能在 any 类型上调用方法 '%s'\n"
                                    "  提示: 使用 if x is Type { x.%s() } 类型收窄\n"
                                    "  或者将参数声明为具体类型",
                                    method_name, method_name);
                                call_diag_reported = 1;   /* 本调用只报这一条 ✓ */
                                /* 根因已报 ⇒ 结果打毒，抑制"声明/返回/链式调用"级联 ✓ */
                                semantic_poison_expr(ast);
                            }
                            expected_arity = -1;
                            return_type = TYPE_ANY;
                        }

                        // 判断方法是否找到
                        // ⚠ 直接问原生表，**不要**拿 arity 的取值当"已找到"的判据：旧代码写的是
                        //   `expected_arity == -1`，而该 -1 在旧约定里兼作"可变参数"与"没找到"两义 ⇒
                        //   哨兵改名后语义断裂（not-found 会被误判为已找到，反过来报不出"没有方法" ✗）。
                        //   返回类型非 ANY ⇒ 一定是从原生表查到的（未找到时该接口返回 ANY）✓
                        int method_found = 0;
                        if (obj_type_name && native_find_instance_method(obj_type_name, method_name)) {
                            method_found = 1;
                        } else if (return_type != TYPE_ANY) {
                            method_found = 1;
                        }

                        // 如果没找到，检查是否是泛型约束类型参数（如 T: Comparable）
                        if (!method_found && obj_sym->type &&
                            obj_sym->type->kind == TYPE_GENERIC_PARAM &&
                            obj_sym->type->constraint_name) {
                            ObjFaceDef* constraint_face = face_def_find(obj_sym->type->constraint_name);
                            if (constraint_face) {
                                for (int mi = 0; mi < constraint_face->method_count; mi++) {
                                    if (strcmp(constraint_face->methods[mi].name, method_name) == 0) {
                                        method_found = 1;
                                        return_type = constraint_face->methods[mi].return_type
                                            ? constraint_face->methods[mi].return_type->kind : TYPE_ANY;
                                        expected_arity = constraint_face->methods[mi].param_count;
                                        break;
                                    }
                                }
                            }
                            // 约束 face 可能在导入的模块中
                            if (!method_found) {
                                for (int mi = 0; mi < s->imported_module_count; mi++) {
                                    ImportedModuleInfo* m = &s->imported_modules[mi];
                                    if (m && m->sym_table) {
                                        ModuleFaceSymbol* face_sym = module_symbol_table_find_face(m->sym_table, obj_sym->type->constraint_name);
                                        if (face_sym) {
                                            for (int fi = 0; fi < face_sym->method_count; fi++) {
                                                if (strcmp(face_sym->methods[fi].name, method_name) == 0) {
                                                    method_found = 1;
                                                    return_type = face_sym->methods[fi].return_type;
                                                    expected_arity = face_sym->methods[fi].param_count;
                                                    break;
                                                }
                                            }
                                            if (method_found) break;
                                        }
                                    }
                                }
                            }
                        }

                        // 如果没找到，检查是否是从模块导入的 struct/cstruct 的方法
                        if (!method_found && obj_sym->type &&
                            (obj_sym->type->kind == TYPE_STRUCT || obj_sym->type->kind == TYPE_CSTRUCT) &&
                            obj_sym->type->struct_name) {
                            for (int i = 0; i < s->imported_module_count; i++) {
                                ImportedModuleInfo* info = &s->imported_modules[i];
                                if (info->sym_table) {
                                    ModuleStructMethod* method = module_symbol_table_find_struct_method(
                                        info->sym_table, obj_sym->type->struct_name, method_name);
                                    if (method) {
                                        method_found = 1;
                                        return_type = method->return_type;
                                        expected_arity = method->param_count;
                                        break;
                                    }
                                }
                            }
                        }

                        if (method_found) {
                        // 方法在该类型的注册表中存在
                        int actual_arity = ast->u.module_call.args.count;

                        // const 检查：实例方法调用不能修改 const 容器
                        if (obj_sym->is_const &&
                            (type_utils_is_array_element_mutator(method_name) ||
                             type_utils_is_dict_element_mutator(method_name))) {
                            error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                     "不能对 const 变量 '%s' 调用修改方法 '%s'",
                                     ast->u.module_call.module_name, method_name);
                        }

                        const InstanceMethodMeta* inst_meta = NULL;
                        if (obj_type_name) {
                            inst_meta = native_find_instance_method(obj_type_name, method_name);
                        }

                        // arity >= 0: 固定参数数量
                        if (expected_arity >= 0 && actual_arity != expected_arity) {
                            error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                     "调用方法 '%s' 时参数数量不匹配: 期望 %d, 实际 %d",
                                     method_name, expected_arity, actual_arity);
                        }
                        // arity < 0: 可变参数，检查是否少于 min_arity
                        else if (expected_arity < 0 && inst_meta && inst_meta->min_arity >= 0 && actual_arity < inst_meta->min_arity) {
                            error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                     "调用方法 '%s' 时参数过少: 最少 %d 个, 实际 %d",
                                     method_name, inst_meta->min_arity, actual_arity);
                        }
                        // arity < 0: 可变参数，检查是否超过 max_arity
                        else if (expected_arity < 0 && inst_meta && inst_meta->max_arity >= 0 && actual_arity > inst_meta->max_arity) {
                            error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                     "调用方法 '%s' 时参数过多: 最多 %d 个, 实际 %d",
                                     method_name, inst_meta->max_arity, actual_arity);
                        }

                        // 检查参数类型（仅当参数数量匹配或可变参数时；可变参数的 arity 是 NATIVE_ARITY_VARARG）
                        if (obj_type_name && (expected_arity == NATIVE_ARITY_VARARG || actual_arity == expected_arity)) {
                            for (int i = 0; i < actual_arity; i++) {
                                TypeKind expected_param_type = native_get_instance_method_param_type(obj_type_name, method_name, i);
                                TypeInfo* arg_type = infer_expr_type(s, ast->u.module_call.args.items[i]);

                                // ★ 泛型参数 → 具体类型 native 形参：**拦**（2026-10-02）
                                //   ⚠ 容器元素 mutator（add/insert/set）跳过：那是"存入"（Value 带 tag、不越界）✓
                                if (!type_utils_is_array_element_mutator(method_name) &&
                                    !type_utils_is_dict_element_mutator(method_name)) {
                                    char callee[256];
                                    snprintf(callee, sizeof(callee), "方法 '%s'", method_name);
                                    if (semantic_check_native_arg(s, ast, arg_type, expected_param_type, callee, i)) {
                                        if (arg_type) type_free(arg_type);
                                        continue;   // 已由需求机制接管 ⇒ 跳过常规比较（见该函数注释 ✓）
                                    }
                                }

                                // 处理数组/字典元素修改方法的类型更新和检查
                                TypeInfo* expected_type_for_check = NULL;
                                
                                // 处理数组元素修改方法（add/insert）
                                if (type_utils_is_array_element_mutator(method_name)) {
                                    int elem_param_idx = type_utils_get_array_element_param_index(method_name, 0); // 0 = 实例方法
                                    if (i == elem_param_idx) {
                                        // 首先尝试更新数组元素类型（如果是第一次 add）
                                        if (type_utils_try_update_array_element_type(obj_sym, arg_type)) {
                                            // 类型已更新，同时更新期望参数类型
                                            expected_param_type = arg_type->kind;
                                        } else if (obj_sym->type && obj_sym->type->kind == TYPE_ARRAY) {
                                            // 数组已有具体元素类型，检查是否匹配
                                            TypeInfo* elem_type = obj_sym->type->element_type;
                                            if (elem_type && elem_type->kind != TYPE_ANY) {
                                                expected_param_type = elem_type->kind;
                                                // 保存完整类型信息用于嵌套类型检查
                                                expected_type_for_check = elem_type;
                                            }
                                        }
                                    }
                                }
                                // 处理字典 set 方法的值类型更新和检查
                                else if (type_utils_is_dict_element_mutator(method_name)) {
                                    int elem_param_idx = type_utils_get_dict_element_param_index(method_name);
                                    if (i == elem_param_idx) {
                                        // 首先尝试更新字典值类型（如果是第一次 set）
                                        //   （位置一并传下去 ⇒ 锁定点会被记下，供后面的报错回溯 ✓）
                                        if (type_utils_try_update_dict_value_type(obj_sym, arg_type, ast->line, ast->column)) {
                                            // 类型已更新，同时更新期望参数类型
                                            expected_param_type = arg_type->kind;
                                        } else if (obj_sym->type && obj_sym->type->kind == TYPE_DICT) {
                                            // 字典已有具体值类型，检查是否匹配
                                            TypeInfo* value_type = obj_sym->type->value_type;
                                            if (value_type && value_type->kind != TYPE_ANY) {
                                                expected_param_type = value_type->kind;
                                                // 保存完整类型信息用于嵌套类型检查
                                                expected_type_for_check = value_type;
                                            }
                                        }
                                    }
                                }
                                // 对于其他方法，如果注册表中是 TYPE_ANY，但容器有具体元素类型，则用该类型检查
                                else if (expected_param_type == TYPE_ANY && obj_sym->type) {
                                    if (obj_sym->type->kind == TYPE_ARRAY) {
                                        TypeInfo* elem_type = obj_sym->type->element_type;
                                        if (elem_type && elem_type->kind != TYPE_ANY) {
                                            expected_param_type = elem_type->kind;
                                            expected_type_for_check = elem_type;
                                        }
                                    } else if (obj_sym->type->kind == TYPE_DICT && i > 0) {
                                        TypeInfo* value_type = obj_sym->type->value_type;
                                        if (value_type && value_type->kind != TYPE_ANY) {
                                            expected_param_type = value_type->kind;
                                            expected_type_for_check = value_type;
                                        }
                                    }
                                }
                                
                                // 类型检查：简单类型比较 kind，复杂类型使用 type_equals 深度比较
                                // ⚠ v3.2.7：**删掉了 `arg_type->kind != TYPE_ANY` 豁免** —— 与模块调用
                                //   那处同一口径：**静态 any 的值不允许流进具体类型的 native 形参**
                                //   （any/到运行期可能是 null/int/...，而 native 侧普遍无校验强转 ⇒ 崩）。
                                //   代价：全仓需要把这类调用点补上显式转换（见实例二十三的清单）。
                                if (arg_type) {
                                    if (expected_type_for_check) {
                                        // 有完整类型信息，进行深度类型比较（支持嵌套类型）
                                        if (!type_is_compatible(expected_type_for_check, arg_type)) {
                                            // 先保存字符串避免 buffer 覆盖
                                            const char* exp_str = type_to_string(expected_type_for_check);
                                            char exp_buf[BUFFER_SMALL];
                                            strncpy(exp_buf, exp_str, sizeof(exp_buf) - 1);
                                            exp_buf[sizeof(exp_buf) - 1] = '\0';
                                            const char* hint = get_type_conversion_hint(expected_type_for_check->kind, arg_type->kind);
                                            if (hint) {
                                                error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                                    "类型错误: 参数 %d 期望 '%s'，但传入 '%s'\n%s%s",
                                                    i + 1, exp_buf, type_to_string(arg_type), hint,
                                                    type_utils_dict_lock_hint(obj_sym));
                                            } else {
                                                error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                                    "类型错误: 参数 %d 期望 '%s'，但传入 '%s'%s",
                                                    i + 1, exp_buf, type_to_string(arg_type),
                                                    type_utils_dict_lock_hint(obj_sym));
                                            }
                                        }
                                    } else if (expected_param_type != TYPE_ANY) {
                                        // 只有 kind 信息，进行简单类型比较
                                        if (arg_type->kind != expected_param_type) {
                                            // ★ 2026-10-12：**不再**在这里手抄一份兼容小表（本处与
                                            //   visit_expr.inc 的「全局内置函数」那处原本是同两份拷贝）——
                                            //   手抄表的通病是"口径加了一处、漏掉另一处"，最近一次漏的是枚举：
                                            //   原生形参是 int、实参是**枚举变量**（口径：枚举 = int）时报
                                            //     「类型错误: 参数 1 期望 'int'，但传入 'int'」← 自相矛盾的假报错 ✗
                                            //   ⇒ 合成一个「只有 kind」的 TypeInfo，交给 type_is_compatible（兼容
                                            //     口径的唯一来源 ✓）。聚合类型走不到这里（kind 不同才进本分支，
                                            //     而同 kind 的聚合在上一句就被放过了 ✓）
                                            TypeInfo expected_ti;
                                            memset(&expected_ti, 0, sizeof(expected_ti));
                                            expected_ti.kind = expected_param_type;
                                            if (!type_is_compatible(&expected_ti, arg_type)) {
                                                const char* hint = get_type_conversion_hint(expected_param_type, arg_type->kind);
                                                if (hint) {
                                                    error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                                        "类型错误: 参数 %d 期望 '%s'，但传入 '%s'\n%s%s",
                                                        i + 1, type_kind_to_string(expected_param_type),
                                                        type_to_string(arg_type), hint,
                                                        type_utils_dict_lock_hint(obj_sym));
                                                } else {
                                                    error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                                        "类型错误: 参数 %d 期望 '%s'，但传入 '%s'%s",
                                                        i + 1, type_kind_to_string(expected_param_type),
                                                        type_to_string(arg_type),
                                                        type_utils_dict_lock_hint(obj_sym));
                                                }
                                            }
                                        }
                                    }
                                }
                                
                                if (arg_type) type_free(arg_type);
                            }
                        }
                    } else if (call_diag_reported) {
                        // 上方已报过根因（any 接收者 / 毒值级联）⇒ 不再重复报"没有方法"
                    } else if (is_func_type_field_call) {
                        // 函数类型字段调用（如 t.cb() 其中 cb 是 func():void 字段），合法调用，不报错
                    } else if (!obj_sym->type || obj_sym->type->kind == TYPE_ANY) {
                        // 未初始化变量
                        error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                 "不能对未初始化的变量 '%s' 调用方法 '%s'",
                                 ast->u.module_call.module_name, method_name);
                    } else {
                        // 方法不存在于该类型
                        // ★ 增强报错：当类型是 TYPE_STRING 且方法名是 contains/includes 时，提示用 has
                        if (obj_sym->type->kind == TYPE_STRING &&
                            (strcmp(method_name, "contains") == 0 || strcmp(method_name, "includes") == 0)) {
                            error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                     "类型 'string' 没有方法 '%s'，是否想用 'has'？（如 s.has(sub)）", method_name);
                            semantic_poison_expr(ast);
                        } else if (obj_sym->type->kind == TYPE_STRUCT && obj_sym->type->struct_name) {
                            // ★ 增强报错：当类型是 TYPE_STRUCT 且无定义时，提示可能是未 use 导入的 clib/cstruct 类型
                            const char* tn = obj_sym->type->struct_name;
                            Symbol* def_sym = scope_resolve_tree_bfs(s->root_scope, tn);
                            if (!def_sym) {
                                // ★ 2026-10-10：提示走**共享实现** ⇒ 直接说清"它是哪个模块导出的" ✓
                                //   （grep 出来的第九处拷贝 ✗ —— 上面几处是按 build/运行时那句找的，
                                //    这一处藏在"类型没有方法"里 ✓ 说明"照着报错文案 grep"才找得全 ✓）
                                char hint[BUFFER_MEDIUM];
                                semantic_undefined_type_hint(s, tn, hint, sizeof(hint));
                                error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                         "类型 '%s' 没有方法 '%s'：类型 '%s' 未定义%s",
                                         type_to_string(obj_sym->type), method_name, tn, hint);
                                semantic_poison_expr(ast);
                            } else {
                                error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                         "类型 '%s' 没有方法 '%s'%s",
                                         type_to_string(obj_sym->type), method_name,
                                         get_similar_struct_method_hint(s, tn, method_name));
                                semantic_poison_expr(ast);
                            }
                        } else {
                            // ★ 2026-10-10（A 组第 7 条）：改走**共享分级提示** ✓
                            //   （`slice*` ⇒ 切片提示优先 ✓ 其次相似名 ✓）—— 实测 `arr.slice(0,1)`
                            //   走的正是**这一处**（不是 visit_expr 那处 ✓），所以两处必须同一份 ✓
                            error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                     "类型 '%s' 没有方法 '%s'%s",
                                     type_to_string(obj_sym->type), method_name,
                                     semantic_no_such_method_hint(obj_sym->type, method_name));
                            semantic_poison_expr(ast);
                        }
                    }

                    // 转换为实例方法调用：obj.method(args) -> obj["method"](args)
                    // 创建属性访问节点 obj["method"]
                    Ast* obj_var = ast_new(AST_VAR, ast->line);
                    obj_var->u.var.name = strdup(ast->u.module_call.module_name);
                    // 使用ref（由resolve_variable_with_upvalue填充）来支持闭包
                    if (ref.name) {
                        obj_var->u.var.ref.kind = ref.kind;
                        obj_var->u.var.ref.index = ref.index;
                        obj_var->u.var.ref.name = strdup(ref.name);
                        obj_var->u.var.ref.type_kind = ref.type_kind;
                    } else if (obj_sym) {
                        obj_var->u.var.ref.kind = obj_sym->kind;
                        obj_var->u.var.ref.index = obj_sym->index;
                        obj_var->u.var.ref.name = strdup(obj_sym->name);
                        obj_var->u.var.ref.type_kind = obj_sym->type ? obj_sym->type->kind : TYPE_ANY;
                    }

                    Ast* method_str = ast_new(AST_STRING, ast->line);
                    method_str->u.string.value = strdup(ast->u.module_call.method_name);
                    method_str->u.string.len = (int)strlen(ast->u.module_call.method_name);

                    Ast* prop_access = ast_new(AST_INDEX, ast->line);
                    prop_access->u.index.obj = obj_var;
                    prop_access->u.index.index = method_str;

                    // 转换为普通函数调用
                    ast->kind = AST_CALL;
                    ast->u.call.callee = prop_access;
                    // 参数列表已经解析好了，直接使用
                    AstList args = ast->u.module_call.args;
                    ast->u.call.args = args;
                }
            } else {
                // 模块和变量都未定义
                // D1: 模块名拼错提示——先在已导入模块的别名里找相似，再在内置模块名里找。
                // 两个 hint 函数都返回"提示串"（\n  提示: ...），找不到返回空串。
                const char* alias_names[64];
                int alias_count = 0;
                for (int i = 0; i < s->imported_module_count && alias_count < 64; i++) {
                    if (s->imported_modules[i].alias) {
                        alias_names[alias_count++] = s->imported_modules[i].alias;
                    }
                }
                const char* hint = get_similar_in_names(alias_names, alias_count,
                                                        ast->u.module_call.module_name);
                if (!hint[0]) hint = native_builtin_module_hint(ast->u.module_call.module_name);
                if (hint[0]) {
                    error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                     "未定义的模块或变量: %s%s",
                                     ast->u.module_call.module_name, hint);
                } else {
                    error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                     "未定义的模块或变量: %s", ast->u.module_call.module_name);
                }
            }
            }

            // A′（编译期类型落地）：模块方法的返回类型在元信息/符号表里是已知的，
            // 这里把推断结果写回 cached_type → codegen 的 get_expr_type_kind() 读到具体类型，
            // 直接发特化指令（OP_MUL_FLOAT / OP_EQ_INT ...），不再退化为通用 opcode + 运行期分派。
            // 只在尚无缓存时写入：clib 路径已写入 TYPE_CLIB 哨兵（供 codegen 识别），不能覆盖；
            // TYPE_ANY 不缓存，保持「返回类型运行期才确定」的方法（如可变返回）走通用路径。
            if (ast->kind == AST_MODULE_CALL && ast->cached_type == NULL) {
                TypeInfo* mc_type = infer_expr_type(s, ast);
                if (mc_type) {
                    if (mc_type->kind != TYPE_ANY) {
                        ast->cached_type = type_copy(mc_type);
                    }
                    type_free(mc_type);
                }
            }
            break;
        }

        case AST_MODULE_ACCESS: {
            // 模块加载失败时，跳过后续检查（错误已注册），避免级联 any 错误
            ImportedModuleInfo* access_module_info = find_imported_module(s, ast->u.module_access.module_name);
            if (access_module_info && access_module_info->load_failed) break;

            Symbol* module_sym = scope_resolve(s->current, ast->u.module_access.module_name);
            int is_module = 0;
            
            if (module_sym) {
                for (int i = 0; i < s->imported_module_count; i++) {
                    if (strcmp(s->imported_modules[i].alias, ast->u.module_access.module_name) == 0) {
                        is_module = 1;
                        break;
                    }
                }
            }
            
            if (!is_module) {
                // 使用resolve_variable_with_upvalue处理变量，支持闭包
                SymRef ref;
                memset(&ref, 0, sizeof(ref));
                Symbol* var_sym = resolve_variable_with_upvalue(s, ast->u.module_access.module_name, &ref);

                Ast* var_ast = ast_new(AST_VAR, ast->line);
                var_ast->u.var.name = strdup(ast->u.module_access.module_name);
                if (ref.name) {
                    var_ast->u.var.ref.kind = ref.kind;
                    var_ast->u.var.ref.index = ref.index;
                    var_ast->u.var.ref.name = strdup(ref.name);
                    var_ast->u.var.ref.type_kind = ref.type_kind;
                    var_ast->u.var.ref.struct_name = ref.struct_name ? strdup(ref.struct_name) : NULL;
                } else if (var_sym) {
                    var_ast->u.var.ref.kind = var_sym->kind;
                    var_ast->u.var.ref.index = var_sym->index;
                    var_ast->u.var.ref.name = strdup(var_sym->name);
                    var_ast->u.var.ref.type_kind = var_sym->type ? var_sym->type->kind : TYPE_ANY;
                    var_ast->u.var.ref.struct_name = (var_sym->type && var_sym->type->struct_name) ? strdup(var_sym->type->struct_name) : NULL;
                } else {
                    const char* hint = get_similar_name_hint(s->current, ast->u.module_access.module_name);
                    error_add_at_fmt(ERR_UNDEFINED_VAR, ast->line, ast->column,
                                     "未定义的变量: %s%s", ast->u.module_access.module_name, hint);
                }
                if (var_sym && var_sym->type) {
                    var_ast->cached_type = type_copy(var_sym->type);
                }
                
                // 检查变量是否是 struct、cstruct 或 face 类型
                int is_struct_type = 0;
                int is_cstruct_type = 0;
                int is_face_type = 0;
                if (var_sym && var_sym->type) {
                    if (var_sym->type->kind == TYPE_STRUCT) {
                        is_struct_type = 1;
                    } else if (var_sym->type->kind == TYPE_CSTRUCT) {
                        is_cstruct_type = 1;
                    } else if (var_sym->type->kind == TYPE_FACE) {
                        is_face_type = 1;
                    }
                }

                // 检查是否是 enum 类型
                int is_enum_type = 0;
                if (var_sym && var_sym->type && var_sym->type->kind == TYPE_ENUM) {
                    is_enum_type = 1;
                }

                if (is_enum_type) {
                    // use 导入的 enum：从 enum symbol 的成员表查找值
                    const char* mname = ast->u.module_access.member_name;
                    int found = 0;
                    int64_t member_value = 0;
                    if (var_sym->enum_value_names) {
                        for (int mi = 0; mi < var_sym->enum_value_count; mi++) {
                            if (strcmp(var_sym->enum_value_names[mi], mname) == 0) {
                                member_value = var_sym->enum_values[mi];
                                found = 1;
                                break;
                            }
                        }
                    }
                    if (found) {
                        char* mod_name = ast->u.module_access.module_name;
                        char* mem_name = ast->u.module_access.member_name;
                        char* ref_name = ast->u.module_access.ref.name;
                        ast->u.module_access.module_name = NULL;
                        ast->u.module_access.member_name = NULL;
                        ast->u.module_access.ref.name = NULL;
                        free(mod_name); free(mem_name);
                        if (ref_name) free(ref_name);
                        if (ast->cached_type) { type_free(ast->cached_type); ast->cached_type = NULL; }
                        ast->kind = AST_NUM;
                        // 不能用 double 承载成员值：超过 2^53 的整数（位标志 `1 << 60` 之类）
                        // 会被静默舍入，于是 use 路径与同模块/运行期路径给出不同的数。
                        // 超出 double 精确整数范围时改走 bigint 表示（与词法层同一判据）。
                        ast->u.num.is_float = 0;
                        ast->u.num.bigint_str = NULL;  // 必须清空：union 中 bigint_str 与 module_access.ref.kind/index 重叠
                        if (member_value > 9007199254740992LL || member_value < -9007199254740992LL) {
                            ast->u.num.value = 0;
                            ast->u.num.is_bigint = 1;
                            ast->u.num.bigint_str = (char*)malloc(32);
                            if (ast->u.num.bigint_str) {
                                snprintf(ast->u.num.bigint_str, 32, "%lld", (long long)member_value);
                            } else {
                                // 兜底：内存不足时退回旧行为（宁可有精度损失，不要崩）
                                ast->u.num.value = (double)member_value;
                                ast->u.num.is_bigint = 0;
                            }
                        } else {
                            ast->u.num.value = (double)member_value;
                            ast->u.num.is_bigint = 0;
                        }
                        ast->cached_type = type_new(TYPE_INT);
                        // enum 值已解析为常量，释放不再需要的 var_ast
                        if (var_ast) {
                            free(var_ast->u.var.name);
                            var_ast->u.var.name = NULL;
                            if (var_ast->u.var.ref.name) { free(var_ast->u.var.ref.name); var_ast->u.var.ref.name = NULL; }
                            if (var_ast->u.var.ref.struct_name) { free(var_ast->u.var.ref.struct_name); var_ast->u.var.ref.struct_name = NULL; }
                            if (var_ast->cached_type) { type_free(var_ast->cached_type); var_ast->cached_type = NULL; }
                            free(var_ast);
                            var_ast = NULL;
                        }
                    } else {
                        // G1: 枚举成员拼错提示——在成员名列表里找最相似的候选
                        const char* hint = "";
                        if (var_sym->enum_value_names) {
                            hint = get_similar_in_names((const char**)var_sym->enum_value_names,
                                                        var_sym->enum_value_count, mname);
                        }
                        if (hint[0]) {
                            error_add_at_fmt(ERR_UNDEFINED_VAR, ast->line, ast->column,
                                     "枚举 '%s' 中没有成员 '%s'%s",
                                     ast->u.module_access.module_name, mname, hint);
                        } else {
                            error_add_at_fmt(ERR_UNDEFINED_VAR, ast->line, ast->column,
                                     "枚举 '%s' 中没有成员 '%s'",
                                     ast->u.module_access.module_name, mname);
                        }
                    }
                } else if (is_struct_type) {
                    // 检查是否是 struct 关联常量访问（StructName.CONST_NAME）
                    ObjStructDef* sdef = struct_def_find(var_sym->type->struct_name);
                    int found_const = 0;
                    if (sdef && sdef->const_count > 0) {
                        const char* mname = ast->u.module_access.member_name;
                        for (int ci = 0; ci < sdef->const_count; ci++) {
                            if (sdef->const_names[ci] && strcmp(sdef->const_names[ci], mname) == 0) {
                                // 找到关联常量，转换为常量节点（类似 enum 的处理方式）
                                Value cv = sdef->const_values[ci];
                                // 释放 module_access 相关内存
                                char* mod_name = ast->u.module_access.module_name;
                                char* mem_name = ast->u.module_access.member_name;
                                char* ref_name = ast->u.module_access.ref.name;
                                ast->u.module_access.module_name = NULL;
                                ast->u.module_access.member_name = NULL;
                                ast->u.module_access.ref.name = NULL;
                                free(mod_name); free(mem_name);
                                if (ref_name) free(ref_name);
                                if (ast->cached_type) { type_free(ast->cached_type); ast->cached_type = NULL; }

                                // 根据 Value 类型转换为相应的 AST 常量节点
                                if (val_is_int(cv)) {
                                    ast->kind = AST_NUM;
                                    ast->u.num.value = (double)val_as_int(cv);
                                    ast->u.num.is_float = 0;
                                    ast->u.num.is_bigint = 0;
                                    ast->u.num.bigint_str = NULL;
                                    ast->cached_type = type_new(TYPE_INT);
                                } else if (val_is_float(cv)) {
                                    ast->kind = AST_NUM;
                                    ast->u.num.value = val_as_double(cv);
                                    ast->u.num.is_float = 1;
                                    ast->u.num.is_bigint = 0;
                                    ast->u.num.bigint_str = NULL;
                                    ast->cached_type = type_new(TYPE_FLOAT);
                                } else if (val_is_string(cv)) {
                                    ObjString* ostr = (ObjString*)val_as_obj(cv);
                                    ast->kind = AST_STRING;
                                    ast->u.string.value = strdup(ostr->chars);
                                    ast->u.string.len = ostr->len;
                                    ast->cached_type = type_new(TYPE_STRING);
                                } else if (val_is_bool(cv)) {
                                    ast->kind = AST_BOOL;
                                    ast->u.boolean = val_as_bool(cv);
                                    ast->cached_type = type_new(TYPE_BOOL);
                                } else if (val_is_null(cv)) {
                                    ast->kind = AST_NULL;
                                    ast->cached_type = type_new(TYPE_ANY);
                                } else {
                                    // 数组/字典等复杂类型：保持为常量引用
                                    // 简单回退：将值编码为 null（复杂类型的关联常量
                                    // 在实际使用场景中极少，主要是整数/浮点/字符串）
                                    ast->kind = AST_NULL;
                                    ast->cached_type = type_new(TYPE_ANY);
                                }

                                // 释放不再需要的 var_ast
                                if (var_ast) {
                                    free(var_ast->u.var.name);
                                    var_ast->u.var.name = NULL;
                                    if (var_ast->u.var.ref.name) { free(var_ast->u.var.ref.name); var_ast->u.var.ref.name = NULL; }
                                    if (var_ast->u.var.ref.struct_name) { free(var_ast->u.var.ref.struct_name); var_ast->u.var.ref.struct_name = NULL; }
                                    if (var_ast->cached_type) { type_free(var_ast->cached_type); var_ast->cached_type = NULL; }
                                    free(var_ast);
                                    var_ast = NULL;
                                }
                                found_const = 1;
                                break;
                            }
                        }
                    }
                    if (found_const) break;

                    // 不是关联常量，继续走 struct 字段访问逻辑
                    // struct/cstruct 字段访问：转换为 AST_FIELD_ACCESS
                    char* field_name = strdup(ast->u.module_access.member_name);
                    char* module_name = ast->u.module_access.module_name;
                    char* member_name = ast->u.module_access.member_name;

                    ast->kind = AST_FIELD_ACCESS;
                    ast->u.field_access.obj = var_ast;
                    ast->u.field_access.field_name = field_name;
                    ast->u.field_access.field_index = -1;

                    // 释放旧的内存
                    free(member_name);
                    free(module_name);

                    // 继续访问转换后的节点
                    visit(s, ast);
                } else if (is_cstruct_type || is_face_type) {
                    // cstruct/face 字段访问：转换为 AST_FIELD_ACCESS
                    char* field_name = strdup(ast->u.module_access.member_name);
                    char* module_name = ast->u.module_access.module_name;
                    char* member_name = ast->u.module_access.member_name;

                    ast->kind = AST_FIELD_ACCESS;
                    ast->u.field_access.obj = var_ast;
                    ast->u.field_access.field_name = field_name;
                    ast->u.field_access.field_index = -1;

                    // 释放旧的内存
                    free(member_name);
                    free(module_name);

                    // 继续访问转换后的节点
                    visit(s, ast);
                } else {
                    // 普通属性访问：转换为 AST_INDEX
                    Ast* index_ast = ast_new(AST_STRING, ast->line);
                    index_ast->u.string.value = strdup(ast->u.module_access.member_name);
                    index_ast->u.string.len = (int)strlen(ast->u.module_access.member_name);

                    ast->kind = AST_INDEX;
                    ast->u.index.obj = var_ast;
                    ast->u.index.index = index_ast;

                    visit(s, ast);
                }
            } else {
                // 检查是否是原生模块的常量访问
                const char* actual_module = native_resolve_module_alias(ast->u.module_access.module_name);
                if (native_is_module(actual_module)) {
                    bool found = false;
                    native_find_module_const(actual_module, ast->u.module_access.member_name, &found);
                    if (!found) {
                        // 不是常量，可能是方法名被误解析为 MODULE_ACCESS
                        // 报告错误
                        error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                 "模块 '%s' 中没有常量 '%s'，如果是方法调用请加括号 ()",
                                 actual_module, ast->u.module_access.member_name);
                    }
                    // 原生模块常量：保留 AST_MODULE_ACCESS，设置类型为 TYPE_INT
                    ast->u.module_access.ref.kind = SYM_MODULE;
                    ast->u.module_access.ref.index = 0;
                    ast->u.module_access.ref.name = strdup(ast->u.module_access.module_name);
                    ast->u.module_access.ref.type_kind = TYPE_INT;
                } else {
                    // .leno 用户模块：按原有逻辑处理
                    ast->u.module_access.ref.kind = module_sym->kind;
                    ast->u.module_access.ref.index = module_sym->index;
                    ast->u.module_access.ref.name = strdup(module_sym->name);
                    // 模块符号本身无类型信息（module_sym->type 为 NULL），常量类型需查模块符号表
                    // 修复：跨模块 export const X = module.Y 转发时，类型此前丢失为 TYPE_ANY
                    //       现从 ImportedModuleInfo.sym_table 查常量真实类型
                    TypeKind member_type = TYPE_ANY;
                    ImportedModuleInfo* mod_info = find_imported_module(s, ast->u.module_access.module_name);
                    if (mod_info && mod_info->sym_table) {
                        ModuleVarSymbol* mv = module_symbol_table_find_var(mod_info->sym_table, ast->u.module_access.member_name);
                        if (mv) {
                            // ★ 2026-10-13：**没 export 的变量跨模块读不到** —— 这里以前静默放过 ✗
                            //   症状（实测）：模块里 `var SECRET = 42`（没 export），
                            //     别处 `var v = m.SECRET` ⇒ **编译 0 错**、运行期 v=null，
                            //     直到拿它做运算才炸「加法运算: null 不能参与运算」✗（指不到真因）
                            //   根因：模块符号表**收录了未导出的 var**（它服务模块内部 + use 传导）
                            //     ⇒ "find_var 查得到" ≠ "跨模块可读"（两者不是同一件事）✗
                            //   判据来源：与 `use` / `m.foo()` **同一份** export 名清单（provider ✓）
                            //     —— 不另立第二套"什么算 export"的口径 ✓
                            //   ⚠ 只在 `mv` 命中时判：enum 成员不在 vars 表里（走下面的 enum 兜底 ✓），
                            //     所以这条不会误伤 `m.RED` 这类枚举成员访问 ✓
                            const char* cur_file = error_get_filename();
                            if (mod_info->file_path &&
                                !module_has_export(mod_info->file_path, cur_file,
                                                   ast->u.module_access.member_name)) {
                                error_add_at_fmt(ERR_UNDEFINED_VAR, ast->line, ast->column,
                                         "模块 '%s' 里的变量 '%s' 没有 export —— 跨模块读不到"
                                         "（请在模块里写成 `export var %s = ...` 或 `export const %s = ...`）",
                                         ast->u.module_access.module_name,
                                         ast->u.module_access.member_name,
                                         ast->u.module_access.member_name,
                                         ast->u.module_access.member_name);
                            }
                            member_type = mv->type;
                            // ★ 优先使用完整类型信息（支持 Array[int] 等泛型变量）
                            if (mv->type_info) {
                                if (ast->cached_type) { type_free(ast->cached_type); }
                                ast->cached_type = type_copy(mv->type_info);
                            }
                        } else {
                            // 也可能是 enum 成员（enum 成员可被引用为常量）
                            for (int ei = 0; ei < mod_info->sym_table->enum_count; ei++) {
                                ModuleEnumSymbol* es = &mod_info->sym_table->enums[ei];
                                for (int mi = 0; mi < es->member_count; mi++) {
                                    if (strcmp(es->member_names[mi], ast->u.module_access.member_name) == 0) {
                                        member_type = TYPE_INT;
                                        break;
                                    }
                                }
                                if (member_type != TYPE_ANY) break;
                            }
                            // 也可能是 enum 类型名（如 Scancode），用于 Module.EnumName.member 访问
                            if (member_type == TYPE_ANY) {
                                ModuleEnumSymbol* es = module_symbol_table_find_enum(mod_info->sym_table, ast->u.module_access.member_name);
                                if (es) {
                                    member_type = TYPE_ENUM;
                                    if (ast->cached_type) { type_free(ast->cached_type); }
                                    ast->cached_type = type_new(TYPE_ENUM);
                                    ast->cached_type->struct_name = strdup(ast->u.module_access.member_name);
                                }
                            }
                            // 也可能是 struct 类型名，用于 Module.StructName.CONST 访问
                            if (member_type == TYPE_ANY) {
                                ModuleStructSymbol* ss = module_symbol_table_find_struct(mod_info->sym_table, ast->u.module_access.member_name);
                                if (ss && !ss->is_cstruct) {
                                    member_type = TYPE_STRUCT;
                                    if (ast->cached_type) { type_free(ast->cached_type); }
                                    ast->cached_type = type_new(TYPE_STRUCT);
                                    ast->cached_type->struct_name = strdup(ast->u.module_access.member_name);
                                }
                            }

                            // ★ N2（2026-10-24）：**模块里根本没有这个成员** ⇒ 编译期报错。
                            //   此前静默放过：`print(m.not_exported_var)`（md01 里压根没这个名字）
                            //   编译 0 错、运行期输出 null（exit=0），而"调用不存在的成员"
                            //   `m.xxx()` 却明确报「模块中没有成员 'xxx'」⇒ 两条路口径不一致 ✗
                            //   判据：上面查过 vars / enum 成员 / enum 类型名 / struct 类型名；
                            //   再补函数 / 类型别名 / clib / cfunc / face 五类，全不中才算"不存在" ✓
                            //   ⚠ 只在 sym_table 可用时判（加载失败的模块走上面的 load_failed 早退 ✓）
                            if (member_type == TYPE_ANY) {
                                const char* mn = ast->u.module_access.member_name;
                                int found_member =
                                    module_symbol_table_find_func(mod_info->sym_table, mn) != NULL ||
                                    module_symbol_table_find_alias(mod_info->sym_table, mn) != NULL ||
                                    module_symbol_table_find_clib(mod_info->sym_table, mn) != NULL ||
                                    module_symbol_table_find_cfunc(mod_info->sym_table, mn) != NULL ||
                                    module_symbol_table_find_face(mod_info->sym_table, mn) != NULL ||
                                    // cstruct 类型名**必须**算存在：上面的 struct 分支故意跳过
                                    //   is_cstruct（那种形态不作为 TYPE_STRUCT 使用），但它作为
                                    //   `new a.Vec()` / `a.SDL_Rect` 的**类型名**是合法成员 ✓
                                    //   （实测漏判：LenoSDL3 的 `core.SDL_Rect` 全被误报 ✗）
                                    module_symbol_table_find_struct(mod_info->sym_table, mn) != NULL;
                                if (!found_member) {
                                    error_add_at_fmt(ERR_UNDEFINED_VAR, ast->line, ast->column,
                                        "模块 '%s' 中没有成员 '%s'%s",
                                        ast->u.module_access.module_name, mn,
                                        module_symbol_table_hint(mod_info->sym_table, mn));
                                }
                            }
                        }
                    }
                    ast->u.module_access.ref.type_kind = member_type;
                }
            }
            break;
        }

        case AST_EXPORT:
            if (ast->u.export.decl) {
                visit(s, ast->u.export.decl);
                // ★ WARN_DEFAULT_VAR_SNAPSHOT（2026-10-17）：**导出函数**的默认值引用
                //   **可变的模块变量**时，跨模块与同模块的求值时机不同 ——
                //   跨模块：调用点拿不到被调 AST，默认值在定义模块编译期折叠 ⇒ **定义时**取一次；
                //   同模块：仍按**调用时**读取 ⇒ 变量被改写后两边得到不同的值（静默分叉 ✗）。
                //   只对导出函数报：非导出函数不可能被别的模块调用，不可能分叉 ✓
                //   const / enum / 字面量 / 表达式（无变量）都不报：它们前后恒等 ✓
                Ast* d = ast->u.export.decl;
                if (d->kind == AST_FUNC_DEF && d->u.func.param_defaults) {
                    for (int pi = 0; pi < d->u.func.pcnt; pi++) {
                        Ast* def = d->u.func.param_defaults[pi];
                        if (!def || def->kind != AST_VAR || !def->u.var.name) continue;
                        SymRef tref;
                        memset(&tref, 0, sizeof(tref));
                        Symbol* dsym = resolve_variable_with_upvalue_raw(s, def->u.var.name, &tref);
                        if (!dsym || dsym->is_const) continue;
                        if (tref.kind != SYM_GLOBAL && tref.kind != SYM_MODULE) continue;
                        warning_add_at_fmt(WARN_DEFAULT_VAR_SNAPSHOT, d->line, d->column,
                            "函数 '%s' 的参数 '%s' 默认值引用了**可变的模块变量 '%s'**："
                            "跨模块调用按**定义时**取值、同模块调用按**调用时**取值 ⇒ 变量被改写后"
                            "两边可能不同。建议改成 `const`，或不写默认值、在函数体里处理",
                            d->u.func.name ? d->u.func.name : "?",
                            d->u.func.params ? d->u.func.params[pi] : "?", def->u.var.name);
                    }
                }
            }
            break;
