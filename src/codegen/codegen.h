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

// 前向声明 - 表达式生成
void gen_expr(CodeGen* gen, Ast* ast);

// 前向声明 - 语句生成
void gen_stmt(CodeGen* gen, Ast* ast);
void gen_block(CodeGen* gen, Ast* ast);
void gen_if(CodeGen* gen, Ast* ast);  // if 语句和表达式共用

// 前向声明 - 函数生成
void gen_func(CodeGen* gen, Ast* ast);

// 前向声明 - 模块语句生成
void gen_stmt_module(CodeGen* gen, Ast* ast);
void gen_block_module(CodeGen* gen, Ast* ast);

// 前向声明 - 赋值相关
void gen_assign(CodeGen* gen, Ast* ast);
void gen_compound_assign(CodeGen* gen, Ast* ast);

// 前向声明 - 导入
void gen_import_inline(CodeGen* gen, Ast* ast);

// Emit 辅助函数声明
void emit_byte(CodeGen* gen, uint8_t byte, int line);
void emit_bytes(CodeGen* gen, uint8_t byte1, uint8_t byte2, int line);
void emit_bytes_2(CodeGen* gen, uint8_t opcode, int operand, int line);
void emit_byte_imm(CodeGen* gen, uint8_t opcode, int8_t imm, int line);  // 单字节立即数操作码
int emit_jump(CodeGen* gen, uint8_t instruction, int line);
void patch_jump(CodeGen* gen, int offset);
void patch_jump_to(CodeGen* gen, int offset, int target);
void emit_loop(CodeGen* gen, int loop_start, int line);
int make_constant(CodeGen* gen, Value value);
void emit_constant(CodeGen* gen, Value value, int line);
void emit_native(CodeGen* gen, int constant, int line);
void emit_closure(CodeGen* gen, int constant, int line);
void emit_get_global(CodeGen* gen, int index, int line);
void emit_set_global(CodeGen* gen, int index, int line);
void emit_define_global(CodeGen* gen, int index, int line);
void emit_get_global_func(CodeGen* gen, int index, int line);
void emit_define_global_func(CodeGen* gen, int index, int line);
void emit_call(CodeGen* gen, int arg_count, int line);
void emit_tail_call(CodeGen* gen, int arg_count, int line);
void emit_call_native(CodeGen* gen, int name_const, int arg_count, int line);
void emit_tail_call_native(CodeGen* gen, int name_const, int arg_count, int line);
void emit_set_local_const(CodeGen* gen, int const_idx, int slot, int line);

// 比较跳转融合指令 emit 函数
// cmp_op: 0=EQ, 1=NE, 2=LT, 3=GT, 4=LE, 5=GE
// 返回值: offset 位置（用于后续 patch_jump）
int emit_cmpjmp_ll_int(CodeGen* gen, int cmp_op, int slot_a, int slot_b, int line);
int emit_cmpjmp_lg_int(CodeGen* gen, int cmp_op, int slot, int global_idx, int line);

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

// 函数内联辅助（定义在 codegen_inline.c 中）
int try_inline_call(CodeGen* gen, Ast* ast, Ast* func_def);
void inline_name_stack_push(const char* name);
void inline_name_stack_pop(void);
void inline_name_stack_reset(void);


#endif // LENO_CODEGEN_INTERNAL_H
