#ifndef LENO_CODEGEN_INTERNAL_H
#define LENO_CODEGEN_INTERNAL_H

#include "include/lenolang.h"
#include "include/leno_ast.h"
#include "include/leno_semantic.h"
#include "include/native.h"
#include "include/leno_codegen.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

// 前向声明模块加载函数
extern char* read_module_file(const char* file_path, const char* current_file);
extern void extract_module_exports(const char* source, char exports[][64], int* count);
extern ObjModule* load_module_file(const char* file_path, const char* current_file, const char* alias_name);

// 前向声明 - 类型推断（用于 struct 方法调用）
TypeInfo* infer_expr_type(Semantic* s, Ast* ast);

// ============================================================================
// 寄存器式 codegen 核心接口
// ============================================================================

// 表达式生成：结果写入指定寄存器 dst
void gen_expr_to(CodeGen* gen, Ast* ast, int dst);

// 表达式生成：借一个临时寄存器，返回寄存器号（调用方负责 reg_free）
int gen_expr(CodeGen* gen, Ast* ast);

// 语句生成
void gen_stmt(CodeGen* gen, Ast* ast);
void gen_block(CodeGen* gen, Ast* ast);
void gen_if(CodeGen* gen, Ast* ast);
// want_value=1（表达式位置）时结果写入 dst；want_value=0 时 dst 被忽略（传 -1）
void gen_if_ex(CodeGen* gen, Ast* ast, int want_value, int dst);

// 函数生成
void gen_func(CodeGen* gen, Ast* ast);

// 模块语句生成
void gen_stmt_module(CodeGen* gen, Ast* ast);
void gen_block_module(CodeGen* gen, Ast* ast);

// 赋值相关
void gen_assign(CodeGen* gen, Ast* ast);
void gen_compound_assign(CodeGen* gen, Ast* ast);
// 裸索引赋值（AST_INDEX_ASSIGN）：dst >= 0 时结果写入 dst，dst < 0 表示丢弃
void gen_index_assign(CodeGen* gen, Ast* ast, int dst);

// 导入
void gen_import_inline(CodeGen* gen, Ast* ast);

// 类型检查 / 安全转换（就地作用于 R[reg]，定义在 codegen_expr.c）
void emit_type_check_to(CodeGen* gen, int reg, TypeInfo* t, int line);
void emit_as_cast_to(CodeGen* gen, int reg, TypeInfo* t, int line);

// --- 表达式内部函数（定义在 codegen_expr.c） ---
void gen_binop(CodeGen* gen, Ast* ast, int dst);
void gen_unary(CodeGen* gen, Ast* ast, int dst);
void gen_call(CodeGen* gen, Ast* ast, int dst);
// 多返回值调用：结果连续落在 R[返回的 base] .. +nresults-1（调用方负责 reg_free_block）
int gen_call_multi(CodeGen* gen, Ast* ast, int nresults, int line);
void gen_interp_string(CodeGen* gen, Ast* ast, int dst);
void gen_module_access(CodeGen* gen, Ast* ast, int dst);
// 跨模块 .leno 调用的公共准备（模块对象 + callee + 实参 + 默认参数补齐）
//   返回 base，*out_expected 回填实参个数；调用方自己 emit_call + reg_free_block
int gen_module_call_prep(CodeGen* gen, Ast* mcall, int nresults, int* out_expected);
void gen_module_call(CodeGen* gen, Ast* ast, int dst);
void gen_struct_init(CodeGen* gen, Ast* ast, int dst);
void gen_safe_access(CodeGen* gen, Ast* ast, int dst);
int emit_jmp_if_true_ex(CodeGen* gen, int a, int line);

// ============================================================================
// 寄存器式 emit 函数声明（定义在 codegen_emit.c）
// ============================================================================

void emit_reg_iABC(CodeGen* gen, OpCode op, int a, int b, int c, int line);
void emit_reg_iABx(CodeGen* gen, OpCode op, int a, int bx, int line);
void emit_reg_iAsBx(CodeGen* gen, OpCode op, int a, int sbx, int line);

// 装载
void emit_loadk_to(CodeGen* gen, int dst, int const_idx, int line);
void emit_loadi_to(CodeGen* gen, int dst, int imm, int line);
void emit_loadf_to(CodeGen* gen, int dst, double val, int line);
void emit_loadnil_to(CodeGen* gen, int dst, int line);
void emit_loadtrue_to(CodeGen* gen, int dst, int line);
void emit_loadfalse_to(CodeGen* gen, int dst, int line);
void emit_mov(CodeGen* gen, int dst, int src, int line);

