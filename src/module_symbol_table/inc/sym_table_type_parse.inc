// ============================================================================
// 类型字符串解析器 — 支持 Array[T]、Dict[K,V] 等复杂类型
// ============================================================================

// 前向声明（递归用）
static TypeInfo* parse_type_from_string_inner(const char** p);

// 跳过空白
static void skip_ws(const char** p) {
    while (**p && (**p == ' ' || **p == '\t')) (*p)++;
}

// 读一个标识符/类型名
static int read_ident(const char* text, int max_len, const char** end) {
    const char* s = text;
    while (*s && (isalnum((unsigned char)*s) || *s == '_')) s++;
    int len = (int)(s - text);
    if (end) *end = s;
    return len < max_len ? len : max_len - 1;
}

// 解析一个基本类型名（int/float/string/bool 等内置类型 或 自定义 struct 名）
static TypeInfo* parse_simple_type_str(const char* s, int len) {
    char buf[64];
    int n = len < 63 ? len : 63;
    memcpy(buf, s, n);
    buf[n] = '\0';
    
    // 基础类型
    if (strcmp(buf, "int") == 0)    return type_new(TYPE_INT);
    if (strcmp(buf, "float") == 0)  return type_new(TYPE_FLOAT);
    if (strcmp(buf, "string") == 0) return type_new(TYPE_STRING);
    if (strcmp(buf, "bool") == 0)   return type_new(TYPE_BOOL);
    if (strcmp(buf, "var") == 0)    return type_new(TYPE_INFER);
    if (strcmp(buf, "any") == 0)    return type_new(TYPE_ANY);
    if (strcmp(buf, "null") == 0)   return type_new(TYPE_NULL);
    if (strcmp(buf, "void") == 0)   return type_new(TYPE_NULL);  // void 映射为 TYPE_NULL
    // 文件/网络类型
    if (strcmp(buf, "File") == 0)   return type_new(TYPE_FILE);
    if (strcmp(buf, "Socket") == 0) return type_new(TYPE_SOCKET);
    if (strcmp(buf, "Channel") == 0) return type_new(TYPE_CHANNEL);
    if (strcmp(buf, "Thread") == 0) return type_new(TYPE_THREAD);
    // 指针类型
    if (strcmp(buf, "ptr") == 0 || strcmp(buf, "Ptr") == 0) return type_new(TYPE_PTR);
    // C 布局类型
    if (strcmp(buf, "i8") == 0)     return type_new(TYPE_I8);
    if (strcmp(buf, "u8") == 0)     return type_new(TYPE_U8);
    if (strcmp(buf, "i16") == 0)    return type_new(TYPE_I16);
    if (strcmp(buf, "u16") == 0)    return type_new(TYPE_U16);
    if (strcmp(buf, "i32") == 0)    return type_new(TYPE_I32);
    if (strcmp(buf, "u32") == 0)    return type_new(TYPE_U32);
    if (strcmp(buf, "i64") == 0)    return type_new(TYPE_I64);
    if (strcmp(buf, "u64") == 0)    return type_new(TYPE_U64);
    if (strcmp(buf, "f32") == 0)    return type_new(TYPE_F32);
    if (strcmp(buf, "f64") == 0)    return type_new(TYPE_F64);
    if (strcmp(buf, "c_int") == 0)  return type_new(TYPE_C_INT);
    if (strcmp(buf, "c_uint") == 0) return type_new(TYPE_C_UINT);
    if (strcmp(buf, "c_long") == 0) return type_new(TYPE_C_LONG);
    if (strcmp(buf, "c_ulong") == 0) return type_new(TYPE_C_ULONG);
    if (strcmp(buf, "c_longlong") == 0)  return type_new(TYPE_C_LONGLONG);
    if (strcmp(buf, "c_ulonglong") == 0) return type_new(TYPE_C_ULONGLONG);
    if (strcmp(buf, "c_size") == 0) return type_new(TYPE_C_SIZE);
    if (strcmp(buf, "c_ssize") == 0) return type_new(TYPE_C_SSIZE);
    if (strcmp(buf, "str8") == 0)   return type_new(TYPE_STR8);
    if (strcmp(buf, "str16") == 0)  return type_new(TYPE_STR16);
    // 容器/集合类型
    if (strcmp(buf, "Array") == 0)  return type_new(TYPE_ARRAY);
    if (strcmp(buf, "Dict") == 0)   return type_new(TYPE_DICT);
    // 自定义类型名（struct 等）→ TYPE_STRUCT
    TypeInfo* ti = type_new(TYPE_STRUCT);
    ti->struct_name = strdup(buf);
    return ti;
}

