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
                    ImportedModuleInfo* info = &s->imported_modules[s->imported_module_count++];
                    info->alias = module_name;
                    info->file_path = strdup(ast->u.import.file_path);

                    // 创建模块符号表并扫描模块文件
                    info->sym_table = module_symbol_table_create(ast->u.import.file_path);
                    if (info->sym_table) {
                        const char* current_file = error_get_filename();
                        module_symbol_table_scan(info->sym_table, current_file);

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
                            if (!struct_sym->is_cstruct && !struct_def_find(struct_sym->name)) {
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

                                    struct_def_register(sdef);
                                }
                            }
                        }
                    }
                } else {
                    free(module_name);
                }
            } else {
                // 原生模块：运行时通过 OP_LOAD_NATIVE_MODULE 加载
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
            // use 语句：将模块中的 struct 类型导入到当前作用域
            // 注意：use 只支持 struct 类型，不支持 func、var 和 enum
            // - struct：编译时类型，需要用于变量声明（如 Point p）
            // - func/var：运行时实体，必须通过模块名访问（如 module.func()）
            // - enum：成员访问通过模块名（如 module.enum.member），不需要 use
            const char* module_name = ast->u.use.module_name;
            const char* symbol_name = ast->u.use.symbol_name;

            // 查找导入的模块
            ImportedModuleInfo* module_info = find_imported_module(s, module_name);
            if (!module_info) {
                char msg[BUFFER_MEDIUM];
                snprintf(msg, sizeof(msg), "use 语句错误：模块 '%s' 未导入", module_name);
                error_add(ERR_SEMANTIC, ast->line, msg);
                break;
            }

            if (!module_info->sym_table) {
                char msg[BUFFER_MEDIUM];
                snprintf(msg, sizeof(msg), "use 语句错误：模块 '%s' 没有符号表", module_name);
                error_add(ERR_SEMANTIC, ast->line, msg);
                break;
            }

            // 尝试查找 struct 定义
            ModuleStructSymbol* struct_sym = module_symbol_table_find_struct(module_info->sym_table, symbol_name);
            if (struct_sym) {
                // 将 struct/cstruct 定义注册到当前作用域
                // cstruct 使用 SYM_CSTRUCT，struct 使用 SYM_STRUCT，以便 codegen 生成正确的指令
                SymKind kind;
                if (struct_sym->is_cstruct) {
                    kind = SYM_CSTRUCT;
                } else {
                    kind = SYM_STRUCT;
                }
                Symbol* sym = scope_define(s->current, symbol_name, kind);
                if (!sym) {
                    // scope_define 返回 NULL 可能是因为同名冲突
                    Symbol* existing = scope_resolve_local(s->current, symbol_name);
                    if (existing && existing->type && (existing->type->kind == TYPE_STRUCT || existing->type->kind == TYPE_CSTRUCT)) {
                        // 检查是否是同一类型的重复导入（来自 import_type_deps 的隐式导入）
                        // 如果现有符号是同一类型名，允许重复 use（跳过不报错）
                        if (existing->type->struct_name && strcmp(existing->type->struct_name, symbol_name) == 0) {
                            // 同一类型的重复导入，静默跳过
                            break;
                        }
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "use 错误：'%s' 已在当前作用域中定义（来自其他模块的 use），请使用模块名限定访问",
                                 symbol_name);
                        error_add(ERR_SEMANTIC, ast->line, msg);
                    }
                    break;
                }
                if (sym) {
                    // 根据 is_cstruct 标记决定类型
                    TypeKind struct_type = struct_sym->is_cstruct ? TYPE_CSTRUCT : TYPE_STRUCT;
                    sym->type = type_new(struct_type);
                    sym->type->struct_name = strdup(symbol_name);
                    sym->struct_field_count = struct_sym->field_count;
                    sym->struct_field_names = (char**)malloc(sizeof(char*) * struct_sym->field_count);
                    sym->struct_field_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * struct_sym->field_count);
                    for (int i = 0; i < struct_sym->field_count; i++) {
                        sym->struct_field_names[i] = strdup(struct_sym->fields[i].name);
                        sym->struct_field_types[i] = type_new(struct_sym->fields[i].type);
                        if (struct_sym->fields[i].struct_name) {
                            sym->struct_field_types[i]->struct_name = strdup(struct_sym->fields[i].struct_name);
                        }
                        // 传播 nullable 标记
                        if (struct_sym->fields[i].nullable) {
                            sym->struct_field_types[i]->nullable = 1;
                        }
                        // 重建 Array[T]/Dict[K,V] 的元素类型信息
                        if ((struct_sym->fields[i].type == TYPE_ARRAY || struct_sym->fields[i].type == TYPE_DICT)
                            && struct_sym->fields[i].element_type != TYPE_PTR) {
                            TypeInfo* elem_type = type_new(struct_sym->fields[i].element_type);
                            if (struct_sym->fields[i].element_struct_name) {
                                elem_type->struct_name = strdup(struct_sym->fields[i].element_struct_name);
                            }
                            sym->struct_field_types[i]->element_type = elem_type;
                        }
                    }
                    // 设置泛型类型参数信息
                    sym->struct_type_param_count = struct_sym->type_param_count;
                    if (struct_sym->type_param_count > 0 && struct_sym->type_param_names) {
                        sym->struct_type_params = (char**)malloc(sizeof(char*) * struct_sym->type_param_count);
                        for (int i = 0; i < struct_sym->type_param_count; i++) {
                            sym->struct_type_params[i] = strdup(struct_sym->type_param_names[i]);
                        }
                    }
                }
                // 同时注册 struct 定义到全局表，用于类型检查（face 实现检查）
                if (!struct_sym->is_cstruct && !struct_def_find(symbol_name)) {
                    ObjStructDef* sdef = struct_def_new(symbol_name, struct_sym->field_count, struct_sym->method_count);
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

                        // 设置 struct 方法名（用于方法查找）
                        for (int i = 0; i < struct_sym->method_count; i++) {
                            // 方法名在模块符号表中是 "StructName::method_name" 格式
                            // 需要提取出纯方法名部分
                            const char* full_method_name = struct_sym->methods[i].name;
                            const char* method_name = full_method_name;
                            // 查找 :: 分隔符
                            const char* sep = strstr(full_method_name, "::");
                            if (sep) {
                                method_name = sep + 2;  // 跳过 "::"
                            }
                            sdef->methods[i].name = strdup(method_name);
                        }

                        // 设置 impl 信息（face 实现）
                        if (struct_sym->impl_count > 0) {
                            sdef->impl_count = struct_sym->impl_count;
                            sdef->impl_names = (char**)malloc(sizeof(char*) * struct_sym->impl_count);
                            for (int i = 0; i < struct_sym->impl_count; i++) {
                                sdef->impl_names[i] = strdup(struct_sym->impl_names[i]);
                            }
                        }

                        struct_def_register(sdef);
                    }
                }

                // 将 struct 方法注册到 func_table，以便方法调用时能正确解析
                // 注意：即使 struct 定义已存在，也需要注册方法到 func_table
                for (int mi = 0; mi < struct_sym->method_count; mi++) {
                    const char* full_method_name = struct_sym->methods[mi].name;
                    // 检查是否已注册
                    Ast* existing = func_table_find(&s->func_table, full_method_name);
                    if (!existing) {
                        // 创建一个占位符函数定义 AST
                        Ast* placeholder = ast_new(AST_FUNC_DEF, 0);
                        if (placeholder) {
                            placeholder->u.func.name = strdup(full_method_name);
                            // pcnt = 参数数量 + 1 (self) + param_count (其他参数)
                            // struct 方法的 pcnt 包含 self 参数
                            placeholder->u.func.pcnt = 1 + struct_sym->methods[mi].param_count;
                            placeholder->u.func.return_type = type_new(struct_sym->methods[mi].return_type);
                            if (struct_sym->methods[mi].return_struct_name) {
                                placeholder->u.func.return_type->struct_name = strdup(struct_sym->methods[mi].return_struct_name);
                            }
                            if (struct_sym->methods[mi].return_type == TYPE_GENERIC_PARAM && struct_sym->methods[mi].return_type_param_name) {
                                placeholder->u.func.return_type->type_param_name = strdup(struct_sym->methods[mi].return_type_param_name);
                            }
                            // 设置返回类型的泛型参数（如 Holder[K] 中的 [K]）
                            if (struct_sym->methods[mi].return_generic_count > 0 && struct_sym->methods[mi].return_generic_param_names) {
                                placeholder->u.func.return_type->generic_count = struct_sym->methods[mi].return_generic_count;
                                placeholder->u.func.return_type->generic_args = (TypeInfo**)malloc(sizeof(TypeInfo*) * struct_sym->methods[mi].return_generic_count);
                                for (int gi = 0; gi < struct_sym->methods[mi].return_generic_count; gi++) {
                                    // 检查是否是泛型参数名（如 K, V）
                                    int is_type_param = 0;
                                    for (int tpi = 0; tpi < struct_sym->type_param_count; tpi++) {
                                        if (struct_sym->type_param_names[tpi] && strcmp(struct_sym->methods[mi].return_generic_param_names[gi], struct_sym->type_param_names[tpi]) == 0) {
                                            is_type_param = 1;
                                            placeholder->u.func.return_type->generic_args[gi] = type_new(TYPE_GENERIC_PARAM);
                                            placeholder->u.func.return_type->generic_args[gi]->type_param_name = strdup(struct_sym->methods[mi].return_generic_param_names[gi]);
                                            break;
                                        }
                                    }
                                    if (!is_type_param) {
                                        // 具体类型（如 int, string 等）—— 简单判断
                                        TypeInfo* arg_type = type_new(TYPE_ANY);
                                        if (strcmp(struct_sym->methods[mi].return_generic_param_names[gi], "int") == 0) arg_type = type_new(TYPE_INT);
                                        else if (strcmp(struct_sym->methods[mi].return_generic_param_names[gi], "float") == 0) arg_type = type_new(TYPE_FLOAT);
                                        else if (strcmp(struct_sym->methods[mi].return_generic_param_names[gi], "string") == 0) arg_type = type_new(TYPE_STRING);
                                        else if (strcmp(struct_sym->methods[mi].return_generic_param_names[gi], "bool") == 0) arg_type = type_new(TYPE_BOOL);
                                        placeholder->u.func.return_type->generic_args[gi] = arg_type;
                                    }
                                }
                            }
                            // 设置参数类型（包括泛型参数名）
                            int total_pcnt = 1 + struct_sym->methods[mi].param_count;
                            placeholder->u.func.param_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * total_pcnt);
                            // self 参数类型
                            placeholder->u.func.param_types[0] = type_new(TYPE_STRUCT);
                            placeholder->u.func.param_types[0]->struct_name = strdup(struct_sym->name);
                            if (struct_sym->type_param_count > 0 && struct_sym->type_param_names) {
                                placeholder->u.func.param_types[0]->generic_count = struct_sym->type_param_count;
                                placeholder->u.func.param_types[0]->generic_args = (TypeInfo**)malloc(sizeof(TypeInfo*) * struct_sym->type_param_count);
                                for (int tpi = 0; tpi < struct_sym->type_param_count; tpi++) {
                                    placeholder->u.func.param_types[0]->generic_args[tpi] = type_new(TYPE_GENERIC_PARAM);
                                    placeholder->u.func.param_types[0]->generic_args[tpi]->type_param_name = strdup(struct_sym->type_param_names[tpi]);
                                }
                            }
                            // 其他参数类型
                            for (int pi = 0; pi < struct_sym->methods[mi].param_count; pi++) {
                                if (struct_sym->methods[mi].param_generic_names && struct_sym->methods[mi].param_generic_names[pi]) {
                                    placeholder->u.func.param_types[1 + pi] = type_new(TYPE_GENERIC_PARAM);
                                    placeholder->u.func.param_types[1 + pi]->type_param_name = strdup(struct_sym->methods[mi].param_generic_names[pi]);
                                } else {
                                    placeholder->u.func.param_types[1 + pi] = type_new(struct_sym->methods[mi].param_types[pi]);
                                }
                            }
                            // 设置泛型类型参数（用于 resolve_generic_in_type）
                            if (struct_sym->type_param_count > 0 && struct_sym->type_param_names) {
                                placeholder->u.func.type_param_count = struct_sym->type_param_count;
                                placeholder->u.func.type_params = (char**)malloc(sizeof(char*) * struct_sym->type_param_count);
                                for (int tpi = 0; tpi < struct_sym->type_param_count; tpi++) {
                                    placeholder->u.func.type_params[tpi] = strdup(struct_sym->type_param_names[tpi]);
                                }
                            }
                            placeholder->u.func.default_count = 0;
                            func_table_add(&s->func_table, full_method_name, placeholder);
                        }
                    }
                }
                break;
            }

            // 尝试查找 clib 类型（优先查符号表，再查函数返回类型）
            {
                int clib_found = 0;
                // 先尝试从模块符号表的 clibs 中查找
                ModuleClibSymbol* clib_sym_tbl = NULL;
                if (module_info->sym_table) {
                    clib_sym_tbl = module_symbol_table_find_clib(module_info->sym_table, symbol_name);
                }
                if (clib_sym_tbl) {
                    clib_found = 1;
                    SymKind kind = s->is_module ? SYM_MODULE : SYM_GLOBAL;
                    Symbol* sym = scope_define(s->current, symbol_name, kind);
                    if (sym) {
                        sym->kind = SYM_TYPE;
                        sym->type = type_new(TYPE_CLIB);
                        sym->type->struct_name = strdup(symbol_name);
                        // 填充 clib 函数签名
                        sym->clib_func_count = clib_sym_tbl->func_count;
                        sym->clib_func_names = (char**)malloc(sizeof(char*) * clib_sym_tbl->func_count);
                        sym->clib_func_return_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * clib_sym_tbl->func_count);
                        sym->clib_func_param_counts = (int*)malloc(sizeof(int) * clib_sym_tbl->func_count);
                        sym->clib_func_param_types = (TypeInfo***)malloc(sizeof(TypeInfo**) * clib_sym_tbl->func_count);
                        for (int fi = 0; fi < clib_sym_tbl->func_count; fi++) {
                            sym->clib_func_names[fi] = strdup(clib_sym_tbl->funcs[fi].name);
                            // 构造返回类型：Ptr[T] 需要 element_type
                            if (clib_sym_tbl->funcs[fi].return_type == TYPE_PTR_GENERIC
                                && clib_sym_tbl->funcs[fi].return_element_type != TYPE_PTR) {
                                sym->clib_func_return_types[fi] = type_ptr_generic(
                                    type_new(clib_sym_tbl->funcs[fi].return_element_type));
                            } else {
                                sym->clib_func_return_types[fi] = type_new(clib_sym_tbl->funcs[fi].return_type);
                            }
                            sym->clib_func_param_counts[fi] = clib_sym_tbl->funcs[fi].param_count;
                            sym->clib_func_param_types[fi] = (TypeInfo**)malloc(sizeof(TypeInfo*) * clib_sym_tbl->funcs[fi].param_count);
                            for (int pi = 0; pi < clib_sym_tbl->funcs[fi].param_count; pi++) {
                                // 构造参数类型：Ptr[T] 需要 element_type
                                if (clib_sym_tbl->funcs[fi].param_types[pi] == TYPE_PTR_GENERIC
                                    && clib_sym_tbl->funcs[fi].param_element_types
                                    && clib_sym_tbl->funcs[fi].param_element_types[pi] != TYPE_PTR) {
                                    sym->clib_func_param_types[fi][pi] = type_ptr_generic(
                                        type_new(clib_sym_tbl->funcs[fi].param_element_types[pi]));
                                } else {
                                    sym->clib_func_param_types[fi][pi] = type_new(clib_sym_tbl->funcs[fi].param_types[pi]);
                                }
                            }
                        }
                    }
                }
                // 回退：通过检查函数返回类型
                if (!clib_found) {
                    for (int fi = 0; fi < module_info->sym_table->func_count && !clib_found; fi++) {
                        ModuleFuncSymbol* func = &module_info->sym_table->funcs[fi];
                        if (func->return_type == TYPE_CLIB && func->return_struct_name &&
                            strcmp(func->return_struct_name, symbol_name) == 0) {
                            clib_found = 1;
                            SymKind kind = s->is_module ? SYM_MODULE : SYM_GLOBAL;
                            Symbol* sym = scope_define(s->current, symbol_name, kind);
                            if (sym) {
                                sym->kind = SYM_TYPE;
                                sym->type = type_new(TYPE_CLIB);
                                sym->type->struct_name = strdup(symbol_name);
                                // 从源模块符号表复制 clib 函数签名，实现编译期参数检查
                                ModuleClibSymbol* src_clib = module_symbol_table_find_clib(module_info->sym_table, symbol_name);
                                if (src_clib && src_clib->func_count > 0) {
                                    sym->clib_func_count = src_clib->func_count;
                                    sym->clib_func_names = (char**)malloc(sizeof(char*) * src_clib->func_count);
                                    sym->clib_func_return_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * src_clib->func_count);
                                    sym->clib_func_param_counts = (int*)malloc(sizeof(int) * src_clib->func_count);
                                    sym->clib_func_param_types = (TypeInfo***)malloc(sizeof(TypeInfo**) * src_clib->func_count);
                                    for (int ci = 0; ci < src_clib->func_count; ci++) {
                                        sym->clib_func_names[ci] = strdup(src_clib->funcs[ci].name);
                                        if (src_clib->funcs[ci].return_type == TYPE_PTR_GENERIC
                                            && src_clib->funcs[ci].return_element_type != TYPE_PTR) {
                                            sym->clib_func_return_types[ci] = type_ptr_generic(
                                                type_new(src_clib->funcs[ci].return_element_type));
                                        } else {
                                            sym->clib_func_return_types[ci] = type_new(src_clib->funcs[ci].return_type);
                                        }
                                        sym->clib_func_param_counts[ci] = src_clib->funcs[ci].param_count;
                                        sym->clib_func_param_types[ci] = (TypeInfo**)malloc(sizeof(TypeInfo*) * src_clib->funcs[ci].param_count);
                                        for (int pi = 0; pi < src_clib->funcs[ci].param_count; pi++) {
                                            if (src_clib->funcs[ci].param_types[pi] == TYPE_PTR_GENERIC
                                                && src_clib->funcs[ci].param_element_types
                                                && src_clib->funcs[ci].param_element_types[pi] != TYPE_PTR) {
                                                sym->clib_func_param_types[ci][pi] = type_ptr_generic(
                                                    type_new(src_clib->funcs[ci].param_element_types[pi]));
                                            } else {
                                                sym->clib_func_param_types[ci][pi] = type_new(src_clib->funcs[ci].param_types[pi]);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                if (clib_found) break;
            }

            // 尝试查找 enum 定义
            if (module_info->sym_table) {
                ModuleEnumSymbol* enum_sym = module_symbol_table_find_enum(module_info->sym_table, symbol_name);
                if (enum_sym) {
                    Symbol* sym = scope_define(s->current, symbol_name, SYM_TYPE);
                    if (sym) {
                        sym->type = type_new(TYPE_ENUM);
                        sym->type->struct_name = strdup(symbol_name);
                        // 存储 enum 成员信息到 symbol，用于 AST_NUM 转换
                        sym->enum_value_count = enum_sym->member_count;
                        sym->enum_value_names = (char**)malloc(sizeof(char*) * enum_sym->member_count);
                        sym->enum_values = (int64_t*)malloc(sizeof(int64_t) * enum_sym->member_count);
                        for (int mi = 0; mi < enum_sym->member_count; mi++) {
                            sym->enum_value_names[mi] = strdup(enum_sym->member_names[mi]);
                            sym->enum_values[mi] = enum_sym->member_values[mi];
                        }
                    }
                    break;
                }
            }

            // 尝试查找 face 定义
            ModuleFaceSymbol* face_sym = module_symbol_table_find_face(module_info->sym_table, symbol_name);
            if (face_sym) {
                // 将 face 定义注册到当前作用域
                // face 类型使用 SYM_TYPE，不占用运行时索引
                Symbol* sym = scope_define(s->current, symbol_name, SYM_TYPE);
                if (sym) {
                    sym->type = type_new(TYPE_FACE);
                    sym->type->struct_name = strdup(symbol_name);
                }
                // 同时注册 face 定义到全局表，用于类型检查
                if (!face_def_find(symbol_name)) {
                    ObjFaceDef* fdef = face_def_new(symbol_name, face_sym->method_count);
                    if (fdef) {
                        for (int i = 0; i < face_sym->method_count; i++) {
                            fdef->methods[i].name = strdup(face_sym->methods[i].name);
                            fdef->methods[i].return_type = type_new(face_sym->methods[i].return_type);
                            fdef->methods[i].param_count = 0;
                            fdef->methods[i].param_types = NULL;
                        }
                        face_def_register(fdef);
                    }
                }
                break;
            }

            // 尝试查找 alias 定义
            ModuleAliasSymbol* alias_sym = module_symbol_table_find_alias(module_info->sym_table, symbol_name);
            if (alias_sym) {
                // 将 alias 注册为类型符号，让后续类型解析能识别
                // 注：import 时已将模块的 export alias 注册到解析器别名表，
                // 这里只需注册到语义作用域即可
                Symbol* sym = scope_define(s->current, symbol_name, SYM_TYPE);
                if (sym && alias_sym->type_info) {
                    sym->type = type_copy(alias_sym->type_info);
                    // 递归导入 alias 底层类型依赖（如 EventHandler = func(Event):bool 中的 Event）
                    import_type_deps(s, module_info, alias_sym->type_info);
                }
                break;
            }

            // 尝试查找函数定义（use module.funcName 导入函数）
            ModuleFuncSymbol* func_sym = module_symbol_table_find_func(module_info->sym_table, symbol_name);
            if (func_sym) {
                Symbol* sym = scope_define(s->current, symbol_name, SYM_FUNC_ALIAS);
                if (sym) {
                    sym->alias_module_name = strdup(module_name);
                    sym->alias_func_name = strdup(symbol_name);
                    // 设置返回类型信息，用于类型推断
                    if (func_sym->return_type != TYPE_ANY && func_sym->return_type != TYPE_INFER) {
                        if (func_sym->return_type_info) {
                            sym->type = type_copy(func_sym->return_type_info);
                        } else {
                            sym->type = type_new(func_sym->return_type);
                            if (func_sym->return_struct_name) {
                                sym->type->struct_name = strdup(func_sym->return_struct_name);
                            }
                        }
                    } else {
                        sym->type = type_new(TYPE_ANY);
                    }
                }
                break;
            }

            // 尝试查找 const 常量定义
            if (module_info->sym_table) {
                ModuleVarSymbol* var_sym = module_symbol_table_find_var(module_info->sym_table, symbol_name);
                if (var_sym && var_sym->is_const) {
                    char msg[BUFFER_MEDIUM];
                    snprintf(msg, sizeof(msg), "use 语句不能导入常量 '%s'，请使用 %s.%s 访问", symbol_name, module_name, symbol_name);
                    error_add(ERR_SEMANTIC, ast->line, msg);
                    break;
                }
            }

            // 没有找到 struct、clib、face、alias 或函数
            char msg[BUFFER_MEDIUM];
            snprintf(msg, sizeof(msg), "use 语句错误：模块 '%s' 中没有类型或函数 '%s'", module_name, symbol_name);
            error_add(ERR_SEMANTIC, ast->line, msg);
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
            }

            // 检查模块是否已导入：原生模块必须显式导入，用户模块也必须导入
            if (is_native_module && !is_imported_module) {
                char msg[BUFFER_MEDIUM];
                snprintf(msg, sizeof(msg), "模块 '%s' 未导入，请先使用 'import %s' 导入模块",
                         ast->u.module_call.module_name, ast->u.module_call.module_name);
                error_add(ERR_SEMANTIC, ast->line, msg);
                break;
            }

            if (is_native_module) {
                // 原生模块调用，检查方法是否存在
                // 先检查方法是否存在（通过查找元信息）
                extern ModuleMethodMeta* native_find_module_method(const char* module_name, const char* method_name);
                ModuleMethodMeta* meta = native_find_module_method(actual_module, ast->u.module_call.method_name);
                
                if (!meta) {
                    // 方法不存在，尝试列出该模块的可用方法作为提示
                    char msg[1024];  /* 大于 BUFFER_MEDIUM，容纳中文 + 多个 %s */
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
                        snprintf(msg, sizeof(msg), "未找到模块方法: %s.%s（可用: %s）",
                                 actual_module, ast->u.module_call.method_name, available);
                        free(all_methods);
                    } else {
                        snprintf(msg, sizeof(msg), "未找到模块方法: %s.%s",
                                 actual_module, ast->u.module_call.method_name);
                    }
                    error_add(ERR_SEMANTIC, ast->line, msg);
                } else {
                    // 方法存在，检查参数数量
                    int expected_arity = meta->arity;
                    int actual_arity = ast->u.module_call.args.count;

                    // arity >= 0: 固定参数数量
                    if (expected_arity >= 0 && actual_arity != expected_arity) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "%s.%s 参数数量不匹配: 期望 %d, 实际 %d",
                                 actual_module, ast->u.module_call.method_name, expected_arity, actual_arity);
                        if (strcmp(ast->u.module_call.method_name, "map") == 0 && actual_arity == 1 && expected_arity == 2) {
                            strncat(msg, "\n  \xf0\x9f\x92\xa1 提示: .map() 会传递 (item, index) 两个参数，\n"
                                    "     函数签名需加第二个参数，如 func compress(string w, int _): string",
                                    sizeof(msg) - strlen(msg) - 1);
                        }
                        error_add(ERR_SEMANTIC, ast->line, msg);
                    }
                    // arity < 0: 可变参数，检查是否少于 min_arity
                    else if (expected_arity < 0 && meta->min_arity >= 0 && actual_arity < meta->min_arity) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "%s.%s 参数过少: 最少 %d 个, 实际 %d",
                                 actual_module, ast->u.module_call.method_name, meta->min_arity, actual_arity);
                        error_add(ERR_SEMANTIC, ast->line, msg);
                    }
                    // arity < 0: 可变参数，检查是否超过 max_arity
                    else if (expected_arity < 0 && meta->max_arity >= 0 && actual_arity > meta->max_arity) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "%s.%s 参数过多: 最多 %d 个, 实际 %d",
                                 actual_module, ast->u.module_call.method_name, meta->max_arity, actual_arity);
                        error_add(ERR_SEMANTIC, ast->line, msg);
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
                    
                    if (expected_type != TYPE_ANY && arg_type && arg_type->kind != TYPE_ANY && arg_type->kind != expected_type) {
                        // 允许 int -> float, bigint -> int, bigint -> float 的隐式转换
                        bool type_compatible = false;
                        if ((expected_type == TYPE_FLOAT && arg_type->kind == TYPE_INT) ||
                            (expected_type == TYPE_INT && arg_type->kind == TYPE_BIGINT) ||
                            (expected_type == TYPE_FLOAT && arg_type->kind == TYPE_BIGINT)) {
                            type_compatible = true;
                        }
                        // 允许 Ptr[T] <-> Ptr 之间的隐式转换
                        if ((expected_type == TYPE_PTR && arg_type->kind == TYPE_PTR_GENERIC) ||
                            (expected_type == TYPE_PTR_GENERIC && arg_type->kind == TYPE_PTR)) {
                            type_compatible = true;
                        }
                        // C 布局类型隐式转换
                        if ((expected_type == TYPE_INT && arg_type->kind >= TYPE_I8 && arg_type->kind <= TYPE_C_SSIZE) ||
                            (arg_type->kind == TYPE_INT && expected_type >= TYPE_I8 && expected_type <= TYPE_C_SSIZE)) {
                            type_compatible = true;
                        }
                        if ((expected_type == TYPE_FLOAT && (arg_type->kind == TYPE_F32 || arg_type->kind == TYPE_F64)) ||
                            (arg_type->kind == TYPE_FLOAT && (expected_type == TYPE_F32 || expected_type == TYPE_F64))) {
                            type_compatible = true;
                        }
                        if ((expected_type == TYPE_STRING && (arg_type->kind == TYPE_STR8 || arg_type->kind == TYPE_STR16)) ||
                            (arg_type->kind == TYPE_STRING && (expected_type == TYPE_STR8 || expected_type == TYPE_STR16))) {
                            type_compatible = true;
                        }

                        // 允许 cfunc 类型匹配
                        if (expected_type == TYPE_CFUNC && arg_type->kind == TYPE_CFUNC) {
                            type_compatible = true;
                        }
                        if (!type_compatible) {
                            char msg[BUFFER_MEDIUM];
                            const char* hint = get_type_conversion_hint(expected_type, arg_type->kind);
                            if (hint) {
                                snprintf(msg, sizeof(msg), "%s.%s 参数 %d 类型错误: 期望 '%s'，但传入 '%s'\n%s",
                                         actual_module, ast->u.module_call.method_name, i + 1,
                                         type_kind_to_string(expected_type), type_to_string(arg_type), hint);
                            } else {
                                snprintf(msg, sizeof(msg), "%s.%s 参数 %d 类型错误: 期望 '%s'，但传入 '%s'",
                                         actual_module, ast->u.module_call.method_name, i + 1,
                                         type_kind_to_string(expected_type), type_to_string(arg_type));
                            }
                            error_add(ERR_SEMANTIC, ast->line, msg);
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
                
                // 首先检查是否是 struct 定义
                ModuleSymbolTable* sym_table = module_symbol_table_create(module_info->file_path);
                int is_struct = 0;
                if (sym_table) {
                    if (module_symbol_table_scan(sym_table, current_file) == 0) {
                        ModuleStructSymbol* struct_sym = module_symbol_table_find_struct(sym_table, ast->u.module_call.method_name);
                        if (struct_sym) {
                            is_struct = 1;
                            // 是 struct 定义，提示使用 new 关键字
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "struct '%s.%s' 需要使用 new 关键字实例化，例如: new %s.%s()",
                                    ast->u.module_call.module_name, struct_sym->name,
                                    ast->u.module_call.module_name, struct_sym->name);
                            error_add(ERR_SEMANTIC, ast->line, msg);
                        }
                    }
                    module_symbol_table_destroy(sym_table);
                }
                
                // 如果不是 struct，检查是否是函数，并推断返回类型
                if (!is_struct) {
                    int has_method = module_has_method(module_info->file_path, current_file, 
                                                       ast->u.module_call.method_name);
                    if (has_method == 0) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "模块 '%s' 中没有方法 '%s'",
                                 ast->u.module_call.module_name, ast->u.module_call.method_name);
                        error_add(ERR_SEMANTIC, ast->line, msg);
                    } else if (module_info->sym_table) {
                        // 从模块符号表推断返回类型
                        ModuleFuncSymbol* func_sym = module_symbol_table_find_func(
                            module_info->sym_table, ast->u.module_call.method_name);
                        if (func_sym && func_sym->return_type != TYPE_ANY && func_sym->return_type != TYPE_INFER) {
                            if (func_sym->return_type_info) {
                                ast->cached_type = type_copy(func_sym->return_type_info);
                            } else {
                                ast->cached_type = type_new(func_sym->return_type);
                                if (func_sym->return_struct_name) {
                                    ast->cached_type->struct_name = strdup(func_sym->return_struct_name);
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
                                        char msg[BUFFER_MEDIUM];
                                        snprintf(msg, sizeof(msg), "clib '%s' 函数 '%s' 参数数量不匹配: 期望 %d, 实际 %d",
                                                 obj_type->struct_name, method_name, expected_count, actual_count);
                                        error_add(ERR_SEMANTIC, ast->line, msg);
                                    }
                                    // 检查每个参数类型
                                    for (int pi = 0; pi < actual_count && pi < expected_count; pi++) {
                                        TypeInfo* expected_param = clib_sym->clib_func_param_types[fi][pi];
                                        Ast* arg = ast->u.module_call.args.items[pi];
                                        TypeInfo* arg_type = infer_expr_type(s, arg);
                                        if (expected_param && arg_type && !type_is_compatible(expected_param, arg_type)) {
                                            char msg[BUFFER_MEDIUM];
                                            format_type_error(msg, sizeof(msg),
                                                "clib '%s3' 函数 '%s4' 参数 %d 类型不匹配: 期望 '%s1', 实际 '%s2'",
                                                expected_param, arg_type,
                                                obj_type->struct_name, method_name);
                                            // 追加参数序号
                                            char final_msg[BUFFER_MEDIUM];
                                            snprintf(final_msg, sizeof(final_msg), msg, pi + 1);
                                            error_add(ERR_TYPE_MISMATCH, ast->line, final_msg);
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
                            char msg[BUFFER_MEDIUM];
                            // 检查是否是因为未 use 导入 clib 类型
                            Symbol* local_clib = scope_resolve(s->current, obj_type->struct_name);
                            if (!local_clib) {
                                snprintf(msg, sizeof(msg), "clib '%s' 函数 '%s' 未找到，请添加 use 语句导入 clib 类型（如 use module.%s）",
                                         obj_type->struct_name, method_name, obj_type->struct_name);
                            } else {
                                snprintf(msg, sizeof(msg), "clib '%s' 没有声明函数 '%s'",
                                         obj_type->struct_name, method_name);
                            }
                            error_add(ERR_SEMANTIC, ast->line, msg);
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
                        // 自定义 struct 方法的参数类型检查
                        if (obj_type && obj_type->kind == TYPE_STRUCT && obj_type->struct_name) {
                            Ast* struct_def_ast = NULL;
                            for (int si = 0; si < s->root->u.block.count; si++) {
                                Ast* stmt = s->root->u.block.items[si];
                                if (stmt->kind == AST_STRUCT_DEF && strcmp(stmt->u.struct_def.name, obj_type->struct_name) == 0) {
                                    struct_def_ast = stmt;
                                    break;
                                }
                            }
                            if (struct_def_ast) {
                                for (int mi = 0; mi < struct_def_ast->u.struct_def.method_count; mi++) {
                                    Ast* method_ast = struct_def_ast->u.struct_def.methods[mi];
                                    if (method_ast && method_ast->kind == AST_FUNC_DEF &&
                                        strcmp(method_ast->u.func.name, method_name) == 0) {
                                        int actual_arity = ast->u.module_call.args.count;
                                        int method_param_count = method_ast->u.func.pcnt - 1;
                                        int default_cnt = method_ast->u.func.default_count;
                                        int required_cnt = method_param_count - default_cnt;
                                        if (actual_arity < required_cnt || actual_arity > method_param_count) {
                                            char msg[BUFFER_MEDIUM];
                                            if (actual_arity < required_cnt) {
                                                snprintf(msg, sizeof(msg), "方法 '%s' 参数不足: 至少需要 %d 个, 实际 %d",
                                                         method_name, required_cnt, actual_arity);
                                            } else {
                                                snprintf(msg, sizeof(msg), "方法 '%s' 参数过多: 最多 %d 个, 实际 %d",
                                                         method_name, method_param_count, actual_arity);
                                            }
                                            if (strcmp(method_name, "map") == 0 && actual_arity == 1 && method_param_count == 2) {
                                                strncat(msg, "\n  \xf0\x9f\x92\xa1 提示: .map() 会传递 (item, index) 两个参数，\n"
                                                        "     函数签名需加第二个参数，如 func compress(string w, int _): string",
                                                        sizeof(msg) - strlen(msg) - 1);
                                            }
                                            error_add(ERR_SEMANTIC, ast->line, msg);
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

                                            for (int i = 0; i < actual_arity; i++) {
                                                TypeInfo* expected_type = method_ast->u.func.param_types[i + 1];
                                                if (!expected_type) continue;

                                                TypeInfo* check_type = expected_type;
                                                if (gp_count > 0) {
                                                    for (int gi = 0; gi < gp_count; gi++) {
                                                        TypeInfo* sub = type_substitute(check_type, gp_names[gi], gp_types[gi]);
                                                        if (sub != check_type) {
                                                            check_type = sub;
                                                        }
                                                    }
                                                }

                                                TypeInfo* arg_type = infer_expr_type(s, ast->u.module_call.args.items[i]);
                                                if (arg_type && arg_type->kind != TYPE_ANY && check_type->kind != TYPE_ANY) {
                                                    if (!type_is_compatible(check_type, arg_type)) {
                                                        char msg[BUFFER_MEDIUM];
                                                        char idx_str[16];
                                                        snprintf(idx_str, sizeof(idx_str), "%d", i + 1);
                                                        format_type_error(msg, sizeof(msg),
                                                            "%s3 第 %s4 个参数类型不匹配: 期望 %s1, 实际 %s2",
                                                            check_type, arg_type,
                                                            method_name, idx_str);
                                                        error_add(ERR_SEMANTIC, ast->line, msg);
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

                        // 根据对象类型名称和方法名查找方法
                        if (obj_type_name) {
                            return_type = native_get_instance_method_return_type(obj_type_name, method_name, &expected_arity);
                        } else if (obj_sym->type && obj_sym->type->kind == TYPE_GENERIC_PARAM) {
                            // 泛型约束类型参数（如 T: Comparable），交给下面的约束检查
                            expected_arity = -1;
                            return_type = TYPE_ANY;
                        } else {
                            // any 类型不能调用方法，提示用户使用类型收窄或指定参数类型
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg),
                                "不能在 any 类型上调用方法 '%s'\n"
                                "  提示: 使用 if x is Type { x.%s() } 类型收窄\n"
                                "  或者将参数声明为具体类型",
                                method_name, method_name);
                            error_add(ERR_SEMANTIC, ast->line, msg);
                            expected_arity = -1;
                            return_type = TYPE_ANY;
                        }

                        // 判断方法是否找到
                        // 可变参数实例方法 arity=-1 但方法是存在的，用 inst_meta 判断
                        int method_found = 0;
                        if (!(return_type == TYPE_ANY && expected_arity == -1)) {
                            method_found = 1;
                        } else if (obj_type_name) {
                            // 可变参数情况：再检查一次实例方法是否存在
                            const InstanceMethodMeta* im = native_find_instance_method(obj_type_name, method_name);
                            if (im) method_found = 1;
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
                        const InstanceMethodMeta* inst_meta = NULL;
                        if (obj_type_name) {
                            inst_meta = native_find_instance_method(obj_type_name, method_name);
                        }

                        // arity >= 0: 固定参数数量
                        if (expected_arity >= 0 && actual_arity != expected_arity) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "调用方法 '%s' 时参数数量不匹配: 期望 %d, 实际 %d",
                                     method_name, expected_arity, actual_arity);
                            error_add(ERR_SEMANTIC, ast->line, msg);
                        }
                        // arity < 0: 可变参数，检查是否少于 min_arity
                        else if (expected_arity < 0 && inst_meta && inst_meta->min_arity >= 0 && actual_arity < inst_meta->min_arity) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "调用方法 '%s' 时参数过少: 最少 %d 个, 实际 %d",
                                     method_name, inst_meta->min_arity, actual_arity);
                            error_add(ERR_SEMANTIC, ast->line, msg);
                        }
                        // arity < 0: 可变参数，检查是否超过 max_arity
                        else if (expected_arity < 0 && inst_meta && inst_meta->max_arity >= 0 && actual_arity > inst_meta->max_arity) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "调用方法 '%s' 时参数过多: 最多 %d 个, 实际 %d",
                                     method_name, inst_meta->max_arity, actual_arity);
                            error_add(ERR_SEMANTIC, ast->line, msg);
                        }

                        // 检查参数类型（仅当参数数量匹配或可变参数时）
                        if (obj_type_name && (expected_arity == -1 || actual_arity == expected_arity)) {
                            for (int i = 0; i < actual_arity; i++) {
                                TypeKind expected_param_type = native_get_instance_method_param_type(obj_type_name, method_name, i);
                                TypeInfo* arg_type = infer_expr_type(s, ast->u.module_call.args.items[i]);
                                
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
                                        if (type_utils_try_update_dict_value_type(obj_sym, arg_type)) {
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
                                if (arg_type && arg_type->kind != TYPE_ANY) {
                                    if (expected_type_for_check) {
                                        // 有完整类型信息，进行深度类型比较（支持嵌套类型）
                                        if (!type_is_compatible(expected_type_for_check, arg_type)) {
                                            char msg[BUFFER_MEDIUM];
                                            // 先保存字符串避免 buffer 覆盖
                                            const char* exp_str = type_to_string(expected_type_for_check);
                                            char exp_buf[BUFFER_SMALL];
                                            strncpy(exp_buf, exp_str, sizeof(exp_buf) - 1);
                                            exp_buf[sizeof(exp_buf) - 1] = '\0';
                                            const char* hint = get_type_conversion_hint(expected_type_for_check->kind, arg_type->kind);
                                            if (hint) {
                                                snprintf(msg, sizeof(msg), "类型错误: 参数 %d 期望 '%s'，但传入 '%s'\n%s",
                                                         i + 1, exp_buf, type_to_string(arg_type), hint);
                                            } else {
                                                snprintf(msg, sizeof(msg), "类型错误: 参数 %d 期望 '%s'，但传入 '%s'",
                                                         i + 1, exp_buf, type_to_string(arg_type));
                                            }
                                            error_add(ERR_SEMANTIC, ast->line, msg);
                                        }
                                    } else if (expected_param_type != TYPE_ANY) {
                                        // 只有 kind 信息，进行简单类型比较
                                        if (arg_type->kind != expected_param_type) {
                                            // 允许 int -> float, bigint -> int, bigint -> float 的隐式转换
                                            bool type_compatible = false;
                                            if ((expected_param_type == TYPE_FLOAT && arg_type->kind == TYPE_INT) ||
                                                (expected_param_type == TYPE_INT && arg_type->kind == TYPE_BIGINT) ||
                                                (expected_param_type == TYPE_FLOAT && arg_type->kind == TYPE_BIGINT)) {
                                                type_compatible = true;
                                            }
                                            // 允许 Ptr[T] <-> Ptr 之间的隐式转换
                                            if ((expected_param_type == TYPE_PTR && arg_type->kind == TYPE_PTR_GENERIC) ||
                                                (expected_param_type == TYPE_PTR_GENERIC && arg_type->kind == TYPE_PTR)) {
                                                type_compatible = true;
                                            }
                                            // C 布局类型隐式转换
                                            if ((expected_param_type == TYPE_INT && arg_type->kind >= TYPE_I8 && arg_type->kind <= TYPE_C_SSIZE) ||
                                                (arg_type->kind == TYPE_INT && expected_param_type >= TYPE_I8 && expected_param_type <= TYPE_C_SSIZE)) {
                                                type_compatible = true;
                                            }
                                            if ((expected_param_type == TYPE_FLOAT && (arg_type->kind == TYPE_F32 || arg_type->kind == TYPE_F64)) ||
                                                (arg_type->kind == TYPE_FLOAT && (expected_param_type == TYPE_F32 || expected_param_type == TYPE_F64))) {
                                                type_compatible = true;
                                            }
                                            if ((expected_param_type == TYPE_STRING && (arg_type->kind == TYPE_STR8 || arg_type->kind == TYPE_STR16)) ||
                                                (arg_type->kind == TYPE_STRING && (expected_param_type == TYPE_STR8 || expected_param_type == TYPE_STR16))) {
                                                type_compatible = true;
                                            }
                                            if (!type_compatible) {
                                                char msg[BUFFER_MEDIUM];
                                                const char* hint = get_type_conversion_hint(expected_param_type, arg_type->kind);
                                                if (hint) {
                                                    snprintf(msg, sizeof(msg), "类型错误: 参数 %d 期望 '%s'，但传入 '%s'\n%s",
                                                             i + 1, type_kind_to_string(expected_param_type), type_to_string(arg_type), hint);
                                                } else {
                                                    snprintf(msg, sizeof(msg), "类型错误: 参数 %d 期望 '%s'，但传入 '%s'",
                                                             i + 1, type_kind_to_string(expected_param_type), type_to_string(arg_type));
                                                }
                                                error_add(ERR_SEMANTIC, ast->line, msg);
                                            }
                                        }
                                    }
                                }
                                
                                if (arg_type) type_free(arg_type);
                            }
                        }
                    } else if (is_func_type_field_call) {
                        // 函数类型字段调用（如 t.cb() 其中 cb 是 func():void 字段），合法调用，不报错
                    } else if (!obj_sym->type || obj_sym->type->kind == TYPE_ANY) {
                        // 未初始化变量
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "不能对未初始化的变量 '%s' 调用方法 '%s'",
                                 ast->u.module_call.module_name, method_name);
                        error_add(ERR_SEMANTIC, ast->line, msg);
                    } else {
                        // 方法不存在于该类型
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "类型 '%s' 没有方法 '%s'",
                                 type_to_string(obj_sym->type), method_name);
                        error_add(ERR_SEMANTIC, ast->line, msg);
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
                char msg[BUFFER_MEDIUM];
                snprintf(msg, sizeof(msg), "未定义的模块或变量: %s", ast->u.module_call.module_name);
                error_add(ERR_SEMANTIC, ast->line, msg);
            }
            }
            break;
        }

        case AST_MODULE_ACCESS: {
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
                    char msg[BUFFER_MEDIUM];
                    snprintf(msg, sizeof(msg), "未定义的变量: %s", ast->u.module_access.module_name);
                    const char* hint = get_similar_name_hint(s->current, ast->u.module_access.module_name);
                    if (hint[0]) strncat(msg, hint, sizeof(msg) - strlen(msg) - 1);
                    error_add(ERR_UNDEFINED_VAR, ast->line, msg);
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
                        ast->u.num.value = (double)member_value;
                        ast->u.num.is_float = 0;
                        ast->u.num.is_bigint = 0;
                        ast->u.num.bigint_str = NULL;  // 必须清空：union 中 bigint_str 与 module_access.ref.kind/index 重叠
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
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "枚举 '%s' 中没有成员 '%s'",
                                 ast->u.module_access.module_name, mname);
                        error_add(ERR_UNDEFINED_VAR, ast->line, msg);
                    }
                } else if (is_struct_type || is_cstruct_type || is_face_type) {
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
                } else {
                    // 普通属性访问：转换为 AST_INDEX
                    Ast* index_ast = ast_new(AST_STRING, ast->line);
                    index_ast->u.string.value = strdup(ast->u.module_access.member_name);
                    index_ast->u.string.len = (int)strlen(ast->u.module_access.member_name);

                    ast->kind = AST_INDEX;
                    ast->u.index.obj = var_ast;
                    ast->u.index.index = index_ast;

                    visit(s, index_ast);
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
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "模块 '%s' 中没有常量 '%s'，如果是方法调用请加括号 ()",
                                 actual_module, ast->u.module_access.member_name);
                        error_add(ERR_SEMANTIC, ast->line, msg);
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
                            member_type = mv->type;
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
            }
            break;