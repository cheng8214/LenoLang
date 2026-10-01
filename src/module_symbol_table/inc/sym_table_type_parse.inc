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
        
        // 带逗号的泛型实参：**只有标识符真的是 `Dict` 才是字典**（v3.2.8 修）
        //   ⚠ 原实现「见到逗号就当 Dict[K,V]」⇒ 把**双类型参数的泛型 struct** 也解析成了字典：
        //     模块里 `export func makePair[K, V](K k, V v): Pair[K, V]` 的返回类型被记成 `Dict[K,V]`
        //     ⇒ 调用方（`var p = gm.makePair[string, int]("x", 10)`）拿到
        //     `Dict[struct K, struct V]` ⇒ `p.getKey()` 报「没有方法 'getKey'」。
        //     实测来源：examples/泛型系统/泛型函数/test_generic_func_multi.leno 与
        //     跨模块泛型/test_generic_import2.leno（两处都是"直接 `new Pair[..]` 好、
        //     经泛型函数返回值就差" ⇒ 类型字符串解析在这条路上被绕过）。
        if (**p == ',') {
            (*p)++; // skip ','
            skip_ws(p);
            
            TypeInfo* param2 = parse_type_from_string_inner(p);
            if (!param2) { type_free(param1); return NULL; }
            
            skip_ws(p);
            if (**p != ']') { type_free(param1); type_free(param2); return NULL; }
            (*p)++; // skip ']'
            
            if (strcmp(ident, "Dict") == 0) {
                // Dict[K, V]
                TypeInfo* dict = type_new(TYPE_DICT);
                dict->key_type = param1;   // 键类型
                dict->value_type = param2; // 值类型
                return dict;
            }

            // 泛型 struct 的**两个**类型实参：Pair[K, V] → TYPE_STRUCT + generic_args[2]
            //   （与单参数那条同一表示法；`type_to_string` 会渲染成 `Pair[K, V]`）
            TypeInfo* generic_struct2 = type_new(TYPE_STRUCT);
            generic_struct2->struct_name = strdup(ident);
            generic_struct2->generic_args = (TypeInfo**)malloc(sizeof(TypeInfo*) * 2);
            if (generic_struct2->generic_args) {
                generic_struct2->generic_args[0] = param1;
                generic_struct2->generic_args[1] = param2;
                generic_struct2->generic_count = 2;
            } else {
                type_free(param1);
                type_free(param2);
            }
            return generic_struct2;
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

// ============================================================================
// (2026-10-01 S10) 此处原为「增强文本推断」整段（约 390 行）：`infer_single_expr_type` /
//   `infer_array_literal_element_type` / `promote_type_kinds` / `is_bigint_literal`。
//   它们是**文本扫描链**专用（拿源码文本去猜 `var x = [...]` 的元素类型），随扫描链整条退役
//   （scan/ 目录已删、sym_table_scan.inc 已摘）⇒ 全仓库**零外部引用**（仅这四者内部互相递归）
//   ⇒ 已删。类型推断的唯一实现是 AST / 语义侧（module_ast_exports.c 的 ast_kind_of / ast_fix_agg_kind ✓）
// ============================================================================