// 递归解析类型字符串
static TypeInfo* parse_type_from_string_inner(const char** p) {
    skip_ws(p);
    if (!**p) return NULL;

    // 检测多返回值类型: [T1, T2, ...]
    if (**p == '[') {
        (*p)++;  // 跳过 '['
        skip_ws(p);

        TypeInfo* mr = type_new(TYPE_MULTI_RET);
        TypeInfo** params = NULL;
        int count = 0;
        int capacity = 4;

        if (**p != ']') {
            params = (TypeInfo**)malloc(sizeof(TypeInfo*) * capacity);
            while (1) {
                if (count >= capacity) {
                    capacity *= 2;
                    params = (TypeInfo**)realloc(params, sizeof(TypeInfo*) * capacity);
                }
                TypeInfo* pt = parse_type_from_string_inner(p);
                if (!pt) break;
                params[count++] = pt;
                skip_ws(p);
                if (**p == ',') {
                    (*p)++;
                    skip_ws(p);
                } else {
                    break;
                }
            }
        }
        skip_ws(p);
        if (**p == ']') (*p)++;  // 跳过 ']'
        mr->param_types = params;
        mr->param_count = count;
        return mr;
    }

    // 读标识符
    const char* name_start = *p;
    const char* name_end;
    int name_len = read_ident(name_start, 64, &name_end);
    if (name_len == 0) return NULL;
    
    // 检查是否是 func 类型（函数类型：func(params):return_type）
    char ident_buf[65];
    int copy_len = name_len < 64 ? name_len : 64;
    memcpy(ident_buf, name_start, copy_len);
    ident_buf[copy_len] = '\0';
    
    if (strcmp(ident_buf, "func") == 0) {
        *p = name_end;
        skip_ws(p);
        
        TypeInfo** param_types = NULL;
        int param_count = 0;
        int param_capacity = 4;
        TypeInfo* return_type = NULL;
        
        // 解析参数列表 (ParamType1, ParamType2, ...)
        if (**p == '(') {
            (*p)++; // 跳过 '('
            skip_ws(p);
            
            if (**p != ')') {
                // 有参数
                param_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * param_capacity);
                while (1) {
                    if (param_count >= param_capacity) {
                        param_capacity *= 2;
                        param_types = (TypeInfo**)realloc(param_types, sizeof(TypeInfo*) * param_capacity);
                    }
                    TypeInfo* pt = parse_type_from_string_inner(p);
                    if (!pt) break;
                    param_types[param_count++] = pt;
                    skip_ws(p);
                    if (**p == ',') {
                        (*p)++;
                        skip_ws(p);
                    } else {
                        break;
                    }
                }
            }
            
            skip_ws(p);
            if (**p == ')') (*p)++; // 跳过 ')'
            skip_ws(p);
        }
        
        // 解析返回类型 :ReturnType
        if (**p == ':') {
            (*p)++; // 跳过 ':'
            skip_ws(p);
            return_type = parse_type_from_string_inner(p);
            // void 返回类型转为 TYPE_NULL
            if (return_type && return_type->kind == TYPE_STRUCT && return_type->struct_name &&
                strcmp(return_type->struct_name, "void") == 0) {
                type_free(return_type);
                return_type = type_new(TYPE_NULL);
            }
        }
        
        // 构建函数类型
        TypeInfo* func_type = type_new(TYPE_FUNCTION);
        func_type->return_type = return_type;
        func_type->param_types = param_types;
        func_type->param_count = param_count;
        return func_type;
    }
    
    *p = name_end;
    skip_ws(p);
    
    // 检查泛型后缀 [T] 或 [K,V]
    if (**p == '[') {
        // 保存标识符用于判断是否是 Array/Dict
        char ident[MOD_MAX_NAMES + 1];
        int copy_len = name_len < 64 ? name_len : 64;
        memcpy(ident, name_start, copy_len);
        ident[copy_len] = '\0';

        (*p)++; // skip '['
        skip_ws(p);
        
        // 解析第一个参数类型
        TypeInfo* param1 = parse_type_from_string_inner(p);
        if (!param1) return NULL;
        
        skip_ws(p);
        
        // 检查是否是 Dict[K, V] (有逗号)
        if (**p == ',') {
            (*p)++; // skip ','
            skip_ws(p);
            
            TypeInfo* param2 = parse_type_from_string_inner(p);
            if (!param2) { type_free(param1); return NULL; }
            
            skip_ws(p);
            if (**p != ']') { type_free(param1); type_free(param2); return NULL; }
            (*p)++; // skip ']'
            
            // Dict[K, V]
            TypeInfo* dict = type_new(TYPE_DICT);
            dict->key_type = param1;   // 键类型
            dict->value_type = param2; // 值类型
            return dict;
        }
        
        // Array[T] — 只有单个参数
        if (**p != ']') { type_free(param1); return NULL; }
        (*p)++; // skip ']'
        
        // 判断标识符类型：Array → TYPE_ARRAY，Ptr → TYPE_PTR_GENERIC，其他 → 泛型 struct（如 Holder[K]）
        if (strcmp(ident, "Array") == 0) {
            TypeInfo* arr = type_new(TYPE_ARRAY);
            arr->element_type = param1;
            return arr;
        }

        if (strcmp(ident, "Ptr") == 0) {
            return type_ptr_generic(param1);
        }

        // 泛型 struct 类型：Holder[K] → TYPE_STRUCT, struct_name="Holder", element_type=K
        TypeInfo* generic_struct = type_new(TYPE_STRUCT);
        generic_struct->struct_name = strdup(ident);
        generic_struct->element_type = param1;
        return generic_struct;
    }
    
    // 简单类型
    return parse_simple_type_str(name_start, name_len);
}

