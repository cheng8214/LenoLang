// ============================================================================
// 寄存器式 codegen：import / use 内联
// ============================================================================

#include "codegen.h"

void gen_import_inline(CodeGen* gen, Ast* ast) {
    (void)gen; (void)ast;
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

// "变量类"表达式：变量 / 属性访问（`d.statements`）/ 索引（`a[i]`）/ 模块成员（`m.X`）。
// ⚠ 必须与栈式同口径（参考目录 D:\CLeno\Leno 的 codegen_utils.c，注释里就点了 d.statements）。
//   移植到寄存器式时这里曾被窄化成"只认 AST_VAR"，于是 `for ast.statements to st`
//   落进 gen_for 的**数值区间**分支：end 槽里放的是**数组**，FOR_PREP 拿
//   value_to_double(数组)=0 当上界 ⇒ 循环体一次都不进（examples/minilang 的
//   TinyLang 解释器整段静默不执行，寄存器版输出为空、栈式版正常）。
int is_var_expr(Ast* ast) {
    if (!ast) return 0;
    return ast->kind == AST_VAR || ast->kind == AST_INDEX ||
           ast->kind == AST_MODULE_ACCESS || ast->kind == AST_FIELD_ACCESS;
}

int is_number_expr(Ast* ast) {
    if (!ast) return 0;
    return ast->kind == AST_NUM;
}
