        case AST_STRUCT_INIT:
            // 处理 struct 构造函数调用
            {
                // 检查是否是模块限定的 struct 名称（如 "math.Point"）
                const char* dot_pos = strchr(ast->u.struct_init.struct_name, '.');
                Symbol* sym = NULL;

                if (dot_pos) {
                    // 模块限定的 struct：从导入的模块中查找
                    int mod_name_len = dot_pos - ast->u.struct_init.struct_name;
                    char* module_name = (char*)malloc(mod_name_len + 1);
                    memcpy(module_name, ast->u.struct_init.struct_name, mod_name_len);
                    module_name[mod_name_len] = '\0';
                    const char* struct_name_part = dot_pos + 1;
                    ImportedModuleInfo* module_info = find_imported_module(s, module_name);
                    if (module_info && module_info->sym_table) {
                        ModuleStructSymbol* mod_struct = module_symbol_table_find_struct(module_info->sym_table, struct_name_part);
                        if (mod_struct) {
                            // 从模块符号表创建虚拟符号用于字段检查
                            sym = (Symbol*)calloc(1, sizeof(Symbol));
                            sym->name = strdup(struct_name_part);
                            sym->type = type_new(TYPE_STRUCT);
                            sym->type->struct_name = strdup(struct_name_part);
                            sym->struct_field_count = mod_struct->field_count;
                            sym->struct_type_param_count = mod_struct->type_param_count;
                            if (mod_struct->type_param_count > 0 && mod_struct->type_param_names) {
                                sym->struct_type_params = (char**)malloc(sizeof(char*) * mod_struct->type_param_count);
                                for (int ti = 0; ti < mod_struct->type_param_count; ti++) {
                                    sym->struct_type_params[ti] = strdup(mod_struct->type_param_names[ti]);
                                }
                            }
                            if (mod_struct->field_count > 0) {
                                sym->struct_field_names = (char**)malloc(sizeof(char*) * mod_struct->field_count);
                                sym->struct_field_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * mod_struct->field_count);
                                for (int fi = 0; fi < mod_struct->field_count; fi++) {
                                    sym->struct_field_names[fi] = strdup(mod_struct->fields[fi].name);
                                    sym->struct_field_types[fi] = type_new(mod_struct->fields[fi].type);
                                }
                            }
                        }
                    }
                    free(module_name);
                } else {
                    // 直接的 struct 名称
                    sym = scope_resolve(s->current, ast->u.struct_init.struct_name);
                }

                if (!sym) {
                    char msg[BUFFER_MEDIUM];
                    snprintf(msg, sizeof(msg), "未定义的 struct 类型 '%s'", ast->u.struct_init.struct_name);
                    error_add(ERR_UNDEFINED_VAR, ast->line, msg);
                } else if (sym->type->kind == TYPE_FACE) {
                    char msg[BUFFER_MEDIUM];
                    snprintf(msg, sizeof(msg), "face '%s' 不能实例化", ast->u.struct_init.struct_name);
                    error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                } else if (sym->type->kind != TYPE_STRUCT) {
                    char msg[BUFFER_MEDIUM];
                    snprintf(msg, sizeof(msg), "'%s' 不是 struct 类型", ast->u.struct_init.struct_name);
                    error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                } else if (sym->struct_type_param_count > 0 && ast->u.struct_init.generic_type_count == 0) {
                    // 检查是否有默认类型参数
                    int found_defaults = 0;
                    Ast* struct_def_ast = NULL;
                    if (s->root) {
                        for (int si = 0; si < s->root->u.block.count; si++) {
                            Ast* stmt = s->root->u.block.items[si];
                            if (stmt->kind == AST_STRUCT_DEF &&
                                stmt->u.struct_def.name &&
                                strcmp(stmt->u.struct_def.name, ast->u.struct_init.struct_name) == 0) {
                                struct_def_ast = stmt;
                                break;
                            }
                        }
                    }
                    if (struct_def_ast && struct_def_ast->u.struct_def.type_param_defaults) {
                        found_defaults = 1;
                        for (int tp = 0; tp < sym->struct_type_param_count; tp++) {
                            if (!struct_def_ast->u.struct_def.type_param_defaults[tp]) {
                                found_defaults = 0;
                                break;
                            }
                        }
                        if (found_defaults) {
                            ast->u.struct_init.generic_type_count = sym->struct_type_param_count;
                            ast->u.struct_init.generic_type_args = (TypeInfo**)malloc(
                                sizeof(TypeInfo*) * sym->struct_type_param_count);
                            for (int tp = 0; tp < sym->struct_type_param_count; tp++) {
                                const char* dft = struct_def_ast->u.struct_def.type_param_defaults[tp];
                                if (strcmp(dft, "int") == 0) ast->u.struct_init.generic_type_args[tp] = type_new(TYPE_INT);
                                else if (strcmp(dft, "float") == 0) ast->u.struct_init.generic_type_args[tp] = type_new(TYPE_FLOAT);
                                else if (strcmp(dft, "string") == 0) ast->u.struct_init.generic_type_args[tp] = type_new(TYPE_STRING);
                                else if (strcmp(dft, "bool") == 0) ast->u.struct_init.generic_type_args[tp] = type_new(TYPE_BOOL);
                                else {
                                    ast->u.struct_init.generic_type_args[tp] = type_new(TYPE_STRUCT);
                                    ast->u.struct_init.generic_type_args[tp]->struct_name = strdup(dft);
                                }
                            }
                        }
                    }
                    if (!found_defaults) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "泛型 struct '%s' 需要类型参数（如 %s[类型]）",
                                 ast->u.struct_init.struct_name, ast->u.struct_init.struct_name);
                        error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                    }
                } else {
                    // 检查字段类型
                    for (int i = 0; i < ast->u.struct_init.field_count; i++) {
                        char* field_name = ast->u.struct_init.field_names[i];
                        Ast* field_value = ast->u.struct_init.field_values[i];
                        
                        // 查找字段定义
                        TypeInfo* expected_type = NULL;
                        for (int j = 0; j < sym->struct_field_count; j++) {
                            if (strcmp(sym->struct_field_names[j], field_name) == 0) {
                                expected_type = sym->struct_field_types[j];
                                break;
                            }
                        }
                        
                        if (!expected_type) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "struct '%s' 没有字段 '%s'", 
                                     ast->u.struct_init.struct_name, field_name);
                            error_add(ERR_UNDEFINED_VAR, ast->line, msg);
                        } else if (field_value) {
                            // 推断参数字段的类型
                            TypeInfo* actual_type = infer_expr_type(s, field_value);
                            
                            // 泛型 struct：用具体类型参数替换字段类型中的泛型参数
                            TypeInfo* check_type = expected_type;
                            if (ast->u.struct_init.generic_type_count > 0) {
                                // 查找 struct 定义获取 type_params
                                Ast* struct_def_ast = NULL;
                                // 遍历 root AST 查找
                                if (s->root) {
                                    for (int si = 0; si < s->root->u.block.count; si++) {
                                        Ast* stmt = s->root->u.block.items[si];
                                        if (stmt->kind == AST_STRUCT_DEF && 
                                            stmt->u.struct_def.name &&
                                            strcmp(stmt->u.struct_def.name, ast->u.struct_init.struct_name) == 0) {
                                            struct_def_ast = stmt;
                                            break;
                                        }
                                    }
                                }
                                if (struct_def_ast && struct_def_ast->u.struct_def.type_param_count > 0) {
                                    // 用具体类型替换泛型参数
                                    check_type = type_copy(expected_type);
                                    for (int gi = 0; gi < struct_def_ast->u.struct_def.type_param_count && gi < ast->u.struct_init.generic_type_count; gi++) {
                                        TypeInfo* sub = type_substitute(check_type, 
                                            struct_def_ast->u.struct_def.type_params[gi],
                                            ast->u.struct_init.generic_type_args[gi]);
                                        type_free(check_type);
                                        check_type = sub;
                                    }
                                }
                            }
                            
                            // 检查类型兼容性
                            if (actual_type) {
                                if (!type_is_compatible(check_type, actual_type)) {
                                    char msg[BUFFER_MEDIUM];
                                    format_type_error(msg, sizeof(msg),
                                        "字段 '%s3' 类型不匹配: 期望 '%s1'，实际 '%s2'",
                                        check_type, actual_type,
                                        field_name, NULL);
                                    error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                                }
                                type_free(actual_type);
                            }
                            // 释放替换后的类型（如果做了替换）
                            if (check_type != expected_type) {
                                type_free(check_type);
                            }
                        }
                        
                        // 递归访问字段值
                        visit(s, field_value);
                    }
                }
            }
            break;