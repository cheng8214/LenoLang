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
    // 自定义类型名（struct 等）→ TYPE_STRUCT
    TypeInfo* ti = type_new(TYPE_STRUCT);
    ti->struct_name = strdup(buf);
    return ti;
}

// 递归解析类型字符串
static TypeInfo* parse_type_from_string_inner(const char** p) {
    skip_ws(p);
    if (!**p) return NULL;
    
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