// 公开接口：从字符串解析完整类型
TypeInfo* parse_type_from_string(const char* type_str) {
    if (!type_str) return NULL;
    const char* p = type_str;
    return parse_type_from_string_inner(&p);
}

// 基本类型解析 - 识别所有内置类型（用于 struct 字段/函数参数扫描）
static TypeKind parse_base_type(const char* type_str) {
    // 基础类型
    if (strcmp(type_str, "int") == 0)    return TYPE_INT;
    if (strcmp(type_str, "float") == 0)  return TYPE_FLOAT;
    if (strcmp(type_str, "string") == 0) return TYPE_STRING;
    if (strcmp(type_str, "bool") == 0)   return TYPE_BOOL;
    if (strcmp(type_str, "any") == 0)    return TYPE_ANY;
    if (strcmp(type_str, "null") == 0)   return TYPE_NULL;
    // 文件/颜色类型
    if (strcmp(type_str, "File") == 0)   return TYPE_FILE;
    // 网络/指针类型
    if (strcmp(type_str, "Socket") == 0) return TYPE_SOCKET;
    if (strcmp(type_str, "Channel") == 0) return TYPE_CHANNEL;
    if (strcmp(type_str, "Thread") == 0) return TYPE_THREAD;
    if (strcmp(type_str, "Ptr") == 0)    return TYPE_PTR;
    // C 布局类型
    if (strcmp(type_str, "i8") == 0)     return TYPE_I8;
    if (strcmp(type_str, "u8") == 0)     return TYPE_U8;
    if (strcmp(type_str, "i16") == 0)    return TYPE_I16;
    if (strcmp(type_str, "u16") == 0)    return TYPE_U16;
    if (strcmp(type_str, "i32") == 0)    return TYPE_I32;
    if (strcmp(type_str, "u32") == 0)    return TYPE_U32;
    if (strcmp(type_str, "i64") == 0)    return TYPE_I64;
    if (strcmp(type_str, "u64") == 0)    return TYPE_U64;
    if (strcmp(type_str, "f32") == 0)    return TYPE_F32;
    if (strcmp(type_str, "f64") == 0)    return TYPE_F64;
    if (strcmp(type_str, "c_int") == 0)  return TYPE_C_INT;
    if (strcmp(type_str, "c_uint") == 0) return TYPE_C_UINT;
    if (strcmp(type_str, "c_long") == 0) return TYPE_C_LONG;
    if (strcmp(type_str, "c_ulong") == 0) return TYPE_C_ULONG;
    if (strcmp(type_str, "c_longlong") == 0)  return TYPE_C_LONGLONG;
    if (strcmp(type_str, "c_ulonglong") == 0) return TYPE_C_ULONGLONG;
    if (strcmp(type_str, "c_size") == 0) return TYPE_C_SIZE;
    if (strcmp(type_str, "c_ssize") == 0) return TYPE_C_SSIZE;
    if (strcmp(type_str, "str8") == 0)   return TYPE_STR8;
    if (strcmp(type_str, "str16") == 0)  return TYPE_STR16;
    // 容器/集合类型
    if (strcmp(type_str, "Array") == 0)  return TYPE_ARRAY;
    if (strcmp(type_str, "Dict") == 0)   return TYPE_DICT;
    return TYPE_ANY;
}

