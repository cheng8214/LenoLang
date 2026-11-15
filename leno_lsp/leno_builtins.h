/**
 * Leno 内置函数元数据
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
    // ★ 2026-11-15：补齐编译器里已注册、但补全表漏掉的下划线全局内置
    //   （对照 `vm_register_native(\"…\")` 全量核对：此前 37 个里只列了 10 个）
    {
        "_arch",
        "_arch()",
        "目标架构名",
        "_arch()             // \"x64\" / \"arm64\"",
        0,
        "string"
    },
    {
        "_args",
        "_args()",
        "命令行参数数组（不含可执行文件名）",
        "var a = _args()     // Array[string]",
        0,
        "Array[string]"
    },
    {
        "_byte",
        "_byte(value)",
        "转成 0..255 的字节值（取低 8 位）",
        "_byte(300)          // 44",
        1,
        "int"
    },
    {
        "_clear",
        "_clear()",
        "清屏",
        "_clear()",
        0,
        "null"
    },
    {
        "_console",
        "_console([show])",
        "显示/隐藏控制台窗口（Windows 有效），返回是否成功",
        "_console(false)     // 隐藏控制台（GUI 程序常用）",
        -1,
        "bool"
    },
    {
        "_env",
        "_env(name)",
        "读环境变量，缺失返回 null（宽容版用 `_env_or`）",
        "_env(\"PATH\")",
        1,
        "string?"
    },
    {
        "_env_or",
        "_env_or(name, default)",
        "读环境变量，缺失时返回默认值",
        "_env_or(\"HOME\", \"C:/\")",
        2,
        "string"
    },
    {
        "_exec",
        "_exec(cmd, [timeout_ms], [cwd])",
        "执行外部命令并等待结束，返回 ExecResult（code / output）",
        "var r = _exec(\"git status\", 30000)",
        -1,
        "ExecResult?"
    },
    {
        "_exit",
        "_exit(code)",
        "立即退出进程并返回退出码",
        "_exit(1)",
        1,
        "null"
    },
    {
        "_field",
        "_field(obj, name)",
        "按名字取字段（运行期动态访问，绕过静态检查）",
        "_field(p, \"x\")",
        2,
        "any"
    },
    {
        "_float_or",
        "_float_or(value, default)",
        "宽容解析为 float，解析失败返回默认值（严格版 `_float` 会抛）",
        "_float_or(\"1.5x\", 0.0)   // 0.0",
        2,
        "float"
    },
    {
        "_gc_stats",
        "_gc_stats()",
        "GC 统计信息（分配次数 / 回收次数 / 堆大小等）",
        "print(_gc_stats())",
        0,
        "Dict"
    },
    {
        "_homedir",
        "_homedir()",
        "当前用户主目录",
        "_homedir()          // \"C:\\\\Users\\\\xxx\"",
        0,
        "string"
    },
    {
        "_instance_activate",
        "_instance_activate([name])",
        "唤起已经运行的同一程序实例（配合 `_single_instance`），返回是否成功",
        "if not _single_instance(\"myapp\") { _instance_activate(\"myapp\"); _exit(0) }",
        -1,
        "bool"
    },
    {
        "_int_or",
        "_int_or(value, default)",
        "宽容解析为 int，解析失败返回默认值（严格版 `_int` 会抛）",
        "_int_or(\"12x\", -1)   // -1",
        2,
        "int"
    },
    {
        "_int32",
        "_int32(value)",
        "转成 32 位有符号整数（超范围按位截断）",
        "_int32(2147483648)   // -2147483648",
        1,
        "int"
    },
    {
        "_int64",
        "_int64(value)",
        "转成 64 位有符号整数",
        "_int64(1e30)",
        1,
        "int"
    },
    {
        "_uint8",
        "_uint8(value)",
        "转成 8 位无符号整数（0..255）",
        "_uint8(300)          // 44",
        1,
        "int"
    },
    {
        "_uint32",
        "_uint32(value)",
        "转成 32 位无符号整数",
        "_uint32(-1)          // 4294967295",
        1,
        "int"
    },
    {
        "_uint64",
        "_uint64(value)",
        "转成 64 位无符号整数（超出 int64 时自动提升为 bigint）",
        "_uint64(-1)",
        1,
        "int"
    },
    {
        "_kill",
        "_kill(pid)",
        "结束由 `_spawn` 启动的进程，返回是否成功",
        "var pid = _spawn(\"notepad.exe\")\n_kill(pid)",
        1,
        "bool"
    },
    {
        "_mem",
        "_mem()",
        "当前进程的内存占用信息",
        "print(_mem())",
        0,
        "Dict"
    },
    {
        "_pid",
        "_pid()",
        "当前进程号",
        "_pid()",
        0,
        "int"
    },
    {
        "_sep",
        "_sep()",
        "路径分隔符（Windows \"\\\\\"、POSIX \"/\"）",
        "_sep()",
        0,
        "string"
    },
    {
        "_single_instance",
        "_single_instance([name])",
        "单实例锁：第一个实例返回 true，后续实例返回 false",
        "if not _single_instance(\"myapp\") { _exit(0) }",
        -1,
        "bool"
    },
    {
        "_spawn",
        "_spawn(exe, [args])",
        "启动子进程（不等待），返回 pid；失败返回 -1",
        "var pid = _spawn(\"notepad.exe\")",
        -1,
        "int"
    },
    {
        "_tmpdir",
        "_tmpdir()",
        "系统临时目录",
        "_tmpdir()",
        0,
        "string"
    },
    {
        "_username",
        "_username()",
        "当前用户名",
        "_username()",
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
