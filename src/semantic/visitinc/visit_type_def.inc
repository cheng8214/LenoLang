        case AST_STRUCT_DEF:
            // 注册 struct 类型
            {
                TypeInfo* struct_type = type_new(TYPE_STRUCT);
                struct_type->struct_name = strdup(ast->u.struct_def.name);

                // 泛型 struct：将字段类型中的泛型参数标记为 TYPE_GENERIC_PARAM
                if (ast->u.struct_def.type_param_count > 0 && ast->u.struct_def.type_params) {
                    for (int i = 0; i < ast->u.struct_def.field_count; i++) {
                        resolve_generic_in_type(ast->u.struct_def.field_types[i],
                            ast->u.struct_def.type_params, ast->u.struct_def.type_param_constraints, ast->u.struct_def.type_param_count);
                    }
                }

                // 先验证所有字段类型是否有效（在注册 struct 之前）
                for (int i = 0; i < ast->u.struct_def.field_count; i++) {
                    TypeInfo* field_type = ast->u.struct_def.field_types[i];

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
                                if (!ref_type || !ref_type->type ||
                                    (ref_type->type->kind != TYPE_STRUCT &&
                                     ref_type->type->kind != TYPE_CLIB &&
                                     ref_type->type->kind != TYPE_CSTRUCT &&
                                     ref_type->type->kind != TYPE_FACE)) {
                                    char msg[BUFFER_MEDIUM];
                                    snprintf(msg, sizeof(msg), "struct '%s' 的字段 '%s' 使用了未定义的类型 '%s'",
                                             ast->u.struct_def.name,
                                             ast->u.struct_def.field_names[i],
                                             field_type->struct_name);
                                    error_add(ERR_UNDEFINED_VAR, ast->line, msg);
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

                // 先检查是否已经预注册
                Symbol* sym = scope_resolve_local(s->current, ast->u.struct_def.name);
                if (sym && sym->type && sym->type->kind == TYPE_STRUCT) {
                    // 已预注册，更新现有符号
                    type_free(sym->type);
                    sym->type = struct_type;
                } else {
                    // 未注册，创建新符号
                    sym = scope_define(s->current, ast->u.struct_def.name, kind);
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
                        } else {
                            sym->struct_field_null_default[i] = 0;
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
                    // 检查是否已经被定义为其他类型（cstruct/enum）
                    Symbol* existing = scope_resolve_local(s->current, ast->u.struct_def.name);
                    if (existing && existing->type) {
                        if (existing->type->kind == TYPE_CSTRUCT) {
                            snprintf(msg, sizeof(msg), "类型 '%s' 已经定义为 cstruct，不能重复定义为 struct", ast->u.struct_def.name);
                        } else if (existing->type->kind == TYPE_ENUM) {
                            snprintf(msg, sizeof(msg), "类型 '%s' 已经定义为 enum，不能重复定义为 struct", ast->u.struct_def.name);
                        } else {
                            snprintf(msg, sizeof(msg), "struct '%s' 重复定义", ast->u.struct_def.name);
                        }
                    } else {
                        snprintf(msg, sizeof(msg), "struct '%s' 重复定义", ast->u.struct_def.name);
                    }
                    error_add(ERR_DUPLICATE_VAR, ast->line, msg);
                }

                // 处理默认值表达式
                for (int i = 0; i < ast->u.struct_def.field_count; i++) {
                    if (ast->u.struct_def.field_defaults[i]) {
                        visit(s, ast->u.struct_def.field_defaults[i]);
                    }
                }

                // 处理方法定义：分两阶段
                // 阶段1：准备所有方法（转换方法体、添加self参数、注册到函数表）
                for (int i = 0; i < ast->u.struct_def.method_count; i++) {
                    Ast* method_ast = ast->u.struct_def.methods[i];
                    if (method_ast && method_ast->kind == AST_FUNC_DEF) {
                        // 1. 转换方法体：将字段名访问转换为 self.字段名，方法名调用转换为 self.方法名
                        // 收集所有方法名
                        char** method_names = NULL;
                        int method_count = ast->u.struct_def.method_count;
                        if (method_count > 0) {
                            method_names = (char**)malloc(sizeof(char*) * method_count);
                            for (int m = 0; m < method_count; m++) {
                                method_names[m] = ast->u.struct_def.methods[m]->u.func.name;
                            }
                        }
                        transform_method_body(method_ast->u.func.body, ast->u.struct_def.field_names, ast->u.struct_def.field_count, method_names, method_count, ast->u.struct_def.name);
                        if (method_names) free(method_names);

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
                
                // 阶段2：处理所有方法体（语义分析）
                // 这样方法间可以互相引用，因为所有方法都已注册到函数表
                // 使用 visit_func_as_struct_method 避免将方法注册到外部作用域
                for (int i = 0; i < ast->u.struct_def.method_count; i++) {
                    Ast* method_ast = ast->u.struct_def.methods[i];
                    if (method_ast && method_ast->kind == AST_FUNC_DEF) {
                        visit_func_as_struct_method(s, method_ast);
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
                                                    if (mface->methods[j].return_type != ast->u.struct_def.methods[k]->u.func.return_type->kind) {
                                                        char msg[256];
                                                        snprintf(msg, sizeof(msg), "struct '%s' 的方法 '%s' 返回类型与 face '%s' 不匹配: 期望 %s, 实际 %s",
                                                                 ast->u.struct_def.name, mname, face_name,
                                                                 type_kind_to_string(mface->methods[j].return_type),
                                                                 type_kind_to_string(ast->u.struct_def.methods[k]->u.func.return_type->kind));
                                                        error_add(ERR_SEMANTIC, ast->line, msg);
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
                                                        error_add(ERR_SEMANTIC, ast->line, msg);
                                                    }
                                                }
                                                break;
                                            }
                                        }
                                        if (!found) {
                                            char msg[256];
                                            snprintf(msg, sizeof(msg), "struct '%s' 声明 impl '%s' 但缺少方法 '%s'",
                                                     ast->u.struct_def.name, face_name, mname);
                                            error_add(ERR_SEMANTIC, ast->line, msg);
                                        }
                                    }
                                }
                            }
                        }
                        if (!found_in_module) {
                            char msg[256];
                            snprintf(msg, sizeof(msg), "未定义的 face: '%s'", face_name);
                            error_add(ERR_SEMANTIC, ast->line, msg);
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
                                    if (expected_kind != TYPE_ANY &&
                                        expected_kind != ast->u.struct_def.methods[k]->u.func.return_type->kind) {
                                        char msg[256];
                                        snprintf(msg, sizeof(msg), "struct '%s' 的方法 '%s' 返回类型与 face '%s' 不匹配: 期望 %s, 实际 %s",
                                                 ast->u.struct_def.name, mname, face_name,
                                                 type_kind_to_string(expected_kind),
                                                 type_kind_to_string(ast->u.struct_def.methods[k]->u.func.return_type->kind));
                                        error_add(ERR_SEMANTIC, ast->line, msg);
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
                                        error_add(ERR_SEMANTIC, ast->line, msg);
                                    }
                                }
                                break;
                            }
                        }
                        if (!found) {
                            char msg[256];
                            snprintf(msg, sizeof(msg), "struct '%s' 声明 impl '%s' 但缺少方法 '%s'",
                                     ast->u.struct_def.name, face_name, mname);
                            error_add(ERR_SEMANTIC, ast->line, msg);
                        }
                    }
                }

                ObjStructDef* early_def = struct_def_find(ast->u.struct_def.name);
                if (!early_def) {
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

                Symbol* existing = scope_resolve_local(s->current, ast->u.face_def.name);
                if (existing && existing->type && existing->type->kind == TYPE_FACE) {
                    type_free(existing->type);
                    existing->type = face_type;
                } else {
                    SymKind kind = SYM_TYPE;
                    Symbol* sym = scope_define(s->current, ast->u.face_def.name, kind);
                    if (sym) {
                        type_free(sym->type);
                        sym->type = face_type;
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