// 检查类型是否是已知的 struct 类型
static int is_known_struct(const char* type_str, char** struct_names, int struct_count) {
    for (int i = 0; i < struct_count; i++) {
        if (strcmp(struct_names[i], type_str) == 0) {
            return 1;
        }
    }
    return 0;
}

// 检查类型是否是已知的 clib 类型
static int is_known_clib(const char* type_str, char** clib_names, int clib_count) {
    for (int i = 0; i < clib_count; i++) {
        if (strcmp(clib_names[i], type_str) == 0) {
            return 1;
        }
    }
    return 0;
}

// 检查类型是否是已知的 face 类型
static int is_known_face(const char* type_str, char** face_names, int face_count) {
    for (int i = 0; i < face_count; i++) {
        if (strcmp(face_names[i], type_str) == 0) {
            return 1;
        }
    }
    return 0;
}

// 检查类型是否是已知的 cstruct 类型
static int is_known_cstruct(const char* type_str, char** cstruct_names, int cstruct_count) {
    for (int i = 0; i < cstruct_count; i++) {
        if (strcmp(cstruct_names[i], type_str) == 0) {
            return 1;
        }
    }
    return 0;
}

// ============================================================================
// 增强文本推断：函数调用/变量引用/简单表达式 → 查同文件符号表
// ============================================================================

// 前向声明（函数定义在下方，因 infer_single_expr_type 与 infer_array_literal_element_type 互相递归）
static TypeInfo* infer_array_literal_element_type(const char* p, ModuleSymbolTable* table);

// 前向声明（定义在 sym_table_add.inc，已在本文件之前 include）
// ModuleFuncSymbol* module_symbol_table_find_func(ModuleSymbolTable* table, const char* func_name);
// ModuleVarSymbol* module_symbol_table_find_var(ModuleSymbolTable* table, const char* var_name);

