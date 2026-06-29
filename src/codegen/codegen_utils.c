#include "codegen.h"

int bigint_str_fits_in_int32(const char* str) {
    const char* p = str;
    while (*p == ' ' || *p == '\t') p++;

    int is_negative = 0;
    if (*p == '-') {
        is_negative = 1;
        p++;
    } else if (*p == '+') {
        p++;
    }

    // 检查十六进制前缀 (0x 或 0X)
    int is_hex = 0;
    if (*p == '0' && (p[1] == 'x' || p[1] == 'X')) {
        is_hex = 1;
        p += 2;  // 跳过 "0x" 前缀
    }

    while (*p == '0') p++;

    const char* start = p;
    if (is_hex) {
        // 十六进制：检查十六进制数字
        while ((*p >= '0' && *p <= '9') ||
               (*p >= 'a' && *p <= 'f') ||
               (*p >= 'A' && *p <= 'F')) {
            p++;
        }
        int hex_digits = p - start;
        // 32位有符号整数的十六进制范围是 -0x80000000 到 0x7FFFFFFF
        // 即最多 8 个十六进制数字（不包括符号）
        if (hex_digits > 8) return 0;
        if (hex_digits < 8) return 1;
        // 8 个十六进制数字，需要比较具体值
        const char* hex_max = is_negative ? "80000000" : "7FFFFFFF";
        for (int i = 0; i < 8; i++) {
            char c = start[i];
            // 转换为大写进行比较
            if (c >= 'a' && c <= 'f') c = c - 'a' + 'A';
            if (c > hex_max[i]) return 0;
            if (c < hex_max[i]) return 1;
        }
        return 1;
    } else {
        // 十进制：原有逻辑
        while (*p >= '0' && *p <= '9') p++;
        int digits = p - start;

        if (digits > 10) return 0;
        if (digits < 10) return 1;

        const char* int32_max = "2147483647";
        const char* int32_min = "2147483648";

        const char* cmp_str = is_negative ? int32_min : int32_max;

        for (int i = 0; i < 10; i++) {
            if (start[i] > cmp_str[i]) return 0;
            if (start[i] < cmp_str[i]) return 1;
        }
        return 1;
    }
}

int is_string_expr(Ast* ast) {
    return ast && ast->kind == AST_STRING;
}

int is_array_expr(Ast* ast) {
    return ast && ast->kind == AST_ARRAY;
}

int is_dict_expr(Ast* ast) {
    return ast && ast->kind == AST_DICT;
}

int is_var_expr(Ast* ast) {
    // 支持变量、属性访问（如 d.statements）、字段访问（如 s.field）和模块访问
    return ast && (ast->kind == AST_VAR || ast->kind == AST_INDEX || ast->kind == AST_MODULE_ACCESS || ast->kind == AST_FIELD_ACCESS);
}

// 检查表达式是否为数字类型（用于优化循环路径）
int is_number_expr(Ast* ast) {
    if (!ast) return 0;
    
    // 字面量数字
    if (ast->kind == AST_NUM) return 1;
    
    // 简单变量（在语义分析后应该能推断为数字）
    // 更复杂的表达式检查需要完整的语义信息
    // 这里简化处理：只要不是数组/字符串/字典字面量，就认为是可能数字
    if (ast->kind == AST_VAR) return 1;
    
    // 二元运算（如 n-1）结果通常是数字
    if (ast->kind == AST_BINOP) return 1;
    
    // 一元运算
    if (ast->kind == AST_UNARY) return 1;
    
    // 函数调用如 arr.len()
    if (ast->kind == AST_CALL) return 1;
    
    return 0;
}

MainFuncInfo find_main_function(Semantic* sem) {
    MainFuncInfo info = {0, -1};

    Scope* global = sem->root_scope;
    if (!global) return info;

    for (int i = 0; i < global->sym_cnt; i++) {
        Symbol* sym = global->syms[i];
        if (sym && sym->kind == SYM_GLOBAL_FUNC && strcmp(sym->name, "main") == 0) {
            info.has_main = 1;
            info.main_index = sym->index;
            break;
        }
    }

    return info;
}
