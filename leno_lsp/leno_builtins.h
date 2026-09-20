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
    {
        "input",
        "input(prompt?)",
        "从标准输入读取一行字符串",
        "input()            // 读取输入\ninput(\"name: \")    // 带提示读取",
        -1,
        "string"
    },
    {
        "sleep",
        "sleep(ms)",
        "暂停当前线程指定的毫秒数",
        "sleep(1000)        // 暂停1秒",
        1,
        "null"
    },
    {
        "assert",
        "assert(condition, message?)",
        "断言条件为真，否则抛出异常",
        "assert(x > 0)\nassert(x > 0, \"must be positive\")",
        -1,
        "null"
    },
    {
        "assert_eq",
        "assert_eq(expected, actual, message?)",
        "断言两个值相等",
        "assert_eq(1, 1)\nassert_eq(a, b, \"should be equal\")",
        -1,
        "null"
    },
    {
        "assert_ne",
        "assert_ne(expected, actual, message?)",
        "断言两个值不相等",
        "assert_ne(1, 2)\nassert_ne(a, b, \"should not be equal\")",
        -1,
        "null"
    },
    {
        "assert_true",
        "assert_true(condition, message?)",
        "断言条件为真",
        "assert_true(x > 0)",
        -1,
        "null"
    },
    {
        "assert_false",
        "assert_false(condition, message?)",
        "断言条件为假",
        "assert_false(x < 0)",
        -1,
        "null"
    },
    {
        "assert_null",
        "assert_null(value, message?)",
        "断言值为 null",
        "assert_null(x)",
        -1,
        "null"
    },
    {
        "format",
        "format(template, ...args)",
        "格式化字符串",
        "format(\"hello %s\", name)\nformat(\"%d items\", count)",
        -1,
        "string"
    },
    {
        "_args",
        "_args()",
        "获取命令行参数列表",
        "_args()             // [\"script.leno\", \"arg1\"]",
        0,
        "Array"
    },
    {
        "_script",
        "_script()",
        "获取当前脚本文件路径",
        "_script()           // \"path/to/script.leno\"",
        0,
        "string"
    },
    {
        "_executable",
        "_executable()",
        "获取当前可执行文件路径",
        "_executable()       // \"/usr/bin/leno\"",
        0,
        "string"
    },
    {
        "_gc",
        "_gc()",
        "手动触发垃圾回收",
        "_gc()",
        0,
        "null"
    },
    {
        "_os",
        "_os()",
        "获取当前操作系统名称",
        "_os()               // \"windows\" 或 \"linux\" 或 \"macos\"",
        0,
        "string"
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