// 判断是否是 bigint 字面量（数字超过 int64 范围）
// Leno 中超过 2^31-1 的整数自动升级为 bigint
static int is_bigint_literal(const char* start, int len) {
    if (len <= 0) return 0;
    // 去掉前导负号
    const char* p = start;
    if (*p == '-') { p++; len--; }
    // 19 位以上一定是 bigint（int64 最大 19 位：9223372036854775807）
    if (len > 18) return 1;
    // 18 位以下通常 int，但 19 位需要比较
    // 简化：18 位以上按 bigint 处理（保守，宁可提升也不丢类型）
    if (len >= 18) return 1;
    return 0;
}

// 从文本推断单个表达式类型（增强版）
// pp: 指向表达式起始位置，返回时跳过整个表达式
// table: 模块符号表（用于查函数返回类型/变量类型）
// out_type_info: 若非 NULL，返回复杂类型的 TypeInfo*（caller 拥有），仅 ARRAY/DICT/STRUCT 设置
// 返回: 推断的 TypeKind
static TypeKind infer_single_expr_type(const char** pp, ModuleSymbolTable* table,
                                        TypeInfo** out_type_info) {
    const char* p = *pp;
    if (out_type_info) *out_type_info = NULL;

    // skip whitespace
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;

    // number literal
    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        const char* num_start = p;
        if (*p == '-') p++;
        while (*p && *p >= '0' && *p <= '9') p++;
        if (*p == '.') {
            p++;
            while (*p && *p >= '0' && *p <= '9') p++;
            *pp = p;
            return TYPE_FLOAT;
        }
        // check for bigint (long integer literal)
        int num_len = (int)(p - num_start);
        if (is_bigint_literal(num_start, num_len)) {
            *pp = p;
            return TYPE_BIGINT;
        }
        *pp = p;
        return TYPE_INT;
    }

    // string literal
    if (*p == '"' || *p == '\'') {
        char quote = *p++;
        while (*p && *p != quote) { if (*p == '\\' && *(p+1)) p += 2; else p++; }
        if (*p) p++;
        *pp = p;
        return TYPE_STRING;
    }

    // bool / null keywords
    if (strncmp(p, "true", 4) == 0 && !(isalnum((unsigned char)p[4]) || p[4] == '_')) {
        *pp = p + 4;
        return TYPE_BOOL;
    }
    if (strncmp(p, "false", 5) == 0 && !(isalnum((unsigned char)p[5]) || p[5] == '_')) {
        *pp = p + 5;
        return TYPE_BOOL;
    }
    if (strncmp(p, "null", 4) == 0 && !(isalnum((unsigned char)p[4]) || p[4] == '_')) {
        *pp = p + 4;
        return TYPE_NULL;
    }

    // nested array literal
    if (*p == '[') {
        TypeInfo* elem = infer_array_literal_element_type(p, table);
        // skip past the array literal
        int depth = 1;
        p++;
        while (*p && depth > 0) {
            if (*p == '"' || *p == '\'') {
                char q = *p++;
                while (*p && *p != q) { if (*p == '\\' && *(p+1)) p += 2; else p++; }
                if (*p) p++;
                continue;
            }
            if (*p == '[') depth++;
            else if (*p == ']') { depth--; if (depth == 0) { p++; break; } }
            p++;
        }
        *pp = p;
        if (out_type_info && elem) {
            *out_type_info = type_new(TYPE_ARRAY);
            (*out_type_info)->element_type = elem;
        } else if (elem) {
            type_free(elem);
        }
        return TYPE_ARRAY;
    }

    // nested dict literal
    if (*p == '{') {
        // skip past the dict literal
        int depth = 1;
        p++;
        while (*p && depth > 0) {
            if (*p == '"' || *p == '\'') {
                char q = *p++;
                while (*p && *p != q) { if (*p == '\\' && *(p+1)) p += 2; else p++; }
                if (*p) p++;
                continue;
            }
            if (*p == '{') depth++;
            else if (*p == '}') { depth--; if (depth == 0) { p++; break; } }
            p++;
        }
        *pp = p;
        return TYPE_DICT;
    }

    // identifier: could be function call, variable reference, or struct/enum
    if (isalpha((unsigned char)*p) || *p == '_') {
        const char* ident_start = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
        int ident_len = (int)(p - ident_start);
        char ident[64];
        int copy_len = ident_len < 63 ? ident_len : 63;
        memcpy(ident, ident_start, copy_len);
        ident[copy_len] = '\0';

        // skip whitespace after identifier
        const char* after_ident = p;
        while (*after_ident && (*after_ident == ' ' || *after_ident == '\t')) after_ident++;

        // function call: ident(...)
        if (*after_ident == '(') {
            // look up function in symbol table
            if (table) {
                ModuleFuncSymbol* func = module_symbol_table_find_func(table, ident);
                if (func) {
                    // skip past the function call arguments
                    int depth = 1;
                    p = after_ident + 1;
                    while (*p && depth > 0) {
                        if (*p == '"' || *p == '\'') {
                            char q = *p++;
                            while (*p && *p != q) { if (*p == '\\' && *(p+1)) p += 2; else p++; }
                            if (*p) p++;
                            continue;
                        }
                        if (*p == '(') depth++;
                        else if (*p == ')') { depth--; if (depth == 0) { p++; break; } }
                        p++;
                    }
                    *pp = p;
                    // return the function's return type
                    if (out_type_info && func->return_type_info) {
                        *out_type_info = type_copy(func->return_type_info);
                    }
                    return func->return_type;
                }
            }
            // function not found, skip past call args
            int depth = 1;
            p = after_ident + 1;
            while (*p && depth > 0) {
                if (*p == '"' || *p == '\'') {
                    char q = *p++;
                    while (*p && *p != q) { if (*p == '\\' && *(p+1)) p += 2; else p++; }
                    if (*p) p++;
                    continue;
                }
                if (*p == '(') depth++;
                else if (*p == ')') { depth--; if (depth == 0) { p++; break; } }
                p++;
            }
            *pp = p;
            return TYPE_ANY;
        }

        // variable reference: look up in symbol table
        if (table) {
            ModuleVarSymbol* var = module_symbol_table_find_var(table, ident);
            if (var) {
                *pp = p;
                if (out_type_info && var->type_info) {
                    *out_type_info = type_copy(var->type_info);
                }
                return var->type;
            }
        }

        // unknown identifier (could be LHS of binary expression, but we can't know its type
        // without AST — fall through to TYPE_ANY)
        *pp = p;
        return TYPE_ANY;
    }

    // simple binary expression: detect "literal op literal" pattern
    // e.g., [1+2, 3*4] → int, [1.0+2, 3*4.0] → float
    // We already handled numbers above, so this branch handles the case where
    // we're at an unexpected character
    {
        // try to parse: number op number
        const char* try_p = p;
        if (*try_p == '-' || (*try_p >= '0' && *try_p <= '9')) {
            // parse first number
            TypeKind lhs_kind;
            if (*try_p == '-') try_p++;
            while (*try_p && *try_p >= '0' && *try_p <= '9') try_p++;
            if (*try_p == '.') {
                lhs_kind = TYPE_FLOAT;
                try_p++;
                while (*try_p && *try_p >= '0' && *try_p <= '9') try_p++;
            } else {
                lhs_kind = TYPE_INT;
            }
            // skip whitespace
            while (*try_p && (*try_p == ' ' || *try_p == '\t')) try_p++;
            // check for operator
            if (*try_p == '+' || *try_p == '-' || *try_p == '*' || *try_p == '/' || *try_p == '%') {
                try_p++;
                while (*try_p && (*try_p == ' ' || *try_p == '\t')) try_p++;
                // parse second number
                if (*try_p == '-' || (*try_p >= '0' && *try_p <= '9')) {
                    TypeKind rhs_kind;
                    if (*try_p == '-') try_p++;
                    while (*try_p && *try_p >= '0' && *try_p <= '9') try_p++;
                    if (*try_p == '.') {
                        rhs_kind = TYPE_FLOAT;
                        try_p++;
                        while (*try_p && *try_p >= '0' && *try_p <= '9') try_p++;
                    } else {
                        rhs_kind = TYPE_INT;
                    }
                    *pp = try_p;
                    // type promotion: int+int→int, int+float→float, float+float→float
                    if (lhs_kind == TYPE_FLOAT || rhs_kind == TYPE_FLOAT) return TYPE_FLOAT;
                    return TYPE_INT;
                }
            }
        }
    }

    // fallback: skip to next comma or closing bracket
    {
        int depth = 0;
        while (*p) {
            if (*p == '"' || *p == '\'') {
                char q = *p++;
                while (*p && *p != q) { if (*p == '\\' && *(p+1)) p += 2; else p++; }
                if (*p) p++;
                continue;
            }
            if (*p == '[' || *p == '{' || *p == '(') depth++;
            else if (*p == ']' || *p == '}' || *p == ')') {
                if (depth == 0) break;
                depth--;
            }
            else if (*p == ',' && depth == 0) break;
            p++;
        }
        *pp = p;
        return TYPE_ANY;
    }
}

