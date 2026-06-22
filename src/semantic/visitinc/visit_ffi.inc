        case AST_CSTRUCT_DEF:
            // 注册 cstruct 类型并计算布局
            {
                // 1. 处理嵌套 cstruct 字段类型
                // 如果字段类型是 TYPE_STRUCT 且类型名对应一个已定义的 cstruct，
                // 则将其转换为 TYPE_CSTRUCT
                for (int i = 0; i < ast->u.cstruct_def.field_count; i++) {
                    TypeInfo* field_type = ast->u.cstruct_def.field_types[i];
                    if (field_type && field_type->kind == TYPE_STRUCT && field_type->struct_name) {
                        // 查找是否是已定义的 cstruct
                        Symbol* nested_cstruct = scope_resolve(s->current, field_type->struct_name);
                        if (nested_cstruct && nested_cstruct->type && nested_cstruct->type->kind == TYPE_CSTRUCT) {
                            // 转换为 TYPE_CSTRUCT
                            type_free(field_type);
                            ast->u.cstruct_def.field_types[i] = type_new(TYPE_CSTRUCT);
                            ast->u.cstruct_def.field_types[i]->struct_name = strdup(nested_cstruct->name);
                        }
                    }
                }

                // 2. 验证所有字段类型是否有效（必须是 C 布局类型）
                for (int i = 0; i < ast->u.cstruct_def.field_count; i++) {
                    TypeInfo* field_type = ast->u.cstruct_def.field_types[i];
                    if (!field_type || !c_layout_is_valid_field_type(field_type->kind)) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "cstruct '%s' 的字段 '%s' 使用了无效的 C 布局类型",
                                 ast->u.cstruct_def.name,
                                 ast->u.cstruct_def.field_names[i]);
                        error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                    }
                }

                // 3. 计算 C 布局（字段偏移、总大小、对齐要求）
                int* field_offsets = (int*)malloc(sizeof(int) * ast->u.cstruct_def.field_count);
                int current_offset = 0;
                int max_alignment = 1;

                for (int i = 0; i < ast->u.cstruct_def.field_count; i++) {
                    TypeInfo* field_type = ast->u.cstruct_def.field_types[i];
                    int field_size = c_layout_type_size(field_type->kind);
                    int field_align = c_layout_type_align(field_type->kind);
                    int array_dim = ast->u.cstruct_def.field_array_dims[i];

                    // 如果是嵌套 cstruct，需要从符号表获取其大小和对齐
                    if (field_type->kind == TYPE_CSTRUCT && field_type->struct_name) {
                        Symbol* nested_cstruct = scope_resolve(s->current, field_type->struct_name);
                        if (nested_cstruct && nested_cstruct->type && nested_cstruct->type->kind == TYPE_CSTRUCT) {
                            // 使用已存储的 cstruct 布局信息
                            field_size = nested_cstruct->cstruct_size;
                            field_align = nested_cstruct->cstruct_alignment;
                        }
                    }

                    // 更新最大对齐要求
                    if (field_align > max_alignment) {
                        max_alignment = field_align;
                    }

                    // 对齐当前偏移
                    current_offset = c_layout_align_up(current_offset, field_align);
                    field_offsets[i] = current_offset;
                    
                    // 计算字段大小（数组字段需要乘以维度）
                    int field_total_size = field_size;
                    if (array_dim > 0) {
                        field_total_size = field_size * array_dim;
                    }
                    current_offset += field_total_size;
                }

                // 结构体总大小需要按最大对齐要求对齐
                int total_size = c_layout_align_up(current_offset, max_alignment);

                // 保存布局信息到 AST
                ast->u.cstruct_def.field_offsets = field_offsets;
                ast->u.cstruct_def.total_size = total_size;
                ast->u.cstruct_def.alignment = max_alignment;

                // 4. 注册到符号表（或更新已预注册的符号）
                TypeInfo* cstruct_type = type_new(TYPE_CSTRUCT);
                cstruct_type->struct_name = strdup(ast->u.cstruct_def.name);

                SymKind kind;
                if (s->current && s->current->parent == NULL) {
                    kind = s->is_module ? SYM_MODULE : SYM_GLOBAL;
                } else {
                    kind = SYM_LOCAL;
                }

                // 先检查是否已经预注册
                Symbol* sym = scope_resolve_local(s->current, ast->u.cstruct_def.name);
                if (sym && sym->type && sym->type->kind == TYPE_CSTRUCT) {
                    // 已预注册，更新现有符号
                    type_free(sym->type);
                    sym->type = cstruct_type;
                } else {
                    // 未注册，创建新符号
                    sym = scope_define(s->current, ast->u.cstruct_def.name, kind);
                }

                if (sym) {
                    sym->type = cstruct_type;
                    // 保存符号引用信息
                    ast->u.cstruct_def.ref.kind = sym->kind;
                    ast->u.cstruct_def.ref.index = sym->index;
                    ast->u.cstruct_def.ref.name = strdup(sym->name);
                    ast->u.cstruct_def.ref.type_kind = sym->type ? sym->type->kind : TYPE_CSTRUCT;

                    // 存储 cstruct 字段信息
                    sym->struct_field_count = ast->u.cstruct_def.field_count;
                    sym->struct_field_names = (char**)malloc(sizeof(char*) * ast->u.cstruct_def.field_count);
                    sym->struct_field_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * ast->u.cstruct_def.field_count);
                    sym->struct_field_null_default = (int*)calloc(ast->u.cstruct_def.field_count, sizeof(int));

                    for (int i = 0; i < ast->u.cstruct_def.field_count; i++) {
                        sym->struct_field_names[i] = strdup(ast->u.cstruct_def.field_names[i]);
                        sym->struct_field_types[i] = type_copy(ast->u.cstruct_def.field_types[i]);
                    }

                    // 存储 cstruct 布局信息
                    sym->cstruct_size = total_size;
                    sym->cstruct_alignment = max_alignment;
                    sym->cstruct_field_offsets = (int*)malloc(sizeof(int) * ast->u.cstruct_def.field_count);
                    for (int i = 0; i < ast->u.cstruct_def.field_count; i++) {
                        sym->cstruct_field_offsets[i] = field_offsets[i];
                    }
                } else {
                    char msg[BUFFER_MEDIUM];
                    // 检查是否已经被定义为其他类型（struct/enum）
                    Symbol* existing = scope_resolve_local(s->current, ast->u.cstruct_def.name);
                    if (existing && existing->type) {
                        if (existing->type->kind == TYPE_STRUCT) {
                            snprintf(msg, sizeof(msg), "类型 '%s' 已经定义为 struct，不能重复定义为 cstruct", ast->u.cstruct_def.name);
                        } else if (existing->type->kind == TYPE_ENUM) {
                            snprintf(msg, sizeof(msg), "类型 '%s' 已经定义为 enum，不能重复定义为 cstruct", ast->u.cstruct_def.name);
                        } else {
                            snprintf(msg, sizeof(msg), "cstruct '%s' 重复定义", ast->u.cstruct_def.name);
                        }
                    } else {
                        snprintf(msg, sizeof(msg), "cstruct '%s' 重复定义", ast->u.cstruct_def.name);
                    }
                    error_add(ERR_DUPLICATE_VAR, ast->line, msg);
                }
            }
            break;

        case AST_CLIB_DEF:
            // 注册 clib 类型并存储函数签名信息
            {
                // 1. 验证函数签名中的类型是否有效（clib 只允许 C 布局类型）
                for (int i = 0; i < ast->u.clib_def.func_count; i++) {
                    TypeInfo* ret_type = ast->u.clib_def.func_return_types[i];
                    // 允许 TYPE_NULL (void), TYPE_I32, TYPE_U32, TYPE_I64, TYPE_U64, TYPE_F32, TYPE_F64,
                    // TYPE_PTR, TYPE_PTR_GENERIC, TYPE_BOOL, TYPE_STR8, TYPE_STR16
                    // 不允许 TYPE_INT, TYPE_FLOAT, TYPE_STRING 等 Leno 类型
                    if (ret_type->kind == TYPE_INT) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "clib 函数 '%s' 返回类型不能使用 'int'，请使用 'i32' 或 'i64'",
                                 ast->u.clib_def.func_names[i]);
                        error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                    } else if (ret_type->kind == TYPE_FLOAT) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "clib 函数 '%s' 返回类型不能使用 'float'，请使用 'f32' 或 'f64'",
                                 ast->u.clib_def.func_names[i]);
                        error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                    } else if (ret_type->kind == TYPE_STRING) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "clib 函数 '%s' 返回类型不能使用 'string'，请使用 'str8' 或 'ptr'",
                                 ast->u.clib_def.func_names[i]);
                        error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                    } else if (ret_type->kind == TYPE_STRUCT && ret_type->struct_name) {
                        // 可能是自定义类型名（如 Ptr 被解析为 TYPE_STRUCT 带 struct_name="Ptr"）
                        if (strcmp(ret_type->struct_name, "Ptr") == 0) {
                            type_free(ret_type);
                            ast->u.clib_def.func_return_types[i] = type_new(TYPE_PTR);
                            ast->u.clib_def.func_return_types[i]->struct_name = strdup("Ptr");
                        } else if (strcmp(ret_type->struct_name, "string") == 0 ||
                                   strcmp(ret_type->struct_name, "String") == 0) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "clib 函数 '%s' 返回类型不能使用 'string'，请使用 'str8' 或 'ptr'",
                                     ast->u.clib_def.func_names[i]);
                            error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                        } else if (strcmp(ret_type->struct_name, "str8") == 0 ||
                                   strcmp(ret_type->struct_name, "Str8") == 0) {
                            type_free(ret_type);
                            ast->u.clib_def.func_return_types[i] = type_new(TYPE_STR8);
                        } else if (strcmp(ret_type->struct_name, "str16") == 0 ||
                                   strcmp(ret_type->struct_name, "Str16") == 0) {
                            type_free(ret_type);
                            ast->u.clib_def.func_return_types[i] = type_new(TYPE_STR16);
                        } else if (cstruct_def_find(ret_type->struct_name)) {
                            // cstruct 类型名 — 修正为 TYPE_CSTRUCT
                            char* name = strdup(ret_type->struct_name);
                            type_free(ret_type);
                            ast->u.clib_def.func_return_types[i] = type_new(TYPE_CSTRUCT);
                            ast->u.clib_def.func_return_types[i]->struct_name = name;
                        } else {
                            // 尝试从符号表查找（cstruct 可能尚未注册到 cstruct_def_table）
                            Symbol* sym = scope_resolve(s->current, ret_type->struct_name);
                            if (!sym) sym = scope_resolve(s->root_scope, ret_type->struct_name);
                            if (sym && sym->type && sym->type->kind == TYPE_CSTRUCT) {
                                char* name = strdup(ret_type->struct_name);
                                type_free(ret_type);
                                ast->u.clib_def.func_return_types[i] = type_new(TYPE_CSTRUCT);
                                ast->u.clib_def.func_return_types[i]->struct_name = name;
                            } else {
                                char msg[BUFFER_MEDIUM];
                                snprintf(msg, sizeof(msg), "clib 函数 '%s' 返回类型 '%s' 不是有效的 C 布局类型，请使用 i32/i64/f32/f64/str8/str16/ptr/bool/void",
                                         ast->u.clib_def.func_names[i], ret_type->struct_name);
                                error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                            }
                        }
                    }

                    // 验证参数类型
                    for (int j = 0; j < ast->u.clib_def.func_param_counts[i]; j++) {
                        TypeInfo* param_type = ast->u.clib_def.func_param_types[i][j];
                        if (param_type->kind == TYPE_INT) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "clib 函数 '%s' 参数 %d 类型不能使用 'int'，请使用 'i32' 或 'i64'",
                                     ast->u.clib_def.func_names[i], j + 1);
                            error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                        } else if (param_type->kind == TYPE_FLOAT) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "clib 函数 '%s' 参数 %d 类型不能使用 'float'，请使用 'f32' 或 'f64'",
                                     ast->u.clib_def.func_names[i], j + 1);
                            error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                        } else if (param_type->kind == TYPE_STRING) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "clib 函数 '%s' 参数 %d 类型不能使用 'string'，请使用 'str8' 或 'ptr'",
                                     ast->u.clib_def.func_names[i], j + 1);
                            error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                        } else if (param_type->kind == TYPE_STRUCT && param_type->struct_name) {
                            if (strcmp(param_type->struct_name, "Ptr") == 0) {
                                type_free(param_type);
                                ast->u.clib_def.func_param_types[i][j] = type_new(TYPE_PTR);
                                ast->u.clib_def.func_param_types[i][j]->struct_name = strdup("Ptr");
                            } else if (strcmp(param_type->struct_name, "string") == 0 ||
                                       strcmp(param_type->struct_name, "String") == 0) {
                                char msg[BUFFER_MEDIUM];
                                snprintf(msg, sizeof(msg), "clib 函数 '%s' 参数 %d 类型不能使用 'string'，请使用 'str8' 或 'ptr'",
                                         ast->u.clib_def.func_names[i], j + 1);
                                error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                            } else if (strcmp(param_type->struct_name, "str8") == 0 ||
                                       strcmp(param_type->struct_name, "Str8") == 0) {
                                type_free(param_type);
                                ast->u.clib_def.func_param_types[i][j] = type_new(TYPE_STR8);
                            } else if (strcmp(param_type->struct_name, "str16") == 0 ||
                                       strcmp(param_type->struct_name, "Str16") == 0) {
                                type_free(param_type);
                                ast->u.clib_def.func_param_types[i][j] = type_new(TYPE_STR16);
                            } else if (cstruct_def_find(param_type->struct_name)) {
                                // cstruct 类型名 — 修正为 TYPE_CSTRUCT，语义为传指针
                                char* name = strdup(param_type->struct_name);
                                type_free(param_type);
                                ast->u.clib_def.func_param_types[i][j] = type_new(TYPE_CSTRUCT);
                                ast->u.clib_def.func_param_types[i][j]->struct_name = name;
                            } else {
                                // 尝试从符号表查找（cstruct 可能尚未注册到 cstruct_def_table）
                                Symbol* sym = scope_resolve(s->current, param_type->struct_name);
                                if (!sym) sym = scope_resolve(s->root_scope, param_type->struct_name);
                                if (sym && sym->type && sym->type->kind == TYPE_CSTRUCT) {
                                    char* name = strdup(param_type->struct_name);
                                    type_free(param_type);
                                    ast->u.clib_def.func_param_types[i][j] = type_new(TYPE_CSTRUCT);
                                    ast->u.clib_def.func_param_types[i][j]->struct_name = name;
                                } else {
                                    char msg[BUFFER_MEDIUM];
                                    snprintf(msg, sizeof(msg), "clib 函数 '%s' 参数 %d 类型 '%s' 不是有效的 C 布局类型，请使用 i32/i64/f32/f64/str8/str16/ptr/bool/void",
                                             ast->u.clib_def.func_names[i], j + 1, param_type->struct_name);
                                    error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                                }
                            }
                        }
                    }
                }

                // 2. 注册到符号表（更新已预注册的符号）
                TypeInfo* clib_type = type_new(TYPE_CLIB);
                clib_type->struct_name = strdup(ast->u.clib_def.name);

                // 先检查是否已经预注册
                Symbol* sym = scope_resolve_local(s->current, ast->u.clib_def.name);
                if (sym && sym->type && sym->type->kind == TYPE_CLIB) {
                    // 已预注册，更新现有符号
                    type_free(sym->type);
                    sym->type = clib_type;
                } else {
                    // 未注册，创建新符号（不应发生，因为预注册已处理）
                    sym = scope_define(s->current, ast->u.clib_def.name, SYM_CLIB);
                }

                if (sym) {
                    sym->type = clib_type;
                    ast->u.clib_def.ref.kind = sym->kind;
                    ast->u.clib_def.ref.index = sym->index;
                    ast->u.clib_def.ref.name = strdup(sym->name);
                    ast->u.clib_def.ref.type_kind = TYPE_CLIB;

                    // 存储 clib 函数签名信息
                    sym->clib_func_count = ast->u.clib_def.func_count;
                    sym->clib_func_names = (char**)malloc(sizeof(char*) * ast->u.clib_def.func_count);
                    sym->clib_func_return_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * ast->u.clib_def.func_count);
                    sym->clib_func_param_counts = (int*)malloc(sizeof(int) * ast->u.clib_def.func_count);
                    sym->clib_func_param_types = (TypeInfo***)malloc(sizeof(TypeInfo**) * ast->u.clib_def.func_count);

                    for (int i = 0; i < ast->u.clib_def.func_count; i++) {
                        sym->clib_func_names[i] = strdup(ast->u.clib_def.func_names[i]);
                        sym->clib_func_return_types[i] = type_copy(ast->u.clib_def.func_return_types[i]);
                        sym->clib_func_param_counts[i] = ast->u.clib_def.func_param_counts[i];
                        sym->clib_func_param_types[i] = (TypeInfo**)malloc(sizeof(TypeInfo*) * ast->u.clib_def.func_param_counts[i]);
                        for (int j = 0; j < ast->u.clib_def.func_param_counts[i]; j++) {
                            sym->clib_func_param_types[i][j] = type_copy(ast->u.clib_def.func_param_types[i][j]);
                        }
                    }
                } else {
                    char msg[BUFFER_MEDIUM];
                    snprintf(msg, sizeof(msg), "clib '%s' 重复定义", ast->u.clib_def.name);
                    error_add(ERR_DUPLICATE_VAR, ast->line, msg);
                }
            }
            break;

        case AST_CFUNC_DECL:
            // 注册 cfunc 回调签名类型
            {
                // 1. 验证返回类型（cfunc 只允许 C 布局类型）
                TypeInfo* ret_type = ast->u.cfunc_decl.return_type;
                if (ret_type) {
                    if (ret_type->kind == TYPE_INT) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "cfunc '%s' 返回类型不能使用 'int'，请使用 'i32' 或 'i64'", ast->u.cfunc_decl.name);
                        error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                    } else if (ret_type->kind == TYPE_FLOAT) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "cfunc '%s' 返回类型不能使用 'float'，请使用 'f32' 或 'f64'", ast->u.cfunc_decl.name);
                        error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                    } else if (ret_type->kind == TYPE_STRING) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "cfunc '%s' 返回类型不能使用 'string'，请使用 'str8' 或 'ptr'", ast->u.cfunc_decl.name);
                        error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                    } else if (ret_type->kind == TYPE_STRUCT && ret_type->struct_name) {
                        if (strcmp(ret_type->struct_name, "Ptr") == 0) {
                            type_free(ret_type);
                            ast->u.cfunc_decl.return_type = type_new(TYPE_PTR);
                            ast->u.cfunc_decl.return_type->struct_name = strdup("Ptr");
                        } else if (strcmp(ret_type->struct_name, "str8") == 0 || strcmp(ret_type->struct_name, "Str8") == 0) {
                            type_free(ret_type);
                            ast->u.cfunc_decl.return_type = type_new(TYPE_STR8);
                        } else if (strcmp(ret_type->struct_name, "str16") == 0 || strcmp(ret_type->struct_name, "Str16") == 0) {
                            type_free(ret_type);
                            ast->u.cfunc_decl.return_type = type_new(TYPE_STR16);
                        } else {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "cfunc '%s' 返回类型 '%s' 不是有效的 C 布局类型", ast->u.cfunc_decl.name, ret_type->struct_name);
                            error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                        }
                    }
                }

                // 2. 验证参数类型
                for (int j = 0; j < ast->u.cfunc_decl.param_count; j++) {
                    TypeInfo* ptype = ast->u.cfunc_decl.param_types[j];
                    if (ptype->kind == TYPE_INT) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "cfunc '%s' 参数 %d 类型不能使用 'int'，请使用 'i32' 或 'i64'", ast->u.cfunc_decl.name, j + 1);
                        error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                    } else if (ptype->kind == TYPE_FLOAT) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "cfunc '%s' 参数 %d 类型不能使用 'float'，请使用 'f32' 或 'f64'", ast->u.cfunc_decl.name, j + 1);
                        error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                    } else if (ptype->kind == TYPE_STRING) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "cfunc '%s' 参数 %d 类型不能使用 'string'，请使用 'str8' 或 'ptr'", ast->u.cfunc_decl.name, j + 1);
                        error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                    } else if (ptype->kind == TYPE_STRUCT && ptype->struct_name) {
                        if (strcmp(ptype->struct_name, "Ptr") == 0) {
                            type_free(ptype);
                            ast->u.cfunc_decl.param_types[j] = type_new(TYPE_PTR);
                            ast->u.cfunc_decl.param_types[j]->struct_name = strdup("Ptr");
                        } else if (strcmp(ptype->struct_name, "str8") == 0 || strcmp(ptype->struct_name, "Str8") == 0) {
                            type_free(ptype);
                            ast->u.cfunc_decl.param_types[j] = type_new(TYPE_STR8);
                        } else if (strcmp(ptype->struct_name, "str16") == 0 || strcmp(ptype->struct_name, "Str16") == 0) {
                            type_free(ptype);
                            ast->u.cfunc_decl.param_types[j] = type_new(TYPE_STR16);
                        } else {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "cfunc '%s' 参数 %d 类型 '%s' 不是有效的 C 布局类型", ast->u.cfunc_decl.name, j + 1, ptype->struct_name);
                            error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                        }
                    }
                }

                // 3. 注册到符号表
                Symbol* sym = scope_resolve_local(s->current, ast->u.cfunc_decl.name);
                if (sym && sym->type && sym->type->kind == TYPE_CFUNC) {
                    // 已预注册，更新签名信息
                    type_free(sym->type);
                    TypeInfo* cfunc_type = type_new(TYPE_CFUNC);
                    cfunc_type->struct_name = strdup(ast->u.cfunc_decl.name);
                    sym->type = cfunc_type;
                } else {
                    sym = scope_define(s->current, ast->u.cfunc_decl.name, SYM_CFUNC);
                    if (sym) {
                        TypeInfo* cfunc_type = type_new(TYPE_CFUNC);
                        cfunc_type->struct_name = strdup(ast->u.cfunc_decl.name);
                        sym->type = cfunc_type;
                    }
                }

                if (sym) {
                    ast->u.cfunc_decl.ref.kind = sym->kind;
                    ast->u.cfunc_decl.ref.index = sym->index;
                    free(ast->u.cfunc_decl.ref.name);
                    ast->u.cfunc_decl.ref.name = strdup(sym->name);
                    ast->u.cfunc_decl.ref.type_kind = TYPE_CFUNC;

                    // 存储 cfunc 签名信息
                    sym->cfunc_param_count = ast->u.cfunc_decl.param_count;
                    sym->cfunc_param_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * ast->u.cfunc_decl.param_count);
                    for (int j = 0; j < ast->u.cfunc_decl.param_count; j++) {
                        sym->cfunc_param_types[j] = type_copy(ast->u.cfunc_decl.param_types[j]);
                    }
                    sym->cfunc_return_type = ast->u.cfunc_decl.return_type ? type_copy(ast->u.cfunc_decl.return_type) : NULL;
                } else {
                    char msg[BUFFER_MEDIUM];
                    snprintf(msg, sizeof(msg), "cfunc '%s' 重复定义", ast->u.cfunc_decl.name);
                    error_add(ERR_DUPLICATE_VAR, ast->line, msg);
                }
            }
            break;