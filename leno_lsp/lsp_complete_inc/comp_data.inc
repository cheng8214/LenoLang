// LenoC 关键字（从 lexer.c 提取）
static const char* leno_keywords[] = {
    // 控制流
    "if", "else", "eif", "then",
    "while", "for", "to", "break", "continue",
    "switch", "case", "default",
    "return",
    // 声明
    "var", "func", "struct", "cstruct", "enum", "face", "impl",
    "alias", "clib", "cfunc",
    // 实例化
    "new",
    // 模块
    "import", "export", "as", "use",
    // 逻辑
    "and", "or", "not", "is", "in",
    // 异常
    "try", "catch", "throw", "finally",
    // 异步
    "async", "await",
    // 字面量
    "true", "false", "null",
    NULL
};

// LenoC 内置类型
static const char* leno_types[] = {
    "int", "float", "string", "bool",
    "Array", "Dict", "File", "Ptr", "any", "face",
    "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "f32", "f64",
    "c_int", "c_uint", "c_long", "c_ulong", "c_longlong", "c_ulonglong", "c_size", "c_ssize",
    "bigint", "str8", "str16", "Thread", "Channel",
    NULL
};

static const char* cstruct_instance_methods[] = {
    "free", "to_ptr", "to_str", "size", "alignment", "debug", "hex", "free_all",
    NULL
};

static const char* cstruct_static_methods[] = {
    "malloc", "from_ptr", "malloc_array",
    NULL
};