// 全局变量
void emit_getglobal_to(CodeGen* gen, int dst, int slot, int line);
void emit_setglobal(CodeGen* gen, int src, int slot, int line);
void emit_defglobal(CodeGen* gen, int src, int slot, int line);
void emit_getglobalfunc_to(CodeGen* gen, int dst, int slot, int line);
void emit_defglobalfunc(CodeGen* gen, int src, int slot, int line);

// upvalue / 闭包
void emit_getupval_to(CodeGen* gen, int dst, int idx, int line);
void emit_setupval(CodeGen* gen, int src, int idx, int line);
void emit_close(CodeGen* gen, int a, int count, int line);
void emit_closure_to(CodeGen* gen, int dst, int func_const_idx, int line);

// 算术
void emit_add(CodeGen* gen, int dst, int b, int c, int line);
void emit_sub(CodeGen* gen, int dst, int b, int c, int line);
void emit_mul(CodeGen* gen, int dst, int b, int c, int line);
void emit_div(CodeGen* gen, int dst, int b, int c, int line);
void emit_mod(CodeGen* gen, int dst, int b, int c, int line);
void emit_neg(CodeGen* gen, int dst, int b, int line);
void emit_not(CodeGen* gen, int dst, int b, int line);

// int 特化
void emit_add_int(CodeGen* gen, int dst, int b, int c, int line);
void emit_sub_int(CodeGen* gen, int dst, int b, int c, int line);
// 立即数版加减（imm ∈ [-128,127]）：R[dst] = R[b] ± imm
void emit_add_int_imm(CodeGen* gen, int dst, int b, int imm, int line);
void emit_sub_int_imm(CodeGen* gen, int dst, int b, int imm, int line);
void emit_lt_int_imm(CodeGen* gen, int dst, int b, int imm, int line);
void emit_gt_int_imm(CodeGen* gen, int dst, int b, int imm, int line);
void emit_le_int_imm(CodeGen* gen, int dst, int b, int imm, int line);
void emit_ge_int_imm(CodeGen* gen, int dst, int b, int imm, int line);
void emit_call_global(CodeGen* gen, int base, int nargs, int slot, int line);
void emit_mul_int(CodeGen* gen, int dst, int b, int c, int line);
void emit_neg_int(CodeGen* gen, int dst, int b, int line);

// float 特化
void emit_add_f(CodeGen* gen, int dst, int b, int c, int line);
void emit_sub_f(CodeGen* gen, int dst, int b, int c, int line);
void emit_mul_f(CodeGen* gen, int dst, int b, int c, int line);
void emit_div_f(CodeGen* gen, int dst, int b, int c, int line);
void emit_neg_f(CodeGen* gen, int dst, int b, int line);

// 比较
void emit_eq(CodeGen* gen, int dst, int b, int c, int line);
void emit_lt(CodeGen* gen, int dst, int b, int c, int line);
void emit_gt(CodeGen* gen, int dst, int b, int c, int line);
void emit_le(CodeGen* gen, int dst, int b, int c, int line);
void emit_ge(CodeGen* gen, int dst, int b, int c, int line);
void emit_neq(CodeGen* gen, int dst, int b, int c, int line);
void emit_lt_int(CodeGen* gen, int dst, int b, int c, int line);
void emit_gt_int(CodeGen* gen, int dst, int b, int c, int line);
void emit_le_int(CodeGen* gen, int dst, int b, int c, int line);
void emit_ge_int(CodeGen* gen, int dst, int b, int c, int line);
void emit_eq_int(CodeGen* gen, int dst, int b, int c, int line);

// 位运算
void emit_bitand(CodeGen* gen, int dst, int b, int c, int line);
void emit_bitor(CodeGen* gen, int dst, int b, int c, int line);
void emit_bitxor(CodeGen* gen, int dst, int b, int c, int line);
void emit_bitnot(CodeGen* gen, int dst, int b, int line);
void emit_shl(CodeGen* gen, int dst, int b, int c, int line);
void emit_shr(CodeGen* gen, int dst, int b, int c, int line);
void emit_ushr(CodeGen* gen, int dst, int b, int c, int line);

// 类型转换
void emit_cast_float(CodeGen* gen, int dst, int b, int line);
void emit_cast_int(CodeGen* gen, int dst, int b, int line);
void emit_cast_string(CodeGen* gen, int dst, int b, int line);
void emit_is_null(CodeGen* gen, int dst, int b, int line);

