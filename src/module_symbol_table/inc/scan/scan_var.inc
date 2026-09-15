            // 检查是否是 var/const 定义
            {
                int is_const_decl = 0;
                char* after_var = NULL;
                if (strncmp(after_export, "const", 5) == 0 && (after_export[5] == ' ' || after_export[5] == '\t' || after_export[5] == '\n' || after_export[5] == '\r' || after_export[5] == '\0')) {
                    is_const_decl = 1;
                    after_var = after_export + 5;
                } else if (strncmp(after_export, "var", 3) == 0 && (after_export[3] == ' ' || after_export[3] == '\t' || after_export[3] == '\n' || after_export[3] == '\r' || after_export[3] == '\0')) {
                    after_var = after_export + 3;
                } else {
                    // 检查是否是"类型在前"无关键字声明: export Array[int] arr = value, export int x = 42
                    // 排除其他 export 关键字 (func, struct, cstruct, enum, alias, face, clib, packed, align)
                    int is_other_kw = 0;
                    if (strncmp(after_export, "func", 4) == 0 && (after_export[4] == ' ' || after_export[4] == '\t' || after_export[4] == '\n' || after_export[4] == '\0')) is_other_kw = 1;
                    else if (strncmp(after_export, "struct", 6) == 0 && (after_export[6] == ' ' || after_export[6] == '\t' || after_export[6] == '\n' || after_export[6] == '\0')) is_other_kw = 1;
                    else if (strncmp(after_export, "cstruct", 7) == 0 && (after_export[7] == ' ' || after_export[7] == '\t' || after_export[7] == '\n' || after_export[7] == '\0')) is_other_kw = 1;
                    else if (strncmp(after_export, "enum", 4) == 0 && (after_export[4] == ' ' || after_export[4] == '\t' || after_export[4] == '\n' || after_export[4] == '\0')) is_other_kw = 1;
                    else if (strncmp(after_export, "alias", 5) == 0 && (after_export[5] == ' ' || after_export[5] == '\t' || after_export[5] == '\n' || after_export[5] == '\0')) is_other_kw = 1;
                    else if (strncmp(after_export, "face", 4) == 0 && (after_export[4] == ' ' || after_export[4] == '\t' || after_export[4] == '\n' || after_export[4] == '\0')) is_other_kw = 1;
                    else if (strncmp(after_export, "clib", 4) == 0 && (after_export[4] == ' ' || after_export[4] == '\t' || after_export[4] == '\n' || after_export[4] == '\0')) is_other_kw = 1;
                    else if (strncmp(after_export, "packed", 6) == 0 && (after_export[6] == ' ' || after_export[6] == '\t' || after_export[6] == '\n' || after_export[6] == '\0')) is_other_kw = 1;
                    else if (strncmp(after_export, "align", 5) == 0 && (after_export[5] == ' ' || after_export[5] == '\t' || after_export[5] == '\n' || after_export[5] == '\0')) is_other_kw = 1;
                    // 如果不是其他 export 关键字，且以标识符开头，视为"类型在前"声明
                    if (!is_other_kw && (isalpha((unsigned char)*after_export) || *after_export == '_')) {
                        after_var = after_export;  // 无 var/const 关键字需跳过
                    }
                }
                if (after_var) {
                    while (*after_var && (*after_var == ' ' || *after_var == '\t')) after_var++;

                    // 检测解构声明: var[T1, T2](a, b) = ... 或 var{"k": T}(a) = ...
                    if (*after_var == '[' || *after_var == '{') {
                        // 跳过形状部分 [...] 或 {...}
                        // 使用通用括号匹配（支持嵌套泛型如 Array[Dict[string, int]]）
                        int depth = 1;
                        after_var++;
                        while (*after_var && depth > 0) {
                            if (*after_var == '[' || *after_var == '{') depth++;
                            else if (*after_var == ']' || *after_var == '}') depth--;
                            if (depth == 0) { after_var++; break; }
                            after_var++;
                        }
                        // 跳过空格
                        while (*after_var && (*after_var == ' ' || *after_var == '\t')) after_var++;
                        // 期望 '('
                        if (*after_var == '(') {
                            after_var++;
                            // 解析变量名列表
                            while (*after_var) {
                                const char* var_iter_start = after_var;   // 进度守卫基准
                                // 跳过空白与逗号。**含换行**：解构列表换行写是合法的；
                                // 只跳空格/制表符时指针会停在 '\n' 上，读名字读到 0 字符、
                                // 本圈一步不推进 ⇒ 死循环（与 parser_module.c 的跨行 use 同族）。
                                while (*after_var && (*after_var == ' ' || *after_var == '\t' ||
                                                      *after_var == '\n' || *after_var == '\r' ||
                                                      *after_var == ',')) after_var++;
                                if (*after_var == ')') { after_var++; break; }
                                const char* name_start = after_var;
                                while (*after_var && (isalnum((unsigned char)*after_var) || *after_var == '_')) after_var++;
                                int name_len = (int)(after_var - name_start);
                                if (name_len > 0 && name_len < 64) {
                                    char var_name[64];
                                    strncpy(var_name, name_start, name_len);
                                    var_name[name_len] = '\0';
                                    module_symbol_table_add_var(table, var_name, TYPE_ANY, NULL, is_const_decl, NULL);
                                }
                                // 本圈没推进就退出；畸形列表交由真正的解析器报错
                                if (after_var == var_iter_start) break;
                            }
                        }
                        p = after_var;
                        continue;
                    }

                    // 先读第一个标识符
                    const char* first_ident_start = after_var;
                    while (*after_var && (isalnum((unsigned char)*after_var) || *after_var == '_')) after_var++;
                    int first_ident_len = (int)(after_var - first_ident_start);
                    char first_ident[64] = {0};
                    if (first_ident_len > 0 && first_ident_len < 64) {
                        strncpy(first_ident, first_ident_start, first_ident_len);
                        first_ident[first_ident_len] = '\0';
                    }

                    // 检查第一个标识符后面是否是 [（泛型后缀）或另一个标识符
                    // 情况1: const Array[int] b = ...  → 第一个标识符是类型，后面是 [，需要跳过泛型参数再读变量名
                    // 情况2: const MyStruct b = ...    → 第一个标识符是类型，后面空格后是变量名
                    // 情况3: const b = ...             → 第一个标识符是变量名，后面是 = 或 ,
                    char* skip_to = after_var;
                    while (*skip_to && (*skip_to == ' ' || *skip_to == '\t')) skip_to++;

                    int is_type_first = 0;  // 是否是"类型在前"语法
                    const char* type_str_start = NULL;
                    int type_str_len = 0;
                    char full_type_str[256] = {0};

                    if (*skip_to == '[') {
                        // 情况1: const Array[int] b = ... → 第一个标识符是泛型类型名
                        is_type_first = 1;
                        type_str_start = first_ident_start;
                        // 跳过泛型参数 [T] 或 [K,V] 等，用括号深度
                        skip_to++; // skip '['
                        int bracket_depth = 1;
                        while (*skip_to && bracket_depth > 0) {
                            if (*skip_to == '[') bracket_depth++;
                            else if (*skip_to == ']') { bracket_depth--; if (bracket_depth == 0) { skip_to++; break; } }
                            skip_to++;
                        }
                        int ft_len = (int)(skip_to - type_str_start);
                        if (ft_len > 0 && ft_len < 256) {
                            strncpy(full_type_str, type_str_start, ft_len);
                            full_type_str[ft_len] = '\0';
                        }
                        after_var = skip_to;
                    } else if (*skip_to && (isalnum((unsigned char)*skip_to) || *skip_to == '_') && first_ident_len > 0) {
                        // 情况2: const MyStruct b = ... → 第一个标识符是类型名，第二个是变量名
                        // 但要排除 const true / const false / const null 等关键字
                        // 只有当第一个标识符不是 var/true/false/null 时才认为是类型
                        if (strcmp(first_ident, "var") != 0 && strcmp(first_ident, "true") != 0 &&
                            strcmp(first_ident, "false") != 0 && strcmp(first_ident, "null") != 0) {
                            is_type_first = 1;
                            type_str_start = first_ident_start;
                            type_str_len = first_ident_len;
                            if (type_str_len > 0 && type_str_len < 256) {
                                strncpy(full_type_str, type_str_start, type_str_len);
                                full_type_str[type_str_len] = '\0';
                            }
                            after_var = skip_to;
                        }
                    }

                    // 根据语法模式确定变量名和类型
                    const char* name_start;
                    int name_len;
                    TypeKind var_type = TYPE_ANY;
                    char var_struct_name[64] = {0};
                    TypeInfo* var_type_info = NULL;  // 完整类型信息

                    if (is_type_first) {
                        // "类型在前"语法: const Type Name = value 或 const Array[int] Name = value
                        // 此时 after_var 已跳过类型部分，指向变量名前的空格
                        while (*after_var && (*after_var == ' ' || *after_var == '\t')) after_var++;
                        name_start = after_var;
                        while (*after_var && (isalnum((unsigned char)*after_var) || *after_var == '_')) after_var++;
                        name_len = (int)(after_var - name_start);

                        // 解析类型
                        if (full_type_str[0]) {
                            // 先检查本地别名表
                            TypeInfo* alias_ti = FIND_LOCAL_ALIAS(full_type_str);
                            if (alias_ti) {
                                var_type_info = type_copy(alias_ti);
                            } else {
                                var_type_info = parse_type_from_string(full_type_str);
                            }
                            if (var_type_info) {
                                var_type = var_type_info->kind;
                                if (var_type_info->struct_name) {
                                    size_t copy_len = strlen(var_type_info->struct_name);
                                    if (copy_len > sizeof(var_struct_name) - 1) copy_len = sizeof(var_struct_name) - 1;
                                    memcpy(var_struct_name, var_type_info->struct_name, copy_len);
                                    var_struct_name[copy_len] = '\0';
                                }
                            }
                        }
                    } else {
                        // "变量名在前"语法: const Name = value
                        name_start = first_ident_start;
                        name_len = first_ident_len;

                        // 从初始化值推断类型
                        // 跳过 = 号
                        while (*after_var && (*after_var == ' ' || *after_var == '	')) after_var++;
                        if (*after_var == '=') {
                            after_var++;
                            while (*after_var && (*after_var == ' ' || *after_var == '	')) after_var++;


                            // 检查是否是数字字面量
                            if (*after_var == '-' || (*after_var >= '0' && *after_var <= '9')) {
                                // 检查是否是浮点数
                                char* num_end = after_var;
                                int is_float = 0;
                                if (*num_end == '-') num_end++;
                                while (*num_end && (*num_end >= '0' && *num_end <= '9')) num_end++;
                                if (*num_end == '.') {
                                    is_float = 1;
                                    num_end++;
                                    while (*num_end && (*num_end >= '0' && *num_end <= '9')) num_end++;
                                }
                                if (is_float) {
                                    var_type = TYPE_FLOAT;
                                } else {
                                    var_type = TYPE_INT;
                                }
                            } else if (*after_var == '"' || *after_var == '\'') {
                                // 字符串字面量（双引号或单引号）
                                var_type = TYPE_STRING;
                            } else if (strncmp(after_var, "true", 4) == 0 || strncmp(after_var, "false", 5) == 0) {
                                // 布尔字面量
                                var_type = TYPE_BOOL;
                            } else if (strncmp(after_var, "null", 4) == 0) {
                                // null
                                var_type = TYPE_NULL;
                            } else if (*after_var == '[') {
                                // ★ 数组字面量 [1, 2, 3]：推断为 TYPE_ARRAY，并推断元素类型
                                var_type = TYPE_ARRAY;
                                var_type_info = type_new(TYPE_ARRAY);
                                TypeInfo* elem_type = infer_array_literal_element_type(after_var, table);
                                if (elem_type) {
                                    var_type_info->element_type = elem_type;
                                }
                            } else if (*after_var == '{') {
                                // ★ 字典字面量 {"k": v}：推断为 TYPE_DICT，并推断值类型
                                var_type = TYPE_DICT;
                                var_type_info = type_new(TYPE_DICT);
                                TypeInfo* val_type = infer_dict_literal_value_type(after_var, table);
                                if (val_type) {
                                    var_type_info->value_type = val_type;
                                    var_type_info->key_type = type_new(TYPE_STRING);
                                }
                            } else {
                                // 尝试解析跨模块引用: module.member
                                // 格式: export const X = core.YYY
                                const char* mod_start = after_var;
                                const char* mod_end = after_var;
                                while (*mod_end && (isalnum((unsigned char)*mod_end) || *mod_end == '_')) mod_end++;
                                int mod_len = (int)(mod_end - mod_start);
                                
                                if (mod_len > 0 && mod_len < 64 && *mod_end == '.') {
                                    char mod_alias[64];
                                    memcpy(mod_alias, mod_start, mod_len);
                                    mod_alias[mod_len] = '\0';
                                    
                                    const char* member_start = mod_end + 1;
                                    const char* member_end = member_start;
                                    while (*member_end && (isalnum((unsigned char)*member_end) || *member_end == '_')) member_end++;
                                    int member_len = (int)(member_end - member_start);
                                    
                                    if (member_len > 0 && member_len < 64) {
                                        char member_name[64];
                                        memcpy(member_name, member_start, member_len);
                                        member_name[member_len] = '\0';
                                        
                                        // 在源码中反向查找 import "path" as mod_alias 来解析模块路径
                                        char* import_search = source;
                                        char resolved_path[1024] = {0};
                                        while (import_search < after_var) {
                                            if (strncmp(import_search, "import", 6) == 0 && (import_search[6] == ' ' || import_search[6] == '\t' || import_search[6] == '\"')) {
                                                import_search += 6;
                                                while (*import_search && (*import_search == ' ' || *import_search == '\t')) import_search++;
                                                if (*import_search == '"') {
                                                    import_search++;
                                                    const char* path_start = import_search;
                                                    while (*import_search && *import_search != '"') import_search++;
                                                    int path_len = (int)(import_search - path_start);
                                                    import_search++; // skip closing "
                                                    
                                                    while (*import_search && (*import_search == ' ' || *import_search == '\t')) import_search++;
                                                    if (strncmp(import_search, "as", 2) == 0 && (import_search[2] == ' ' || import_search[2] == '\t')) {
                                                        import_search += 2;
                                                        while (*import_search && (*import_search == ' ' || *import_search == '\t')) import_search++;
                                                        const char* as_start = import_search;
                                                        while (*import_search && (isalnum((unsigned char)*import_search) || *import_search == '_')) import_search++;
                                                        int as_len = (int)(import_search - as_start);
                                                        if (as_len == mod_len && strncmp(as_start, mod_alias, mod_len) == 0) {
                                                            if (path_len > 0 && path_len < 1023) {
                                                                memcpy(resolved_path, path_start, path_len);
                                                                resolved_path[path_len] = '\0';
                                                            }
                                                            break;
                                                        }
                                                    }
                                                }
                                            }
                                            if (*import_search) import_search++;
                                        }
                                        
                                        // 扫描依赖模块，查找变量类型
                                        if (resolved_path[0]) {
                                            // 进程内记忆化：同一依赖路径只扫描一次（关缓存时同样成立 ——
                                            // 否则扫描次数 = 依赖图路径数，40+ 模块互相 import 时表现为"卡死"）
                                            ModuleSymbolTable* dep_table = module_symbol_table_get_shared(resolved_path, normalized_current);
                                            if (dep_table) {
                                                if (module_symbol_table_scan(dep_table, normalized_current) == 0) {
                                                    // 记录依赖路径（含传递依赖，用于 .lenosymc 缓存失效判定）
                                                    {
                                                        char dep_full_path[MAX_PATH_LEN] = {0};
                                                        resolve_module_full_path(dep_full_path, MAX_PATH_LEN, resolved_path, normalized_current);
                                                        module_symbol_table_add_dep(table, dep_full_path);
                                                        for (int di = 0; di < dep_table->dep_count; di++) {
                                                            module_symbol_table_add_dep(table, dep_table->dep_paths[di]);
                                                        }
                                                    }
                                                    // 查找变量
                                                    ModuleVarSymbol* dep_var = module_symbol_table_find_var(dep_table, member_name);
                                                    if (dep_var) {
                                                        var_type = dep_var->type;
                                                        if ((dep_var->type == TYPE_STRUCT || dep_var->type == TYPE_CSTRUCT) && dep_var->struct_name) {
                                                            size_t copy_len = strlen(dep_var->struct_name);
                                                            if (copy_len > sizeof(var_struct_name) - 1) copy_len = sizeof(var_struct_name) - 1;
                                                            memcpy(var_struct_name, dep_var->struct_name, copy_len);
                                                            var_struct_name[copy_len] = '\0';
                                                        }
                                                        // ★ 复制完整类型信息（支持 Array[int] 等泛型）
                                                        if (dep_var->type_info) {
                                                            if (var_type_info) type_free(var_type_info);
                                                            var_type_info = type_copy(dep_var->type_info);
                                                        }
                                                    }
                                                    // 查找 enum 成员（enum 成员也可被引用为常量）
                                                    if (var_type == TYPE_ANY) {
                                                        for (int ei = 0; ei < dep_table->enum_count; ei++) {
                                                            ModuleEnumSymbol* es = &dep_table->enums[ei];
                                                            for (int mi = 0; mi < es->member_count; mi++) {
                                                                if (strcmp(es->member_names[mi], member_name) == 0) {
                                                                    var_type = TYPE_INT;
                                                                    break;
                                                                }
                                                            }
                                                            if (var_type != TYPE_ANY) break;
                                                        }
                                                    }
                                                }
                                                module_symbol_table_destroy(dep_table);
                                            }
                                        }
                                    }
                                }
                                // 其他情况：尝试本地函数调用或变量引用
                                if (var_type == TYPE_ANY) {
                                    // 不是 module.member 模式，检查是否是本地函数调用或变量引用
                                    // 读取标识符
                                    const char* id_start = after_var;
                                    const char* id_end = after_var;
                                    while (*id_end && (isalnum((unsigned char)*id_end) || *id_end == '_')) id_end++;
                                    int id_len = (int)(id_end - id_start);
                                    
                                    if (id_len > 0 && id_len < 64) {
                                        char id_name[64];
                                        memcpy(id_name, id_start, id_len);
                                        id_name[id_len] = '\0';
                                        
                                        // skip whitespace after identifier
                                        const char* after_id = id_end;
                                        while (*after_id && (*after_id == ' ' || *after_id == '\t')) after_id++;
                                        
                                        if (*after_id == '(') {
                                            // function call: name(...)
                                            ModuleFuncSymbol* func = module_symbol_table_find_func(table, id_name);
                                            if (func) {
                                                var_type = func->return_type;
                                                if ((func->return_type == TYPE_STRUCT || func->return_type == TYPE_FACE) && func->return_struct_name) {
                                                    size_t copy_len = strlen(func->return_struct_name);
                                                    if (copy_len > sizeof(var_struct_name) - 1) copy_len = sizeof(var_struct_name) - 1;
                                                    memcpy(var_struct_name, func->return_struct_name, copy_len);
                                                    var_struct_name[copy_len] = '\0';
                                                }
                                                if (func->return_type_info) {
                                                    if (var_type_info) type_free(var_type_info);
                                                    var_type_info = type_copy(func->return_type_info);
                                                }
                                            }
                                        } else {
                                            // variable reference: name
                                            ModuleVarSymbol* var_sym = module_symbol_table_find_var(table, id_name);
                                            if (var_sym) {
                                                var_type = var_sym->type;
                                                if ((var_sym->type == TYPE_STRUCT || var_sym->type == TYPE_CSTRUCT) && var_sym->struct_name) {
                                                    size_t copy_len = strlen(var_sym->struct_name);
                                                    if (copy_len > sizeof(var_struct_name) - 1) copy_len = sizeof(var_struct_name) - 1;
                                                    memcpy(var_struct_name, var_sym->struct_name, copy_len);
                                                    var_struct_name[copy_len] = '\0';
                                                }
                                                if (var_sym->type_info) {
                                                    if (var_type_info) type_free(var_type_info);
                                                    var_type_info = type_copy(var_sym->type_info);
                                                }
                                            }
                                        }
                                    }
                                }
                                // 其他情况保持 TYPE_ANY
                            }
                        }

                    }

                    // 构造 var_name（两种语法共用）
                    char var_name[64] = {0};
                    if (name_len > 0 && name_len < 64) {
                        strncpy(var_name, name_start, name_len);
                        var_name[name_len] = '\0';
                    }

                    module_symbol_table_add_var(table, var_name, var_type,
                        var_struct_name[0] ? var_struct_name : NULL, is_const_decl, var_type_info);
                    // 释放临时的 var_type_info（所有权已通过 type_copy 转移到符号表）
                    if (var_type_info) {
                        type_free(var_type_info);
                        var_type_info = NULL;
                    }

                    p = after_var;
                    continue;
                }
            }
