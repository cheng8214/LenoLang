        case AST_STRUCT_DEF:
            // 注册 struct 类型
            {
                // 关联常量求值函数（定义在 codegen_stmt.c 中）
                extern Value ast_default_to_value(Ast* expr);
                TypeInfo* struct_type = type_new(TYPE_STRUCT);
                struct_type->struct_name = strdup(ast->u.struct_def.name);

                // ★ 与 native 类型**同名**的编译期诊断（2026-09-27）：
                //   类型名两边共用同一个全局名字空间，而字段解析是"先查符号表" ⇒ 脚本定义会
                //   **静默遮蔽** native 规格：同一个名字下，字段按脚本定义解析、native 值却按
                //   native 规格解释 ⇒ 形状不一致时只有**运行期**才报错（native_struct_def_for 的
                //   "同形才复用"）。这里提前到编译期讲清楚。
                //   用**警告**而非错误：形状完全一致时是合法用法（运行期复用同一份定义）。
                {
                    const NativeStructSpec* ns = native_find_struct_spec(ast->u.struct_def.name);
                    if (ns) {
                        char msg[BUFFER_MEDIUM];
                        const char* owner = (ns->module_name && ns->module_name[0]) ? ns->module_name : "?";
                        snprintf(msg, sizeof(msg),
                                 "struct '%s' 与 native 类型同名（来自 '%s'）：字段解析会优先用你这个"
                                 "脚本定义，而 native 值按 native 规格解释 —— 形状不一致时运行期会"
                                 "直接报错。建议改名（如 'My%s'），或 `use %s.%s` 直接用 native 类型",
                                 ast->u.struct_def.name, owner,
                                 ast->u.struct_def.name, owner, ast->u.struct_def.name);
                        warning_add_at(WARN_NATIVE_TYPE_NAME_CLASH, ast->line, ast->column, msg);
                    }
                }

                // 泛型 struct：将字段类型中的泛型参数标记为 TYPE_GENERIC_PARAM
                if (ast->u.struct_def.type_param_count > 0 && ast->u.struct_def.type_params) {
                    for (int i = 0; i < ast->u.struct_def.field_count; i++) {
                        resolve_generic_in_type(ast->u.struct_def.field_types[i],
                            ast->u.struct_def.type_params, ast->u.struct_def.type_param_constraints, ast->u.struct_def.type_param_count);
                    }
                    // ★ 2026-10-03：**方法签名**也要解析（此前只解析了字段 ✗）
                    //   不解析的后果：方法参数/返回里的 T 停在"TYPE_STRUCT 占位"表示、且**不带约束**
                    //   —— 实测（调试打印）：`struct Cell[T: int]` 的方法体里 arg_cons=null、
                    //     method_cons0=null，只有接收者声明表里有约束 ⇒ 只能靠各处兜底 ✗；
                    //   解析后：`T` 统一成 TYPE_GENERIC_PARAM + constraint_name ✓
                    //   （那一批"只认 TYPE_GENERIC_PARAM"的判据在方法参数上也因此受益 ✓）
                    for (int mi = 0; mi < ast->u.struct_def.method_count; mi++) {
                        Ast* ma = ast->u.struct_def.methods[mi];
                        if (!ma || ma->kind != AST_FUNC_DEF) continue;
                        for (int pi = 0; pi < ma->u.func.pcnt; pi++) {
                            resolve_generic_in_type(ma->u.func.param_types[pi],
                                ast->u.struct_def.type_params, ast->u.struct_def.type_param_constraints,
                                ast->u.struct_def.type_param_count);
                        }
                        resolve_generic_in_type(ma->u.func.return_type,
                            ast->u.struct_def.type_params, ast->u.struct_def.type_param_constraints,
                            ast->u.struct_def.type_param_count);
                    }
                }

                // 先验证所有字段类型是否有效（在注册 struct 之前）
                for (int i = 0; i < ast->u.struct_def.field_count; i++) {
                    TypeInfo* field_type = ast->u.struct_def.field_types[i];

                    // ★ 解析字段类型中的 alias（如 Color = int）
                    //   跨模块 use 导入的 alias 在 parser 阶段可能未传导到 parser alias 表，
                    //   导致字段类型保留为 TYPE_STRUCT + struct_name="Color"。
                    //   此处与函数参数/变量声明一样调用 resolve_alias_in_type，
                    //   通过语义作用域中注册的 SYM_TYPE 符号解析为底层类型。
                    if (field_type) {
                        resolve_alias_in_type(s, &ast->u.struct_def.field_types[i], ast->line);
                        field_type = ast->u.struct_def.field_types[i]; // resolve 后指针可能变
                    }

                    // struct 字段不能使用 var 或 any，必须有具体类型
                    if (field_type && (field_type->kind == TYPE_INFER || field_type->kind == TYPE_ANY)) {
                        char msg[BUFFER_MEDIUM];
                        const char* kind_str = (field_type->kind == TYPE_INFER) ? "var" : "any";
                        snprintf(msg, sizeof(msg), "struct '%s' 的字段 '%s' 不能使用 %s，必须使用具体类型",
                                 ast->u.struct_def.name, ast->u.struct_def.field_names[i], kind_str);
                        error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
                    }

                    // 如果字段类型是 struct/clib/cstruct/face，验证是否已定义
                    if (field_type && field_type->struct_name) {
                        int is_named_type = (field_type->kind == TYPE_STRUCT ||
                                            field_type->kind == TYPE_CLIB ||
                                            field_type->kind == TYPE_CSTRUCT ||
                                            field_type->kind == TYPE_FACE);
                        if (!is_named_type && field_type->kind == TYPE_STRUCT) is_named_type = 1;
                        if (is_named_type) {
                            // 允许自引用（struct 包含自身类型的字段，如链表节点）
                            if (strcmp(field_type->struct_name, ast->u.struct_def.name) != 0) {
                                // 检查引用的类型是否已定义
                                Symbol* ref_type = scope_resolve(s->current, field_type->struct_name);
                                if (!ref_type && semantic_native_struct_known(field_type->struct_name)) {
                                    // native struct 兜底（v3.2.5）：脚本 struct 的字段可以是
                                    //   `DirInfo` / `DirEntry`（字段表由 native 规格提供，
                                    //   与运行期 ObjStructDef 同源）
                                } else if (!ref_type || !ref_type->type ||
                                    (ref_type->type->kind != TYPE_STRUCT &&
                                     ref_type->type->kind != TYPE_CLIB &&
                                     ref_type->type->kind != TYPE_CSTRUCT &&
                                     ref_type->type->kind != TYPE_FACE)) {
                                    char msg[1024];   /* 大于 BUFFER_MEDIUM：容纳中文 + 提示（同 §"方法不存在"那处 ✓） */
                                    char hint[BUFFER_MEDIUM];
                                    // ★ 2026-10-10：提示走共享实现 ⇒ 会去已导入模块里找它 ✓
                                    semantic_undefined_type_hint(s, field_type->struct_name, hint, sizeof(hint));
                                    snprintf(msg, sizeof(msg), "%s: struct '%s' 的字段 '%s' 使用了未定义的类型 '%s'",
                                             "未定义的类型", ast->u.struct_def.name,
                                             ast->u.struct_def.field_names[i],
                                             field_type->struct_name);
                                    strncat(msg, hint, sizeof(msg) - strlen(msg) - 1);
                                    error_add_at(ERR_UNDEFINED_VAR, ast->line, ast->column, msg);
                                } else if (ref_type->type->kind == TYPE_CLIB) {
                                    // 修正字段类型为 TYPE_CLIB
                                    field_type->kind = TYPE_CLIB;
                                } else if (ref_type->type->kind == TYPE_CSTRUCT) {
                                    field_type->kind = TYPE_CSTRUCT;
                                } else if (ref_type->type->kind == TYPE_FACE) {
                                    field_type->kind = TYPE_FACE;
                                }
                            }
                        }
                    }
                }

                // 注册到符号表（或更新已预注册的符号）
                SymKind kind;
                if (s->current && s->current->parent == NULL) {
                    // 根作用域：模块模式下使用 SYM_MODULE，否则使用 SYM_GLOBAL
                    kind = s->is_module ? SYM_MODULE : SYM_GLOBAL;
                } else {
                    kind = SYM_LOCAL;
                }

                // 同名冲突标记：跨种类重名或同类重复定义时置 1。
                // 四张类型表（struct/cstruct/enum/face）统一策略——**报错后不再写入全局定义表**，
                // 否则一个非法定义会留在表里，被后续 *_def_find / struct_def_find 取到并派生次级错误。
                int struct_dup = 0;

                // 先检查是否已经预注册
                Symbol* sym = scope_resolve_local(s->current, ast->u.struct_def.name);
                if (sym && sym->type && sym->type->kind == TYPE_STRUCT) {
                    // 同名同种类：既可能是"预注册占位"（同一条声明），也可能是"源码里又写了
                    // 一条"。type_decl_ast 记的是该符号归哪条声明：为空 = 预注册刚建的、主阶段
                    // 还没认领，故必是同一条声明；已有值且不等于当前 AST，就是真的又声明了一次。
                    // 此前这里只看 kind 就无条件复用，于是 `struct E1 {..}` 写两遍静默通过，
                    // 且第二份把第一份的字段整份顶掉（直到用 E1.x 才报"没有字段 x"，归因跑偏）。
                    if (sym->type_decl_ast && sym->type_decl_ast != (const void*)ast) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "struct '%s' 重复定义", ast->u.struct_def.name);
                        error_add_at(ERR_DUPLICATE_VAR, ast->line, ast->column, msg);
                        type_free(struct_type);
                        break;  // 不更新符号、不写全局定义表
                    }
                    // 已预注册（或同一条声明重复进入），更新现有符号
                    type_free(sym->type);
                    sym->type = struct_type;
                    sym->type_decl_ast = ast;
                } else {
                    // 未注册，创建新符号
                    sym = scope_define(s->current, ast->u.struct_def.name, kind);
                    if (sym) sym->type_decl_ast = ast;
                }

                if (sym) {
                    sym->type = struct_type;

                    // 存储 struct 字段信息
                    sym->struct_field_count = ast->u.struct_def.field_count;
                    sym->struct_field_names = (char**)malloc(sizeof(char*) * ast->u.struct_def.field_count);
                    sym->struct_field_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * ast->u.struct_def.field_count);
                    sym->struct_field_null_default = (int*)malloc(sizeof(int) * ast->u.struct_def.field_count);

                    for (int i = 0; i < ast->u.struct_def.field_count; i++) {
                        sym->struct_field_names[i] = strdup(ast->u.struct_def.field_names[i]);
                        sym->struct_field_types[i] = type_copy(ast->u.struct_def.field_types[i]);
                        // 检查字段默认值是否为 null
                        if (ast->u.struct_def.field_defaults[i] &&
                            ast->u.struct_def.field_defaults[i]->kind == AST_NULL) {
                            sym->struct_field_null_default[i] = 1;
                        } else if (!ast->u.struct_def.field_defaults[i] &&
                                   ast->u.struct_def.field_types[i]->nullable) {
                            // nullable 字段无显式默认值时，默认为 null
                            sym->struct_field_null_default[i] = 1;
                        } else {
                            sym->struct_field_null_default[i] = 0;
                        }

                        // ★ 标量字段未显式初始化 ⇒ 运行时是 **null**（不是 0 ✗）
                        //   实测：`struct S { int k }` 里 k + 1 直接报「null 不能参与运算」✗。
                        //   Array 字段自动成空数组 ✓、Ptr 自动 null ✓（都安全 ✓），只有标量最坑 ✗
                        //   ⇒ 给一条编译期警告 ✓（可空标量字段 `int? k` 是显式选择 null，跳过 ✓）
                        {
                            TypeInfo* ftt = ast->u.struct_def.field_types[i];
                            int is_scalar = ftt && (ftt->kind == TYPE_INT || ftt->kind == TYPE_FLOAT ||
                                                    ftt->kind == TYPE_BOOL || ftt->kind == TYPE_STRING);
                            if (is_scalar && !ftt->nullable && !ast->u.struct_def.field_defaults[i]) {
                                char wmsg[BUFFER_MEDIUM];
                                snprintf(wmsg, sizeof(wmsg),
                                         "struct '%s' 的字段 '%s' 未显式初始化 —— 标量字段默认是 null（不是 0），"
                                         "参与运算会直接报错；请写明初值（如 '%s %s = 0'）或改用可空类型 '%s? %s'",
                                         ast->u.struct_def.name, ast->u.struct_def.field_names[i],
                                         type_kind_to_string(ftt->kind), ast->u.struct_def.field_names[i],
                                         type_kind_to_string(ftt->kind), ast->u.struct_def.field_names[i]);
                                warning_add_at(WARN_FIELD_NO_INIT, ast->line, ast->column, wmsg);
                            }
                        }
                    }

                    // 存储泛型类型参数信息
                    if (ast->u.struct_def.type_param_count > 0 && ast->u.struct_def.type_params) {
                        sym->struct_type_param_count = ast->u.struct_def.type_param_count;
                        sym->struct_type_params = (char**)malloc(sizeof(char*) * ast->u.struct_def.type_param_count);
                        for (int i = 0; i < ast->u.struct_def.type_param_count; i++) {
                            sym->struct_type_params[i] = strdup(ast->u.struct_def.type_params[i]);
                        }
                    }
                } else {
                    char msg[BUFFER_MEDIUM];
                    // 检查是否已经被定义为其他类型（cstruct/enum/face）
                    Symbol* existing = scope_resolve_local(s->current, ast->u.struct_def.name);
                    if (existing && existing->type) {
                        if (existing->type->kind == TYPE_CSTRUCT) {
                            snprintf(msg, sizeof(msg), "类型 '%s' 已经定义为 cstruct，不能重复定义为 struct", ast->u.struct_def.name);
                        } else if (existing->type->kind == TYPE_ENUM) {
                            snprintf(msg, sizeof(msg), "类型 '%s' 已经定义为 enum，不能重复定义为 struct", ast->u.struct_def.name);
                        } else if (existing->type->kind == TYPE_FACE) {
                            snprintf(msg, sizeof(msg), "类型 '%s' 已经定义为 face，不能重复定义为 struct", ast->u.struct_def.name);
                        } else {
                            snprintf(msg, sizeof(msg), "struct '%s' 重复定义", ast->u.struct_def.name);
                        }
                    } else {
                        snprintf(msg, sizeof(msg), "struct '%s' 重复定义", ast->u.struct_def.name);
                    }
                    struct_dup = 1;
                    error_add_at(ERR_DUPLICATE_VAR, ast->line, ast->column, msg);
                }

                // 处理关联常量：语义检查 + 求值存入 ObjStructDef
                // 必须在字段默认值之前处理，以便字段初始化器可以引用 StructName.CONST
                if (ast->u.struct_def.const_count > 0 && ast->u.struct_def.const_values) {
                    ObjStructDef* cdef = struct_def_find(ast->u.struct_def.name);
                    if (cdef) {
                        cdef->const_count = ast->u.struct_def.const_count;
                        cdef->const_names = (char**)malloc(sizeof(char*) * cdef->const_count);
                        cdef->const_values = (Value*)calloc(cdef->const_count, sizeof(Value));
                        for (int ci = 0; ci < cdef->const_count; ci++) {
                            // 语义检查常量表达式
                            if (ast->u.struct_def.const_values[ci]) {
                                visit(s, ast->u.struct_def.const_values[ci]);
                                // 求值为常量 Value
                                Value cv = ast_default_to_value(ast->u.struct_def.const_values[ci]);
                                if (val_is_null(cv) && ast->u.struct_def.const_values[ci]->kind != AST_NULL) {
                                    char msg[BUFFER_MEDIUM];
                                    snprintf(msg, sizeof(msg),
                                        "struct '%s' 的关联常量 '%s' 不是常量表达式（仅支持数字、字符串、bool、null、数组字面量、字典字面量）",
                                        ast->u.struct_def.name, ast->u.struct_def.const_names[ci]);
                                    error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
                                }
                                cdef->const_values[ci] = cv;
                            } else {
                                cdef->const_values[ci] = val_null();
                            }
                            cdef->const_names[ci] = strdup(ast->u.struct_def.const_names[ci]);
                        }
                    }
                }

                // 处理默认值表达式（在关联常量之后，以便字段初始化器可引用 StructName.CONST）
                // 先将字段初始化器中的裸常量名转换为 StructName.CONST（AST_MODULE_ACCESS）
                // 传 NULL/0 给 field_names 和 method_names，只转换常量名，不转换字段/方法引用
                if (ast->u.struct_def.const_count > 0) {
                    for (int i = 0; i < ast->u.struct_def.field_count; i++) {
                        if (ast->u.struct_def.field_defaults[i]) {
                            transform_method_body(ast->u.struct_def.field_defaults[i],
                                NULL, 0, NULL, 0, ast->u.struct_def.name,
                                NULL, 0, ast->u.struct_def.const_names, ast->u.struct_def.const_count, NULL,
                                NULL, NULL, 0,
                                NULL, NULL);
                        }
                    }
                }
                for (int i = 0; i < ast->u.struct_def.field_count; i++) {
                    if (ast->u.struct_def.field_defaults[i]) {
                        visit(s, ast->u.struct_def.field_defaults[i]);
                    }
                }

                // 处理方法定义：分两阶段
                // ★ 诊断用「同名歧义表」（2026-09-30）：本 struct 的方法名中，哪些**还能按裸名调用到**
                //   全局/模块函数（scope_resolve 命中 SYM_GLOBAL_FUNC/SYM_MODULE ⇒ 裸名确实可调用 ✓），
                //   并记下那个函数的形参个数。方法体内的裸 `NAME(...)` 会被优先解析成**方法** ⇒
                //   个数两边都吻合时两种解释都成立（静默走错目标 ✗）⇒ 由 transform 报 [同名歧义] ✓
                //   ⚠ 用 scope_resolve 而不是 func_table_find 单独判：前者才是"这个裸名在这里到底
                //     能不能调到东西"的口径（能挡掉同名但不可裸调的库函数，少误报 ✓）
                char** amb_names = NULL;
                int*   amb_pcnts = NULL;
                int*   amb_meth_pcnts = NULL;   // ★ 撞名**方法自己**的形参个数（配合个数闸门 ✓）
                int    amb_count = 0;
                if (ast->u.struct_def.method_count > 0) {
                    amb_names = (char**)malloc(sizeof(char*) * ast->u.struct_def.method_count);
                    amb_pcnts = (int*)malloc(sizeof(int) * ast->u.struct_def.method_count);
                    amb_meth_pcnts = (int*)malloc(sizeof(int) * ast->u.struct_def.method_count);
                }
                for (int m = 0; amb_names && m < ast->u.struct_def.method_count; m++) {
                    Ast* ma = ast->u.struct_def.methods[m];
                    if (!ma || ma->kind != AST_FUNC_DEF || !ma->u.func.name) continue;
                    Symbol* sym = scope_resolve(s->current, ma->u.func.name);
                    if (!sym || (sym->kind != SYM_GLOBAL_FUNC && sym->kind != SYM_MODULE)) continue;
                    Ast* gf = func_table_find(&s->func_table, ma->u.func.name);
                    if (!gf || gf->kind != AST_FUNC_DEF) continue;
                    // ★★ 2026-10-10 改：**不再只收"个数相同"的** ✗
                    //   旧口径（`if (gf->u.func.pcnt != ma->u.func.pcnt) continue;`）有个致命副作用：
                    //   个数**不同**时（= 裸调用其实只可能是那个函数的 ✓）既不收进表、也没人管 ⇒
                    //   裸调用照样被"方法优先"劫持 ⇒ 报“参数过多”或**静默调错目标** ✗
                    //   （上游 LenoSDL3 那次「打包后句柄下发静默失效」+ 本仓 10 组同名撞车 ✓）
                    //   现在**全收** ✓：个数闸门（semantic_visit_method.c）拿它判"这行调用到底更像哪一个" ✓；
                    //   下面的歧义告警自带"两边个数都吻合"的条件 ✓ ⇒ 告警语义与从前逐字一致 ✓
                    //   ⚠ 两列含义别搞混：amb_pcnts = 同名**函数**的个数 ✓、amb_meth_pcnts = **方法**自己的 ✓
                    amb_names[amb_count] = ma->u.func.name;
                    amb_pcnts[amb_count] = gf->u.func.pcnt;
                    amb_meth_pcnts[amb_count] = ma->u.func.pcnt;
                    amb_count++;
                }
                // ★★ 2026-10-10：方法名表 / 形参个数表**必须在"逐个方法补 self"之前**就建好 ✓
                //   为什么：下面阶段1 每处理完一个方法就把它的 `u.func.pcnt` **+1**（补隐式 self ✓）
                //   ⇒ 若在循环内部建表，**前面已处理过的方法**读到的是"形参个数 + 1" ✗
                //   （实测：`func poke(Widget c)` 被闸门读成 2 ✗ ⇒ 判据失效、劫持照旧 ✗）
                //   这里一律在进循环前读，此时每个 pcnt 都还是"声明里写的那个" ✓
                char** method_names = NULL;
                int*   method_pcounts = NULL;      // 与 method_names 平行：各方法声明的形参个数 ✓
                int    method_count = ast->u.struct_def.method_count;
                if (method_count > 0) {
                    method_names = (char**)malloc(sizeof(char*) * method_count);
                    method_pcounts = (int*)malloc(sizeof(int) * method_count);
                    for (int m = 0; m < method_count; m++) {
                        Ast* mi = ast->u.struct_def.methods[m];
                        method_names[m] = (mi && mi->u.func.name) ? mi->u.func.name : NULL;
                        method_pcounts[m] = (mi && mi->kind == AST_FUNC_DEF) ? mi->u.func.pcnt : -1;
                    }
                }
                // 阶段1：准备所有方法（转换方法体、添加self参数、注册到函数表）
                for (int i = 0; i < ast->u.struct_def.method_count; i++) {
                    Ast* method_ast = ast->u.struct_def.methods[i];
                    if (method_ast && method_ast->kind == AST_FUNC_DEF) {
                        // 1. 转换方法体：将字段名访问转换为 self.字段名，方法名调用转换为 self.方法名
                        transform_method_body(method_ast->u.func.body, ast->u.struct_def.field_names, ast->u.struct_def.field_count, method_names, method_count, ast->u.struct_def.name,
                            method_ast->u.func.params, method_ast->u.func.pcnt, ast->u.struct_def.const_names, ast->u.struct_def.const_count,
                            method_ast->u.func.name,   // 本方法名（诊断用：判裸调用是否与自身同名 ⇒ 自递归 ✓）
                            amb_names, amb_pcnts, amb_count,   // 同名表（见上 ✓）
                            method_pcounts, amb_meth_pcnts);   // ★ 个数闸门的两张表 ✓

                        // 2. 添加隐式 self 参数作为第一个参数
                        int new_pcnt = method_ast->u.func.pcnt + 1;
                        char** new_params = (char**)malloc(sizeof(char*) * new_pcnt);
                        TypeInfo** new_param_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * new_pcnt);
                        Ast** new_param_defaults = (Ast**)calloc(new_pcnt, sizeof(Ast*));

                        new_params[0] = strdup("self");
                        new_param_types[0] = type_new(TYPE_STRUCT);
                        new_param_types[0]->struct_name = strdup(ast->u.struct_def.name);
                        // 携带泛型类型参数（如 Calc[T] 的 T）
                        if (ast->u.struct_def.type_param_count > 0 && ast->u.struct_def.type_params) {
                            new_param_types[0]->generic_count = ast->u.struct_def.type_param_count;
                            new_param_types[0]->generic_args = (TypeInfo**)malloc(sizeof(TypeInfo*) * ast->u.struct_def.type_param_count);
                            for (int gi = 0; gi < ast->u.struct_def.type_param_count; gi++) {
                                TypeInfo* gp = type_new(TYPE_GENERIC_PARAM);
                                gp->type_param_name = strdup(ast->u.struct_def.type_params[gi]);
                                new_param_types[0]->generic_args[gi] = gp;
                            }
                        }
                        new_param_defaults[0] = NULL;

                        for (int j = 0; j < method_ast->u.func.pcnt; j++) {
                            new_params[j + 1] = method_ast->u.func.params[j];
                            new_param_types[j + 1] = method_ast->u.func.param_types[j];
                            new_param_defaults[j + 1] = method_ast->u.func.param_defaults ? method_ast->u.func.param_defaults[j] : NULL;
                        }

                        free(method_ast->u.func.params);
                        free(method_ast->u.func.param_types);
                        free(method_ast->u.func.param_defaults);

                        method_ast->u.func.params = new_params;
                        method_ast->u.func.param_types = new_param_types;
                        method_ast->u.func.param_defaults = new_param_defaults;
                        method_ast->u.func.pcnt = new_pcnt;

                        // 3. 注册方法到函数表（使用 struct_name::method_name 格式，避免与全局函数冲突）
                        // 但不注册到符号表作用域链，struct 方法不能从外部直接调用
                        char method_key[256];
                        snprintf(method_key, sizeof(method_key), "%s::%s", ast->u.struct_def.name, method_ast->u.func.name);
                        func_table_add(&s->func_table, method_key, method_ast);
                    }
                }
                if (amb_names) free(amb_names);   // 同名歧义表用完即释（表里存的是 method_name 指针，不拥有 ✓）
                if (amb_pcnts) free(amb_pcnts);
                if (amb_meth_pcnts) free(amb_meth_pcnts);   // ★ 新增的同名表（个数闸门用 ✓）
                if (method_names) free(method_names);       // ★ 名字表/个数表也随阶段1 结束释放 ✓
                if (method_pcounts) free(method_pcounts);

                
                // 阶段2：处理所有方法体（语义分析）
                // 这样方法间可以互相引用，因为所有方法都已注册到函数表
                // 使用 visit_func_as_struct_method 避免将方法注册到外部作用域
                for (int i = 0; i < ast->u.struct_def.method_count; i++) {
                    Ast* method_ast = ast->u.struct_def.methods[i];
                    if (method_ast && method_ast->kind == AST_FUNC_DEF) {
                        // ★ 泛型 struct 的方法体上下文（B 方案第二步，2026-10-02）：方法体里的 T 属于
                        //   **本 struct**（方法自己不声明类型参数）⇒ 需求按 (struct, 方法, T) 记，
                        //   随后在该方法的**调用点**用接收者的类型实参校验 ✓
                        Ast* saved_gs = s->cur_generic_struct;
                        char* saved_gm = s->cur_struct_method_name;
                        s->cur_generic_struct = (ast->u.struct_def.type_param_count > 0) ? ast : NULL;
                        s->cur_struct_method_name = method_ast->u.func.name;

                        visit_func_as_struct_method(s, method_ast);

                        s->cur_generic_struct = saved_gs;
                        s->cur_struct_method_name = saved_gm;
                    }
                }

                // 检查 impl 声明：struct 是否真正满足声明的 face
                for (int i = 0; i < ast->u.struct_def.impl_count; i++) {
                    const char* face_name = ast->u.struct_def.impl_names[i];
                    ObjFaceDef* fdef = face_def_find(face_name);
                    if (!fdef) {
                        int found_in_module = 0;
                        for (int mi = 0; mi < s->imported_module_count && !found_in_module; mi++) {
                            ImportedModuleInfo* m = &s->imported_modules[mi];
                            if (m && m->sym_table) {
                                ModuleFaceSymbol* mface = module_symbol_table_find_face(m->sym_table, face_name);
                                if (mface) {
                                    found_in_module = 1;
                                    for (int j = 0; j < mface->method_count; j++) {
                                        const char* mname = mface->methods[j].name;
                                        int found = 0;
                                        for (int k = 0; k < ast->u.struct_def.method_count; k++) {
                                            if (strcmp(ast->u.struct_def.methods[k]->u.func.name, mname) == 0) {
                                                found = 1;
                                                if (mface->methods[j].return_type != TYPE_ANY && ast->u.struct_def.methods[k]->u.func.return_type) {
                                                    TypeKind face_ret = mface->methods[j].return_type;
                                                    TypeKind impl_ret = ast->u.struct_def.methods[k]->u.func.return_type->kind;
                                                    // ★ v3.2.8：同同文件那条 —— impl 方法**没标返回类型**时
                                                    //   先推函数体，推不出才跳过（宁漏不误报）
                                                    int impl_ret_unknown = 0;
                                                    if (impl_ret == TYPE_INFER) {
                                                        TypeInfo* body_ret = infer_return_type_from_body(
                                                            s, ast->u.struct_def.methods[k]->u.func.body);
                                                        if (body_ret && body_ret->kind != TYPE_ANY) {
                                                            impl_ret = body_ret->kind;
                                                        } else {
                                                            impl_ret_unknown = 1;
                                                        }
                                                        if (body_ret) type_free(body_ret);
                                                    }
                                                    // 允许 TYPE_STRUCT ↔ TYPE_CLIB 互转（都是命名自定义类型）
                                                    int type_match = (face_ret == impl_ret);
                                                    if (!type_match &&
                                                        ((face_ret == TYPE_STRUCT && impl_ret == TYPE_CLIB) ||
                                                         (face_ret == TYPE_CLIB && impl_ret == TYPE_STRUCT))) {
                                                        type_match = 1;
                                                    }
                                                    // ★ v3.2.8：语言允许 **int → float 的隐式加宽**
                                                    //   （实测 `func f(int a): float { return a }` ✓ /
                                                    //   `float x = intVar` ✓），而**收窄**（float → int）
                                                    //   是报错的（要求 `_int()`）⇒ 只放行"期望 float、
                                                    //   实际 int"这一个方向（实测误报：
                                                    //   examples/switch/test_switch_is_face.leno）。
                                                    if (!type_match && face_ret == TYPE_FLOAT && impl_ret == TYPE_INT) {
                                                        type_match = 1;
                                                    }
                                                    if (!impl_ret_unknown && !type_match) {
                                                        char msg[256];
                                                        snprintf(msg, sizeof(msg), "struct '%s' 的方法 '%s' 返回类型与 face '%s' 不匹配: 期望 %s, 实际 %s",
                                                                 ast->u.struct_def.name, mname, face_name,
                                                                 type_kind_to_string(face_ret),
                                                                 type_kind_to_string(impl_ret));
                                                        error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
                                                    }
                                                }
                                                if (mface->methods[j].param_count >= 0 && ast->u.struct_def.methods[k]->u.func.pcnt >= 1) {
                                                    int expected_params = mface->methods[j].param_count;
                                                    int actual_params = ast->u.struct_def.methods[k]->u.func.pcnt - 1;
                                                    if (expected_params != actual_params) {
                                                        char msg[256];
                                                        snprintf(msg, sizeof(msg), "struct '%s' 的方法 '%s' 参数数量与 face '%s' 不匹配: 期望 %d, 实际 %d",
                                                                 ast->u.struct_def.name, mname, face_name,
                                                                 expected_params, actual_params);
                                                        error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
                                                    }
                                                }
                                                break;
                                            }
                                        }
                                        if (!found) {
                                            char msg[256];
                                            snprintf(msg, sizeof(msg), "struct '%s' 声明 impl '%s' 但缺少方法 '%s'",
                                                     ast->u.struct_def.name, face_name, mname);
                                            error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
                                        }
                                    }
                                }
                            }
                        }
                        if (!found_in_module) {
                            char msg[256];
                            snprintf(msg, sizeof(msg), "未定义的 face: '%s'", face_name);
                            error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
                        }
                        continue;
                    }
                    for (int j = 0; j < fdef->method_count; j++) {
                        const char* mname = fdef->methods[j].name;
                        int found = 0;
                        for (int k = 0; k < ast->u.struct_def.method_count; k++) {
                            if (strcmp(ast->u.struct_def.methods[k]->u.func.name, mname) == 0) {
                                found = 1;
                                if (fdef->methods[j].return_type && ast->u.struct_def.methods[k]->u.func.return_type) {
                                    // 泛型 face impl：如果返回类型是泛型参数，替换为 impl 提供的具体类型
                                    TypeKind expected_kind = fdef->methods[j].return_type->kind;
                                    if (expected_kind == TYPE_GENERIC_PARAM &&
                                        fdef->type_param_count > 0 &&
                                        ast->u.struct_def.impl_type_args &&
                                        i < ast->u.struct_def.impl_count &&
                                        ast->u.struct_def.impl_type_args[i]) {
                                        const char* gp_name = fdef->methods[j].return_type->type_param_name;
                                        if (gp_name) {
                                            for (int gi = 0; gi < fdef->type_param_count; gi++) {
                                                if (fdef->type_param_names[gi] &&
                                                    strcmp(fdef->type_param_names[gi], gp_name) == 0 &&
                                                    gi < ast->u.struct_def.impl_type_arg_counts[i]) {
                                                    expected_kind = ast->u.struct_def.impl_type_args[i][gi]->kind;
                                                    break;
                                                }
                                            }
                                        }
                                    }
                                    TypeKind impl_ret2 = ast->u.struct_def.methods[k]->u.func.return_type->kind;
                                    // ★ v3.2.8：impl 方法**没标返回类型**时不能拿 `var` 直接比 ——
                                    //   实测误报（examples/switch/test_switch_{simple2,face2,is_face}.leno
                                    //   三个都撞上）：`func area() { return width * height }` 配 face 的
                                    //   `: int` 报「期望 int, 实际 var」。
                                    //   口径与实例二十五/二十七一致：**没标注就推函数体**
                                    //   （`infer_return_type_from_body`，唯一实现），推不出才跳过（宁漏不误报）。
                                    int impl_ret2_unknown = 0;
                                    if (impl_ret2 == TYPE_INFER) {
                                        TypeInfo* body_ret = infer_return_type_from_body(
                                            s, ast->u.struct_def.methods[k]->u.func.body);
                                        if (body_ret && body_ret->kind != TYPE_ANY) {
                                            impl_ret2 = body_ret->kind;
                                        } else {
                                            impl_ret2_unknown = 1;
                                        }
                                        if (body_ret) type_free(body_ret);
                                    }
                                    int type_match2 = (expected_kind == impl_ret2);
                                    if (!type_match2 &&
                                        ((expected_kind == TYPE_STRUCT && impl_ret2 == TYPE_CLIB) ||
                                         (expected_kind == TYPE_CLIB && impl_ret2 == TYPE_STRUCT))) {
                                        type_match2 = 1;
                                    }
                                    // ★ v3.2.8：同跨模块那条 —— 只放行 int → float 的**加宽**方向
                                    //   （收窄 float → int 仍要报，语言要求显式 `_int()`）
                                    if (!type_match2 && expected_kind == TYPE_FLOAT && impl_ret2 == TYPE_INT) {
                                        type_match2 = 1;
                                    }
                                    if (!impl_ret2_unknown && expected_kind != TYPE_ANY && !type_match2) {
                                        char msg[256];
                                        snprintf(msg, sizeof(msg), "struct '%s' 的方法 '%s' 返回类型与 face '%s' 不匹配: 期望 %s, 实际 %s",
                                                 ast->u.struct_def.name, mname, face_name,
                                                 type_kind_to_string(expected_kind),
                                                 type_kind_to_string(impl_ret2));
                                        error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
                                    }
                                }
                                if (fdef->methods[j].param_count >= 0 && ast->u.struct_def.methods[k]->u.func.pcnt >= 0) {
                                    int expected_params = fdef->methods[j].param_count;
                                    int actual_params = ast->u.struct_def.methods[k]->u.func.pcnt - 1;
                                    if (expected_params != actual_params) {
                                        char msg[256];
                                        snprintf(msg, sizeof(msg), "struct '%s' 的方法 '%s' 参数数量与 face '%s' 不匹配: 期望 %d, 实际 %d",
                                                 ast->u.struct_def.name, mname, face_name,
                                                 expected_params, actual_params);
                                        error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
                                    }
                                }
                                break;
                            }
                        }
                        if (!found) {
                            char msg[256];
                            snprintf(msg, sizeof(msg), "struct '%s' 声明 impl '%s' 但缺少方法 '%s'",
                                     ast->u.struct_def.name, face_name, mname);
                            error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
                        }
                    }
                }

                // 同名冲突已报错：不再创建/覆盖全局 struct 定义表（与 cstruct/enum/face 统一策略）
                if (struct_dup) break;

                ObjStructDef* early_def = struct_def_find(ast->u.struct_def.name);
                if (early_def) {
                    // 预注册阶段已创建 early_def，但可能缺少 ctor/dtor 信息，补上
                    for (int i = 0; i < ast->u.struct_def.method_count; i++) {
                        if (ast->u.struct_def.methods[i]->u.func.is_ctor) {
                            if (early_def->has_ctor) {
                                error_add_at(ERR_SEMANTIC, ast->line, ast->column, "结构体不能有多个构造函数");
                            }
                            early_def->has_ctor = 1;
                            early_def->ctor_index = i;
                        }
                        if (ast->u.struct_def.methods[i]->u.func.is_dtor) {
                            if (early_def->has_dtor) {
                                error_add_at(ERR_SEMANTIC, ast->line, ast->column, "结构体不能有多个析构函数");
                            }
                            early_def->has_dtor = 1;
                            early_def->dtor_index = i;
                        }
                    }
                } else {
                    early_def = struct_def_new(ast->u.struct_def.name, ast->u.struct_def.field_count, ast->u.struct_def.method_count);
                    if (early_def) {
                        early_def->impl_count = ast->u.struct_def.impl_count;
                        early_def->type_param_count = ast->u.struct_def.type_param_count;
                        if (early_def->type_param_count > 0 && ast->u.struct_def.type_params) {
                            early_def->type_param_names = (char**)malloc(sizeof(char*) * early_def->type_param_count);
                            for (int i = 0; i < early_def->type_param_count; i++) {
                                early_def->type_param_names[i] = strdup(ast->u.struct_def.type_params[i]);
                            }
                        }
                        if (early_def->impl_count > 0) {
                            early_def->impl_names = (char**)malloc(sizeof(char*) * early_def->impl_count);
                            for (int i = 0; i < early_def->impl_count; i++) {
                                early_def->impl_names[i] = strdup(ast->u.struct_def.impl_names[i]);
                            }
                        }
                        for (int i = 0; i < ast->u.struct_def.method_count; i++) {
                            early_def->methods[i].name = strdup(ast->u.struct_def.methods[i]->u.func.name);
                            // 检查构造/析构函数
                            if (ast->u.struct_def.methods[i]->u.func.is_ctor) {
                                if (early_def->has_ctor) {
                                    error_add_at(ERR_SEMANTIC, ast->line, ast->column, "结构体不能有多个构造函数");
                                }
                                early_def->has_ctor = 1;
                                early_def->ctor_index = i;
                            }
                            if (ast->u.struct_def.methods[i]->u.func.is_dtor) {
                                if (early_def->has_dtor) {
                                    error_add_at(ERR_SEMANTIC, ast->line, ast->column, "结构体不能有多个析构函数");
                                }
                                early_def->has_dtor = 1;
                                early_def->dtor_index = i;
                            }
                        }
                        struct_def_register(early_def);
                    }
                }
            }
            break;

        case AST_FACE_DEF:
            {
                TypeInfo* face_type = type_new(TYPE_FACE);
                face_type->struct_name = strdup(ast->u.face_def.name);

                // 同名冲突标记（与 struct 分支同一策略：报错后不再写入全局 face 定义表）
                int face_dup = 0;

                Symbol* existing = scope_resolve_local(s->current, ast->u.face_def.name);
                if (existing && existing->type && existing->type->kind == TYPE_FACE) {
                    // 同名同种类：type_decl_ast 为空 = 预注册刚建的、主阶段还没认领（同一条声明）；
                    // 已有值且不等于当前 AST = 源码里真的又写了一条同种类同名声明 ⇒ 报重复定义。
                    // 此前只看 kind 就无条件复用，`face E1 {..}` 写两遍静默通过、后写的那份生效。
                    if (existing->type_decl_ast && existing->type_decl_ast != (const void*)ast) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "face '%s' 重复定义", ast->u.face_def.name);
                        error_add_at(ERR_DUPLICATE_VAR, ast->line, ast->column, msg);
                        type_free(face_type);
                        break;  // 不更新符号、不写全局定义表
                    }
                    type_free(existing->type);
                    existing->type = face_type;
                    existing->type_decl_ast = ast;
                } else if (existing && existing->type) {
                    // 跨种类同名：struct 分支早已检查（cstruct/enum 亦然），唯独 face 漏了，
                    // 于是 `struct Foo` + `face Foo` 不报错、两张全局表各存一份，后续按名查
                    // 全局表会取到"碰巧先命中的那份"。这里补齐同一套检查。
                    char msg[BUFFER_MEDIUM];
                    if (existing->type->kind == TYPE_STRUCT) {
                        snprintf(msg, sizeof(msg), "类型 '%s' 已经定义为 struct，不能重复定义为 face", ast->u.face_def.name);
                    } else if (existing->type->kind == TYPE_CSTRUCT) {
                        snprintf(msg, sizeof(msg), "类型 '%s' 已经定义为 cstruct，不能重复定义为 face", ast->u.face_def.name);
                    } else if (existing->type->kind == TYPE_ENUM) {
                        snprintf(msg, sizeof(msg), "类型 '%s' 已经定义为 enum，不能重复定义为 face", ast->u.face_def.name);
                    } else {
                        snprintf(msg, sizeof(msg), "face '%s' 重复定义", ast->u.face_def.name);
                    }
                    face_dup = 1;
                    error_add_at(ERR_DUPLICATE_VAR, ast->line, ast->column, msg);
                    type_free(face_type);   // 未挂到任何 symbol 上，避免泄漏
                } else {
                    SymKind kind = SYM_TYPE;
                    Symbol* sym = scope_define(s->current, ast->u.face_def.name, kind);
                    if (sym) {
                        type_free(sym->type);
                        sym->type = face_type;
                        sym->type_decl_ast = ast;
                        sym->struct_field_count = ast->u.face_def.method_count;
                        sym->struct_field_names = (char**)malloc(sizeof(char*) * ast->u.face_def.method_count);
                        sym->struct_field_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * ast->u.face_def.method_count);
                        for (int i = 0; i < ast->u.face_def.method_count; i++) {
                            sym->struct_field_names[i] = strdup(ast->u.face_def.method_names[i]);
                            if (ast->u.face_def.method_return_types[i]) {
                                sym->struct_field_types[i] = type_copy(ast->u.face_def.method_return_types[i]);
                                resolve_generic_in_type(sym->struct_field_types[i],
                                    ast->u.face_def.type_params, ast->u.face_def.type_param_constraints,
                                    ast->u.face_def.type_param_count);
                            } else {
                                sym->struct_field_types[i] = type_new(TYPE_ANY);
                            }
                        }
                    }
                }

                // 同名冲突已报错：不再注册（全局 face 定义表保持原状）
                if (face_dup) break;

                ObjFaceDef* fdef = face_def_new(ast->u.face_def.name, ast->u.face_def.method_count);
                if (fdef) {
                    if (ast->u.face_def.type_param_count > 0 && ast->u.face_def.type_params) {
                        face_def_set_type_params(fdef, ast->u.face_def.type_param_count,
                            ast->u.face_def.type_params, ast->u.face_def.type_param_constraints);
                    }
                    for (int i = 0; i < ast->u.face_def.method_count; i++) {
                        fdef->methods[i].name = strdup(ast->u.face_def.method_names[i]);
                        fdef->methods[i].param_count = ast->u.face_def.method_param_counts[i];
                        if (ast->u.face_def.method_return_types && ast->u.face_def.method_return_types[i]) {
                            fdef->methods[i].return_type = type_copy(ast->u.face_def.method_return_types[i]);
                            resolve_generic_in_type(fdef->methods[i].return_type,
                                ast->u.face_def.type_params, ast->u.face_def.type_param_constraints,
                                ast->u.face_def.type_param_count);
                        } else {
                            fdef->methods[i].return_type = NULL;
                        }
                    }
                    face_def_register(fdef);
                }
            }
            break;