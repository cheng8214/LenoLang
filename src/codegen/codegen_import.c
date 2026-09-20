// ============================================================================
// 寄存器式 codegen：import / use 内联
// ============================================================================

#include "codegen.h"

void gen_import_inline(CodeGen* gen, Ast* ast) {
    // import 语句在寄存器式下不生成字节码
    // 模块加载在 lenolang_run / --compile 中处理
}

// ============================================================================
// 工具函数
// ============================================================================

int bigint_str_fits_in_int32(const char* str) {
    // 检查 bigint 字符串是否可以用 int32 表示
    if (!str) return 0;
    // 简化：如果以 '-' 开头且超过 10 位数字，或无 '-' 且超过 10 位，则不 fit
    int len = (int)strlen(str);
    if (len == 0) return 0;
    if (str[0] == '-') {
        if (len > 11) return 0;
        if (len == 11 && strcmp(str, "-2147483648") > 0) return 0;
    } else {
        if (len > 10) return 0;
        if (len == 10 && strcmp(str, "2147483647") > 0) return 0;
    }
    return 1;
}

int is_string_expr(Ast* ast) {
    if (!ast) return 0;
    return ast->kind == AST_STRING || ast->kind == AST_INTERP_STRING;
}

int is_array_expr(Ast* ast) {
    if (!ast) return 0;
    return ast->kind == AST_ARRAY;
}

int is_dict_expr(Ast* ast) {
    if (!ast) return 0;
    return ast->kind == AST_DICT;
}

int is_var_expr(Ast* ast) {
    if (!ast) return 0;
    return ast->kind == AST_VAR;
}

int is_number_expr(Ast* ast) {
    if (!ast) return 0;
    return ast->kind == AST_NUM;
}
