        case AST_ENUM_DEF:
            // 注册 enum 类型
            {
                TypeInfo* enum_type = type_new(TYPE_ENUM);
                enum_type->struct_name = strdup(ast->u.enum_def.name);

                // 注册到符号表
                SymKind kind;
                if (s->current && s->current->parent == NULL) {
                    kind = s->is_module ? SYM_MODULE : SYM_GLOBAL;
                } else {
                    kind = SYM_LOCAL;
                }
                Symbol* sym = scope_define(s->current, ast->u.enum_def.name, kind);
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
                    sym->enum_values = (int*)malloc(sizeof(int) * ast->u.enum_def.member_count);
                    for (int i = 0; i < ast->u.enum_def.member_count; i++) {
                        sym->enum_value_names[i] = strdup(ast->u.enum_def.member_names[i]);
                        sym->enum_values[i] = (int)ast->u.enum_def.member_values[i];
                    }
                    
                    // 注册 enum 成员到作用域
                    for (int i = 0; i < ast->u.enum_def.member_count; i++) {
                        char* member_name = ast->u.enum_def.member_names[i];
                        Symbol* member_sym = scope_define(s->current, member_name, kind);
                        if (member_sym) {
                            member_sym->type = type_new(TYPE_ENUM);
                            member_sym->type->struct_name = strdup(ast->u.enum_def.name);
                        }
                    }
                } else {
                    char msg[BUFFER_MEDIUM];
                    // 检查是否已经被定义为其他类型（struct/cstruct）
                    Symbol* existing = scope_resolve_local(s->current, ast->u.enum_def.name);
                    if (existing && existing->type) {
                        if (existing->type->kind == TYPE_STRUCT) {
                            snprintf(msg, sizeof(msg), "类型 '%s' 已经定义为 struct，不能重复定义为 enum", ast->u.enum_def.name);
                        } else if (existing->type->kind == TYPE_CSTRUCT) {
                            snprintf(msg, sizeof(msg), "类型 '%s' 已经定义为 cstruct，不能重复定义为 enum", ast->u.enum_def.name);
                        } else {
                            snprintf(msg, sizeof(msg), "enum '%s' 重复定义", ast->u.enum_def.name);
                        }
                    } else {
                        snprintf(msg, sizeof(msg), "enum '%s' 重复定义", ast->u.enum_def.name);
                    }
                    error_add(ERR_DUPLICATE_VAR, ast->line, msg);
                }
            }
            break;

        case AST_ALIAS:
            if (ast->u.alias.expr) {
                // 值别名：alias X = EnumName.member
                // 对表达式做语义分析（会将 AST_MODULE_ACCESS 转换为 AST_NUM）
                visit(s, ast->u.alias.expr);
                // 注册为全局变量
                Symbol* sym = scope_define(s->current, ast->u.alias.name, SYM_GLOBAL);
                if (sym) {
                    sym->type = type_new(TYPE_INT);
                    ast->u.alias.ref.kind = sym->kind;
                    ast->u.alias.ref.index = sym->index;
                    ast->u.alias.ref.name = strdup(sym->name);
                }
            }
            // 类型别名已在解析阶段注册到解析器别名表，无需运行时处理
            break;