// ============================================================================
// 类型提升规则（对齐主文件 type_is_compatible）
// ============================================================================

// 两个 TypeKind 做类型提升，返回提升后的 TypeKind
// 规则：int+float→float, bool+int→int, bool+float→float, int+bigint→bigint, bool+bigint→bigint
static TypeKind promote_type_kinds(TypeKind a, TypeKind b) {
    if (a == b) return a;
    // int + float → float
    if ((a == TYPE_INT && b == TYPE_FLOAT) || (a == TYPE_FLOAT && b == TYPE_INT))
        return TYPE_FLOAT;
    // bool + int → int (bool is subset of int)
    if ((a == TYPE_BOOL && b == TYPE_INT) || (a == TYPE_INT && b == TYPE_BOOL))
        return TYPE_INT;
    // bool + float → float
    if ((a == TYPE_BOOL && b == TYPE_FLOAT) || (a == TYPE_FLOAT && b == TYPE_BOOL))
        return TYPE_FLOAT;
    // int + bigint → bigint
    if ((a == TYPE_INT && b == TYPE_BIGINT) || (a == TYPE_BIGINT && b == TYPE_INT))
        return TYPE_BIGINT;
    // bool + bigint → bigint
    if ((a == TYPE_BOOL && b == TYPE_BIGINT) || (a == TYPE_BIGINT && b == TYPE_BOOL))
        return TYPE_BIGINT;
    // float + bigint → float
    if ((a == TYPE_FLOAT && b == TYPE_BIGINT) || (a == TYPE_BIGINT && b == TYPE_FLOAT))
        return TYPE_FLOAT;
    // incompatible
    return TYPE_ANY;
}

