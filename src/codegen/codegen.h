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
