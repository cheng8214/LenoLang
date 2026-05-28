/**
 * LenoC 内置函数元数据
 * 从 native.c 提取的函数签名和文档
 */

#ifndef LENO_BUILTINS_H
#define LENO_BUILTINS_H

// 内置函数元数据结构
typedef struct {
    const char* name;           // 函数名
    const char* signature;      // 函数签名
    const char* description;    // 描述
    const char* example;        // 示例代码
    int arity;                  // 参数数量 (-1 表示可变参数)
    const char* return_type;    // 返回类型
} BuiltinFunctionMeta;

// 全局内置函数表
static const BuiltinFunctionMeta builtin_functions[] = {
    {
        "print",
        "print(...values)",
        "输出内容到控制台，多个值用空格分隔，自动换行",
        "print(\"hello\")\nprint(123)\nprint(\"a =\", 10)",
        -1,
        "null"
    },
    {
        "printf",
        "printf(...values)",
        "输出内容到控制台，多个值用空格分隔，不换行",
        "printf(\"hello\")\nprintf(123)",
        -1,
        "null"
    },
    {
        "type",
        "type(value)",
        "返回值的类型名称字符串",
        "type(123)           // \"int\"\n"
        "type(\"hello\")      // \"string\"\n"
        "type([1,2,3])       // \"Array[int]\"\n"
        "type({\"a\":1})      // \"Dict[string, int]\"",
        1,
        "string"
    },
    {
        "len",
        "len(collection)",
        "返回数组、字符串或字典的长度",
        "len([1,2,3])        // 3\n"
        "len(\"hello\")       // 5\n"
        "len({\"a\":1, \"b\":2})  // 2",
        1,
        "int"
    },
    {
        "_int",
        "_int(value)",
        "将值转换为整数",
        "_int(\"123\")        // 123\n"
        "_int(3.14)          // 3\n"
        "_int(true)          // 1",
        1,
        "int"
    },
    {
        "_float",
        "_float(value)",
        "将值转换为浮点数",
        "_float(\"3.14\")     // 3.14\n"
        "_float(123)         // 123.0",
        1,
        "float"
    },
    {
        "_bool",
        "_bool(value)",
        "将值转换为布尔值",
        "_bool(1)            // true\n"
        "_bool(0)            // false\n"
        "_bool(\"hello\")     // true",
        1,
        "bool"
    },
    {
        "_str",
        "_str(value)",
        "将值转换为字符串",
        "_str(123)           // \"123\"\n"
        "_str(true)          // \"true\"",
        1,
        "string"
    },
    {
        "_ptr",
        "_ptr(value)",
        "将值转换为指针（用于 FFI）",
        "_ptr(obj)           // Ptr",
        1,
        "Ptr"
    },
    {NULL, NULL, NULL, NULL, 0, NULL}
};

// 查找内置函数元数据
static inline const BuiltinFunctionMeta* find_builtin_function(const char* name) {
    for (int i = 0; builtin_functions[i].name != NULL; i++) {
        if (strcmp(builtin_functions[i].name, name) == 0) {
            return &builtin_functions[i];
        }
    }
    return NULL;
}

// 生成内置函数的悬停文档
static inline char* generate_builtin_doc(const BuiltinFunctionMeta* meta) {
    if (!meta) return NULL;
    
    int len = 256 + strlen(meta->name) + strlen(meta->signature) + 
              strlen(meta->description) + strlen(meta->example) + 
              strlen(meta->return_type);
    
    char* doc = (char*)malloc(len);
    if (!doc) return NULL;
    
    snprintf(doc, len, "**%s** - %s\n\n"
             "```leno\n"
             "%s\n"
             "```\n\n"
             "%s\n\n"
             "**参数**: %s\n"
             "**返回**: %s",
             meta->name,
             meta->description,
             meta->example,
             meta->signature,
             meta->arity == -1 ? "可变参数" : "固定参数",
             meta->return_type);
    
    return doc;
}

#endif // LENO_BUILTINS_H