// ============================================================================
// 字面量元素类型推断 — 从数组/字典字面量文本推断元素/值类型（增强版）
// ============================================================================

// 从数组字面量文本推断元素类型
// p 指向开头的 '['，返回推断的元素类型 TypeInfo*（caller 拥有），NULL 表示空数组
// table: 模块符号表（用于查函数返回类型/变量类型）
static TypeInfo* infer_array_literal_element_type(const char* p, ModuleSymbolTable* table) {
    if (!p || *p != '[') return NULL;
    p++; // skip '['

    TypeKind common_kind = TYPE_UNKNOWN;
    TypeInfo* common_nested = NULL;  // 嵌套数组时的元素类型
    int has_elements = 0;

    while (*p) {
        // skip whitespace
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;

        if (*p == ']') break;
        if (*p == ',') { p++; continue; }

        TypeKind elem_kind = TYPE_ANY;
        TypeInfo* elem_nested = NULL;

        // use enhanced single-expression type inference
        const char* before = p;
        TypeInfo* inferred = NULL;
        elem_kind = infer_single_expr_type(&p, table, &inferred);
        if (inferred) {
            if (inferred->kind == TYPE_ARRAY && inferred->element_type) {
                elem_nested = inferred->element_type;
                free(inferred); // keep only element_type
            } else {
                type_free(inferred);
            }
        }
        // safety: if p didn't advance, skip one char to avoid infinite loop
        if (p == before) { p++; }

        has_elements = 1;

        // update common type
        if (common_kind == TYPE_UNKNOWN) {
            // first element sets the common type
            common_kind = elem_kind;
            if (elem_nested) { common_nested = elem_nested; elem_nested = NULL; }
        } else if (common_kind != elem_kind) {
            // type mismatch - try numeric type promotion
            common_kind = promote_type_kinds(common_kind, elem_kind);
            if (common_nested) { type_free(common_nested); common_nested = NULL; }
        } else if (common_kind == TYPE_ARRAY) {
            // both are arrays, check if nested element types match
            if (common_nested && elem_nested) {
                if (common_nested->kind != elem_nested->kind) {
                    // nested types differ -> any
                    common_kind = TYPE_ANY;
                    type_free(common_nested);
                    common_nested = NULL;
                }
                // else: nested types match, keep common_nested
            } else if (!common_nested && !elem_nested) {
                // both empty arrays -> keep as is
            } else {
                // one has element type, other doesn't -> any
                common_kind = TYPE_ANY;
                if (common_nested) { type_free(common_nested); common_nested = NULL; }
            }
        }

        if (elem_nested) type_free(elem_nested);
    }

    if (!has_elements) return NULL; // empty array

    TypeInfo* result = type_new(common_kind == TYPE_UNKNOWN ? TYPE_ANY : common_kind);
    if (common_kind == TYPE_ARRAY && common_nested) {
        result->element_type = common_nested;
    }
    return result;
}

