        case AST_BINOP: {
            visit(s, ast->u.binop.l);
            visit(s, ast->u.binop.r);
            TypeInfo* binop_type = infer_expr_type(s, ast);
            if (binop_type) { type_free(binop_type); }
            // 泛型运算符无约束警告
            {
                TypeInfo* lt = ast->u.binop.l ? ast->u.binop.l->cached_type : NULL;
                TypeInfo* rt = ast->u.binop.r ? ast->u.binop.r->cached_type : NULL;
                if ((lt && lt->kind == TYPE_GENERIC_PARAM) || (rt && rt->kind == TYPE_GENERIC_PARAM)) {
                    if (s->current_func && s->current_func->u.func.type_param_count > 0) {
                        int op = ast->u.binop.op;
                        int is_arith = (op == TOK_PLUS || op == TOK_MINUS || op == TOK_STAR ||
                                        op == TOK_SLASH || op == TOK_MOD || op == TOK_BITAND ||
                                        op == TOK_BITOR || op == TOK_BITXOR || op == TOK_SHL ||
                                        op == TOK_SHR || op == TOK_USHR);
                        int is_cmp = (op == TOK_LT || op == TOK_GT || op == TOK_LE || op == TOK_GE);
                        if (is_arith || is_cmp) {
                            const char* gname = (lt && lt->kind == TYPE_GENERIC_PARAM) ? lt->type_param_name :
                                                (rt && rt->kind == TYPE_GENERIC_PARAM) ? rt->type_param_name : NULL;
                            if (gname) {
                                int has_c = 0;
                                for (int tp = 0; tp < s->current_func->u.func.type_param_count; tp++) {
                                    if (s->current_func->u.func.type_params[tp] &&
                                        strcmp(s->current_func->u.func.type_params[tp], gname) == 0) {
                                        if (s->current_func->u.func.type_param_constraints &&
                                            s->current_func->u.func.type_param_constraints[tp]) has_c = 1;
                                        break;
                                    }
                                }
                                if (!has_c) {
                                    fprintf(stderr, "[WARN] %s:%d: 泛型参数 '%s' 参与%s运算无约束\n",
                                        error_get_filename(), ast->line, gname, is_arith ? "算术" : "比较");
                                }
                            }
                        }
                    }
                }
            }
            break;
        }

        case AST_INDEX_ASSIGN: {
            // 初始化字段索引为 -1（未确定）
            ast->u.index_assign.field_index = -1;
            
            visit(s, ast->u.index_assign.obj);
            visit(s, ast->u.index_assign.index);
            visit(s, ast->u.index_assign.value);

            // 获取被赋值对象的类型（支持嵌套字段访问如 r.position.x）
            TypeInfo* obj_type = infer_expr_type(s, ast->u.index_assign.obj);

            // face 类型限制：不能给 face 变量的底层 struct 字段赋值
            if (obj_type && obj_type->kind == TYPE_FACE && obj_type->struct_name &&
                ast->u.index_assign.index->kind == AST_STRING) {
                const char* field_name = ast->u.index_assign.index->u.string.value;
                ObjFaceDef* fdef = face_def_find(obj_type->struct_name);
                if (fdef && fdef->method_count > 0) {
                    int is_face_method = 0;
                    for (int i = 0; i < fdef->method_count; i++) {
                        if (strcmp(fdef->methods[i].name, field_name) == 0) {
                            is_face_method = 1;
                            break;
                        }
                    }
                    if (!is_face_method) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "face '%s' 没有字段 '%s'（使用 'as' 转型访问底层 struct）",
                                 obj_type->struct_name, field_name);
                        error_add(ERR_SEMANTIC, ast->line, msg);
                    }
                }
            }

            if (obj_type && obj_type->kind == TYPE_STRUCT) {
                if (ast->u.index_assign.index->kind == AST_STRING) {
                    char* field_name = ast->u.index_assign.index->u.string.value;

                    // 从 struct 定义中查找字段类型和索引
                    TypeInfo* expected_type = NULL;
                    if (obj_type->struct_name) {
                        Symbol* struct_def_sym = scope_resolve(s->current, obj_type->struct_name);
                        if (struct_def_sym && struct_def_sym->struct_field_count > 0) {
                            for (int j = 0; j < struct_def_sym->struct_field_count; j++) {
                                if (strcmp(struct_def_sym->struct_field_names[j], field_name) == 0) {
                                    expected_type = struct_def_sym->struct_field_types[j];
                                    ast->u.index_assign.field_index = j; // 存储字段索引
                                    break;
                                }
                            }
                        }
                    }

                    if (expected_type) {
                        // 推断值的类型
                        TypeInfo* value_type = infer_expr_type(s, ast->u.index_assign.value);

                        // 检查类型兼容性
                        if (value_type && !type_is_compatible(expected_type, value_type)) {
                            char msg[BUFFER_MEDIUM];
                            format_type_error(msg, sizeof(msg),
                                "字段 '%s3' 类型不匹配: 期望 '%s1'，实际 '%s2'",
                                expected_type, value_type,
                                field_name, NULL);
                            error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                        }

                        if (value_type) type_free(value_type);
                    }
                }
            }
            // cstruct 字段赋值：设置字段索引
            else if (obj_type && obj_type->kind == TYPE_CSTRUCT) {
                if (ast->u.index_assign.index->kind == AST_STRING) {
                    char* field_name = ast->u.index_assign.index->u.string.value;
                    if (obj_type->struct_name) {
                        // 先尝试从作用域查找
                        Symbol* cstruct_def_sym = scope_resolve(s->current, obj_type->struct_name);
                        if (cstruct_def_sym && cstruct_def_sym->struct_field_count > 0) {
                            for (int j = 0; j < cstruct_def_sym->struct_field_count; j++) {
                                if (strcmp(cstruct_def_sym->struct_field_names[j], field_name) == 0) {
                                    ast->u.index_assign.field_index = j;
                                    break;
                                }
                            }
                        } else {
                            // 从全局 cstruct 定义表查找
                            ObjCStructDef* cdef = cstruct_def_find(obj_type->struct_name);
                            if (cdef) {
                                int idx = cstruct_get_field_index(cdef, field_name);
                                if (idx >= 0) {
                                    ast->u.index_assign.field_index = idx;
                                }
                            } else {
                                // 从导入模块的符号表查找
                                for (int mi = 0; mi < s->imported_module_count; mi++) {
                                    ImportedModuleInfo* mod = &s->imported_modules[mi];
                                    if (mod->sym_table) {
                                        ModuleStructSymbol* ssym = module_symbol_table_find_struct(mod->sym_table, obj_type->struct_name);
                                        if (ssym && ssym->is_cstruct) {
                                            for (int fi = 0; fi < ssym->field_count; fi++) {
                                                if (strcmp(ssym->fields[fi].name, field_name) == 0) {
                                                    ast->u.index_assign.field_index = fi;
                                                    break;
                                                }
                                            }
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            // 检查数组索引赋值的元素类型
            else if (obj_type && obj_type->kind == TYPE_ARRAY) {
                TypeInfo* value_type = infer_expr_type(s, ast->u.index_assign.value);
                type_utils_check_array_index_assignment(obj_type, value_type, ast->line);
                if (value_type) type_free(value_type);
            }
            // 检查字典值类型和键是否存在
            else if (obj_type && obj_type->kind == TYPE_DICT) {
                // 对于变量访问，检查字典值类型
                if (ast->u.index_assign.obj->kind == AST_VAR) {
                    const char* var_name = ast->u.index_assign.obj->u.var.name;
                    SymRef ref;
                    memset(&ref, 0, sizeof(ref));
                    Symbol* sym = resolve_variable_with_upvalue(s, var_name, &ref);
                    
                    if (sym) {
                        TypeInfo* assign_type = infer_expr_type(s, ast->u.index_assign.value);
                        type_utils_check_dict_index_assignment(sym, assign_type, ast->line);
                        if (assign_type) type_free(assign_type);
                    }
                }
                // 注意：允许通过 d["key"] = value 添加新键或更新已有键
            }

            if (obj_type) type_free(obj_type);
            break;
        }

        case AST_UNARY: {
            visit(s, ast->u.unary.operand);
            // 触发类型推断并缓存到 AST 节点，供代码生成器使用
            TypeInfo* unary_type = infer_expr_type(s, ast);
            if (unary_type) {
                type_free(unary_type);
            }
            break;
        }
            
        case AST_CALL: {
            Symbol* sym = NULL;
            const char* func_name = NULL;
            
            // 处理方法调用：callee 是 INDEX (obj["method"])
            if (ast->u.call.callee && ast->u.call.callee->kind == AST_INDEX) {
                Ast* index_ast = ast->u.call.callee;
                visit(s, index_ast->u.index.obj);
                visit(s, index_ast->u.index.index);
                visit_list(s, &ast->u.call.args);
                
                // 检查是否是实例方法调用
                if (index_ast->u.index.index->kind == AST_STRING) {
                    const char* method_name = index_ast->u.index.index->u.string.value;
                    TypeInfo* receiver_type = infer_expr_type(s, index_ast->u.index.obj);
                    
                    if (receiver_type) {
                        // 检查是否是 clib 类型的方法调用
                        int is_clib_call = 0;
                        if (receiver_type->kind == TYPE_CLIB && receiver_type->struct_name) {
                            Symbol* clib_sym = scope_resolve_tree_bfs(s->root_scope, receiver_type->struct_name);
                            if (!clib_sym) {
                                clib_sym = scope_resolve(s->current, receiver_type->struct_name);
                            }
                            if (clib_sym && clib_sym->clib_func_count > 0) {
                                for (int fi = 0; fi < clib_sym->clib_func_count; fi++) {
                                    if (strcmp(clib_sym->clib_func_names[fi], method_name) == 0) {
                                        is_clib_call = 1;
                                        int expected_count = clib_sym->clib_func_param_counts[fi];
                                        int actual_count = ast->u.call.args.count;
                                        if (actual_count != expected_count) {
                                            char msg[BUFFER_MEDIUM];
                                            snprintf(msg, sizeof(msg), "clib '%s' 函数 '%s' 参数数量不匹配: 期望 %d, 实际 %d",
                                                     receiver_type->struct_name, method_name, expected_count, actual_count);
                                            error_add(ERR_SEMANTIC, ast->line, msg);
                                        }
                                        for (int pi = 0; pi < actual_count && pi < expected_count; pi++) {
                                            TypeInfo* expected_param = clib_sym->clib_func_param_types[fi][pi];
                                            Ast* arg = ast->u.call.args.items[pi];
                                            TypeInfo* arg_type = infer_expr_type(s, arg);
                                            if (expected_param && arg_type && !type_is_compatible(expected_param, arg_type)) {
                                                char msg[BUFFER_MEDIUM];
                                                format_type_error(msg, sizeof(msg),
                                                    "clib '%s3' 函数 '%s4' 参数 %d 类型不匹配: 期望 '%s1', 实际 '%s2'",
                                                    expected_param, arg_type,
                                                    receiver_type->struct_name, method_name);
                                                char final_msg[BUFFER_MEDIUM];
                                                snprintf(final_msg, sizeof(final_msg), msg, pi + 1);
                                                error_add(ERR_TYPE_MISMATCH, ast->line, final_msg);
                                            }
                                            if (arg_type) type_free(arg_type);
                                        }
                                        // 设置 cached_type 让 codegen 识别 clib 调用
                                        if (!ast->cached_type) {
                                            TypeInfo* ret_type = clib_sym->clib_func_return_types[fi];
                                            if (ret_type) {
                                                TypeKind rk = ret_type->kind;
                                                TypeKind leno_rk = c_layout_type_to_leno(rk);
                                                if (leno_rk != rk) {
                                                    ast->cached_type = type_new(leno_rk);
                                                } else {
                                                    switch (rk) {
                                                        case TYPE_PTR:
                                                            ast->cached_type = type_new(TYPE_PTR);
                                                            ast->cached_type->struct_name = strdup("Ptr"); break;
                                                        case TYPE_PTR_GENERIC:
                                                            // 保留元素类型信息，使类型安全检查生效
                                                            if (ret_type->element_type) {
                                                                ast->cached_type = type_ptr_generic(type_copy(ret_type->element_type));
                                                            } else {
                                                                ast->cached_type = type_new(TYPE_PTR);
                                                                ast->cached_type->struct_name = strdup("Ptr");
                                                            }
                                                            break;
                                                        case TYPE_BOOL:
                                                            ast->cached_type = type_new(TYPE_BOOL); break;
                                                        case TYPE_NULL:
                                                            ast->cached_type = type_new(TYPE_NULL); break;
                                                        default:
                                                            ast->cached_type = type_copy(ret_type); break;
                                                    }
                                                }
                                            }
                                        }
                                        break;
                                    }
                                }
                            } else if (clib_sym) {
                                // 导入的 clib 类型：函数表为空，允许任意调用
                                is_clib_call = 1;
                            }
                            if (!is_clib_call) {
                                char msg[BUFFER_MEDIUM];
                                snprintf(msg, sizeof(msg), "clib '%s' 没有声明函数 '%s'",
                                         receiver_type->struct_name, method_name);
                                error_add(ERR_SEMANTIC, ast->line, msg);
                            }
                            type_free(receiver_type);
                            break;
                        }
                        // 非 clib 类型：走原生方法检查
                        const char* type_name = native_get_type_name(receiver_type->kind);
                        
                        // face 类型：查找 face 定义验证方法存在性
                        if (receiver_type->kind == TYPE_FACE && receiver_type->struct_name) {
                            int face_method_found = 0;
                            ObjFaceDef* fdef = face_def_find(receiver_type->struct_name);
                            if (fdef) {
                                for (int fi = 0; fi < fdef->method_count; fi++) {
                                    if (strcmp(fdef->methods[fi].name, method_name) == 0) {
                                        face_method_found = 1;
                                        break;
                                    }
                                }
                            }
                            if (!face_method_found) {
                                // 在导入的模块符号表中查找
                                for (int mi = 0; mi < s->imported_module_count && !face_method_found; mi++) {
                                    ImportedModuleInfo* m = &s->imported_modules[mi];
                                    if (m && m->sym_table) {
                                        ModuleFaceSymbol* face_sym = module_symbol_table_find_face(m->sym_table, receiver_type->struct_name);
                                        if (face_sym) {
                                            for (int fi = 0; fi < face_sym->method_count; fi++) {
                                                if (strcmp(face_sym->methods[fi].name, method_name) == 0) {
                                                    face_method_found = 1;
                                                    break;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            if (!face_method_found) {
                                char msg[BUFFER_MEDIUM];
                                snprintf(msg, sizeof(msg), "face '%s' 没有方法 '%s'",
                                         receiver_type->struct_name, method_name);
                                error_add(ERR_SEMANTIC, ast->line, msg);
                            }
                            type_free(receiver_type);
                            break;
                        }
                        
                        if (!type_name) {
                            // any 类型不能调用方法
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg),
                                "不能在 any 类型上调用方法 '%s'\n"
                                "  提示: 使用 if ... is Type { ... } 类型收窄或指定参数类型",
                                method_name);
                            error_add(ERR_SEMANTIC, ast->line, msg);
                        }
                        if (type_name) {
                            int expected_arity;
                            TypeKind return_type = native_get_instance_method_return_type(type_name, method_name, &expected_arity);

                            // 当原生方法表找不到时，回退到 func_table 查找模块导入的 struct 方法
                            int fallback_default_count = 0;
                            if (return_type == TYPE_ANY && expected_arity == -1 && receiver_type->struct_name) {
                                char full_method_name[256];
                                snprintf(full_method_name, sizeof(full_method_name), "%s::%s", receiver_type->struct_name, method_name);
                                Ast* func_def = func_table_find(&s->func_table, full_method_name);
                                if (func_def && func_def->kind == AST_FUNC_DEF) {
                                    // 从占位符函数获取参数数量和返回类型
                                    expected_arity = func_def->u.func.pcnt - 1; // 减去 self
                                    fallback_default_count = func_def->u.func.default_count;
                                    if (func_def->u.func.return_type) {
                                        return_type = func_def->u.func.return_type->kind;
                                    }
                                } else {
                                    // 回退到模块符号表查找
                                    for (int mi = 0; mi < s->imported_module_count; mi++) {
                                        ImportedModuleInfo* minfo = &s->imported_modules[mi];
                                        if (minfo->sym_table) {
                                            ModuleStructMethod* msym = module_symbol_table_find_struct_method(
                                                minfo->sym_table, receiver_type->struct_name, method_name);
                                            if (msym) {
                                                expected_arity = msym->param_count;
                                                return_type = msym->return_type;
                                                break;
                                            }
                                        }
                                    }
                                }
                            }

                            // 当 return_type == TYPE_ANY 且 expected_arity == -1 时表示方法不存在
                            if (!(return_type == TYPE_ANY && expected_arity == -1)) {
                                // 是实例方法，检查参数
                                int actual_arity = ast->u.call.args.count;
                                // 如果第一个参数是 self（方法内部调用 self["method"](self, ...)），
                                // self 已经被 func_table 的 pcnt 计入了，需要从 actual_arity 中减去
                                if (actual_arity > 0 && ast->u.call.args.items[0] &&
                                    ast->u.call.args.items[0]->kind == AST_VAR &&
                                    ast->u.call.args.items[0]->u.var.name &&
                                    strcmp(ast->u.call.args.items[0]->u.var.name, "self") == 0) {
                                    actual_arity--;
                                }
                                const InstanceMethodMeta* meta = native_find_instance_method(type_name, method_name);

                                // arity >= 0: 固定参数数量（考虑默认参数）
                                int required_arity = expected_arity - fallback_default_count;
                                if (expected_arity >= 0 && (actual_arity < required_arity || actual_arity > expected_arity)) {
                                    char msg[BUFFER_MEDIUM];
                                    if (actual_arity < required_arity) {
                                        snprintf(msg, sizeof(msg), "调用方法 '%s' 时参数不足: 至少需要 %d, 实际 %d",
                                                 method_name, required_arity, actual_arity);
                                    } else {
                                        snprintf(msg, sizeof(msg), "调用方法 '%s' 时参数过多: 最多 %d, 实际 %d",
                                                 method_name, expected_arity, actual_arity);
                                    }
                                    if (strcmp(method_name, "map") == 0 && actual_arity == 1 && expected_arity == 2) {
                                        strncat(msg, "\n  \xf0\x9f\x92\xa1 提示: .map() 会传递 (item, index) 两个参数，"
                                                "\n     函数签名需加第二个参数，如 func compress(string w, int _): string",
                                                sizeof(msg) - strlen(msg) - 1);
                                    }
                                    error_add(ERR_SEMANTIC, ast->line, msg);
                                }
                                // arity < 0: 可变参数，检查是否少于 min_arity
                                else if (expected_arity < 0 && meta && meta->min_arity >= 0 && actual_arity < meta->min_arity) {
                                    char msg[BUFFER_MEDIUM];
                                    snprintf(msg, sizeof(msg), "调用方法 '%s' 时参数过少: 最少 %d 个, 实际 %d",
                                             method_name, meta->min_arity, actual_arity);
                                    error_add(ERR_SEMANTIC, ast->line, msg);
                                }
                                // arity < 0: 可变参数，检查是否超过 max_arity
                                else if (expected_arity < 0 && meta && meta->max_arity >= 0 && actual_arity > meta->max_arity) {
                                    char msg[BUFFER_MEDIUM];
                                    snprintf(msg, sizeof(msg), "调用方法 '%s' 时参数过多: 最多 %d 个, 实际 %d",
                                             method_name, meta->max_arity, actual_arity);
                                    error_add(ERR_SEMANTIC, ast->line, msg);
                                }

                                // 检查参数类型
                                if (expected_arity == -1 || actual_arity == expected_arity) {
                                    // 处理数组方法
                                    if (receiver_type && receiver_type->kind == TYPE_ARRAY) {
                                        // 对于数组方法，从 receiver_type 获取元素类型（如果有的话）
                                        TypeKind array_elem_kind = TYPE_ANY;
                                        if (receiver_type->element_type) {
                                            array_elem_kind = receiver_type->element_type->kind;
                                        }
                                        
                                        for (int i = 0; i < actual_arity; i++) {
                                            TypeKind expected_param_type = native_get_instance_method_param_type(type_name, method_name, i);
                                            
                                            // 如果是数组的 add/insert 方法，且 receiver 有具体元素类型，使用该类型
                                            if (type_utils_is_array_element_mutator(method_name)) {
                                                int elem_param_idx = type_utils_get_array_element_param_index(method_name, 0);
                                                if (i == elem_param_idx && array_elem_kind != TYPE_ANY) {
                                                    expected_param_type = array_elem_kind;
                                                }
                                            }
                                            
                                            TypeInfo* arg_type = infer_expr_type(s, ast->u.call.args.items[i]);
                                            
                                            // 处理数组元素修改方法（add/insert）的类型更新
                                            if (type_utils_is_array_element_mutator(method_name)) {
                                                int elem_param_idx = type_utils_get_array_element_param_index(method_name, 0);
                                                if (i == elem_param_idx) {
                                                    // 尝试更新数组元素类型
                                                    Symbol* arr_sym = type_utils_resolve_var_symbol(s, index_ast->u.index.obj);
                                                    int updated = 0;
                                                    // 先尝试普通数组更新（arr.add() 这种情况）
                                                    if (type_utils_try_update_array_element_type(arr_sym, arg_type)) {
                                                        expected_param_type = arg_type->kind;
                                                        updated = 1;
                                                    } else {
                                                        // 再尝试嵌套数组更新（arr[0].add()、arr[0][0].add() 等任意层级）
                                                        if (type_utils_try_update_nested_array_element_type_ex(arr_sym, index_ast->u.index.obj, arg_type)) {
                                                            expected_param_type = arg_type->kind;
                                                            updated = 1;
                                                        }
                                                    }
                                                    // 如果更新失败（类型已确定），从符号表重新获取元素类型
                                                    if (!updated && arr_sym && arr_sym->type && arr_sym->type->kind == TYPE_ARRAY) {
                                                        // 根据索引深度获取正确的元素类型
                                                        TypeInfo* current_type = arr_sym->type;
                                                        Ast* current_index = index_ast->u.index.obj;
                                                        while (current_index && current_index->kind == AST_INDEX) {
                                                            if (current_type->element_type) {
                                                                current_type = current_type->element_type;
                                                            }
                                                            current_index = current_index->u.index.obj;
                                                        }
                                                        if (current_type && current_type->kind == TYPE_ARRAY && current_type->element_type) {
                                                            expected_param_type = current_type->element_type->kind;
                                                        }
                                                    }
                                                }
                                            }
                                            
                                            if (expected_param_type != TYPE_ANY && arg_type && arg_type->kind != TYPE_ANY) {
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
                                                    // 允许 impl face 的 struct 传给 face 类型的参数
                                                    if (expected_param_type == TYPE_FACE && arg_type->kind == TYPE_STRUCT && arg_type->struct_name) {
                                                        // 从 receiver_type 获取数组元素类型的 face 名称
                                                        if (receiver_type && receiver_type->element_type && receiver_type->element_type->struct_name) {
                                                            ObjFaceDef* fdef = face_def_find(receiver_type->element_type->struct_name);
                                                            ObjStructDef* sdef = struct_def_find(arg_type->struct_name);
                                                            if (fdef && sdef && struct_implements_face(sdef, fdef)) {
                                                                type_compatible = true;
                                                            }
                                                        }
                                                    }

                                                    if (!type_compatible) {
                                                        char msg[BUFFER_MEDIUM];
                                                        char context[256];
                                                        snprintf(context, sizeof(context), "函数 '%s' 的参数 %d 类型不匹配",
                                                                 method_name, i + 1);
                                                        TypeInfo* expected_type = type_new(expected_param_type);
                                                        format_detailed_type_error(msg, sizeof(msg),
                                                            expected_type, arg_type, context);
                                                        type_free(expected_type);
                                                        error_add(ERR_SEMANTIC, ast->line, msg);
                                                    }
                                                }
                                            }
                                            if (arg_type) type_free(arg_type);
                                        }
                                    }
                                    // 处理字典 set 方法的值类型检查
                                    else if (receiver_type && receiver_type->kind == TYPE_DICT) {
                                        // 对于字典的 set 方法，从 receiver_type 获取值类型（如果有的话）
                                        TypeKind dict_value_kind = TYPE_ANY;
                                        TypeInfo* dict_value_type = NULL;
                                        if (receiver_type->value_type) {
                                            dict_value_kind = receiver_type->value_type->kind;
                                            dict_value_type = receiver_type->value_type;
                                        }
                                        
                                        // 获取参数数量
                                        int actual_arity = ast->u.call.args.count;
                                        
                                        for (int i = 0; i < actual_arity; i++) {
                                            TypeKind expected_param_type = native_get_instance_method_param_type(type_name, method_name, i);
                                            
                                            TypeInfo* arg_type = infer_expr_type(s, ast->u.call.args.items[i]);
                                            
                                            // 处理字典 set 方法的值类型更新和检查
                                            TypeInfo* expected_type_for_check = NULL;
                                            if (type_utils_is_dict_element_mutator(method_name)) {
                                                int elem_param_idx = type_utils_get_dict_element_param_index(method_name);
                                                if (i == elem_param_idx) {
                                                    // 获取字典变量符号
                                                    Symbol* dict_sym = type_utils_resolve_var_symbol(s, index_ast->u.index.obj);
                                                    // 首先尝试更新字典值类型（如果是第一次 set）
                                                    if (type_utils_try_update_dict_value_type(dict_sym, arg_type)) {
                                                        // 类型已更新，同时更新期望参数类型
                                                        expected_param_type = arg_type->kind;
                                                    } else if (dict_value_kind != TYPE_ANY) {
                                                        // 字典已有具体值类型，使用该类型检查
                                                        expected_param_type = dict_value_kind;
                                                        expected_type_for_check = dict_value_type;
                                                    }
                                                }
                                            }
                                            
                                            // 类型检查：简单类型比较 kind，复杂类型使用 type_is_compatible 深度比较
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
                                }
                            }
                        }
                        // 自定义 struct 方法的参数类型检查
                        else if (receiver_type->kind == TYPE_STRUCT && receiver_type->struct_name) {
                            // 在 AST 中查找 struct 定义
                            Ast* struct_def_ast = NULL;
                            for (int si = 0; si < s->root->u.block.count; si++) {
                                Ast* stmt = s->root->u.block.items[si];
                                if (stmt->kind == AST_STRUCT_DEF && strcmp(stmt->u.struct_def.name, receiver_type->struct_name) == 0) {
                                    struct_def_ast = stmt;
                                    break;
                                }
                            }
                            if (struct_def_ast) {
                                // 查找方法定义
                                for (int mi = 0; mi < struct_def_ast->u.struct_def.method_count; mi++) {
                                    Ast* method_ast = struct_def_ast->u.struct_def.methods[mi];
                                    if (method_ast && method_ast->kind == AST_FUNC_DEF &&
                                        strcmp(method_ast->u.func.name, method_name) == 0) {
                                        // 找到方法，检查参数类型
                                        int actual_arity = ast->u.call.args.count;
                                        // 方法参数从第1个开始（第0个是 self）
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
                                            error_add(ERR_SEMANTIC, ast->line, msg);
                                        } else {
                                            // 构建泛型参数替换表
                                            char** gp_names = NULL;
                                            TypeInfo** gp_types = NULL;
                                            int gp_count = 0;

                                            if (receiver_type->generic_count > 0 && receiver_type->generic_args) {
                                                gp_count = receiver_type->generic_count;
                                                gp_names = (char**)malloc(sizeof(char*) * gp_count);
                                                gp_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * gp_count);
                                                if (struct_def_ast->u.struct_def.type_params) {
                                                    for (int gi = 0; gi < gp_count; gi++) {
                                                        gp_names[gi] = struct_def_ast->u.struct_def.type_params[gi];
                                                        gp_types[gi] = type_copy(receiver_type->generic_args[gi]);
                                                    }
                                                }
                                            }

                                            for (int i = 0; i < actual_arity; i++) {
                                                TypeInfo* expected_type = method_ast->u.func.param_types[i + 1]; // +1 跳过 self
                                                if (!expected_type) continue;

                                                // 替换泛型参数
                                                TypeInfo* check_type = expected_type;
                                                if (gp_count > 0) {
                                                    for (int gi = 0; gi < gp_count; gi++) {
                                                        TypeInfo* sub = type_substitute(check_type, gp_names[gi], gp_types[gi]);
                                                        if (sub != check_type) {
                                                            check_type = sub;
                                                        }
                                                    }
                                                }

                                                TypeInfo* arg_type = infer_expr_type(s, ast->u.call.args.items[i]);
                                                if (arg_type && arg_type->kind != TYPE_ANY && check_type->kind != TYPE_ANY) {
                                                    // 泛型方法参数可能是 TYPE_STRUCT "T" 而非 TYPE_GENERIC_PARAM
                                                    int is_generic = 0;
                                                    if (check_type->kind == TYPE_STRUCT && check_type->struct_name &&
                                                        struct_def_ast && struct_def_ast->u.struct_def.type_params) {
                                                        for (int tp = 0; tp < struct_def_ast->u.struct_def.type_param_count; tp++) {
                                                            if (strcmp(check_type->struct_name, struct_def_ast->u.struct_def.type_params[tp]) == 0) {
                                                                is_generic = 1; break;
                                                            }
                                                        }
                                                    }
                                                    if (!is_generic && !type_is_compatible(check_type, arg_type)) {
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

                                            // 清理泛型参数替换表
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
                        type_free(receiver_type);
                    }
                }
                break;
            }
            
            // 处理 clib 调用：lib.func(args)
            if (ast->u.call.callee && ast->u.call.callee->kind == AST_FIELD_ACCESS) {
                Ast* field_access = ast->u.call.callee;
                visit(s, field_access->u.field_access.obj);
                visit_list(s, &ast->u.call.args);

                // 检查是否是 clib 类型的方法调用
                TypeInfo* obj_type = infer_expr_type(s, field_access->u.field_access.obj);
                if (obj_type && obj_type->kind == TYPE_CLIB && obj_type->struct_name) {
                    // 查找 clib 符号以获取函数签名
                    Symbol* clib_sym = scope_resolve(s->current, obj_type->struct_name);
                    if (clib_sym && clib_sym->clib_func_count > 0) {
                        const char* func_name = field_access->u.field_access.field_name;
                        int func_found = 0;

                        for (int i = 0; i < clib_sym->clib_func_count; i++) {
                            if (strcmp(clib_sym->clib_func_names[i], func_name) == 0) {
                                func_found = 1;
                                int expected_params = clib_sym->clib_func_param_counts[i];
                                int actual_params = ast->u.call.args.count;

                                // 检查参数数量
                                if (actual_params != expected_params) {
                                    char msg[BUFFER_MEDIUM];
                                    snprintf(msg, sizeof(msg), "clib '%s' 的函数 '%s' 参数数量不匹配: 期望 %d, 实际 %d",
                                             obj_type->struct_name, func_name, expected_params, actual_params);
                                    error_add(ERR_SEMANTIC, ast->line, msg);
                                }

                                // 设置缓存类型：C 类型映射为 Leno 类型（零摩擦）
                                // i32/u32/i64/u64/i8/u8/i16/u16 → int
                                // f32/f64 → float
                                // str8/str16 → string
                                // Ptr → Ptr
                                // bool → bool
                                // void → null
                                TypeInfo* ret_type = clib_sym->clib_func_return_types[i];
                                if (!ast->cached_type) {
                                    TypeKind rk = ret_type ? ret_type->kind : TYPE_NULL;
                                    TypeKind leno_rk = c_layout_type_to_leno(rk);
                                    if (leno_rk != rk) {
                                        ast->cached_type = type_new(leno_rk);
                                    } else {
                                        switch (rk) {
                                            case TYPE_PTR:
                                                ast->cached_type = type_new(TYPE_PTR);
                                                ast->cached_type->struct_name = strdup("Ptr");
                                                break;
                                            case TYPE_PTR_GENERIC:
                                                // 保留元素类型信息，使类型安全检查生效
                                                if (ret_type->element_type) {
                                                    ast->cached_type = type_ptr_generic(type_copy(ret_type->element_type));
                                                } else {
                                                    ast->cached_type = type_new(TYPE_PTR);
                                                    ast->cached_type->struct_name = strdup("Ptr");
                                                }
                                                break;
                                            case TYPE_BOOL:
                                                ast->cached_type = type_new(TYPE_BOOL);
                                                break;
                                            case TYPE_NULL:
                                                ast->cached_type = type_new(TYPE_NULL);
                                                break;
                                            default:
                                                ast->cached_type = type_copy(ret_type);
                                                break;
                                        }
                                    }
                                }
                                break;
                            }
                        }

                        if (!func_found) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "clib '%s' 没有声明函数 '%s'",
                                     obj_type->struct_name, func_name);
                            error_add(ERR_SEMANTIC, ast->line, msg);
                        }
                    }
                }
                // face 类型：查找 face 定义验证方法存在性
                if (obj_type && obj_type->kind == TYPE_FACE && obj_type->struct_name) {
                    const char* method_name = field_access->u.field_access.field_name;
                    int face_method_found = 0;
                    ObjFaceDef* fdef = face_def_find(obj_type->struct_name);
                    if (fdef) {
                        for (int fi = 0; fi < fdef->method_count; fi++) {
                            if (strcmp(fdef->methods[fi].name, method_name) == 0) {
                                face_method_found = 1;
                                break;
                            }
                        }
                    }
                    if (!face_method_found) {
                        for (int mi = 0; mi < s->imported_module_count && !face_method_found; mi++) {
                            ImportedModuleInfo* m = &s->imported_modules[mi];
                            if (m && m->sym_table) {
                                ModuleFaceSymbol* face_sym = module_symbol_table_find_face(m->sym_table, obj_type->struct_name);
                                if (face_sym) {
                                    for (int fi = 0; fi < face_sym->method_count; fi++) {
                                        if (strcmp(face_sym->methods[fi].name, method_name) == 0) {
                                            face_method_found = 1;
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    if (!face_method_found) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "face '%s' 没有方法 '%s'",
                                 obj_type->struct_name, method_name);
                        error_add(ERR_SEMANTIC, ast->line, msg);
                    }
                }
                if (obj_type) type_free(obj_type);
                break;
            }

            if (ast->u.call.callee && ast->u.call.callee->kind == AST_VAR) {
                func_name = ast->u.call.callee->u.var.name;
                // 使用resolve_variable_with_upvalue处理函数调用，支持闭包
                sym = resolve_variable_with_upvalue(s, func_name, &ast->u.call.callee->u.var.ref);
                if (!sym) {
                    // 函数未定义，检查是否是 struct 类型
                    Symbol* struct_sym = scope_resolve(s->current, func_name);
                    if (struct_sym && struct_sym->type && struct_sym->type->kind == TYPE_STRUCT) {
                        // 是 struct 类型，提示使用 new 关键字
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "struct '%s' 需要使用 new 关键字实例化，例如: new %s()",
                                func_name, func_name);
                        error_add(ERR_SEMANTIC, ast->line, msg);
                    } else {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "未定义的函数: %s", func_name);
                        error_add(ERR_UNDEFINED_FUNC, ast->line, msg);
                    }
                } else {
                    ast->u.call.callee->u.var.ref.type_kind = sym->type ? sym->type->kind : TYPE_ANY;

                    // 检查是否是 use 导入的函数别名，转换为模块调用
                    if (sym->kind == SYM_FUNC_ALIAS) {
                        // 将 AST_CALL 转换为 AST_MODULE_CALL
                        // funcName(args) -> module.funcName(args)
                        char* mod_name = sym->alias_module_name;
                        char* fn_name = sym->alias_func_name;
                        AstList args = ast->u.call.args;

                        // 释放原来的 callee
                        ast_free(ast->u.call.callee);

                        // 转换为 AST_MODULE_CALL
                        ast->kind = AST_MODULE_CALL;
                        ast->u.module_call.module_name = strdup(mod_name);
                        ast->u.module_call.method_name = strdup(fn_name);
                        ast->u.module_call.args = args;
                        ast->u.module_call.generic_type_args = NULL;
                        ast->u.module_call.generic_type_count = 0;
                        memset(&ast->u.module_call.lib_ref, 0, sizeof(SymRef));

                        // 重新访问转换后的 AST_MODULE_CALL
                        visit(s, ast);
                        return;
                    }

                    // 检查是否是 struct 类型调用（未使用 new 关键字）
                    if (sym->type && sym->type->kind == TYPE_STRUCT) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "struct '%s' 需要使用 new 关键字实例化，例如: new %s()",
                                func_name, func_name);
                        error_add(ERR_SEMANTIC, ast->line, msg);
                    }
                }
            } else {
                visit(s, ast->u.call.callee);
            }
            visit_list(s, &ast->u.call.args);
            
            // 检查 native 函数的参数数量
            if (sym && sym->kind == SYM_NATIVE) {
                int count;
                const NativeFunctionMeta* builtins = native_get_all_functions(&count);
                for (int i = 0; i < count; i++) {
                    if (strcmp(builtins[i].name, func_name) == 0) {
                        int actual_arity = ast->u.call.args.count;
                        // arity >= 0: 固定参数数量
                        if (builtins[i].arity >= 0 && actual_arity != builtins[i].arity) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "%s 参数数量不匹配: 期望 %d, 实际 %d",
                                     func_name, builtins[i].arity, actual_arity);
                            error_add(ERR_SEMANTIC, ast->line, msg);
                        }
                        // arity < 0: 可变参数，检查是否少于 min_arity
                        else if (builtins[i].arity < 0 && builtins[i].min_arity >= 0 && actual_arity < builtins[i].min_arity) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "%s 参数过少: 最少 %d 个, 实际 %d",
                                     func_name, builtins[i].min_arity, actual_arity);
                            error_add(ERR_SEMANTIC, ast->line, msg);
                        }
                        // arity < 0: 可变参数，检查是否超过 max_arity
                        else if (builtins[i].arity < 0 && builtins[i].max_arity >= 0 && actual_arity > builtins[i].max_arity) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "%s 参数过多: 最多 %d 个, 实际 %d",
                                     func_name, builtins[i].max_arity, actual_arity);
                            error_add(ERR_SEMANTIC, ast->line, msg);
                        }
                        else if (builtins[i].arity >= 0 && actual_arity == builtins[i].arity) {
                            // 参数数量正确，检查参数类型
                            for (int j = 0; j < actual_arity; j++) {
                                TypeKind expected_param_type = builtins[i].param_types[j];
                                // 如果参数类型不是 TYPE_ANY，则进行类型检查
                                if (expected_param_type != TYPE_ANY) {
                                    Ast* arg = ast->u.call.args.items[j];
                                    TypeInfo* arg_type = infer_expr_type(s, arg);
                                    if (arg_type && arg_type->kind != TYPE_ANY) {
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
                                                    snprintf(msg, sizeof(msg), "类型错误: 函数 '%s' 的参数 %d 期望 '%s'，但传入 '%s'\n%s",
                                                             func_name, j + 1, type_kind_to_string(expected_param_type), type_to_string(arg_type), hint);
                                                } else {
                                                    snprintf(msg, sizeof(msg), "类型错误: 函数 '%s' 的参数 %d 期望 '%s'，但传入 '%s'",
                                                             func_name, j + 1, type_kind_to_string(expected_param_type), type_to_string(arg_type));
                                                }
                                                error_add(ERR_SEMANTIC, ast->line, msg);
                                            }
                                        }
                                    }
                                    if (arg_type) type_free(arg_type);
                                }
                            }
                        }
                        break;
                    }
                }
            }
            
            // 检查用户定义函数的参数类型
            if (sym && (sym->kind == SYM_GLOBAL_FUNC || sym->kind == SYM_LOCAL)) {
                // 在函数表中查找函数定义（O(1) 哈希表查找）
                Ast* func_def = func_table_find(&s->func_table, func_name);
                
                if (func_def && func_def->kind == AST_FUNC_DEF) {
                    // 泛型类型参数推断
                    int is_generic = (func_def->u.func.type_param_count > 0);
                    char** inferred_names = NULL;
                    TypeInfo** inferred_types = NULL;
                    
                    if (is_generic) {
                        inferred_names = (char**)malloc(sizeof(char*) * func_def->u.func.type_param_count);
                        inferred_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * func_def->u.func.type_param_count);
                        
                        // 从实参中推断类型参数
                        for (int i = 0; i < func_def->u.func.type_param_count; i++) {
                            inferred_names[i] = func_def->u.func.type_params[i];
                            inferred_types[i] = NULL;
                        }
                        
                        // 如果调用时显式提供了泛型类型参数（如 identity[int](42)），优先使用
                        if (ast->u.call.generic_type_count > 0 && ast->u.call.generic_type_args) {
                            for (int i = 0; i < func_def->u.func.type_param_count && i < ast->u.call.generic_type_count; i++) {
                                inferred_types[i] = type_copy(ast->u.call.generic_type_args[i]);
                            }
                        } else {
                            // 从实参推断类型参数
                            for (int i = 0; i < ast->u.call.args.count && i < func_def->u.func.pcnt; i++) {
                                TypeInfo* param_type = func_def->u.func.param_types[i];
                                TypeInfo* arg_type = infer_expr_type(s, ast->u.call.args.items[i]);
                                if (param_type && arg_type) {
                                    infer_generic_bindings(param_type, arg_type, inferred_names, inferred_types, func_def->u.func.type_param_count);
                                }
                            }
                        }
                        
                        // 为未推断的类型参数填默认 any
                        for (int i = 0; i < func_def->u.func.type_param_count; i++) {
                            if (!inferred_types[i]) {
                                inferred_types[i] = type_new(TYPE_ANY);
                            }
                        }
                        
                        // 编译期约束校验：检查推断出的类型是否满足 T: Face 约束
                        if (func_def->u.func.type_param_constraints) {
                            for (int i = 0; i < func_def->u.func.type_param_count; i++) {
                                const char* constraint = func_def->u.func.type_param_constraints[i];
                                if (!constraint) continue;  // 无约束
                                TypeInfo* inferred = inferred_types[i];
                                if (!inferred || inferred->kind == TYPE_ANY) continue;  // 无法推断，跳过
                                
                                // 只有 struct 类型需要检查 impl
                                if (inferred->kind == TYPE_STRUCT && inferred->struct_name) {
                                    ObjStructDef* sdef = struct_def_find(inferred->struct_name);
                                    ObjFaceDef* fdef = face_def_find(constraint);
                                    if (sdef && fdef && !struct_implements_face(sdef, fdef)) {
                                        char msg[BUFFER_MEDIUM];
                                        snprintf(msg, sizeof(msg),
                                            "类型 '%s' 不满足约束 '%s': 需要实现 face '%s'",
                                            inferred->struct_name, constraint, constraint);
                                        error_add(ERR_SEMANTIC, ast->line, msg);
                                    }
                                }
                            }
                        }
                        
                        // 存储特化信息
                        ast->u.call.generic_type_names = inferred_names;
                        ast->u.call.generic_type_args = inferred_types;
                        ast->u.call.generic_type_count = func_def->u.func.type_param_count;
                    }
                    
                    int expected_cnt = func_def->u.func.pcnt;
                    int actual_cnt = ast->u.call.args.count;
                    int default_cnt = func_def->u.func.default_count;
                    int required_cnt = expected_cnt - default_cnt;  // 必选参数数量
                    
                    // 检查参数数量（支持默认参数）
                    if (actual_cnt < required_cnt || actual_cnt > expected_cnt) {
                        char msg[BUFFER_MEDIUM];
                        if (actual_cnt < required_cnt) {
                            snprintf(msg, sizeof(msg), "%s 参数数量不足: 至少需要 %d 个参数，实际传入 %d 个",
                                     func_name, required_cnt, actual_cnt);
                        } else {
                            snprintf(msg, sizeof(msg), "%s 参数数量过多: 最多接受 %d 个参数，实际传入 %d 个",
                                     func_name, expected_cnt, actual_cnt);
                        }
                        error_add(ERR_SEMANTIC, ast->line, msg);
                    } else {
                        // 检查每个提供的参数的类型
                        for (int i = 0; i < actual_cnt; i++) {
                            TypeInfo* expected_type = func_def->u.func.param_types[i];
                            if (!expected_type || expected_type->kind == TYPE_INFER) continue; // 类型推断模式，跳过检查

                            // 泛型函数：用推断的类型替换泛型参数
                            TypeInfo* check_type = expected_type;
                            if (is_generic && inferred_types) {
                                check_type = expected_type;
                                for (int p = 0; p < func_def->u.func.type_param_count; p++) {
                                    TypeInfo* sub = type_substitute(check_type, func_def->u.func.type_params[p], inferred_types[p]);
                                    if (sub != check_type) {
                                        check_type = sub;  // 使用替换后的类型
                                    }
                                }
                            }

                            TypeInfo* arg_type = infer_expr_type(s, ast->u.call.args.items[i]);
                            if (arg_type) {
                                // 使用类型兼容性检查
                                if (!type_is_compatible(check_type, arg_type)) {
                                    char msg[BUFFER_MEDIUM];
                                    char idx_str[16];
                                    snprintf(idx_str, sizeof(idx_str), "%d", i + 1);
                                    format_type_error(msg, sizeof(msg),
                                        "%s3 第 %s4 个参数类型不匹配: 期望 %s1, 实际 %s2",
                                        check_type, arg_type,
                                        func_name, idx_str);
                                    // 如果是 struct 未实现 face，追加提示
                                    if (check_type->kind == TYPE_FACE && arg_type->kind == TYPE_STRUCT
                                        && check_type->struct_name && arg_type->struct_name) {
                                        char hint[256];
                                        snprintf(hint, sizeof(hint), " (struct '%s' 未实现 face '%s'，请添加 impl: struct %s impl %s { ... })",
                                            arg_type->struct_name, check_type->struct_name,
                                            arg_type->struct_name, check_type->struct_name);
                                        strncat(msg, hint, sizeof(msg) - strlen(msg) - 1);
                                    }
                                    error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                                }
                            }
                            if (arg_type) type_free(arg_type);
                            // 清理泛型替换产生的临时类型
                            if (check_type != expected_type) type_free(check_type);
                        }
                    }
                }
            }
            
            {
                TypeInfo* t = infer_expr_type(s, ast);
                if (t) type_free(t);
            }
            break;
        }

            case AST_INDEX:
            visit(s, ast->u.index.obj);
            visit(s, ast->u.index.index);

            // 编译时检查数组索引类型
            TypeInfo* obj_type = infer_expr_type(s, ast->u.index.obj);

            // face 类型限制：obj["field"] 中 obj 是 face 类型时，只能访问 face 定义的方法
            if (obj_type && obj_type->kind == TYPE_FACE && obj_type->struct_name &&
                ast->u.index.index->kind == AST_STRING) {
                const char* field_name = ast->u.index.index->u.string.value;
                ObjFaceDef* fdef = face_def_find(obj_type->struct_name);
                if (fdef && fdef->method_count > 0) {
                    int is_face_method = 0;
                    for (int i = 0; i < fdef->method_count; i++) {
                        if (strcmp(fdef->methods[i].name, field_name) == 0) {
                            is_face_method = 1;
                            break;
                        }
                    }
                    if (!is_face_method) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "face '%s' 没有字段或方法 '%s'（使用 'as' 转型访问底层 struct）",
                                 obj_type->struct_name, field_name);
                        error_add(ERR_SEMANTIC, ast->line, msg);
                    }
                }
            }

            if (obj_type && obj_type->kind == TYPE_ARRAY) {
                TypeInfo* index_type = infer_expr_type(s, ast->u.index.index);
                if (index_type && index_type->kind != TYPE_INT && index_type->kind != TYPE_ANY) {
                    char msg[BUFFER_MEDIUM];
                    snprintf(msg, sizeof(msg), "数组索引必须是整数类型，实际为 %s",
                             type_kind_to_string(index_type->kind));
                    error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                }
                if (index_type) type_free(index_type);
            }

            // 检查嵌套字段访问中的 null 默认值（如 self["head"]["next"] 或 self["items"][0]）
            // 当 obj 是 INDEX 类型，且最内层是 self 变量时，检查中间字段的 null 默认值
            if (ast->u.index.obj->kind == AST_INDEX) {
                Ast* inner_index = ast->u.index.obj;
                // 检查是否是最内层 self["field"] 模式
                if (inner_index->u.index.obj->kind == AST_VAR &&
                    strcmp(inner_index->u.index.obj->u.var.name, "self") == 0 &&
                    inner_index->u.index.index->kind == AST_STRING) {

                    char* inner_field = inner_index->u.index.index->u.string.value;
                    // 外部索引可能是字符串（字段名）或数字（数组索引）
                    char* outer_field = NULL;
                    char outer_field_str[32];
                    if (ast->u.index.index->kind == AST_STRING) {
                        outer_field = ast->u.index.index->u.string.value;
                    } else if (ast->u.index.index->kind == AST_NUM) {
                        snprintf(outer_field_str, sizeof(outer_field_str), "%d", (int)ast->u.index.index->u.num.value);
                        outer_field = outer_field_str;
                    } else if (ast->u.index.index->kind == AST_VAR) {
                        outer_field = ast->u.index.index->u.var.name;
                    }

                    // 通过 self 变量的类型来确定 struct 定义
                    // self 的类型就是当前正在定义的 struct 类型（如 LinkedList）
                    Symbol* self_sym = scope_resolve(s->current, "self");
                    Symbol* struct_sym = NULL;

                    if (self_sym && self_sym->type && self_sym->type->kind == TYPE_STRUCT &&
                        self_sym->type->struct_name) {
                        // 查找该类型的 struct 定义
                        Symbol* type_sym = scope_resolve(s->current, self_sym->type->struct_name);
                        if (type_sym && type_sym->type && type_sym->type->kind == TYPE_STRUCT &&
                            type_sym->struct_field_count > 0) {
                            struct_sym = type_sym;
                        }
                    }

                    // 如果找不到，尝试查找当前正在定义的 struct（用于方法体内）
                    if (!struct_sym) {
                        Scope* current_scope = s->current;
                        while (current_scope) {
                            for (int j = 0; j < current_scope->sym_cnt; j++) {
                                Symbol* sym = current_scope->syms[j];
                                if (sym->type && sym->type->kind == TYPE_STRUCT &&
                                    sym->struct_field_count > 0) {
                                    // 检查是否有匹配的字段
                                    for (int k = 0; k < sym->struct_field_count; k++) {
                                        if (strcmp(sym->struct_field_names[k], inner_field) == 0) {
                                            struct_sym = sym;
                                            break;
                                        }
                                    }
                                    if (struct_sym) break;
                                }
                            }
                            if (struct_sym) break;
                            current_scope = current_scope->parent;
                        }
                    }

                    // 检查内层字段是否为 null 默认值
                    if (struct_sym && struct_sym->struct_field_null_default) {
                        for (int i = 0; i < struct_sym->struct_field_count; i++) {
                            if (strcmp(struct_sym->struct_field_names[i], inner_field) == 0) {
                                if (struct_sym->struct_field_null_default[i]) {
                                    char msg[BUFFER_MEDIUM];
                                    if (outer_field) {
                                        snprintf(msg, sizeof(msg),
                                            "字段 'self.%s' 默认值为 null，访问其字段 '%s' 可能导致运行时错误",
                                            inner_field, outer_field);
                                    } else {
                                        snprintf(msg, sizeof(msg),
                                            "字段 'self.%s' 默认值为 null，访问其成员可能导致运行时错误",
                                            inner_field);
                                    }
                                    warning_add(WARN_NULL_FIELD_CHAIN, ast->line, msg);
                                }
                                break;
                            }
                        }
                    }
                }
            }

            // 检查外部变量链式访问中的 null 默认值（如 o.inner.value 或 o.items[0]）
            // 当 obj 是 FIELD_ACCESS 类型时，检查该字段是否有 null 默认值
            if (ast->u.index.obj->kind == AST_FIELD_ACCESS) {
                Ast* field_access = ast->u.index.obj;
                char* inner_field = field_access->u.field_access.field_name;

                // 外部索引可能是字符串（字段名）或数字（数组索引）
                char* outer_field = NULL;
                char outer_field_str[32];
                if (ast->u.index.index->kind == AST_STRING) {
                    outer_field = ast->u.index.index->u.string.value;
                } else if (ast->u.index.index->kind == AST_NUM) {
                    snprintf(outer_field_str, sizeof(outer_field_str), "%d", (int)ast->u.index.index->u.num.value);
                    outer_field = outer_field_str;
                } else if (ast->u.index.index->kind == AST_VAR) {
                    outer_field = ast->u.index.index->u.var.name;
                }

                // 获取被访问对象（如 o）的类型信息，而不是字段（如 inner）的类型
                TypeInfo* obj_type_info = NULL;
                Symbol* struct_sym = NULL;
                const char* base_var_name = NULL;

                if (field_access->u.field_access.obj) {
                    obj_type_info = infer_expr_type(s, field_access->u.field_access.obj);
                }

                // 从对象类型获取 struct 定义（如 Outer）
                if (obj_type_info && obj_type_info->kind == TYPE_STRUCT && obj_type_info->struct_name) {
                    Symbol* type_sym = scope_resolve(s->current, obj_type_info->struct_name);
                    if (type_sym && type_sym->type && type_sym->type->kind == TYPE_STRUCT &&
                        type_sym->struct_field_count > 0) {
                        struct_sym = type_sym;
                    }
                }

                // 尝试获取基础变量名（如 o.inner 中的 o）
                if (field_access->u.field_access.obj && field_access->u.field_access.obj->kind == AST_VAR) {
                    base_var_name = field_access->u.field_access.obj->u.var.name;
                }

                if (obj_type_info) type_free(obj_type_info);

                // 检查内层字段（如 inner）是否为 null 默认值
                if (struct_sym && struct_sym->struct_field_null_default) {
                    for (int i = 0; i < struct_sym->struct_field_count; i++) {
                        if (strcmp(struct_sym->struct_field_names[i], inner_field) == 0) {
                            if (struct_sym->struct_field_null_default[i]) {
                                char msg[BUFFER_MEDIUM];
                                if (base_var_name && outer_field) {
                                    snprintf(msg, sizeof(msg),
                                        "字段 '%s.%s' 默认值为 null，访问其字段 '%s' 可能导致运行时错误",
                                        base_var_name, inner_field, outer_field);
                                } else if (base_var_name) {
                                    snprintf(msg, sizeof(msg),
                                        "字段 '%s.%s' 默认值为 null，访问其成员可能导致运行时错误",
                                        base_var_name, inner_field);
                                } else if (outer_field) {
                                    snprintf(msg, sizeof(msg),
                                        "字段 '%s' 默认值为 null，访问其字段 '%s' 可能导致运行时错误",
                                        inner_field, outer_field);
                                } else {
                                    snprintf(msg, sizeof(msg),
                                        "字段 '%s' 默认值为 null，访问其成员可能导致运行时错误",
                                        inner_field);
                                }
                                warning_add(WARN_NULL_FIELD_CHAIN, ast->line, msg);
                            }
                            break;
                        }
                    }
                }
            }

            if (obj_type) type_free(obj_type);
            infer_expr_type(s, ast);
            break;
            
        case AST_SLICE:
            visit(s, ast->u.slice.obj);
            if (ast->u.slice.start) visit(s, ast->u.slice.start);
            if (ast->u.slice.end) visit(s, ast->u.slice.end);
            
            // 编译时检查切片对象类型
            TypeInfo* slice_obj_type = infer_expr_type(s, ast->u.slice.obj);
            bool is_valid_slice_target = false;
            
            if (slice_obj_type) {
                if (slice_obj_type->kind == TYPE_ARRAY || slice_obj_type->kind == TYPE_ANY) {
                    is_valid_slice_target = true;
                }
                // 检查是否是 cstruct 数组字段访问
                else if (ast->u.slice.obj->kind == AST_FIELD_ACCESS) {
                    Ast* field_access = ast->u.slice.obj;
                    if (field_access->u.field_access.obj) {
                        TypeInfo* obj_type = infer_expr_type(s, field_access->u.field_access.obj);
                        if (obj_type && obj_type->kind == TYPE_CSTRUCT) {
                            // 检查字段是否是数组字段
                            const char* field_name = field_access->u.field_access.field_name;
                            const char* var_name = NULL;
                            
                            if (field_access->u.field_access.obj->kind == AST_VAR) {
                                var_name = field_access->u.field_access.obj->u.var.name;
                            }
                            
                            if (var_name) {
                                Symbol* var_sym = scope_resolve(s->current, var_name);
                                if (var_sym && var_sym->struct_field_count > 0) {
                                    for (int i = 0; i < var_sym->struct_field_count; i++) {
                                        if (strcmp(var_sym->struct_field_names[i], field_name) == 0) {
                                            // 检查符号表中是否存储了数组维度信息
                                            // 这里我们假设如果字段名匹配，可能是数组字段
                                            // 运行时会在 OP_SLICE 中再次检查
                                            is_valid_slice_target = true;
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                        if (obj_type) type_free(obj_type);
                    }
                }
                
                if (!is_valid_slice_target) {
                    char msg[BUFFER_MEDIUM];
                    snprintf(msg, sizeof(msg), "切片操作需要数组类型，实际为 %s",
                             type_kind_to_string(slice_obj_type->kind));
                    error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                }
            }
            if (slice_obj_type) type_free(slice_obj_type);
            
            // 检查起始索引类型
            if (ast->u.slice.start) {
                TypeInfo* start_type = infer_expr_type(s, ast->u.slice.start);
                if (start_type && start_type->kind != TYPE_INT && start_type->kind != TYPE_ANY) {
                    char msg[BUFFER_MEDIUM];
                    snprintf(msg, sizeof(msg), "切片起始索引必须是整数类型，实际为 %s",
                             type_kind_to_string(start_type->kind));
                    error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                }
                if (start_type) type_free(start_type);
            }
            
            // 检查结束索引类型
            if (ast->u.slice.end) {
                TypeInfo* end_type = infer_expr_type(s, ast->u.slice.end);
                if (end_type && end_type->kind != TYPE_INT && end_type->kind != TYPE_ANY) {
                    char msg[BUFFER_MEDIUM];
                    snprintf(msg, sizeof(msg), "切片结束索引必须是整数类型，实际为 %s",
                             type_kind_to_string(end_type->kind));
                    error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                }
                if (end_type) type_free(end_type);
            }
            break;