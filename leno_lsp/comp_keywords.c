/**
 * 关键字、类型、代码片段、内置函数补全提供者
 * 
 * 修复的核心问题：
 * 关键字/类型不再被 if-else 分支互斥排除，而是始终可用。
 * 过滤完全交给 LSP 客户端（标准做法），服务器不再做 strncmp 过滤。
 */

#include "lsp_completion.h"
#include "leno_builtins.h"
#include "../src/include/leno_types.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ========== 数据表 ========== */

// 关键字表
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

// 内置类型表
static const char* leno_types[] = {
    "int", "float", "string", "bool",
    "Array", "Dict", "File", "Ptr", "any", "face",
    "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "f32", "f64",
    "c_int", "c_uint", "c_long", "c_ulong", "c_longlong", "c_ulonglong", "c_size", "c_ssize",
    "bigint", "str8", "str16", "Thread", "Channel",
    NULL
};

// 关键字详细信息（用于 detail 和 documentation）
typedef struct {
    const char* keyword;
    const char* detail;
    const char* doc;  // 可以是 NULL，表示用 detail 作为 doc
} KeywordInfo;

static const KeywordInfo keyword_info[] = {
    {"if", "control flow", "Conditional execution: `if condition { ... }`"},
    {"else", "control flow", "Else branch: `else { ... }`"},
    {"eif", "control flow", "Else-if branch: `eif condition { ... }`"},
    {"then", "control flow", NULL},
    {"while", "control flow", "Loop: `while condition { ... }`"},
    {"for", "control flow", "Loop: `for start:end { ... }` or `for i:pairs(dict) { ... }`"},
    {"to", "control flow", NULL},
    {"break", "control flow", "Break out of loop"},
    {"continue", "control flow", "Continue to next iteration"},
    {"switch", "control flow", "Switch statement: `switch value { case x: ... }`"},
    {"case", "control flow", NULL},
    {"default", "control flow", NULL},
    {"return", "control flow", "Return from function: `return value`"},
    {"var", "declaration", "Variable declaration: `var name: type = value`"},
    {"func", "declaration", "Function definition: `func name(args) { ... }`"},
    {"struct", "declaration", "Struct definition: `struct Name { ... }`"},
    {"cstruct", "declaration", "C struct definition: `cstruct Name { ... }`"},
    {"enum", "declaration", "Enum definition: `enum Name { ... }`"},
    {"face", "declaration", "Interface definition: `face Name { ... }`"},
    {"impl", "declaration", "Implementation block: `impl Type { ... }`"},
    {"alias", "declaration", "Type alias: `alias Name = ExistingType`"},
    {"clib", "declaration", "C library binding"},
    {"cfunc", "declaration", "C function binding"},
    {"new", "instantiation", "Create struct instance: `new TypeName()`"},
    {"import", "module", "Import module: `import \"path\"` or `import ModuleAlias`"},
    {"export", "module", "Export declaration: `export func ...`"},
    {"as", "module", "Module alias: `import \"path\" as Alias`"},
    {"use", "module", "Use module symbols: `use ModuleName.(FuncA, FuncB)`"},
    {"and", "logic", "Logical AND"},
    {"or", "logic", "Logical OR"},
    {"not", "logic", "Logical NOT"},
    {"is", "logic", "Type guard: `x is Type`"},
    {"in", "logic", "Membership test: `x in collection`"},
    {"try", "exception", "Try block: `try { ... } catch e: Error { ... }`"},
    {"catch", "exception", "Catch block"},
    {"throw", "exception", "Throw exception: `throw Error(\"message\")`"},
    {"finally", "exception", "Finally block"},
    {"async", "async", "Async function: `async func name() { ... }`"},
    {"await", "async", "Await async result"},
    {"true", "literal", "Boolean true"},
    {"false", "literal", "Boolean false"},
    {"null", "literal", "Null value"},
    {NULL, NULL, NULL}
};

// 代码片段
typedef struct {
    const char* label;
    const char* insert;
    const char* detail;
} SnippetDef;

static const SnippetDef snippets[] = {
    {"if", "if ${1:condition} {\n\t$0\n}", "if...else snippet"},
    {"else", "else {\n\t$0\n}", "else block"},
    {"eif", "eif ${1:condition} {\n\t$0\n}", "else-if block"},
    {"while", "while ${1:condition} {\n\t$0\n}", "while loop"},
    {"for", "for ${1:i}:${2:end} {\n\t$0\n}", "for loop"},
    {"forpairs", "for ${1:k}:${2:v}:pairs(${3:dict}) {\n\t$0\n}", "for key:value in pairs"},
    {"switch", "switch ${1:value} {\n\tcase ${2:x}:\n\t\t$0\n}", "switch statement"},
    {"func", "func ${1:name}(${2:args}) {\n\t$0\n}", "function definition"},
    {"export func", "export func ${1:name}(${2:args}) {\n\t$0\n}", "exported function"},
    {"struct", "struct ${1:Name} {\n\t$0\n}", "struct definition"},
    {"enum", "enum ${1:Name} {\n\t${2:MEMBER_A},\n\t${3:MEMBER_B}\n}", "enum definition"},
    {"face", "face ${1:Name} {\n\t$0\n}", "interface definition"},
    {"try", "try {\n\t$1\n} catch ${2:e}: Error {\n\t$0\n}", "try...catch block"},
    {"impl", "impl ${1:TypeName} {\n\t$0\n}", "implementation block"},
};

