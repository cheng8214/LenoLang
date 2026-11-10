        case AST_ENUM_DEF:
            // 注册 enum 类型
            {
                // ★ 2026-11-10（P4 步 2/6）：**带载荷 enum** 已经在
                //   `enum_payload_synthesize()`（semantic.c）里降级成**同名 struct**
                //   `struct Msg { int tag; p0; p1 … }` 并插到顶层最前面 ⇒
                //   那条 struct 声明已经把这个类型名注册好（字段索引也在那边），
                //   这条 enum 声明本身**什么都不用做**（成员不是 int 常量，不能注册成值）。
                if (ast->u.enum_def.payload_state == 1) break;
                if (ast->u.enum_def.payload_state == 2) break;   // 合成阶段已报错，别级联
                if (ast->u.enum_def.member_param_counts) {
                    int payload_member_count = 0;
                    for (int i = 0; i < ast->u.enum_def.member_count; i++) {
                        if (ast->u.enum_def.member_param_counts[i] > 0) payload_member_count++;
                    }
                    if (payload_member_count > 0) {
                        // 能走到这里 = 降级没做成（合成阶段只扫**顶层**语句列表）
                        error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                            "带载荷 enum '%s' 不支持这个位置 —— 它必须声明在**文件顶层**"
                            "（不能写在函数/块里，也不能 `export`）：编译器要把它降级成**同名的** "
                            "`struct { tag; p0; … }`，只在顶层做这件事。"
                            "见 docs/enum载荷_设计草案.md",
                            ast->u.enum_def.name);
                        break;
                    }
                }

                // ★ 2026-11-04：enum 的成员数在 `OP_ENUM_DEF` 里用 `cw_u8` 编码 ⇒ >255 会被
                //   截断成 (n & 0xFF)，随后成员名/值元数据被当成字节码 ⇒ 模块反序列化失败
                //   （实测 256 个成员：--check 全绿，运行报「内存反序列化失败(6)」✗）。
                //   ⚠ 判据 >255（不是 >256）：255 能正常编码；256 写成 0。
                if (ast->u.enum_def.member_count > 255) {
                    error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                                     "enum '%s' 有 %d 个成员，超出上限（最多 255 个）："
                                     "成员数在 enum 定义头里是 1 字节，超出会截断（运行期反序列化失败）；"
                                     "请拆成几个 enum（或改用 const 分组）",
                                     ast->u.enum_def.name, ast->u.enum_def.member_count);
                }

                TypeInfo* enum_type = type_new(TYPE_ENUM);
                enum_type->struct_name = strdup(ast->u.enum_def.name);

                // 注册到符号表
                SymKind kind;
                if (s->current && s->current->parent == NULL) {
                    kind = s->is_module ? SYM_MODULE : SYM_GLOBAL;
                } else {
                    kind = SYM_LOCAL;
                }
                // 预注册复用（与 struct/cstruct/face 同一策略）：预注册阶段已按同名建好
                // TYPE_ENUM 符号时直接复用；否则 scope_define 会因重名返回 NULL，反被下面
                // 的 else 当成"重复定义"误报。kind 保持与预注册时一致。
                Symbol* sym = scope_resolve_local(s->current, ast->u.enum_def.name);
                if (sym && sym->type && sym->type->kind == TYPE_ENUM) {
                    // 同名同种类：type_decl_ast 为空 = 预注册刚建的、主阶段还没认领（同一条声明）；
                    // 已有值且不等于当前 AST = 源码里真的又写了一条同种类同名声明 ⇒ 报重复定义。
                    // 注意这条报错是"补回来"的：02a3d50f 加复用路径之前，第二条 `enum E1` 会走
                    // 下面的 else 报"enum 'E1' 重复定义"，加复用后连真·重复定义也一起吞掉了。
                    if (sym->type_decl_ast && sym->type_decl_ast != (const void*)ast) {
                        error_add_at_fmt(ERR_DUPLICATE_VAR, ast->line, ast->column,
                                         "enum '%s' 重复定义", ast->u.enum_def.name);
                        type_free(enum_type);
                        break;  // 不注册成员、不更新符号
                    }
                    type_free(sym->type);
                    sym->type = enum_type;
                    sym->type_decl_ast = ast;
                } else {
                    sym = scope_define(s->current, ast->u.enum_def.name, kind);
                    if (sym) sym->type_decl_ast = ast;
                }
                if (sym) {
                    sym->type = enum_type;
                    // 保存符号引用信息
                    ast->u.enum_def.ref.kind = sym->kind;
                    ast->u.enum_def.ref.index = sym->index;
                    ast->u.enum_def.ref.name = strdup(sym->name);
                    ast->u.enum_def.ref.type_kind = sym->type ? sym->type->kind : TYPE_ANY;
                    
                    // 存储 enum 成员值信息（供 EnumName.member 访问时查找）
                    sym->enum_value_count = ast->u.enum_def.member_count;
                    sym->enum_value_names = (char**)malloc(sizeof(char*) * ast->u.enum_def.member_count);
                    sym->enum_values = (int64_t*)malloc(sizeof(int64_t) * ast->u.enum_def.member_count);
                    for (int i = 0; i < ast->u.enum_def.member_count; i++) {
                        sym->enum_value_names[i] = strdup(ast->u.enum_def.member_names[i]);
                        sym->enum_values[i] = ast->u.enum_def.member_values[i];
                    }
                    
                    // 注册 enum 成员到作用域
                    for (int i = 0; i < ast->u.enum_def.member_count; i++) {
                        char* member_name = ast->u.enum_def.member_names[i];
                        Symbol* member_sym = scope_define(s->current, member_name, kind);
                        if (member_sym) {
                            // 标记为 enum 成员：裸写 `red` 时 visit_var 会定向报"请写 Color.red"
                            // （codegen 没给这个槽赋值 ⇒ 放行会静默得到 null）
                            member_sym->is_enum_member = 1;
                            member_sym->type = type_new(TYPE_ENUM);
                            member_sym->type->struct_name = strdup(ast->u.enum_def.name);
                        }
                    }
                } else {
                    // 检查是否已经被定义为其他类型（struct/cstruct/face）
                    Symbol* existing = scope_resolve_local(s->current, ast->u.enum_def.name);
                    if (existing && existing->type) {
                        if (existing->type->kind == TYPE_STRUCT) {
                            error_add_at_fmt(ERR_DUPLICATE_VAR, ast->line, ast->column,
                                "类型 '%s' 已经定义为 struct，不能重复定义为 enum", ast->u.enum_def.name);
                        } else if (existing->type->kind == TYPE_CSTRUCT) {
                            error_add_at_fmt(ERR_DUPLICATE_VAR, ast->line, ast->column,
                                "类型 '%s' 已经定义为 cstruct，不能重复定义为 enum", ast->u.enum_def.name);
                        } else if (existing->type->kind == TYPE_FACE) {
                            error_add_at_fmt(ERR_DUPLICATE_VAR, ast->line, ast->column,
                                "类型 '%s' 已经定义为 face，不能重复定义为 enum", ast->u.enum_def.name);
                        } else {
                            error_add_at_fmt(ERR_DUPLICATE_VAR, ast->line, ast->column,
                                "enum '%s' 重复定义", ast->u.enum_def.name);
                        }
                    } else {
                        error_add_at_fmt(ERR_DUPLICATE_VAR, ast->line, ast->column,
                            "enum '%s' 重复定义", ast->u.enum_def.name);
                    }
                }
            }
            break;

        case AST_ALIAS:
            if (ast->u.alias.expr) {
                // 值别名：alias X = EnumName.member
                // 对表达式做语义分析（会将 AST_MODULE_ACCESS 转换为 AST_NUM）
                visit(s, ast->u.alias.expr);
                // ★ 2026-11-07：重复的值别名（`alias R = Color.red` 写两遍）此前**零诊断** ✗
                //   scope_define 重名返回 NULL ⇒ 后写的整条被静默丢弃（实测仍打印第一条的值）。
                if (ast->u.alias.name && scope_resolve_local(s->current, ast->u.alias.name)) {
                    error_add_at_fmt(ERR_DUPLICATE_VAR, ast->line, ast->column,
                        "值别名 '%s' 重复定义（已有同名符号）—— 后写的不会生效；请改名或删掉一个",
                        ast->u.alias.name);
                } else {
                    // 注册为全局变量
                    Symbol* sym = scope_define(s->current, ast->u.alias.name, SYM_GLOBAL);
                    if (sym) {
                        sym->type = type_new(TYPE_INT);
                        ast->u.alias.ref.kind = sym->kind;
                        ast->u.alias.ref.index = sym->index;
                        ast->u.alias.ref.name = strdup(sym->name);
                    }
                }
            } else if (ast->u.alias.type) {
                // 类型别名：alias EventHandler = func(Event):bool
                // 注册到语义作用域，使 resolve_alias_in_type 能找到
                if (!scope_resolve(s->current, ast->u.alias.name)) {
                    Symbol* sym = scope_define(s->current, ast->u.alias.name, SYM_TYPE);
                    if (sym) {
                        sym->type = type_copy(ast->u.alias.type);
                    }
                }
            }
            break;