// 从字典字面量文本推断值类型
// p 指向开头的 '{'，返回推断的值类型 TypeInfo*（caller 拥有），NULL 表示空字典
// table: 模块符号表（用于查函数返回类型/变量类型）
static TypeInfo* infer_dict_literal_value_type(const char* p, ModuleSymbolTable* table) {
    if (!p || *p != '{') return NULL;
    p++; // skip '{'

    TypeKind common_kind = TYPE_UNKNOWN;
    int has_elements = 0;

    while (*p) {
        // skip whitespace
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;

        if (*p == '}') break;
        if (*p == ',') { p++; continue; }

        // skip key: "string" or identifier
        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            while (*p && *p != quote) { if (*p == '\\' && *(p+1)) p += 2; else p++; }
            if (*p) p++; // skip closing quote
        } else {
            while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
        }

        // skip whitespace and ':'
        while (*p && (*p == ' ' || *p == '\t')) p++;
        if (*p == ':') p++;
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;

        // now at value - use enhanced single-expression type inference
        TypeKind val_kind;
        const char* before = p;
        val_kind = infer_single_expr_type(&p, table, NULL);
        // safety: if p didn't advance, skip one char to avoid infinite loop
        if (p == before) { p++; }

        has_elements = 1;

        if (common_kind == TYPE_UNKNOWN) {
            common_kind = val_kind;
        } else if (common_kind != val_kind) {
            // type mismatch - try numeric type promotion
            common_kind = promote_type_kinds(common_kind, val_kind);
        }
    }

    if (!has_elements) return NULL; // empty dict

    return type_new(common_kind == TYPE_UNKNOWN ? TYPE_ANY : common_kind);
}