/* ========== 辅助函数 ========== */

static const char* get_keyword_detail(const char* kw) {
    for (int i = 0; keyword_info[i].keyword; i++) {
        if (strcmp(keyword_info[i].keyword, kw) == 0) {
            return keyword_info[i].detail ? keyword_info[i].detail : "keyword";
        }
    }
    return "keyword";
}

/* ========== 提供者实现 ========== */

void comp_provider_add_keywords(CompletionSet* set, const char* filter) {
    (void)filter;  // 过滤完全由客户端处理
    
    for (int i = 0; leno_keywords[i]; i++) {
        const char* kw = leno_keywords[i];
        const char* detail = get_keyword_detail(kw);
        
        const char* doc = NULL;
        for (int j = 0; keyword_info[j].keyword; j++) {
            if (strcmp(keyword_info[j].keyword, kw) == 0) {
                doc = keyword_info[j].doc;
                break;
            }
        }
        
        char doc_buf[256];
        if (doc) {
            snprintf(doc_buf, sizeof(doc_buf), "```leno\n%s\n```", doc);
            doc = doc_buf;
        }
        
        comp_set_add(set, kw, LSP_COMP_KEYWORD, PRIO_KEYWORD,
                     detail, doc, NULL, NULL);
    }
}

void comp_provider_add_types(CompletionSet* set, const char* filter) {
    (void)filter;
    
    for (int i = 0; leno_types[i]; i++) {
        const char* t = leno_types[i];
        const char* category = "type";
        
        if (strcmp(t, "int") == 0 || strncmp(t, "i", 1) == 0 || strncmp(t, "u", 1) == 0 ||
            strcmp(t, "float") == 0 || strncmp(t, "f32", 3) == 0 || strncmp(t, "f64", 3) == 0 ||
            strcmp(t, "bigint") == 0) {
            category = "numeric type";
        } else if (strcmp(t, "string") == 0 || strcmp(t, "str8") == 0 || strcmp(t, "str16") == 0) {
            category = "string type";
        } else if (strcmp(t, "bool") == 0) {
            category = "boolean type";
        } else if (strcmp(t, "Array") == 0 || strcmp(t, "Dict") == 0) {
            category = "collection type";
        }
        
        comp_set_add(set, t, LSP_COMP_CLASS, PRIO_TYPE,
                     category, NULL, NULL, NULL);
    }
}

void comp_provider_add_snippets(CompletionSet* set, const char* filter) {
    (void)filter;
    
    int num_snippets = sizeof(snippets) / sizeof(snippets[0]);
    for (int i = 0; i < num_snippets; i++) {
        comp_set_add(set, snippets[i].label, LSP_COMP_SNIPPET, PRIO_SNIPPET,
                     snippets[i].detail, NULL, snippets[i].insert, NULL);
    }
}

void comp_provider_add_builtins(CompletionSet* set, const char* filter) {
    (void)filter;
    
    for (int i = 0; builtin_functions[i].name != NULL; i++) {
        const BuiltinFunctionMeta* meta = &builtin_functions[i];
        
        char detail_buf[512];
        if (meta->arity == -1) {
            snprintf(detail_buf, sizeof(detail_buf), "%s(...)", meta->name);
        } else if (meta->arity == 0) {
            snprintf(detail_buf, sizeof(detail_buf), "%s()", meta->name);
        } else {
            snprintf(detail_buf, sizeof(detail_buf), "%s(%d args)", meta->name, meta->arity);
        }
        
        char doc_buf[1024];
        snprintf(doc_buf, sizeof(doc_buf),
                 "**%s** - %s\n\n"
                 "```leno\n%s\n```\n\n"
                 "**Example**:\n```leno\n%s\n```\n\n"
                 "**Returns**: `%s`",
                 meta->name, meta->description,
                 meta->example, meta->signature,
                 meta->return_type);
        
        comp_set_add(set, meta->name, LSP_COMP_FUNCTION, PRIO_BUILTIN,
                     detail_buf, doc_buf, NULL, NULL);
    }
}

/* ========== self 关键字 ========== */

void comp_provider_add_self_keyword(CompletionSet* set,
                                     const char* content,
                                     LspPosition pos) {
    if (!set || !content) return;
    
    int cursor_offset = lsp_position_to_offset(content, pos);
    char* enclosing_struct = find_enclosing_struct_name(content, cursor_offset);
    if (!enclosing_struct) return;
    
    char doc_buf[256];
    snprintf(doc_buf, sizeof(doc_buf), "Self reference in `%s` method", enclosing_struct);
    
    comp_set_add(set, "self", LSP_COMP_KEYWORD, PRIO_KEYWORD,
                 "current instance", doc_buf, NULL, NULL);
    
    free(enclosing_struct);
}