// 字符串拼接
void emit_strcat(CodeGen* gen, int dst, int b, int c, int line);

// 跳转
int emit_jmp(CodeGen* gen, int line);
int emit_jmp_if_false(CodeGen* gen, int a, int line);
// 「比较 + 条件跳转」融合（T10-①）：返回跳转偏移回填位置（失败返回 -1 ⇒ 走原路径）
//   want_true = 0：比较为**假**则跳（if / while 入口用，与 JMP_IF_FALSE 同向）
//   want_true = 1：比较为**真**则跳（while 回边用 —— 这样回边不必再跟一条独立 OP_JMP）
int emit_cmpjmp(CodeGen* gen, OpCode op, int a, int b, int is_imm, int want_true, int line);
int try_emit_cmpjmp(CodeGen* gen, Ast* cond, int want_true, int line);
// 语句位置的 `arr.add(x)` ⇒ OP_ARRAY_APPEND(need_result=0)（T10-②）成功返回 1
int try_emit_stmt_array_add(CodeGen* gen, Ast* e);
// 语句位置的 `i++`/`++i`/`i--`（局部量/参数）⇒ 只发 OP_INC/OP_DEC（省掉搬旧值的 MOV）
int try_emit_stmt_incdec(CodeGen* gen, Ast* e);
// 多字段累加融合：`s.cx + s.cy + ...`（同一对象的 float 字段）⇒ OP_ACC_FIELDS，成功返回 1
int try_emit_acc_fields(CodeGen* gen, Ast* ast, int dst);
// 写路径「直取变量寄存器」的配套判据（定义在 codegen_expr.c，供 codegen_stmt.c 的
// `arr[i] = v` / `obj.f = v` 复用）：
//   direct_local_reg —— 是普通局部变量/参数则返回它自己的寄存器号，否则 -1
//   ast_may_write_slot —— 该表达式是否会**改写**槽位 slot（保守：含调用/赋值一律返回 1）
int direct_local_reg(Ast* e);
int ast_may_write_slot(Ast* ast, int slot);
void emit_mul_int_imm(CodeGen* gen, int dst, int b, int imm, int line);
// 浮点有序比较特化（T10-④）
void emit_lt_f(CodeGen* gen, int dst, int b, int c, int line);
void emit_le_f(CodeGen* gen, int dst, int b, int c, int line);
void emit_gt_f(CodeGen* gen, int dst, int b, int c, int line);
void emit_ge_f(CodeGen* gen, int dst, int b, int c, int line);
int emit_jmp_if_true(CodeGen* gen, int a, int line);
void patch_jmp(CodeGen* gen, int pos);
void patch_jmp_to(CodeGen* gen, int pos, int target);
// 写入 2 字节 sBx 数据（OP_FOR_PREP / OP_FOR_LOOP 紧随指令的跳转偏移）
void patch_sbx_at(CodeGen* gen, int pos, int sbx);
void emit_loop(CodeGen* gen, int target, int line);

// 自增自减
void emit_inc(CodeGen* gen, int dst, int b, int line);
void emit_dec(CodeGen* gen, int dst, int b, int line);

// 调用 / 返回
void emit_call(CodeGen* gen, int a, int nargs, int nresults, int line);
void emit_call_native(CodeGen* gen, int dst, int name_const_idx, int nargs, int line);
void emit_return(CodeGen* gen, int a, int nresults, int line);
void emit_return_multi(CodeGen* gen, int a, int count_reg, int line);
void emit_tail_call(CodeGen* gen, int a, int nargs, int line);

// 常量表辅助
int make_constant(CodeGen* gen, Value value);

// 数组 add 操作生成（公共函数，供 codegen_stmt.c 检测表达式语句优化）
void gen_array_add(CodeGen* gen, Ast* receiver_ast, Ast* arg_ast, int need_result, int line);
void gen_array_add_by_symbol(CodeGen* gen, Symbol* var_sym, Ast* arg_ast, int need_result, int line);

// 工具函数声明
int bigint_str_fits_in_int32(const char* str);
int is_string_expr(Ast* ast);
int is_array_expr(Ast* ast);
int is_dict_expr(Ast* ast);
int is_var_expr(Ast* ast);
int is_number_expr(Ast* ast);

typedef struct {
    int has_main;
    int main_index;
} MainFuncInfo;

MainFuncInfo find_main_function(Semantic* sem);

#endif // LENO_CODEGEN_INTERNAL_H
