#include "codegen.h"
#include <stdio.h>
#include "include/leno_error.h"

// 链式循环上下文操作（堆分配，无嵌套深度限制）
static LoopContext* loop_push(CodeGen* gen) {
    LoopContextNode* node = (LoopContextNode*)malloc(sizeof(LoopContextNode));
    if (!node) return NULL;
    node->ctx.break_count = 0;
    node->ctx.continue_count = 0;
    node->prev = gen->loop_head;
    gen->loop_head = node;
    gen->loop_count++;
    return &node->ctx;
}

static void loop_pop(CodeGen* gen) {
    if (!gen->loop_head) return;
    LoopContextNode* node = gen->loop_head;
    gen->loop_head = node->prev;
    free(node);
    gen->loop_count--;
}

static LoopContext* loop_current(CodeGen* gen) {
    return gen->loop_head ? &gen->loop_head->ctx : NULL;
}

Value ast_default_to_value(Ast* expr);

Value ast_default_to_value(Ast* expr) {
    if (!expr) return val_null();
    switch (expr->kind) {
        case AST_NUM:
            if (expr->u.num.is_float) return val_float(expr->u.num.value);
            return val_num(expr->u.num.value);
        case AST_STRING: {
            ObjString* str = str_copy(expr->u.string.value, expr->u.string.len);
            return val_obj((Object*)str);
        }
        case AST_BOOL:
            return val_bool(expr->u.boolean);
        case AST_NULL:
            return val_null();
        case AST_UNARY: {
            // 支持一元负号常量（如 -1、-3.14）
            Value operand_val = ast_default_to_value(expr->u.unary.operand);
            if (val_is_null(operand_val)) return val_null();
            if (expr->u.unary.op == TOK_MINUS) {
                if (val_is_int(operand_val)) return val_num(-val_as_int(operand_val));
                if (val_is_float(operand_val)) return val_float(-val_as_double(operand_val));
            }
            return val_null();
        }
        case AST_ARRAY: {
            ObjArray* arr = arr_new(expr->u.array.count > 0 ? expr->u.array.count : 1);
            for (int i = 0; i < expr->u.array.count; i++) {
                arr_grow(arr);
                arr_write(arr, i, ast_default_to_value(expr->u.array.items[i]));
            }
            return val_obj((Object*)arr);
        }
        case AST_DICT: {
            ObjDict* dict = dict_new(expr->u.dict.count > 0 ? expr->u.dict.count : 8);
            for (int i = 0; i < expr->u.dict.count; i++) {
                Value key_val = ast_default_to_value(expr->u.dict.entries[i].key);
                Value val = ast_default_to_value(expr->u.dict.entries[i].value);
                dict_set(dict, key_val, val);
            }
            return val_obj((Object*)dict);
        }
        default:
            return val_null();
    }
}

void gen_func(CodeGen* gen, Ast* ast);

// 辅助函数：获取表达式的类型 kind
static TypeKind get_expr_type_kind(Ast* ast) {
    if (!ast || !ast->cached_type) return TYPE_ANY;
    return ast->cached_type->kind;
}

static void gen_var_decl(CodeGen* gen, Ast* ast);
static void gen_return(CodeGen* gen, Ast* ast);
static void gen_while(CodeGen* gen, Ast* ast);
static void gen_for(CodeGen* gen, Ast* ast);
static void gen_switch(CodeGen* gen, Ast* ast);
static void gen_expr_stmt(CodeGen* gen, Ast* ast);

static void gen_switch(CodeGen* gen, Ast* ast) {
    gen_expr(gen, ast->u.switch_.expr);

    int case_count = ast->u.switch_.case_count;
    int has_default = ast->u.switch_.default_body ? 1 : 0;

    if (case_count == 0 && !has_default) {
        emit_byte(gen, OP_POP, ast->line);
        return;
    }

    typedef struct {
        int* match_jumps;
        int match_jump_count;
        int body_start;
    } CaseJumpInfo;

    CaseJumpInfo* case_infos = malloc(sizeof(CaseJumpInfo) * case_count);
    int* case_end_jumps = malloc(sizeof(int) * case_count);
    int case_end_jump_count = 0;

    for (int i = 0; i < case_count; i++) {
        int value_count = ast->u.switch_.cases[i].values.count;
        int type_count = ast->u.switch_.cases[i].match_type_count;
        // 分配足够容纳多类型检查的 match_jumps
        int max_jumps = value_count > type_count ? value_count : type_count;
        if (max_jumps < 1) max_jumps = 1;
        case_infos[i].match_jumps = malloc(sizeof(int) * max_jumps);
        case_infos[i].match_jump_count = 0;

        if (ast->u.switch_.cases[i].is_type_match) {
            // case is Type 模式：对每个匹配类型生成类型检查（逗号合并时多个类型跳转同一 body）
            int mt_count = type_count > 0 ? type_count : 1;
            TypeInfo** mt_arr = ast->u.switch_.cases[i].match_types;
            if (!mt_arr) {
                // 单类型回退（match_types 未设置时用 match_type）
                mt_count = 1;
                mt_arr = &ast->u.switch_.cases[i].match_type;
            }
            for (int mi = 0; mi < mt_count; mi++) {
                TypeInfo* mt = mt_arr[mi];
                emit_byte(gen, OP_DUP, ast->line);
                emit_byte(gen, OP_TYPE_CHECK, ast->line);
                if (mt && mt->kind == TYPE_FACE && mt->struct_name) {
                    ObjString* face_name_str = str_copy(mt->struct_name,
                                                         strlen(mt->struct_name));
                    int face_name_const = make_constant(gen, val_obj((Object*)face_name_str));
                    emit_byte(gen, (uint8_t)TYPE_FACE, ast->line);
                    emit_byte(gen, (face_name_const >> 8) & 0xff, ast->line);
                    emit_byte(gen, face_name_const & 0xff, ast->line);
                } else if (mt && mt->kind == TYPE_STRUCT && mt->struct_name) {
                    ObjString* struct_name_str = str_copy(mt->struct_name,
                                                           strlen(mt->struct_name));
                    int struct_name_const = make_constant(gen, val_obj((Object*)struct_name_str));
                    emit_byte(gen, (uint8_t)TYPE_STRUCT, ast->line);
                    emit_byte(gen, (struct_name_const >> 8) & 0xff, ast->line);
                    emit_byte(gen, struct_name_const & 0xff, ast->line);
                } else if (mt && mt->kind == TYPE_ENUM && mt->struct_name) {
                    ObjString* enum_name_str = str_copy(mt->struct_name,
                                                         strlen(mt->struct_name));
                    int enum_name_const = make_constant(gen, val_obj((Object*)enum_name_str));
                    emit_byte(gen, (uint8_t)TYPE_ENUM, ast->line);
                    emit_byte(gen, (enum_name_const >> 8) & 0xff, ast->line);
                    emit_byte(gen, enum_name_const & 0xff, ast->line);
                } else {
                    if (mt) {
                        emit_byte(gen, (uint8_t)mt->kind, ast->line);
                        if (mt->element_type) {
                            emit_byte(gen, (uint8_t)mt->element_type->kind, ast->line);
                        } else {
                            emit_byte(gen, (uint8_t)TYPE_ANY, ast->line);
                        }
                    } else {
                        emit_byte(gen, (uint8_t)TYPE_ANY, ast->line);
                        emit_byte(gen, (uint8_t)TYPE_ANY, ast->line);
                    }
                }
                case_infos[i].match_jumps[case_infos[i].match_jump_count++] =
                    emit_jump(gen, OP_JUMP_IF_TRUE, ast->line);
                emit_byte(gen, OP_POP, ast->line);
            }
        } else if (value_count > 0) {
            for (int j = 0; j < value_count; j++) {
                emit_byte(gen, OP_DUP, ast->line);

                gen_expr(gen, ast->u.switch_.cases[i].values.items[j]);

                emit_byte(gen, OP_EQ, ast->line);

                case_infos[i].match_jumps[case_infos[i].match_jump_count++] =
                    emit_jump(gen, OP_JUMP_IF_TRUE, ast->line);

                emit_byte(gen, OP_POP, ast->line);
            }
        } else {
            emit_byte(gen, OP_DUP, ast->line);
            emit_byte(gen, OP_NULL, ast->line);
            emit_byte(gen, OP_EQ, ast->line);
            case_infos[i].match_jumps[case_infos[i].match_jump_count++] =
                emit_jump(gen, OP_JUMP_IF_TRUE, ast->line);
            emit_byte(gen, OP_POP, ast->line);
        }
    }

    int default_jump = emit_jump(gen, OP_JUMP, ast->line);

    for (int i = 0; i < case_count; i++) {
        case_infos[i].body_start = gen->chunk->len;

        for (int j = 0; j < case_infos[i].match_jump_count; j++) {
            patch_jump(gen, case_infos[i].match_jumps[j]);
        }

        emit_byte(gen, OP_POP, ast->line);

        // 解构字段提取：case is Point(x, y) → 从 guard_var 提取字段赋给 x, y
        if (ast->u.switch_.cases[i].destructure_count > 0 &&
            ast->u.switch_.cases[i].match_type &&
            ast->u.switch_.cases[i].match_type->kind == TYPE_STRUCT &&
            ast->u.switch_.cases[i].match_type->struct_name &&
            ast->u.switch_.cases[i].destructure_indices &&
            ast->u.switch_.cases[i].destructure_field_names) {
            for (int di = 0; di < ast->u.switch_.cases[i].destructure_count; di++) {
                const char* var_name = ast->u.switch_.cases[i].destructure_vars[di];
                const char* field_name = ast->u.switch_.cases[i].destructure_field_names[di];
                if (strcmp(var_name, "_") == 0 || !field_name) continue;
                int local_idx = ast->u.switch_.cases[i].destructure_indices[di];
                if (local_idx < 0) continue;
                // 加载 guard_var
                SymRef* gref = &ast->u.switch_.cases[i].guard_var_ref;
                if (gref->kind == SYM_UPVALUE) {
                    emit_bytes_2(gen, OP_GET_UPVALUE, gref->index, ast->line);
                } else if (gref->kind == SYM_GLOBAL) {
                    emit_byte(gen, OP_GET_GLOBAL, ast->line);
                    int gname_const = make_constant(gen, val_obj((Object*)str_copy(gref->name, strlen(gref->name))));
                    emit_byte(gen, (gname_const >> 8) & 0xff, ast->line);
                    emit_byte(gen, gname_const & 0xff, ast->line);
                } else {
                    emit_bytes_2(gen, OP_GET_LOCAL, gref->index, ast->line);
                }
                // 通过字段名获取字段值：OP_CONST + OP_INDEX
                ObjString* fname_str = str_copy(field_name, strlen(field_name));
                int fname_const = make_constant(gen, val_obj((Object*)fname_str));
                emit_byte(gen, OP_CONST, ast->line);
                emit_byte(gen, (fname_const >> 8) & 0xff, ast->line);
                emit_byte(gen, fname_const & 0xff, ast->line);
                emit_byte(gen, OP_INDEX, ast->line);
                // 存入解构变量
                emit_bytes_2(gen, OP_SET_LOCAL_POP, local_idx, ast->line);
            }
        }

        if (ast->u.switch_.cases[i].body->kind == AST_BLOCK) {
            gen_block(gen, ast->u.switch_.cases[i].body);
        } else {
            gen_stmt(gen, ast->u.switch_.cases[i].body);
        }

        case_end_jumps[case_end_jump_count++] = emit_jump(gen, OP_JUMP, ast->line);
    }

    patch_jump(gen, default_jump);

    if (has_default) {
        if (ast->u.switch_.default_body->kind == AST_BLOCK) {
            gen_block(gen, ast->u.switch_.default_body);
        } else {
            gen_stmt(gen, ast->u.switch_.default_body);
        }
    }

    for (int i = 0; i < case_end_jump_count; i++) {
        patch_jump(gen, case_end_jumps[i]);
    }

    emit_byte(gen, OP_POP, ast->line);

    for (int i = 0; i < case_count; i++) {
        free(case_infos[i].match_jumps);
    }
    free(case_infos);
    free(case_end_jumps);
}

void gen_if(CodeGen* gen, Ast* ast) {
    // 检查是否是类型守卫（if a is type）
    // 或者是否是否定类型守卫（if a not is type）
    int is_negated = 0;
    if (ast->u.if_.guard_var != NULL && ast->u.if_.cond && ast->u.if_.cond->kind == AST_TYPE_CHECK) {
        // 条件已经是 AST_TYPE_CHECK，直接通过 gen_expr 生成（避免与 guard_var 路径重复生成）
        gen_expr(gen, ast->u.if_.cond);
    } else if (ast->u.if_.guard_var != NULL && ast->u.if_.cond &&
               (ast->u.if_.cond->kind == AST_UNARY ||
                ast->u.if_.cond->kind == AST_TYPE_CHECK)) {
        // 仅当条件是纯类型守卫（AST_TYPE_CHECK）或否定类型守卫（!type_check）时
        // 才走快速通道。对于复合条件（如 x is int and x > 5），guard_var 可能
        // 由语义分析重新设置，必须用 gen_expr 生成完整条件。
        // 检查条件表达式是否是否定（not）
        if (ast->u.if_.cond->kind == AST_UNARY && 
            ast->u.if_.cond->u.unary.op == TOK_NOT) {
            is_negated = 1;
        }
        
        // 生成运行时类型检查
        // 使用语义分析阶段保存的符号引用
        SymRef* ref = &ast->u.if_.guard_var_ref;
        // 生成获取变量的字节码
        if (ref->kind == SYM_LOCAL || ref->kind == SYM_PARAM) {
            emit_bytes_2(gen, OP_GET_LOCAL, ref->index, ast->line);
        } else if (ref->kind == SYM_GLOBAL) {
            emit_get_global(gen, ref->index, ast->line);
        } else if (ref->kind == SYM_UPVALUE) {
            emit_bytes_2(gen, OP_GET_UPVALUE, ref->index, ast->line);
        }

        // 生成类型检查字节码
        if (ast->u.if_.guard_type && ast->u.if_.guard_type->kind == TYPE_FACE && ast->u.if_.guard_type->struct_name) {
            ObjString* face_name_str = str_copy(ast->u.if_.guard_type->struct_name,
                                                 strlen(ast->u.if_.guard_type->struct_name));
            int face_name_const = make_constant(gen, val_obj((Object*)face_name_str));
            emit_byte(gen, OP_TYPE_CHECK, ast->line);
            emit_byte(gen, (uint8_t)TYPE_FACE, ast->line);
            emit_byte(gen, (face_name_const >> 8) & 0xff, ast->line);
            emit_byte(gen, face_name_const & 0xff, ast->line);
        } else if (ast->u.if_.guard_type && ast->u.if_.guard_type->kind == TYPE_STRUCT && ast->u.if_.guard_type->struct_name) {
            ObjString* struct_name_str = str_copy(ast->u.if_.guard_type->struct_name,
                                                   strlen(ast->u.if_.guard_type->struct_name));
            int struct_name_const = make_constant(gen, val_obj((Object*)struct_name_str));
            emit_byte(gen, OP_TYPE_CHECK, ast->line);
            emit_byte(gen, (uint8_t)TYPE_STRUCT, ast->line);
            emit_byte(gen, (struct_name_const >> 8) & 0xff, ast->line);
            emit_byte(gen, struct_name_const & 0xff, ast->line);
        } else if (ast->u.if_.guard_type && ast->u.if_.guard_type->kind == TYPE_ENUM && ast->u.if_.guard_type->struct_name) {
            ObjString* enum_name_str = str_copy(ast->u.if_.guard_type->struct_name,
                                                 strlen(ast->u.if_.guard_type->struct_name));
            int enum_name_const = make_constant(gen, val_obj((Object*)enum_name_str));
            emit_byte(gen, OP_TYPE_CHECK, ast->line);
            emit_byte(gen, (uint8_t)TYPE_ENUM, ast->line);
            emit_byte(gen, (enum_name_const >> 8) & 0xff, ast->line);
            emit_byte(gen, enum_name_const & 0xff, ast->line);
        } else {
            emit_byte(gen, OP_TYPE_CHECK, ast->line);
            if (ast->u.if_.guard_type) {
                emit_byte(gen, (uint8_t)ast->u.if_.guard_type->kind, ast->line);
                if (ast->u.if_.guard_type->element_type) {
                    emit_byte(gen, (uint8_t)ast->u.if_.guard_type->element_type->kind, ast->line);
                } else {
                    emit_byte(gen, (uint8_t)TYPE_ANY, ast->line);
                }
            } else {
                emit_byte(gen, (uint8_t)TYPE_ANY, ast->line);
                emit_byte(gen, (uint8_t)TYPE_ANY, ast->line);
            }
        }
        
        // 如果是否定类型守卫，添加 NOT 操作
        if (is_negated) {
            emit_byte(gen, OP_NOT, ast->line);
        }
    } else {
        // 普通条件表达式
        gen_expr(gen, ast->u.if_.cond);
    }

    int then_jump = emit_jump(gen, OP_JUMP_IF_FALSE, ast->line);
    emit_byte(gen, OP_POP, ast->line);
    
    // 区分语句 if 和表达式 if
    // 表达式 if 的 then/else 不是 AST_BLOCK，而是普通表达式
    if (ast->u.if_.then->kind == AST_BLOCK) {
        gen_stmt(gen, ast->u.if_.then);
    } else {
        gen_expr(gen, ast->u.if_.then);
    }

    int else_jump = emit_jump(gen, OP_JUMP, ast->line);
    patch_jump(gen, then_jump);
    emit_byte(gen, OP_POP, ast->line);

    if (ast->u.if_.else_) {
        if (ast->u.if_.else_->kind == AST_BLOCK) {
            gen_stmt(gen, ast->u.if_.else_);
        } else {
            gen_expr(gen, ast->u.if_.else_);
        }
    }
    patch_jump(gen, else_jump);
}

static void gen_while(CodeGen* gen, Ast* ast) {
    LoopContextNode* node = (LoopContextNode*)malloc(sizeof(LoopContextNode));
    if (!node) return;
    node->ctx.break_count = 0;
    node->ctx.continue_count = 0;
    node->prev = gen->loop_head;
    gen->loop_head = node;
    gen->loop_count++;
    LoopContext* loop = &node->ctx;

    int loop_start = gen->chunk->len;
    loop->continue_target = gen->chunk->len;

    // 优化: while true { } - 无需条件检查
    if (ast->u.while_.cond->kind == AST_BOOL && ast->u.while_.cond->u.boolean == 1) {
        // 无限循环，直接生成循环体
        gen_stmt(gen, ast->u.while_.body);
        emit_loop(gen, loop_start, ast->line);
        // 无限循环没有退出跳转需要修补
        for (int i = 0; i < loop->break_count; i++) {
            patch_jump(gen, loop->break_jumps[i]);
        }
        // 回填 continue 跳转到循环体开始位置
        for (int i = 0; i < loop->continue_count; i++) {
            patch_jump_to(gen, loop->continue_jumps[i], loop_start);
        }
        loop_pop(gen);
        return;
    }

    // 优化: while false { } - 永不执行的循环，直接跳过
    if (ast->u.while_.cond->kind == AST_BOOL && ast->u.while_.cond->u.boolean == 0) {
        // 循环体永不执行，直接跳过
        loop_pop(gen);
        return;
    }

    gen_expr(gen, ast->u.while_.cond);
    int exit_jump = emit_jump(gen, OP_JUMP_IF_FALSE, ast->line);
    emit_byte(gen, OP_POP, ast->line);

    gen_stmt(gen, ast->u.while_.body);

    emit_loop(gen, loop_start, ast->line);
    patch_jump(gen, exit_jump);
    emit_byte(gen, OP_POP, ast->line);

    for (int i = 0; i < loop->break_count; i++) {
        patch_jump(gen, loop->break_jumps[i]);
    }

    // 回填 continue 跳转到循环条件检查位置（loop_start）
    for (int i = 0; i < loop->continue_count; i++) {
        patch_jump_to(gen, loop->continue_jumps[i], loop_start);
    }

    loop_pop(gen);
}

static void gen_for_iter(CodeGen* gen, Ast* ast, int loop_var_slot) {
    LoopContext* loop = loop_current(gen);

    gen_expr(gen, ast->u.for_.end);

    int obj_slot = ast->u.for_.end_index;
    emit_bytes_2(gen, OP_SET_LOCAL, obj_slot, ast->line);
    emit_byte(gen, OP_POP, ast->line);

    emit_byte(gen, OP_ZERO, ast->line);
    int idx_slot = ast->u.for_.counter_index;
    emit_bytes_2(gen, OP_SET_LOCAL, idx_slot, ast->line);
    emit_byte(gen, OP_POP, ast->line);

    int loop_start = gen->chunk->len;

    emit_bytes_2(gen, OP_GET_LOCAL, idx_slot, ast->line);
    emit_bytes_2(gen, OP_GET_LOCAL, obj_slot, ast->line);
    emit_byte(gen, OP_LENGTH, ast->line);
    emit_byte(gen, OP_LT, ast->line);

    int exit_jump = emit_jump(gen, OP_JUMP_IF_FALSE, ast->line);
    emit_byte(gen, OP_POP, ast->line);

    emit_bytes_2(gen, OP_GET_LOCAL, obj_slot, ast->line);
    emit_bytes_2(gen, OP_GET_LOCAL, idx_slot, ast->line);
    emit_byte(gen, OP_ITER_GET, ast->line);

    emit_bytes_2(gen, OP_SET_LOCAL, loop_var_slot, ast->line);
    emit_byte(gen, OP_POP, ast->line);
    
    // 如果存在索引变量，将当前索引赋值给它
    if (ast->u.for_.index_var_name) {
        // 检查是否是字典/struct遍历
        int is_dict_iter = is_dict_expr(ast->u.for_.end);
        if (!is_dict_iter && ast->u.for_.end->kind == AST_VAR) {
            TypeKind var_type = ast->u.for_.end->u.var.ref.type_kind;
            if (var_type == TYPE_DICT || var_type == TYPE_STRUCT) {
                is_dict_iter = 1;
            }
        }
        
        if (is_dict_iter) {
            // 字典遍历：获取值而不是索引
            emit_bytes_2(gen, OP_GET_LOCAL, obj_slot, ast->line);
            emit_bytes_2(gen, OP_GET_LOCAL, idx_slot, ast->line);
            emit_byte(gen, OP_ITER_GET_VALUE, ast->line);
            emit_bytes_2(gen, OP_SET_LOCAL, ast->u.for_.index_var_index, ast->line);
            emit_byte(gen, OP_POP, ast->line);
        } else {
            // 数组/字符串遍历：获取索引
            emit_bytes_2(gen, OP_GET_LOCAL, idx_slot, ast->line);
            emit_bytes_2(gen, OP_SET_LOCAL, ast->u.for_.index_var_index, ast->line);
            emit_byte(gen, OP_POP, ast->line);
        }
    }

    gen_stmt(gen, ast->u.for_.body);

    // continue 应该跳转到索引递增的位置
    int continue_target = gen->chunk->len;
    loop->continue_target = continue_target;

    emit_bytes_2(gen, OP_GET_LOCAL, idx_slot, ast->line);
    emit_byte(gen, OP_ONE, ast->line);
    emit_byte(gen, OP_ADD, ast->line);
    emit_bytes_2(gen, OP_SET_LOCAL, idx_slot, ast->line);
    emit_byte(gen, OP_POP, ast->line);

    emit_loop(gen, loop_start, ast->line);

    patch_jump(gen, exit_jump);
    emit_byte(gen, OP_POP, ast->line);

    // 回填 continue 跳转到索引递增的位置
    for (int i = 0; i < loop->continue_count; i++) {
        patch_jump_to(gen, loop->continue_jumps[i], continue_target);
    }
}

static void gen_for(CodeGen* gen, Ast* ast) {
    LoopContext* loop = loop_push(gen);
    if (!loop) return;

    int loop_var_slot = ast->u.for_.loop_var_index;
    int end_slot = ast->u.for_.end_index;
    int step_slot = ast->u.for_.step_index;
    int start_slot = ast->u.for_.start_index;
    int counter_slot = ast->u.for_.counter_index;

    Ast* end_expr = ast->u.for_.end;

    if (!ast->u.for_.start && ast->u.for_.var_name) {
        // 对字面量数组/字符串/字典，或变量使用迭代循环
        // 注意：变量可能是数组/字符串/字典，需要运行时遍历
        // 纯数字应该使用 start:end 范围语法
        int is_iterable = is_string_expr(end_expr) || is_array_expr(end_expr) ||
                          is_dict_expr(end_expr);
        
        // 如果是变量，检查其类型 - 如果是数字类型则使用数值循环
        if (!is_iterable && is_var_expr(end_expr)) {
            if (end_expr->kind == AST_VAR) {
                // 获取变量类型
                TypeKind var_type = end_expr->u.var.ref.type_kind;
                // 如果是明确的数字类型，使用数值循环
                if (var_type == TYPE_INT || var_type == TYPE_FLOAT) {
                    is_iterable = 0;  // 是数字类型，继续执行数值循环代码
                } else if (var_type == TYPE_ARRAY || var_type == TYPE_STRING || var_type == TYPE_DICT) {
                    is_iterable = 1;  // 是容器类型，使用迭代循环
                } else {
                    // TYPE_INFER/TYPE_ANY/其他：保守处理，使用迭代循环
                    is_iterable = 1;
                }
            } else {
                // 复杂表达式（如属性访问、索引等），保守处理
                is_iterable = 1;
            }
        }
        
        if (is_iterable) {
            gen_for_iter(gen, ast, loop_var_slot);

            for (int i = 0; i < loop->break_count; i++) {
                patch_jump(gen, loop->break_jumps[i]);
            }
            loop_pop(gen);
            return;
        }
    }

    int has_loop_var = ast->u.for_.var_name != NULL;

    // 初始化 start, end, step 到局部变量
    if (ast->u.for_.start) {
        gen_expr(gen, ast->u.for_.start);
    } else {
        emit_byte(gen, OP_ZERO, ast->line);
    }
    emit_bytes_2(gen, OP_SET_LOCAL_POP, start_slot, ast->line);

    gen_expr(gen, ast->u.for_.end);
    emit_bytes_2(gen, OP_SET_LOCAL_POP, end_slot, ast->line);

    if (ast->u.for_.step) {
        gen_expr(gen, ast->u.for_.step);
        emit_bytes_2(gen, OP_SET_LOCAL_POP, step_slot, ast->line);
    } else {
        // 方案 B：无显式 step 时默认正向 step=+1（不再根据 start/end 自动倒序）。
        // 倒序必须显式写 for A:B:-1。
        // 编译期提示：字面量区间 start > end 且无步长时，该循环现在不会执行（空循环）。
        if (ast->u.for_.start && ast->u.for_.start->kind == AST_NUM &&
            ast->u.for_.end && ast->u.for_.end->kind == AST_NUM &&
            ast->u.for_.start->u.num.value > ast->u.for_.end->u.num.value) {
            char wbuf[BUFFER_MEDIUM];
            snprintf(wbuf, sizeof(wbuf),
                     "for 区间 %g:%g 无显式步长，按正向规则不会执行（如需倒序请写 for A:B:-1）",
                     ast->u.for_.start->u.num.value,
                     ast->u.for_.end->u.num.value);
            warning_add(WARN_FOR_EMPTY_RANGE, ast->line, wbuf);
        }
        emit_byte(gen, OP_ONE, ast->line);
        emit_bytes_2(gen, OP_SET_LOCAL_POP, step_slot, ast->line);
    }

    // 所有数值循环使用专用字节码 OP_FOR_PREP 和 OP_FOR_LOOP
    // OP_FOR_PREP 布局: opcode(1) + 4个操作数slot + 1个inclusive + 2字节jump_offset = 8字节
    // OP_FOR_LOOP 布局: opcode(1) + 3个操作数slot + 1个inclusive + 2字节jump_offset = 7字节

    if (!has_loop_var) {
        // 无循环变量：使用 counter_slot 作为 loop_var
        int prep_start = gen->chunk->len;

        // OP_FOR_PREP: 初始化并检查条件
        emit_byte(gen, OP_FOR_PREP, ast->line);
        emit_byte(gen, start_slot, ast->line);
        emit_byte(gen, end_slot, ast->line);
        emit_byte(gen, step_slot, ast->line);
        emit_byte(gen, counter_slot, ast->line);
        emit_byte(gen, ast->u.for_.inclusive ? 1 : 0, ast->line);
        emit_byte(gen, 0, ast->line); // jump_offset 占位高字节
        emit_byte(gen, 0, ast->line); // jump_offset 占位低字节

        // 循环体开始位置（OP_FOR_PREP之后）
        int body_start = gen->chunk->len;

        // 预留 OP_FOR_LOOP 的位置（在循环体之后）
        // 先设置一个临时的 continue_target（循环体开始处）
        // 等生成完循环体后再更新为实际的 OP_FOR_LOOP 位置
        loop->continue_target = body_start;

        gen_stmt(gen, ast->u.for_.body);

        // 现在生成 OP_FOR_LOOP，并更新 continue_target
        int loop_insn_start = gen->chunk->len;
        emit_byte(gen, OP_FOR_LOOP, ast->line);
        emit_byte(gen, counter_slot, ast->line);
        emit_byte(gen, step_slot, ast->line);
        emit_byte(gen, end_slot, ast->line);
        emit_byte(gen, ast->u.for_.inclusive ? 1 : 0, ast->line);
        // 计算跳转偏移：从 OP_FOR_LOOP 结束位置跳回 body_start
        int after_loop_insn = gen->chunk->len + 2; // +2 是因为还要写入 2 字节偏移
        int back_offset = after_loop_insn - body_start;
        emit_byte(gen, (back_offset >> 8) & 0xFF, ast->line);
        emit_byte(gen, back_offset & 0xFF, ast->line);

        // 回填 continue 跳转到 OP_FOR_LOOP 的位置
        for (int i = 0; i < loop->continue_count; i++) {
            patch_jump_to(gen, loop->continue_jumps[i], loop_insn_start);
        }

        // 修补 OP_FOR_PREP 中的 forward jump_offset
        // 如果条件不满足，跳过整个循环体（从 OP_FOR_PREP 之后到循环结束）
        int after_loop = gen->chunk->len;
        int skip_offset = after_loop - (prep_start + 8); // 8 = OP_FOR_PREP(1) + 7操作数
        gen->chunk->code[prep_start + 6] = (skip_offset >> 8) & 0xFF;
        gen->chunk->code[prep_start + 7] = skip_offset & 0xFF;
    } else {
        // 有循环变量：使用 loop_var_slot
        int prep_start = gen->chunk->len;

        emit_byte(gen, OP_FOR_PREP, ast->line);
        emit_byte(gen, start_slot, ast->line);
        emit_byte(gen, end_slot, ast->line);
        emit_byte(gen, step_slot, ast->line);
        emit_byte(gen, loop_var_slot, ast->line);
        emit_byte(gen, ast->u.for_.inclusive ? 1 : 0, ast->line);
        emit_byte(gen, 0, ast->line);
        emit_byte(gen, 0, ast->line);

        int body_start = gen->chunk->len;

        gen_stmt(gen, ast->u.for_.body);

        // 生成 OP_FOR_LOOP
        int loop_insn_start = gen->chunk->len;
        emit_byte(gen, OP_FOR_LOOP, ast->line);
        emit_byte(gen, loop_var_slot, ast->line);
        emit_byte(gen, step_slot, ast->line);
        emit_byte(gen, end_slot, ast->line);
        emit_byte(gen, ast->u.for_.inclusive ? 1 : 0, ast->line);
        int after_loop_insn = gen->chunk->len + 2;
        int back_offset = after_loop_insn - body_start;
        emit_byte(gen, (back_offset >> 8) & 0xFF, ast->line);
        emit_byte(gen, back_offset & 0xFF, ast->line);

        // 回填 continue 跳转到 OP_FOR_LOOP 的位置
        for (int i = 0; i < loop->continue_count; i++) {
            patch_jump_to(gen, loop->continue_jumps[i], loop_insn_start);
        }

        int after_loop = gen->chunk->len;
        int skip_offset = after_loop - (prep_start + 8); // 8 = OP_FOR_PREP(1) + 7操作数
        gen->chunk->code[prep_start + 6] = (skip_offset >> 8) & 0xFF;
        gen->chunk->code[prep_start + 7] = skip_offset & 0xFF;
    }

    for (int i = 0; i < loop->break_count; i++) {
        patch_jump(gen, loop->break_jumps[i]);
    }

    loop_pop(gen);
}

static void gen_var_decl(CodeGen* gen, Ast* ast) {
    if (ast->u.var_decl.init) {
        gen_expr(gen, ast->u.var_decl.init);

        if (ast->u.var_decl.type) {
            TypeKind var_type = ast->u.var_decl.type->kind;
            if (var_type == TYPE_FLOAT) {
                emit_byte(gen, OP_CAST_FLOAT, ast->line);
            }
            else if (var_type == TYPE_INT) {
                emit_byte(gen, OP_CAST_INT, ast->line);
            }
            else if (var_type == TYPE_STRING) {
                emit_byte(gen, OP_CAST_STRING, ast->line);
            }
        }
    } else {
        // 检查是否是数组类型，如果是则创建空数组
        if (ast->u.var_decl.type && ast->u.var_decl.type->kind == TYPE_ARRAY) {
            emit_byte(gen, OP_ARRAY, ast->line);
            emit_byte(gen, 0, ast->line); // count 高字节
            emit_byte(gen, 0, ast->line); // count 低字节
        }
        // 检查是否是字典类型，如果是则创建空字典
        else if (ast->u.var_decl.type && ast->u.var_decl.type->kind == TYPE_DICT) {
            emit_byte(gen, OP_DICT, ast->line);
            emit_byte(gen, 0, ast->line); // count 高字节
            emit_byte(gen, 0, ast->line); // count 低字节
        } else {
            emit_byte(gen, OP_NULL, ast->line);
        }
    }

    // Ptr[T] 类型：在赋值前设置运行时 element_type（此时栈顶是值）
    if (ast->u.var_decl.type && ast->u.var_decl.type->kind == TYPE_PTR_GENERIC &&
        ast->u.var_decl.type->element_type) {
        emit_byte(gen, OP_SET_PTR_ELEM_TYPE, ast->line);
        emit_byte(gen, (uint8_t)ast->u.var_decl.type->element_type->kind, ast->line);
    }

    // face 类型：设置运行时 declared_face（此时栈顶是 struct 实例）
    if (ast->u.var_decl.type && ast->u.var_decl.type->kind == TYPE_FACE &&
        ast->u.var_decl.type->struct_name) {
        ObjString* face_name_str = str_copy(ast->u.var_decl.type->struct_name,
                                             strlen(ast->u.var_decl.type->struct_name));
        int face_name_const = make_constant(gen, val_obj((Object*)face_name_str));
        emit_byte(gen, OP_SET_DECLARED_FACE, ast->line);
        emit_byte(gen, (face_name_const >> 8) & 0xff, ast->line);
        emit_byte(gen, face_name_const & 0xff, ast->line);
    }

    SymRef* ref = &ast->u.var_decl.ref;
    if (!ref->name) {
        error_add(ERR_SEMANTIC, ast->line, "未解析的变量声明");
        return;
    }

    if (ref->kind == SYM_GLOBAL) {
        emit_define_global(gen, ref->index, ast->line);
    } else if (ref->kind == SYM_LOCAL) {
        emit_bytes_2(gen, OP_SET_LOCAL_POP, ref->index, ast->line);
        // 追踪需要析构的局部变量（仅 var x = new StructWithDtor() 场景）
        if (ast->u.var_decl.init &&
            ast->u.var_decl.init->kind == AST_STRUCT_INIT &&
            ast->u.var_decl.init->u.struct_init.has_dtor) {
            codegen_add_dtor_entry(gen, ref->index);
        }
    } else if (ref->kind == SYM_MODULE) {
        emit_bytes_2(gen, OP_SET_MODULE_VAR, ref->index, ast->line);
        emit_byte(gen, OP_POP, ast->line);
    }
}

void gen_assign(CodeGen* gen, Ast* ast) {
    int left_count = ast->u.assign.name_count;
    
    // 并行赋值：先求所有右值存到临时槽位，再逐个赋值
    if (left_count > 1) {
        int orig_max = gen->max_local_slot;
        
        // 临时槽位：从 orig_max+1 开始，每次 gen_assign 固定复用同一区域
        // value_slots[0..left_count-1] 存右值
        // aux_slot                    复用槽位（INDEX/FIELD_ACCESS 时临时保存 value）
        int slot_base = orig_max + 1;
        int aux_slot  = slot_base + left_count;
        int needed    = aux_slot;  // 需要的最大槽位号
        
        if (needed > gen->max_local_slot) {
            gen->max_local_slot = needed;
        }
        if (needed > gen->peak_local_slot) {
            gen->peak_local_slot = needed;
        }
        
        // 阶段1：求所有右值 → 写入临时槽位
        Ast* arr = ast->u.assign.value;
        int right_count = (arr && arr->kind == AST_ARRAY) ? arr->u.array.count : 0;
        
        for (int i = 0; i < left_count; i++) {
            if (arr && arr->kind == AST_ARRAY && i < right_count) {
                gen_expr(gen, arr->u.array.items[i]);
            } else if (!(arr && arr->kind == AST_ARRAY) && i == 0) {
                gen_expr(gen, ast->u.assign.value);
            } else {
                emit_byte(gen, OP_NULL, ast->line);
            }
            emit_bytes_2(gen, OP_SET_LOCAL, slot_base + i, ast->line);
            emit_byte(gen, OP_POP, ast->line);
        }
        
        // 阶段2：逐个加载右值 → 赋值给左目标
        for (int i = 0; i < left_count; i++) {
            Ast* target = ast->u.assign.targets[i];
            SymRef* ref  = &ast->u.assign.refs[i];
            int val_slot = slot_base + i;
            
            if (target->kind == AST_FIELD_ACCESS) {
                // 保存 value → aux，生成 obj，重载 value，SET_FIELD
                emit_bytes_2(gen, OP_GET_LOCAL, val_slot, ast->line);
                emit_bytes_2(gen, OP_SET_LOCAL, aux_slot, ast->line);
                emit_byte(gen, OP_POP, ast->line);
                
                gen_expr(gen, target->u.field_access.obj);
                emit_bytes_2(gen, OP_GET_LOCAL, aux_slot, ast->line);
                
                int field_idx = target->u.field_access.field_index;
                if (field_idx < 0) {
                    error_add(ERR_SEMANTIC, ast->line, "无法确定字段索引，struct 类型可能未定义");
                    field_idx = 0;
                }
                emit_byte(gen, OP_SET_FIELD, ast->line);
                emit_byte(gen, (uint8_t)field_idx, ast->line);
                emit_byte(gen, OP_POP, ast->line);
                
            } else if (target->kind == AST_INDEX) {
                // 保存 value → aux，生成 obj+index，重载 value，INDEX_SET
                emit_bytes_2(gen, OP_GET_LOCAL, val_slot, ast->line);
                emit_bytes_2(gen, OP_SET_LOCAL, aux_slot, ast->line);
                emit_byte(gen, OP_POP, ast->line);
                
                gen_expr(gen, target->u.index.obj);
                gen_expr(gen, target->u.index.index);
                emit_bytes_2(gen, OP_GET_LOCAL, aux_slot, ast->line);
                
                emit_byte(gen, OP_INDEX_SET, ast->line);
                emit_byte(gen, OP_POP, ast->line);
                
            } else if (target->kind == AST_VAR) {
                emit_bytes_2(gen, OP_GET_LOCAL, val_slot, ast->line);
                
                if (!ref->name) {
                    error_add(ERR_SEMANTIC, ast->line, "未解析的赋值目标");
                    return;
                }
                switch (ref->kind) {
                    case SYM_LOCAL:
                    case SYM_PARAM:
                        emit_bytes_2(gen, OP_SET_LOCAL, ref->index, ast->line);
                        break;
                    case SYM_GLOBAL:
                        emit_set_global(gen, ref->index, ast->line);
                        break;
                    case SYM_UPVALUE:
                        emit_bytes_2(gen, OP_SET_UPVALUE, ref->index, ast->line);
                        break;
                    case SYM_MODULE:
                        emit_bytes_2(gen, OP_SET_MODULE_VAR, ref->index, ast->line);
                        emit_byte(gen, OP_POP, ast->line);
                        break;
                    case SYM_TYPE:
                    case SYM_STRUCT:
                    case SYM_CSTRUCT:
                    case SYM_ENUM:
                        error_add(ERR_SEMANTIC, ast->line, "类型定义不能赋值");
                        return;
                    default:
                        error_add(ERR_SEMANTIC, ast->line, "未知的符号类型");
                        return;
                }
                emit_byte(gen, OP_POP, ast->line);
            } else {
                error_add(ERR_SEMANTIC, ast->line, "不支持的赋值目标类型");
                return;
            }
        }
        
        // 最后一个值留在栈上作为赋值表达式的结果
        if (left_count > 0) {
            int last_val_slot = slot_base + left_count - 1;
            Ast* last_target = ast->u.assign.targets[left_count - 1];
            if (last_target->kind == AST_VAR) {
                SymRef* last_ref = &ast->u.assign.refs[left_count - 1];
                switch (last_ref->kind) {
                    case SYM_LOCAL:
                    case SYM_PARAM:
                        emit_bytes_2(gen, OP_GET_LOCAL, last_ref->index, ast->line);
                        break;
                    case SYM_GLOBAL:
                        emit_bytes_2(gen, OP_GET_GLOBAL, last_ref->index, ast->line);
                        break;
                    case SYM_UPVALUE:
                        emit_bytes_2(gen, OP_GET_UPVALUE, last_ref->index, ast->line);
                        break;
                    case SYM_MODULE:
                        emit_bytes_2(gen, OP_GET_MODULE_VAR, last_ref->index, ast->line);
                        break;
                    default:
                        break;
                }
            } else if (last_target->kind == AST_INDEX || last_target->kind == AST_FIELD_ACCESS) {
                // INDEX/FIELD_ACCESS 赋值后，从临时槽位取回值
                emit_bytes_2(gen, OP_GET_LOCAL, last_val_slot, ast->line);
            }
        }
        
        // 恢复 max_local_slot，下次 gen_assign 从同一基底开始（槽位复用，不级联膨胀）
        gen->max_local_slot = orig_max;
    } else {
        // 单个赋值
        Ast* target = ast->u.assign.targets[0];
        
        if (target->kind == AST_FIELD_ACCESS) {
            // struct 字段赋值: obj.field = value
            // OP_SET_FIELD 期望栈: [obj, value] (value 在栈顶)
            gen_expr(gen, target->u.field_access.obj);
            gen_expr(gen, ast->u.assign.value);
            
            // 使用编译期确定的字段索引（优化：避免运行时线性搜索）
            int field_idx = target->u.field_access.field_index;
            
            if (field_idx < 0) {
                error_add(ERR_SEMANTIC, ast->line, "无法确定字段索引，struct 类型可能未定义");
                field_idx = 0; // 使用 0 作为默认值，避免生成无效字节码
            }
            
            // 调用 OP_SET_FIELD
            emit_byte(gen, OP_SET_FIELD, ast->line);
            emit_byte(gen, (uint8_t)field_idx, ast->line);
        } else if (target->kind == AST_INDEX) {
            // 索引赋值: obj[index] = value
            gen_expr(gen, target->u.index.obj);
            gen_expr(gen, target->u.index.index);
            gen_expr(gen, ast->u.assign.value);
            emit_byte(gen, OP_INDEX_SET, ast->line);
        } else if (target->kind == AST_VAR) {
            SymRef* ref = &ast->u.assign.refs[0];
            if (!ref->name) {
                error_add(ERR_SEMANTIC, ast->line, "未解析的赋值目标");
                return;
            }

            // 优化: a = b (两个局部变量) → OP_MOVE_LOCAL
            if (ast->u.assign.value->kind == AST_VAR &&
                (ref->kind == SYM_LOCAL || ref->kind == SYM_PARAM)) {
                SymRef* src_ref = &ast->u.assign.value->u.var.ref;
                if (src_ref->name &&
                    (src_ref->kind == SYM_LOCAL || src_ref->kind == SYM_PARAM)) {
                    emit_byte(gen, OP_MOVE_LOCAL, ast->line);
                    emit_byte(gen, (uint8_t)(src_ref->index >> 8), ast->line);
                    emit_byte(gen, (uint8_t)(src_ref->index & 0xFF), ast->line);
                    emit_byte(gen, (uint8_t)(ref->index >> 8), ast->line);
                    emit_byte(gen, (uint8_t)(ref->index & 0xFF), ast->line);
                    return;
                }
            }

            // 普通变量赋值
            gen_expr(gen, ast->u.assign.value);

            switch (ref->kind) {
                case SYM_LOCAL:
                case SYM_PARAM:
                    emit_bytes_2(gen, OP_SET_LOCAL, ref->index, ast->line);
                    break;
                case SYM_GLOBAL:
                    emit_set_global(gen, ref->index, ast->line);
                    break;
                case SYM_UPVALUE:
                    emit_bytes_2(gen, OP_SET_UPVALUE, ref->index, ast->line);
                    break;
                case SYM_MODULE:
                    // 模块变量赋值
                    emit_bytes_2(gen, OP_SET_MODULE_VAR, ref->index, ast->line);
                    emit_byte(gen, OP_POP, ast->line);
                    break;
                case SYM_TYPE:
                case SYM_STRUCT:
                case SYM_CSTRUCT:
                case SYM_ENUM:
                    // 类型定义不能赋值
                    error_add(ERR_SEMANTIC, ast->line, "类型定义不能赋值");
                    return;
                default:
                    error_add(ERR_SEMANTIC, ast->line, "未知的符号类型");
                    return;
            }
        } else {
            error_add(ERR_SEMANTIC, ast->line, "不支持的赋值目标类型");
            return;
        }
    }
}

void gen_compound_assign(CodeGen* gen, Ast* ast) {
    SymRef* ref = &ast->u.compound_assign.ref;
    if (!ref->name) {
        error_add(ERR_SEMANTIC, ast->line, "未解析的复合赋值目标");
        return;
    }

    LenoTokenType op = ast->u.compound_assign.op;

    // 检查是否是 struct 字段（通过 __self_field__ 标记）
    int is_self_field = (strcmp(ref->name, "__self_field__") == 0);

    if (is_self_field) {
        // struct 字段的复合赋值：self.field += value
        // 1. 获取 self（局部变量 0）
        emit_bytes_2(gen, OP_GET_LOCAL, 0, ast->line);
        // 2. 复制一份用于后续设置
        emit_byte(gen, OP_DUP, ast->line);
        // 3. 使用编译期确定的字段索引（优化：避免运行时线性搜索）
        int field_idx = ast->u.compound_assign.ref.index;
        // 4. 获取字段值 (OP_GET_FIELD + 1字节字段索引)
        emit_byte(gen, OP_GET_FIELD, ast->line);
        emit_byte(gen, (uint8_t)field_idx, ast->line);
    } else if ((ref->kind == SYM_LOCAL || ref->kind == SYM_PARAM) &&
        ast->u.compound_assign.value->kind == AST_NUM &&
        ast->u.compound_assign.value->u.num.value == 1.0) {

        if (op == TOK_PLUSEQ) {
            emit_bytes_2(gen, OP_INC_LOCAL, ref->index, ast->line);
            return;
        } else if (op == TOK_MINUSEQ) {
            emit_bytes_2(gen, OP_DEC_LOCAL, ref->index, ast->line);
            return;
        }
    }

    // 检测 shift-imm 复合赋值：x <<= N / x >>= N / x >>>= N，N 为小正整数字面量
    int shift_imm = 0;
    uint8_t shift_val = 0;
    if ((op == TOK_SHLEQ || op == TOK_SHREQ || op == TOK_USHREQ) &&
        ast->u.compound_assign.value->kind == AST_NUM &&
        !ast->u.compound_assign.value->u.num.is_float &&
        !ast->u.compound_assign.value->u.num.is_bigint) {
        double val = ast->u.compound_assign.value->u.num.value;
        if (val >= 0 && val <= 127 && val == (double)(int)val) {
            shift_imm = 1;
            shift_val = (uint8_t)(int)val;
        }
    }

    if (!is_self_field) {
        if (ref->kind == SYM_LOCAL || ref->kind == SYM_PARAM) {
            emit_bytes_2(gen, OP_GET_LOCAL, ref->index, ast->line);
        } else if (ref->kind == SYM_GLOBAL) {
            emit_get_global(gen, ref->index, ast->line);
        } else if (ref->kind == SYM_UPVALUE) {
            emit_bytes_2(gen, OP_GET_UPVALUE, ref->index, ast->line);
        } else if (ref->kind == SYM_MODULE) {
            emit_bytes_2(gen, OP_GET_MODULE_VAR, ref->index, ast->line);
        } else {
            error_add(ERR_SEMANTIC, ast->line, "不支持的变量类型用于复合赋值");
            return;
        }
    }

    // shift-imm 优化：跳过 RHS 的 OP_CONST 生成
    if (!shift_imm) {
        gen_expr(gen, ast->u.compound_assign.value);
    }

    // 类型特化：检查变量类型和值类型
    TypeKind var_type = ref->type_kind;
    TypeKind value_type = get_expr_type_kind(ast->u.compound_assign.value);
    int is_int_op = (var_type == TYPE_INT && value_type == TYPE_INT);

    switch (op) {
        case TOK_PLUSEQ:
            emit_byte(gen, is_int_op ? OP_ADD_INT : OP_ADD, ast->line);
            break;
        case TOK_MINUSEQ:
            emit_byte(gen, is_int_op ? OP_SUB_INT : OP_SUB, ast->line);
            break;
        case TOK_STAREQ:
            emit_byte(gen, is_int_op ? OP_MUL_INT : OP_MUL, ast->line);
            break;
        case TOK_SLASHEQ:
            emit_byte(gen, is_int_op ? OP_DIV_INT : OP_DIV, ast->line);
            break;
        case TOK_MODEQ:
            emit_byte(gen, is_int_op ? OP_MOD_INT : OP_MOD, ast->line);
            break;
        case TOK_BITANDEQ: emit_byte(gen, OP_BITAND, ast->line); break;
        case TOK_BITOREQ:  emit_byte(gen, OP_BITOR, ast->line); break;
        case TOK_BITXOREQ: emit_byte(gen, OP_BITXOR, ast->line); break;
        case TOK_SHLEQ:
            if (shift_imm) emit_byte_imm(gen, OP_SHL_IMM, shift_val, ast->line);
            else           emit_byte(gen, OP_SHL, ast->line);
            break;
        case TOK_SHREQ:
            if (shift_imm) emit_byte_imm(gen, OP_SHR_IMM, shift_val, ast->line);
            else           emit_byte(gen, OP_SHR, ast->line);
            break;
        case TOK_USHREQ:
            if (shift_imm) emit_byte_imm(gen, OP_USHR_IMM, shift_val, ast->line);
            else           emit_byte(gen, OP_USHR, ast->line);
            break;
        default:
            error_add(ERR_SEMANTIC, ast->line, "未知的复合赋值运算符");
            return;
    }

    if (is_self_field) {
        // struct 字段的复合赋值：使用 OP_SET_FIELD
        // 栈顶是计算后的值，下面是 self（被 OP_DUP 复制的那份）
        // 使用编译期确定的字段索引（优化：避免运行时线性搜索）
        int field_idx = ast->u.compound_assign.ref.index;
        // OP_SET_FIELD + 1字节字段索引
        emit_byte(gen, OP_SET_FIELD, ast->line);
        emit_byte(gen, (uint8_t)field_idx, ast->line);
    } else {
        switch (ref->kind) {
            case SYM_LOCAL:
            case SYM_PARAM:
                emit_bytes_2(gen, OP_SET_LOCAL, ref->index, ast->line);
                break;
            case SYM_GLOBAL:
                emit_set_global(gen, ref->index, ast->line);
                break;
            case SYM_UPVALUE:
                emit_bytes_2(gen, OP_SET_UPVALUE, ref->index, ast->line);
                break;
            case SYM_MODULE:
                emit_bytes_2(gen, OP_SET_MODULE_VAR, ref->index, ast->line);
                emit_byte(gen, OP_POP, ast->line);
                break;
            case SYM_TYPE:
            case SYM_STRUCT:
            case SYM_CSTRUCT:
            case SYM_ENUM:
                // 类型定义不能赋值
                error_add(ERR_SEMANTIC, ast->line, "类型定义不能赋值");
                return;
            default:
                error_add(ERR_SEMANTIC, ast->line, "未知的符号类型");
                return;
        }
    }
}

// 生成尾调用表达式（return func() 模式）
static void gen_tail_call_expr(CodeGen* gen, Ast* ast) {
    // 只处理简单函数调用，不处理方法调用 arr.method()
    if (ast->kind != AST_CALL) {
        return;
    }
    
    // 检查是否是方法调用 arr.method() - 暂不优化
    if (ast->u.call.callee->kind == AST_INDEX) {
        return;
    }
    
    // 生成参数（顺序：先arg1, arg2, ...，最后callee）
    for (int i = 0; i < ast->u.call.args.count; i++) {
        gen_expr(gen, ast->u.call.args.items[i]);
    }
    
    // 检测原生函数尾调用，合并为 OP_TAIL_CALL_NATIVE
    if (ast->u.call.callee->kind == AST_VAR) {
        Symbol* callee_sym = scope_resolve(gen->sem->current, ast->u.call.callee->u.var.name);
        if (callee_sym && callee_sym->kind == SYM_NATIVE) {
            ObjString* nameStr = str_copy(callee_sym->name, (int)strlen(callee_sym->name));
            int name_const = make_constant(gen, val_obj((Object*)nameStr));
            int total_args = ast->u.call.args.count;
            emit_tail_call_native(gen, name_const, total_args, ast->line);
            ast->u.call.is_tail_call = 1;
            return;
        }
    }
    
    // 生成被调用的函数
    gen_expr(gen, ast->u.call.callee);
    
    // 使用尾调用指令
    emit_tail_call(gen, ast->u.call.args.count, ast->line);
    
    // 标记已处理
    ast->u.call.is_tail_call = 1;
}

// 检查表达式是否是可优化的尾调用
static int is_tail_call(CodeGen* gen, Ast* ast) {
    if (!ast || ast->kind != AST_CALL) return 0;
    
    // 暂不处理方法调用 arr.method()
    if (ast->u.call.callee->kind == AST_INDEX) return 0;
    
    // async 函数不能使用尾调用优化
    // 因为 async 函数需要 OP_ASYNC_CALL 创建独立协程，
    // 而 OP_TAIL_CALL 会复用当前帧，导致协程帧追踪混乱
    if (ast->u.call.callee_is_async) return 0;
    // 也通过 func_table 检查（callee_is_async 可能在语义分析阶段未设置）
    if (ast->u.call.callee->kind == AST_VAR) {
        Ast* func_def = func_table_find(&gen->sem->func_table, ast->u.call.callee->u.var.name);
        if (func_def && func_def->kind == AST_FUNC_DEF && func_def->u.func.is_async) {
            return 0;
        }
    }
    
    return 1;
}

static void gen_return(CodeGen* gen, Ast* ast) {
    // 函数内联上下文：return 变为 set_local + jump
    if (gen->inline_depth > 0) {
        if (ast->u.ret) {
            gen_expr(gen, ast->u.ret);
        } else {
            emit_byte(gen, OP_NULL, ast->line);
        }
        emit_bytes_2(gen, OP_SET_LOCAL_POP, gen->inline_result_slot, ast->line);
        // 记录跳转，稍后回填到内联块末尾
        if (gen->inline_return_jump_count < 256) {
            gen->inline_return_jumps[gen->inline_return_jump_count++] =
                emit_jump(gen, OP_JUMP, ast->line);
        }
        return;
    }

    if (gen->dtor_count > 0) {
        // 有需要析构的变量，禁用尾调用优化
        if (ast->u.ret) {
            gen_expr(gen, ast->u.ret);
            // 保存返回值到临时槽位
            if (gen->dtor_temp_slot < 0) {
                gen->dtor_temp_slot = gen->current_func->local_count;
                gen->current_func->local_count++;
            }
            emit_bytes_2(gen, OP_SET_LOCAL, gen->dtor_temp_slot, ast->line);
        }
        // 逆序调用所有析构函数
        for (int i = gen->dtor_count - 1; i >= 0; i--) {
            emit_byte(gen, OP_DTOR_LOCAL, ast->line);
            emit_byte(gen, (gen->dtor_entries[i].local_slot >> 8) & 0xff, ast->line);
            emit_byte(gen, gen->dtor_entries[i].local_slot & 0xff, ast->line);
            emit_byte(gen, OP_POP, ast->line);
        }
        // 恢复返回值
        if (ast->u.ret) {
            emit_bytes_2(gen, OP_GET_LOCAL, gen->dtor_temp_slot, ast->line);
        } else {
            emit_byte(gen, OP_NULL, ast->line);
        }
        emit_byte(gen, OP_RETURN, ast->line);
    } else {
        // 原有逻辑（支持尾调用优化）
        if (ast->u.ret) {
            // 检测是否是尾调用：return func(...)
            if (is_tail_call(gen, ast->u.ret)) {
                // 使用尾调用优化
                gen_tail_call_expr(gen, ast->u.ret);
                // 尾调用会处理返回值，直接返回即可
                emit_byte(gen, OP_RETURN, ast->line);
            } else {
                gen_expr(gen, ast->u.ret);
                emit_byte(gen, OP_RETURN, ast->line);
            }
        } else {
            emit_byte(gen, OP_NULL, ast->line);
            emit_byte(gen, OP_RETURN, ast->line);
        }
    }
}

static void gen_expr_stmt(CodeGen* gen, Ast* ast) {
    Ast* expr = ast->u.expr_stmt.expr;

    // 检测 arr.add(x) 模式：表达式语句直接用 OP_ARRAY_APPEND_NOPUSH 省掉 OP_POP
    if (expr->kind == AST_CALL &&
        expr->u.call.callee->kind == AST_INDEX &&
        expr->u.call.callee->u.index.index->kind == AST_STRING &&
        expr->u.call.args.count == 1 &&
        strcmp(expr->u.call.callee->u.index.index->u.string.value, "add") == 0) {
        TypeInfo* receiver_type = infer_expr_type(gen->sem, expr->u.call.callee->u.index.obj);
        int is_array_type = (receiver_type && receiver_type->kind == TYPE_ARRAY);
        if (receiver_type) type_free(receiver_type);
        if (is_array_type) {
            gen_array_add(gen, expr->u.call.callee->u.index.obj, expr->u.call.args.items[0], 0, ast->line);
            return;
        }
    }

    // 优化：i++ / i-- 语句（后缀，局部变量）→ OP_INC_LOCAL_NOPUSH / OP_DEC_LOCAL_NOPUSH
    if (expr->kind == AST_UNARY && expr->u.unary.is_postfix &&
        (expr->u.unary.op == TOK_INC || expr->u.unary.op == TOK_DEC) &&
        expr->u.unary.operand->kind == AST_VAR) {
        SymRef* ref = &expr->u.unary.operand->u.var.ref;
        if (ref->name && (ref->kind == SYM_LOCAL || ref->kind == SYM_PARAM)) {
            emit_bytes_2(gen, expr->u.unary.op == TOK_INC ? OP_INC_LOCAL_NOPUSH : OP_DEC_LOCAL_NOPUSH,
                         ref->index, ast->line);
            return;
        }
    }

    // 窥孔优化：赋值语句到局部变量 → OP_SET_LOCAL_POP（省掉 OP_POP 分发开销）
    // 场景1: a = b（两个局部变量）→ OP_MOVE_LOCAL_POP（不压栈）
    // 场景2: a = expr（a 是局部变量）→ gen_expr(expr) + OP_SET_LOCAL_POP
    if (expr->kind == AST_ASSIGN && expr->u.assign.name_count == 1) {
        Ast* target = expr->u.assign.targets[0];
        if (target && target->kind == AST_VAR) {
            SymRef* ref = &expr->u.assign.refs[0];
            if (ref->name && (ref->kind == SYM_LOCAL || ref->kind == SYM_PARAM)) {
                // a = b（两个局部变量）→ OP_MOVE_LOCAL_POP（不压栈）
                if (expr->u.assign.value->kind == AST_VAR) {
                    SymRef* src_ref = &expr->u.assign.value->u.var.ref;
                    if (src_ref->name && (src_ref->kind == SYM_LOCAL || src_ref->kind == SYM_PARAM)) {
                        emit_byte(gen, OP_MOVE_LOCAL_POP, ast->line);
                        emit_byte(gen, (uint8_t)(src_ref->index >> 8), ast->line);
                        emit_byte(gen, (uint8_t)(src_ref->index & 0xFF), ast->line);
                        emit_byte(gen, (uint8_t)(ref->index >> 8), ast->line);
                        emit_byte(gen, (uint8_t)(ref->index & 0xFF), ast->line);
                        return;
                    }
                }
                // a = expr → gen_expr(expr) + OP_SET_LOCAL_POP（省一次分发）
                gen_expr(gen, expr->u.assign.value);
                emit_bytes_2(gen, OP_SET_LOCAL_POP, ref->index, ast->line);
                return;
            }
        }
        // 索引赋值语句: obj[index] = value → 直接生成 OP_INDEX_SET_NOPUSH
        // （不能走后置窥孔，因为 OP_SET_FIELD+field_idx 的 field_idx 字节
        //   可能与 OP_INDEX_SET 枚举值碰撞，导致误替换）
        if (target && target->kind == AST_INDEX) {
            gen_expr(gen, target->u.index.obj);
            gen_expr(gen, target->u.index.index);
            gen_expr(gen, expr->u.assign.value);
            emit_byte(gen, OP_INDEX_SET_NOPUSH, ast->line);
            return;
        }
    }

    if (expr->kind == AST_COMPOUND_ASSIGN) {
        SymRef* ref = &expr->u.compound_assign.ref;
        if (ref->name && (ref->kind == SYM_LOCAL || ref->kind == SYM_PARAM) &&
            strcmp(ref->name, "__self_field__") != 0) {
            gen_expr(gen, expr);
            // 检查末尾是否是 OP_SET_LOCAL（排除 INC_LOCAL/DEC_LOCAL 提前返回的情况）
            if (gen->chunk->len >= 3 &&
                gen->chunk->code[gen->chunk->len - 3] == OP_SET_LOCAL) {
                gen->chunk->code[gen->chunk->len - 3] = OP_SET_LOCAL_POP;
                return;  // 跳过 OP_POP
            }
            // INC_LOCAL/DEC_LOCAL 等情况仍需 OP_POP
            emit_byte(gen, OP_POP, ast->line);
            return;
        }
    }

    // 默认：生成表达式 + OP_POP
    // 设置 discard 标志：内联 void 函数时可省掉 OP_NULL + OP_POP
    gen->inline_discard_result = 1;
    gen->inline_no_result = 0;
    gen_expr(gen, expr);
    gen->inline_discard_result = 0;

    // 内联 void 函数已跳过 OP_NULL，栈上无值，跳过 OP_POP
    if (gen->inline_no_result) {
        gen->inline_no_result = 0;
        return;
    }

    // 后置窥孔优化：仅对 AST_CALL 表达式检查，避免误匹配其他指令的操作数
    if (expr->kind == AST_CALL) {
        // OP_CALL_NATIVE(5字节) → OP_CALL_NATIVE_VOID（省掉 OP_POP）
        if (gen->chunk->len >= 5 &&
            gen->chunk->code[gen->chunk->len - 5] == OP_CALL_NATIVE) {
            gen->chunk->code[gen->chunk->len - 5] = OP_CALL_NATIVE_VOID;
            return;
        }
        // OP_DICT_SET(1字节) → OP_DICT_SET_NOPUSH
        if (gen->chunk->len >= 1 &&
            gen->chunk->code[gen->chunk->len - 1] == OP_DICT_SET) {
            gen->chunk->code[gen->chunk->len - 1] = OP_DICT_SET_NOPUSH;
            return;
        }
    }

    emit_byte(gen, OP_POP, ast->line);
}

void gen_block(CodeGen* gen, Ast* ast) {
    // 策略：先预生成所有局部函数，支持前向引用
    // 然后再按顺序执行其他语句
    // 注意：在循环体内定义的函数不预生成，而是在运行时每次迭代时创建

    int dtor_count_at_entry = gen->dtor_count;  // 记录进入块时的析构条目数

    typedef struct {
        Ast* ast;
        ObjFunction* proto;
    } FuncProtoEntry;

    FuncProtoEntry local_funcs[64];
    int local_func_count = 0;

    // 第一遍：预注册全局函数，并为局部函数预分配槽位
    // 跳过在循环体内定义的函数（is_in_loop=1），让它们运行时创建
    for (int i = 0; i < ast->u.block.count; i++) {
        Ast* stmt = ast->u.block.items[i];
        if (stmt->kind == AST_FUNC_DEF && stmt->u.func.ref.name) {
            if (stmt->u.func.ref.kind == SYM_GLOBAL_FUNC) {
                // 全局函数：直接生成
                gen_func(gen, stmt);
            } else if (stmt->u.func.ref.kind == SYM_LOCAL && !stmt->u.func.is_in_loop) {
                // 局部函数（非循环体内）：预分配槽位（null），支持前向引用
                emit_byte(gen, OP_NULL, stmt->line);
                emit_bytes_2(gen, OP_SET_LOCAL, stmt->u.func.ref.index, stmt->line);
                emit_byte(gen, OP_POP, stmt->line);
                // 保存函数定义供后续使用
                if (local_func_count < 64) {
                    local_funcs[local_func_count].ast = stmt;
                    local_funcs[local_func_count].proto = NULL; // 稍后生成
                    local_func_count++;
                }
            }
        }
    }

    // 第二遍：生成所有局部函数（在函数调用之前）
    // 跳过在循环体内定义的函数（is_in_loop=1），让它们运行时创建
    for (int i = 0; i < ast->u.block.count; i++) {
        Ast* stmt = ast->u.block.items[i];
        if (stmt->kind == AST_FUNC_DEF && stmt->u.func.ref.name) {
            if (stmt->u.func.ref.kind == SYM_LOCAL && !stmt->u.func.is_in_loop) {
                // 找到对应的函数条目
                for (int j = 0; j < local_func_count; j++) {
                    if (local_funcs[j].ast == stmt) {
                        // 生成函数原型和闭包
                        if (local_funcs[j].proto == NULL) {
                            local_funcs[j].proto = gen_func_proto(gen, stmt);
                        }
                        gen_func_closure(gen, stmt, local_funcs[j].proto);
                        break;
                    }
                }
            }
        }
    }

    // 第三遍：按顺序执行其他语句
    // 在循环体内定义的函数会在这里生成（运行时每次迭代时）
    // 如果块中有 defer 语句，用 try-finally 包裹，确保 defer 在作用域退出时执行

    // 先扫描是否有 defer 语句
    int has_defer = 0;
    for (int i = 0; i < ast->u.block.count; i++) {
        Ast* stmt = ast->u.block.items[i];
        if (stmt->kind == AST_DEFER) {
            has_defer = 1;
            break;
        }
    }

    // 收集 defer 表达式（最多 64 个）
    Ast* defer_exprs[64];
    int defer_count = 0;

    int try_start = -1;
    int finally_patch = -1;

    if (has_defer) {
        // 发射 OP_TRY（无 catch，只有 finally）
        try_start = gen->chunk->len;
        emit_byte(gen, OP_TRY, ast->line);
        finally_patch = gen->chunk->len;
        emit_byte(gen, 0, ast->line); // catch 偏移量 = 0（无 catch）
        emit_byte(gen, 0, ast->line);
        emit_byte(gen, 0, ast->line); // finally 偏移量占位
        emit_byte(gen, 0, ast->line);
    }

    for (int i = 0; i < ast->u.block.count; i++) {
        Ast* stmt = ast->u.block.items[i];
        if (stmt->kind == AST_FUNC_DEF && stmt->u.func.is_in_loop) {
            // 循环体内定义的函数：在运行时创建（每次迭代）
            gen_func(gen, stmt);
        } else if (stmt->kind == AST_DEFER) {
            // 收集 defer 表达式，不在当前位置生成代码
            if (defer_count < 64) {
                defer_exprs[defer_count++] = stmt->u.defer_.expr;
            }
        } else if (stmt->kind != AST_FUNC_DEF) {
            gen_stmt(gen, stmt);
        }
    }

    // 块结束时，逆序生成新增 dtor 条目的析构调用
    while (gen->dtor_count > dtor_count_at_entry) {
        gen->dtor_count--;
        emit_byte(gen, OP_DTOR_LOCAL, ast->line);
        emit_byte(gen, (gen->dtor_entries[gen->dtor_count].local_slot >> 8) & 0xff, ast->line);
        emit_byte(gen, gen->dtor_entries[gen->dtor_count].local_slot & 0xff, ast->line);
        emit_byte(gen, OP_POP, ast->line);  // 弹出析构函数返回值(null)
    }

    // 如果有 defer，生成 finally 块
    if (has_defer) {
        // 正常结束 try 块后，需要跳转到 finally 块执行 defer
        // 这里不需要 OP_JUMP——直接顺序执行到 finally 即可
        // 因为 OP_TRY 的 finally_ip 已经指向了 finally 块，
        // 在异常时会自动跳转到 finally_ip；
        // 在正常退出时，代码顺序流入 finally 块。

        // 回填 finally 偏移量（指向接下来的 OP_FINALLY）
        int finally_start = gen->chunk->len;
        int finally_offset = finally_start - try_start;
        gen->chunk->code[finally_patch + 2] = (finally_offset >> 8) & 0xff;
        gen->chunk->code[finally_patch + 3] = finally_offset & 0xff;

        // 生成 finally 块
        emit_byte(gen, OP_FINALLY, ast->line);
        // 逆序执行 defer 表达式（后注册的先执行）
        for (int i = defer_count - 1; i >= 0; i--) {
            gen_expr(gen, defer_exprs[i]);
            emit_byte(gen, OP_POP, ast->line);
        }
        emit_byte(gen, OP_END_TRY, ast->line);
    }
}

void gen_stmt(CodeGen* gen, Ast* ast) {
    if (!ast) return;

    switch (ast->kind) {
        case AST_BLOCK:
            gen_block(gen, ast);
            break;
        case AST_IF:
            gen_if(gen, ast);
            break;
        case AST_WHILE:
            gen_while(gen, ast);
            break;
        case AST_FOR:
            gen_for(gen, ast);
            break;
        case AST_SWITCH:
            gen_switch(gen, ast);
            break;
        case AST_FUNC_DEF:
            break;
        case AST_RETURN:
            gen_return(gen, ast);
            break;
        case AST_BREAK: {
            if (!gen->loop_head) {
                error_add(ERR_SYNTAX, ast->line, "break 只能在循环中使用");
                return;
            }
            LoopContext* loop = &gen->loop_head->ctx;
            if (loop->break_count >= MAX_BREAK_JUMPS) {
                error_add(ERR_RUNTIME, ast->line, "break 太多");
                return;
            }
            loop->break_jumps[loop->break_count++] = emit_jump(gen, OP_JUMP, ast->line);
            break;
        }
        case AST_CONTINUE: {
            if (!gen->loop_head) {
                error_add(ERR_SYNTAX, ast->line, "continue 只能在循环中使用");
                return;
            }
            LoopContext* loop = &gen->loop_head->ctx;
            // 对于需要回填的循环（如 for to），使用占位符
            if (loop->continue_count >= MAX_CONTINUE_JUMPS) {
                error_add(ERR_RUNTIME, ast->line, "continue 太多");
                return;
            }
            // 记录跳转位置，稍后回填
            loop->continue_jumps[loop->continue_count++] = emit_jump(gen, OP_JUMP, ast->line);
            break;
        }
        case AST_VAR_DECL:
            gen_var_decl(gen, ast);
            break;
        case AST_ASSIGN:
            gen_assign(gen, ast);
            break;
        case AST_COMPOUND_ASSIGN:
            gen_compound_assign(gen, ast);
            break;
        case AST_EXPR_STMT:
            gen_expr_stmt(gen, ast);
            break;
        case AST_IMPORT:
            gen_import_inline(gen, ast);
            break;

        case AST_EXPORT:
            if (ast->u.export.decl) {
                if (ast->u.export.decl->kind == AST_FUNC_DEF) {
                    gen_func(gen, ast->u.export.decl);
                } else {
                    gen_stmt(gen, ast->u.export.decl);
                }
            }
            break;

        case AST_USE:
            // use 语句是编译时指令，不需要生成字节码
            // 符号已经在语义分析阶段注册到当前作用域
            // 在 gen_stmt / gen_stmt_module 中都只是 break
            break;

        case AST_TRY: {
            // 生成 try-catch-finally 的字节码
            // 结构：
            // OP_TRY <catch_offset> <finally_offset>
            // ... try body ...
            // OP_JUMP <finally_or_end>  (正常结束跳到 finally 或结束)
            // OP_CATCH (if has catch)
            // ... catch body ...
            // OP_JUMP <finally_or_end>  (catch 结束跳到 finally 或结束)
            // OP_FINALLY (if has finally)
            // ... finally body ...
            // OP_END_TRY
            
            int try_start = gen->chunk->len;
            
            // 预留 OP_TRY 指令空间
            emit_byte(gen, OP_TRY, ast->line);
            int catch_jump = gen->chunk->len;
            emit_byte(gen, 0, ast->line); // catch 偏移量占位
            emit_byte(gen, 0, ast->line);
            int finally_jump = gen->chunk->len;
            emit_byte(gen, 0, ast->line); // finally 偏移量占位
            emit_byte(gen, 0, ast->line);
            
            // 生成 try 体
            gen_stmt(gen, ast->u.try_.try_body);
            
            // 正常结束 try 块，跳到 finally 或结束
            int try_to_finally_jump = emit_jump(gen, OP_JUMP, ast->line);
            
            int catch_start = gen->chunk->len;
            
            // 生成 catch 体（如果有）
            if (ast->u.try_.catch_body) {
                emit_byte(gen, OP_CATCH, ast->line);
                
                // 如果有 catch 变量，将异常值存入局部变量
                if (ast->u.try_.catch_var && ast->u.try_.catch_var_ref.name) {
                    int var_idx = ast->u.try_.catch_var_ref.index;
                    // 将栈顶的异常值存入局部变量
                    emit_bytes_2(gen, OP_SET_LOCAL_POP, var_idx, ast->line);
                } else {
                    // 没有 catch 变量，直接弹出异常值
                    emit_byte(gen, OP_POP, ast->line);
                }
                
                gen_stmt(gen, ast->u.try_.catch_body);
            }
            
            // catch 结束，跳到 finally 或结束（只有有 catch 体时才需要）
            int catch_to_finally_jump = -1;
            if (ast->u.try_.catch_body) {
                catch_to_finally_jump = emit_jump(gen, OP_JUMP, ast->line);
            }
            
            int finally_start = gen->chunk->len;
            
            // 回填 try 结束后的跳转（跳到 finally）- 必须在生成 finally 之前回填
            patch_jump(gen, try_to_finally_jump);
            
            // 回填 catch 结束后的跳转（跳到 finally）- 必须在生成 finally 之前回填
            if (catch_to_finally_jump >= 0) {
                patch_jump(gen, catch_to_finally_jump);
            }
            
            // 生成 finally 体（如果有）
            if (ast->u.try_.finally_body) {
                emit_byte(gen, OP_FINALLY, ast->line);
                gen_stmt(gen, ast->u.try_.finally_body);
            }
            
            emit_byte(gen, OP_END_TRY, ast->line);
            
            // 回填 catch 跳转地址
            if (ast->u.try_.catch_body) {
                int catch_offset = catch_start - try_start;
                gen->chunk->code[catch_jump] = (catch_offset >> 8) & 0xff;
                gen->chunk->code[catch_jump + 1] = catch_offset & 0xff;
            }
            
            // 回填 finally 跳转地址
            if (ast->u.try_.finally_body) {
                int finally_offset = finally_start - try_start;
                gen->chunk->code[finally_jump] = (finally_offset >> 8) & 0xff;
                gen->chunk->code[finally_jump + 1] = finally_offset & 0xff;
            }
            
            break;
        }
        case AST_THROW:
            if (ast->u.throw_.expr) {
                gen_expr(gen, ast->u.throw_.expr);
            } else {
                emit_byte(gen, OP_NULL, ast->line);
            }
            emit_byte(gen, OP_THROW, ast->line);
            break;
        case AST_DEFER:
            // defer 由 gen_block 统一处理，这里不应到达
            // 如果直接调用 gen_stmt(AST_DEFER)，生成表达式 + POP（降级为普通表达式语句）
            gen_expr(gen, ast->u.defer_.expr);
            emit_byte(gen, OP_POP, ast->line);
            break;
        case AST_FACE_DEF: {
            ObjString* face_name = str_copy(ast->u.face_def.name, strlen(ast->u.face_def.name));
            int name_const = make_constant(gen, val_obj((Object*)face_name));
            emit_byte(gen, OP_FACE_DEF, ast->line);
            emit_byte(gen, (name_const >> 8) & 0xff, ast->line);
            emit_byte(gen, name_const & 0xff, ast->line);
            emit_byte(gen, ast->u.face_def.method_count, ast->line);
            // 泛型类型参数
            emit_byte(gen, ast->u.face_def.type_param_count, ast->line);
            for (int tp = 0; tp < ast->u.face_def.type_param_count; tp++) {
                ObjString* tp_name = str_copy(ast->u.face_def.type_params[tp],
                    strlen(ast->u.face_def.type_params[tp]));
                int tp_const = make_constant(gen, val_obj((Object*)tp_name));
                emit_byte(gen, (tp_const >> 8) & 0xff, ast->line);
                emit_byte(gen, tp_const & 0xff, ast->line);
            }
            for (int i = 0; i < ast->u.face_def.method_count; i++) {
                ObjString* mname = str_copy(ast->u.face_def.method_names[i],
                                            strlen(ast->u.face_def.method_names[i]));
                int mname_const = make_constant(gen, val_obj((Object*)mname));
                emit_byte(gen, (mname_const >> 8) & 0xff, ast->line);
                emit_byte(gen, mname_const & 0xff, ast->line);
                emit_byte(gen, ast->u.face_def.method_param_counts[i], ast->line);
                TypeInfo* rt = ast->u.face_def.method_return_types[i];
                emit_byte(gen, rt ? (uint8_t)rt->kind : (uint8_t)TYPE_INFER, ast->line);
                for (int j = 0; j < ast->u.face_def.method_param_counts[i]; j++) {
                    TypeInfo* pt = ast->u.face_def.method_param_types[i][j];
                    emit_byte(gen, pt ? (uint8_t)pt->kind : (uint8_t)TYPE_INFER, ast->line);
                }
            }
            break;
        }
        case AST_STRUCT_DEF: {
            // 预生成方法函数对象常量
            int* method_name_consts = NULL;
            int* method_func_consts = NULL;
            int method_count = ast->u.struct_def.method_count;
            if (method_count > 0) {
                method_name_consts = (int*)malloc(sizeof(int) * method_count);
                method_func_consts = (int*)malloc(sizeof(int) * method_count);
                for (int i = 0; i < method_count; i++) {
                    Ast* method_ast = ast->u.struct_def.methods[i];
                    if (method_ast && method_ast->kind == AST_FUNC_DEF) {
                        // 生成函数原型（函数对象）
                        ObjFunction* func = gen_func_proto(gen, method_ast);
                        // 方法名作为常量
                        ObjString* method_name = str_copy(method_ast->u.func.name, strlen(method_ast->u.func.name));
                        method_name_consts[i] = make_constant(gen, val_obj((Object*)method_name));
                        if (func) {
                            method_func_consts[i] = make_constant(gen, val_obj((Object*)func));
                        } else {
                            method_func_consts[i] = make_constant(gen, val_null());
                        }
                    } else {
                        method_name_consts[i] = make_constant(gen, val_null());
                        method_func_consts[i] = make_constant(gen, val_null());
                    }
                }
            }

            // 生成结构体定义指令
            // 将结构体名称作为常量
            ObjString* struct_name = str_copy(ast->u.struct_def.name, strlen(ast->u.struct_def.name));
            int name_const = make_constant(gen, val_obj((Object*)struct_name));

            emit_byte(gen, OP_STRUCT_DEF, ast->line);
            emit_byte(gen, (name_const >> 8) & 0xff, ast->line);
            emit_byte(gen, name_const & 0xff, ast->line);
            emit_byte(gen, ast->u.struct_def.field_count, ast->line);
            emit_byte(gen, method_count, ast->line);

            // 编码 impl 声明的 face 名称
            emit_byte(gen, ast->u.struct_def.impl_count, ast->line);
            for (int i = 0; i < ast->u.struct_def.impl_count; i++) {
                ObjString* impl_name = str_copy(ast->u.struct_def.impl_names[i],
                                                strlen(ast->u.struct_def.impl_names[i]));
                int impl_name_const = make_constant(gen, val_obj((Object*)impl_name));
                emit_byte(gen, (impl_name_const >> 8) & 0xff, ast->line);
                emit_byte(gen, impl_name_const & 0xff, ast->line);
            }

            // 编码泛型类型参数信息
            emit_byte(gen, ast->u.struct_def.type_param_count, ast->line);
            for (int i = 0; i < ast->u.struct_def.type_param_count && ast->u.struct_def.type_params; i++) {
                ObjString* param_name = str_copy(ast->u.struct_def.type_params[i],
                                                  strlen(ast->u.struct_def.type_params[i]));
                int param_name_const = make_constant(gen, val_obj((Object*)param_name));
                emit_byte(gen, (param_name_const >> 8) & 0xff, ast->line);
                emit_byte(gen, param_name_const & 0xff, ast->line);
            }

            // 为每个字段生成信息
            for (int i = 0; i < ast->u.struct_def.field_count; i++) {
                // 字段名
                ObjString* field_name = str_copy(ast->u.struct_def.field_names[i],
                                                  strlen(ast->u.struct_def.field_names[i]));
                int field_name_const = make_constant(gen, val_obj((Object*)field_name));
                emit_byte(gen, (field_name_const >> 8) & 0xff, ast->line);
                emit_byte(gen, field_name_const & 0xff, ast->line);

                // 字段类型
                TypeKind field_type = ast->u.struct_def.field_types[i]->kind;
                emit_byte(gen, field_type, ast->line);

                // nullable 标记
                uint8_t is_nullable = ast->u.struct_def.field_types[i]->nullable ? 1 : 0;
                emit_byte(gen, is_nullable, ast->line);

                // 如果字段是 struct 类型，输出 struct 类型名
                if (field_type == TYPE_STRUCT) {
                    const char* struct_type_name = ast->u.struct_def.field_types[i]->struct_name;
                    if (struct_type_name) {
                        ObjString* type_name = str_copy(struct_type_name, strlen(struct_type_name));
                        int type_name_const = make_constant(gen, val_obj((Object*)type_name));
                        emit_byte(gen, 1, ast->line); // 有 struct 类型名
                        emit_byte(gen, (type_name_const >> 8) & 0xff, ast->line);
                        emit_byte(gen, type_name_const & 0xff, ast->line);
                    } else {
                        emit_byte(gen, 0, ast->line); // 没有 struct 类型名
                    }
                }

                // 如果字段是 Ptr[T] 类型，输出元素类型
                if (field_type == TYPE_PTR_GENERIC) {
                    TypeInfo* ft = ast->u.struct_def.field_types[i];
                    TypeKind elem_type = ft->element_type ? ft->element_type->kind : TYPE_PTR;
                    emit_byte(gen, elem_type, ast->line);
                }

                // 是否有默认值
                if (ast->u.struct_def.field_defaults[i]) {
                    Ast* default_expr = ast->u.struct_def.field_defaults[i];
                    Value default_val = ast_default_to_value(default_expr);
                    // 非常量默认值（如 new Point(x=99)）无法在编译期求值
                    if (val_is_null(default_val) && default_expr->kind != AST_NULL) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg),
                            "struct 字段 '%s' 的默认值不是常量表达式（仅支持数字、字符串、bool、null、数组字面量、字典字面量），"
                            "请使用构造器初始化",
                            ast->u.struct_def.field_names[i]);
                        error_add(ERR_SEMANTIC, ast->line, msg);
                        emit_byte(gen, 0, ast->line); // 无默认值
                    } else {
                        emit_byte(gen, 1, ast->line);
                        int default_const = make_constant(gen, default_val);
                        emit_byte(gen, (default_const >> 8) & 0xff, ast->line);
                        emit_byte(gen, default_const & 0xff, ast->line);
                    }
                } else {
                    emit_byte(gen, 0, ast->line);
                }
            }

            // 为每个方法生成信息
            for (int i = 0; i < method_count; i++) {
                emit_byte(gen, (method_name_consts[i] >> 8) & 0xff, ast->line);
                emit_byte(gen, method_name_consts[i] & 0xff, ast->line);
                emit_byte(gen, (method_func_consts[i] >> 8) & 0xff, ast->line);
                emit_byte(gen, method_func_consts[i] & 0xff, ast->line);
            }

            // 构造/析构函数标志和索引
            // 查找 ctor/dtor 索引
            int ctor_idx = -1, dtor_idx = -1;
            for (int i = 0; i < method_count; i++) {
                Ast* method_ast = ast->u.struct_def.methods[i];
                if (method_ast && method_ast->u.func.is_ctor) ctor_idx = i;
                if (method_ast && method_ast->u.func.is_dtor) dtor_idx = i;
            }
            uint8_t ctor_dtor_flags = 0;
            if (ctor_idx >= 0) ctor_dtor_flags |= 1;
            if (dtor_idx >= 0) ctor_dtor_flags |= 2;
            emit_byte(gen, ctor_dtor_flags, ast->line);
            if (ctor_idx >= 0) emit_byte(gen, (uint8_t)ctor_idx, ast->line);
            if (dtor_idx >= 0) emit_byte(gen, (uint8_t)dtor_idx, ast->line);

            // 编码关联常量信息
            emit_byte(gen, ast->u.struct_def.const_count, ast->line);
            for (int i = 0; i < ast->u.struct_def.const_count; i++) {
                // 常量名
                ObjString* cname = str_copy(ast->u.struct_def.const_names[i],
                                             strlen(ast->u.struct_def.const_names[i]));
                int cname_const = make_constant(gen, val_obj((Object*)cname));
                emit_byte(gen, (cname_const >> 8) & 0xff, ast->line);
                emit_byte(gen, cname_const & 0xff, ast->line);
                // 常量值
                Ast* cexpr = ast->u.struct_def.const_values[i];
                Value cval = ast_default_to_value(cexpr);
                if (val_is_null(cval) && cexpr && cexpr->kind != AST_NULL) {
                    // 非常量表达式，用 null 作为占位
                    cval = val_null();
                }
                int cval_const = make_constant(gen, cval);
                emit_byte(gen, (cval_const >> 8) & 0xff, ast->line);
                emit_byte(gen, cval_const & 0xff, ast->line);
            }

            if (method_name_consts) free(method_name_consts);
            if (method_func_consts) free(method_func_consts);
            break;
        }
        case AST_CSTRUCT_DEF: {
            // 将 cstruct 名称作为常量
            ObjString* cstruct_name = str_copy(ast->u.cstruct_def.name, strlen(ast->u.cstruct_def.name));
            int name_const = make_constant(gen, val_obj((Object*)cstruct_name));

            // 生成 CSTRUCT_DEF 指令
            emit_byte(gen, OP_CSTRUCT_DEF, ast->line);
            emit_byte(gen, (name_const >> 8) & 0xff, ast->line);
            emit_byte(gen, name_const & 0xff, ast->line);
            emit_byte(gen, ast->u.cstruct_def.field_count, ast->line);
            emit_byte(gen, (ast->u.cstruct_def.total_size >> 8) & 0xff, ast->line);
            emit_byte(gen, ast->u.cstruct_def.total_size & 0xff, ast->line);
            emit_byte(gen, ast->u.cstruct_def.alignment, ast->line);

            // 为每个字段生成信息
            for (int i = 0; i < ast->u.cstruct_def.field_count; i++) {
                // 字段名
                ObjString* field_name = str_copy(ast->u.cstruct_def.field_names[i],
                                                  strlen(ast->u.cstruct_def.field_names[i]));
                int field_name_const = make_constant(gen, val_obj((Object*)field_name));
                emit_byte(gen, (field_name_const >> 8) & 0xff, ast->line);
                emit_byte(gen, field_name_const & 0xff, ast->line);

                // 字段类型
                TypeInfo* field_type = ast->u.cstruct_def.field_types[i];
                emit_byte(gen, field_type->kind, ast->line);

                // 字段偏移量
                emit_byte(gen, (ast->u.cstruct_def.field_offsets[i] >> 8) & 0xff, ast->line);
                emit_byte(gen, ast->u.cstruct_def.field_offsets[i] & 0xff, ast->line);

                // 数组维度（0 表示非数组，>0 表示数组大小）
                int array_dim = ast->u.cstruct_def.field_array_dims[i];
                emit_byte(gen, (array_dim >> 8) & 0xff, ast->line);
                emit_byte(gen, array_dim & 0xff, ast->line);

                // 嵌套结构体类型名（仅当 type == TYPE_CSTRUCT 时有效）
                if (field_type->kind == TYPE_CSTRUCT && field_type->struct_name) {
                    ObjString* struct_name = str_copy(field_type->struct_name, strlen(field_type->struct_name));
                    int struct_name_const = make_constant(gen, val_obj((Object*)struct_name));
                    emit_byte(gen, (struct_name_const >> 8) & 0xff, ast->line);
                    emit_byte(gen, struct_name_const & 0xff, ast->line);
                } else {
                    // 非嵌套结构体字段，输出 0xFFFF 表示无效
                    emit_byte(gen, 0xff, ast->line);
                    emit_byte(gen, 0xff, ast->line);
                }

                // 如果字段是 Ptr[T] 类型，输出元素类型
                if (field_type->kind == TYPE_PTR_GENERIC) {
                    TypeKind elem_type = field_type->element_type ? field_type->element_type->kind : TYPE_PTR;
                    emit_byte(gen, elem_type, ast->line);
                }
            }

            // 将 cstruct 定义存储到变量
            if (ast->u.cstruct_def.ref.kind == SYM_GLOBAL) {
                emit_bytes_2(gen, OP_SET_GLOBAL, ast->u.cstruct_def.ref.index, ast->line);
            } else if (ast->u.cstruct_def.ref.kind == SYM_LOCAL) {
                emit_bytes_2(gen, OP_SET_LOCAL, ast->u.cstruct_def.ref.index, ast->line);
            } else if (ast->u.cstruct_def.ref.kind == SYM_MODULE) {
                emit_bytes_2(gen, OP_SET_MODULE_VAR, ast->u.cstruct_def.ref.index, ast->line);
                emit_byte(gen, OP_POP, ast->line);
            } else {
                emit_byte(gen, OP_POP, ast->line);
            }
            break;
        }
        case AST_CLIB_DEF:
            // clib 定义是纯编译期语法糖，不生成运行时指令
            // 所有函数签名信息已在语义分析阶段存入 Symbol
            break;
        case AST_CFUNC_DECL:
            // cfunc 声明是纯编译期语法糖，不生成运行时指令
            // 签名信息在语义分析阶段存入 Symbol，供 ffi.callback 调用时使用
            break;
        case AST_ALIAS:
            if (ast->u.alias.expr) {
                // 值别名：生成表达式求值 + 定义变量
                gen_expr(gen, ast->u.alias.expr);
                if (ast->u.alias.ref.kind == SYM_GLOBAL) {
                    emit_define_global(gen, ast->u.alias.ref.index, ast->line);
                } else if (ast->u.alias.ref.kind == SYM_MODULE) {
                    emit_bytes_2(gen, OP_SET_MODULE_VAR, ast->u.alias.ref.index, ast->line);
                    emit_byte(gen, OP_POP, ast->line);
                }
            }
            // 类型别名是纯编译期语法糖，不生成运行时指令
            break;
        case AST_ENUM_DEF: {
            // 生成 enum 定义指令
            // 将 enum 名称作为常量
            ObjString* enum_name = str_copy(ast->u.enum_def.name, strlen(ast->u.enum_def.name));
            int name_const = make_constant(gen, val_obj((Object*)enum_name));

            emit_byte(gen, OP_ENUM_DEF, ast->line);
            emit_byte(gen, (name_const >> 8) & 0xff, ast->line);
            emit_byte(gen, name_const & 0xff, ast->line);
            emit_byte(gen, ast->u.enum_def.member_count, ast->line);

            // 为每个成员生成信息
            for (int i = 0; i < ast->u.enum_def.member_count; i++) {
                // 成员名
                ObjString* member_name = str_copy(ast->u.enum_def.member_names[i],
                                                  strlen(ast->u.enum_def.member_names[i]));
                int member_name_const = make_constant(gen, val_obj((Object*)member_name));
                emit_byte(gen, (member_name_const >> 8) & 0xff, ast->line);
                emit_byte(gen, member_name_const & 0xff, ast->line);

                // 成员值（作为常量）- 使用val_int_safe支持大整数
                Value member_val = val_int_safe(ast->u.enum_def.member_values[i]);
                int member_val_const = make_constant(gen, member_val);
                emit_byte(gen, (member_val_const >> 8) & 0xff, ast->line);
                emit_byte(gen, member_val_const & 0xff, ast->line);
            }

            // 将 enum 定义存储到变量，以便后续访问
            if (ast->u.enum_def.ref.kind == SYM_GLOBAL) {
                emit_bytes_2(gen, OP_SET_GLOBAL, ast->u.enum_def.ref.index, ast->line);
            } else if (ast->u.enum_def.ref.kind == SYM_LOCAL) {
                emit_bytes_2(gen, OP_SET_LOCAL, ast->u.enum_def.ref.index, ast->line);
            } else if (ast->u.enum_def.ref.kind == SYM_MODULE) {
                emit_bytes_2(gen, OP_SET_MODULE_VAR, ast->u.enum_def.ref.index, ast->line);
                emit_byte(gen, OP_POP, ast->line);
            } else {
                // 未知类型，弹出栈顶
                emit_byte(gen, OP_POP, ast->line);
            }
            break;
        }
        default:
            gen_expr(gen, ast);
            emit_byte(gen, OP_POP, ast->line);
            break;
    }
}

// ============================================================================
// 模块代码生成
// ============================================================================

// 全局函数字典（用于模块代码生成）
static ObjDict* g_func_dict = NULL;

void codegen_set_func_dict(void* dict) {
    g_func_dict = (ObjDict*)dict;
}

// 生成模块函数定义
static void gen_func_module(CodeGen* gen, Ast* ast) {
    if (!ast || ast->kind != AST_FUNC_DEF) return;

    // 如果全局函数字典存在，从中获取函数对象
    if (g_func_dict) {
        ObjString* key = str_copy(ast->u.func.name, (int)strlen(ast->u.func.name));
        Value func_val = dict_get(g_func_dict, val_obj((Object*)key));
        if (!val_is_null(func_val)) {
            int func_const = make_constant(gen, func_val);
            emit_closure(gen, func_const, ast->line);
            // 使用 OP_DEFINE_MODULE_FUNC 定义模块函数
            emit_bytes_2(gen, OP_DEFINE_MODULE_FUNC, ast->u.func.ref.index, ast->line);
            return;
        }
    }
    
    // 回退到原来的实现（使用 null 占位符）
    int func_const = make_constant(gen, val_null());
    emit_closure(gen, func_const, ast->line);
    // 使用 OP_DEFINE_MODULE_FUNC 定义模块函数
    emit_bytes_2(gen, OP_DEFINE_MODULE_FUNC, ast->u.func.ref.index, ast->line);
}

// 生成模块变量声明
static void gen_var_decl_module(CodeGen* gen, Ast* ast) {
    if (!ast || ast->kind != AST_VAR_DECL) return;

    // 生成初始化表达式
    if (ast->u.var_decl.init) {
        gen_expr(gen, ast->u.var_decl.init);
    } else {
        // 检查是否是数组类型，如果是则创建空数组
        if (ast->u.var_decl.type && ast->u.var_decl.type->kind == TYPE_ARRAY) {
            emit_byte(gen, OP_ARRAY, ast->line);
            emit_byte(gen, 0, ast->line); // count 高字节
            emit_byte(gen, 0, ast->line); // count 低字节
        }
        // 检查是否是字典类型，如果是则创建空字典
        else if (ast->u.var_decl.type && ast->u.var_decl.type->kind == TYPE_DICT) {
            emit_byte(gen, OP_DICT, ast->line);
            emit_byte(gen, 0, ast->line); // count 高字节
            emit_byte(gen, 0, ast->line); // count 低字节
        } else {
            emit_byte(gen, OP_NULL, ast->line);
        }
    }

    // 使用 OP_SET_MODULE_VAR 设置模块变量
    emit_bytes_2(gen, OP_SET_MODULE_VAR, ast->u.var_decl.ref.index, ast->line);
    emit_byte(gen, OP_POP, ast->line);
}

// 生成模块结构体定义
static void gen_struct_module(CodeGen* gen, Ast* ast) {
    if (!ast || ast->kind != AST_STRUCT_DEF) return;

    // 预生成方法函数对象常量
    int* method_name_consts = NULL;
    int* method_func_consts = NULL;
    int method_count = ast->u.struct_def.method_count;
    if (method_count > 0) {
        method_name_consts = (int*)malloc(sizeof(int) * method_count);
        method_func_consts = (int*)malloc(sizeof(int) * method_count);
        for (int i = 0; i < method_count; i++) {
            Ast* method_ast = ast->u.struct_def.methods[i];
            if (method_ast && method_ast->kind == AST_FUNC_DEF) {
                // 方法名作为常量
                ObjString* method_name = str_copy(method_ast->u.func.name, strlen(method_ast->u.func.name));
                method_name_consts[i] = make_constant(gen, val_obj((Object*)method_name));
                
                // 如果全局函数字典存在，从中获取函数对象
                // 注意：func_dict 中以 "StructName::methodName" 格式存储方法
                if (g_func_dict) {
                    char method_key[256];
                    snprintf(method_key, sizeof(method_key), "%s::%s",
                             ast->u.struct_def.name, method_ast->u.func.name);
                    ObjString* dict_key = str_copy(method_key, (int)strlen(method_key));
                    Value func_val = dict_get(g_func_dict, val_obj((Object*)dict_key));
                    if (!val_is_null(func_val)) {
                        method_func_consts[i] = make_constant(gen, func_val);
                    } else {
                        // 回退：生成函数原型
                        ObjFunction* func = gen_func_proto(gen, method_ast);
                        if (func) {
                            method_func_consts[i] = make_constant(gen, val_obj((Object*)func));
                        } else {
                            method_func_consts[i] = make_constant(gen, val_null());
                        }
                    }
                } else {
                    // 生成函数原型（函数对象）
                    ObjFunction* func = gen_func_proto(gen, method_ast);
                    if (func) {
                        method_func_consts[i] = make_constant(gen, val_obj((Object*)func));
                    } else {
                        method_func_consts[i] = make_constant(gen, val_null());
                    }
                }
            } else {
                method_name_consts[i] = make_constant(gen, val_null());
                method_func_consts[i] = make_constant(gen, val_null());
            }
        }
    }

    // 生成结构体定义指令
    // 将结构体名称作为常量
    ObjString* struct_name = str_copy(ast->u.struct_def.name, strlen(ast->u.struct_def.name));
    int name_const = make_constant(gen, val_obj((Object*)struct_name));

    emit_byte(gen, OP_STRUCT_DEF, ast->line);
    emit_byte(gen, (name_const >> 8) & 0xff, ast->line);
    emit_byte(gen, name_const & 0xff, ast->line);
    emit_byte(gen, ast->u.struct_def.field_count, ast->line);
    emit_byte(gen, method_count, ast->line);

    // 编码 impl 声明的 face 名称
    emit_byte(gen, ast->u.struct_def.impl_count, ast->line);
    for (int i = 0; i < ast->u.struct_def.impl_count; i++) {
        ObjString* impl_name = str_copy(ast->u.struct_def.impl_names[i],
                                        strlen(ast->u.struct_def.impl_names[i]));
        int impl_name_const = make_constant(gen, val_obj((Object*)impl_name));
        emit_byte(gen, (impl_name_const >> 8) & 0xff, ast->line);
        emit_byte(gen, impl_name_const & 0xff, ast->line);
    }

    // 编码泛型类型参数信息
    emit_byte(gen, ast->u.struct_def.type_param_count, ast->line);
    for (int i = 0; i < ast->u.struct_def.type_param_count && ast->u.struct_def.type_params; i++) {
        ObjString* param_name = str_copy(ast->u.struct_def.type_params[i],
                                          strlen(ast->u.struct_def.type_params[i]));
        int param_name_const = make_constant(gen, val_obj((Object*)param_name));
        emit_byte(gen, (param_name_const >> 8) & 0xff, ast->line);
        emit_byte(gen, param_name_const & 0xff, ast->line);
    }

    // 为每个字段生成信息
    for (int i = 0; i < ast->u.struct_def.field_count; i++) {
        // 字段名
        ObjString* field_name = str_copy(ast->u.struct_def.field_names[i],
                                          strlen(ast->u.struct_def.field_names[i]));
        int field_name_const = make_constant(gen, val_obj((Object*)field_name));
        emit_byte(gen, (field_name_const >> 8) & 0xff, ast->line);
        emit_byte(gen, field_name_const & 0xff, ast->line);

        // 字段类型
        TypeKind field_type = ast->u.struct_def.field_types[i]->kind;
        emit_byte(gen, field_type, ast->line);

        // nullable 标记
        uint8_t is_nullable2 = ast->u.struct_def.field_types[i]->nullable ? 1 : 0;
        emit_byte(gen, is_nullable2, ast->line);

        // 如果字段是 struct 类型，输出 struct 类型名
        if (field_type == TYPE_STRUCT) {
            const char* struct_type_name = ast->u.struct_def.field_types[i]->struct_name;
            if (struct_type_name) {
                ObjString* type_name = str_copy(struct_type_name, strlen(struct_type_name));
                int type_name_const = make_constant(gen, val_obj((Object*)type_name));
                emit_byte(gen, 1, ast->line); // 有 struct 类型名
                emit_byte(gen, (type_name_const >> 8) & 0xff, ast->line);
                emit_byte(gen, type_name_const & 0xff, ast->line);
            } else {
                emit_byte(gen, 0, ast->line); // 没有 struct 类型名
            }
        }

        // 如果字段是 Ptr[T] 类型，输出元素类型
        if (field_type == TYPE_PTR_GENERIC) {
            TypeInfo* ft = ast->u.struct_def.field_types[i];
            TypeKind elem_type = ft->element_type ? ft->element_type->kind : TYPE_PTR;
            emit_byte(gen, elem_type, ast->line);
        }

        // 是否有默认值
        if (ast->u.struct_def.field_defaults[i]) {
            Ast* default_expr = ast->u.struct_def.field_defaults[i];
            Value default_val = ast_default_to_value(default_expr);
            // 非常量默认值（如 new Point(x=99)）无法在编译期求值
            if (val_is_null(default_val) && default_expr->kind != AST_NULL) {
                char msg[BUFFER_MEDIUM];
                snprintf(msg, sizeof(msg),
                    "struct 字段 '%s' 的默认值不是常量表达式（仅支持数字、字符串、bool、null、数组字面量、字典字面量），"
                    "请使用构造器初始化",
                    ast->u.struct_def.field_names[i]);
                error_add(ERR_SEMANTIC, ast->line, msg);
                emit_byte(gen, 0, ast->line); // 无默认值
            } else {
                emit_byte(gen, 1, ast->line);
                int default_const = make_constant(gen, default_val);
                emit_byte(gen, (default_const >> 8) & 0xff, ast->line);
                emit_byte(gen, default_const & 0xff, ast->line);
            }
        } else {
            emit_byte(gen, 0, ast->line);
        }
    }

    // 输出方法信息
    for (int i = 0; i < method_count; i++) {
        emit_byte(gen, (method_name_consts[i] >> 8) & 0xff, ast->line);
        emit_byte(gen, method_name_consts[i] & 0xff, ast->line);
        emit_byte(gen, (method_func_consts[i] >> 8) & 0xff, ast->line);
        emit_byte(gen, method_func_consts[i] & 0xff, ast->line);
    }

    // 构造/析构函数标志和索引
    int ctor_idx = -1, dtor_idx = -1;
    for (int i = 0; i < method_count; i++) {
        Ast* method_ast = ast->u.struct_def.methods[i];
        if (method_ast && method_ast->u.func.is_ctor) ctor_idx = i;
        if (method_ast && method_ast->u.func.is_dtor) dtor_idx = i;
    }
    uint8_t ctor_dtor_flags = 0;
    if (ctor_idx >= 0) ctor_dtor_flags |= 1;
    if (dtor_idx >= 0) ctor_dtor_flags |= 2;
    emit_byte(gen, ctor_dtor_flags, ast->line);
    if (ctor_idx >= 0) emit_byte(gen, (uint8_t)ctor_idx, ast->line);
    if (dtor_idx >= 0) emit_byte(gen, (uint8_t)dtor_idx, ast->line);

    // 编码关联常量信息
    emit_byte(gen, ast->u.struct_def.const_count, ast->line);
    for (int i = 0; i < ast->u.struct_def.const_count; i++) {
        // 常量名
        ObjString* cname = str_copy(ast->u.struct_def.const_names[i],
                                     strlen(ast->u.struct_def.const_names[i]));
        int cname_const = make_constant(gen, val_obj((Object*)cname));
        emit_byte(gen, (cname_const >> 8) & 0xff, ast->line);
        emit_byte(gen, cname_const & 0xff, ast->line);
        // 常量值
        Ast* cexpr = ast->u.struct_def.const_values[i];
        Value cval = ast_default_to_value(cexpr);
        if (val_is_null(cval) && cexpr && cexpr->kind != AST_NULL) {
            cval = val_null();
        }
        int cval_const = make_constant(gen, cval);
        emit_byte(gen, (cval_const >> 8) & 0xff, ast->line);
        emit_byte(gen, cval_const & 0xff, ast->line);
    }

    if (method_name_consts) free(method_name_consts);
    if (method_func_consts) free(method_func_consts);
}

// 生成模块中的 enum 定义（存储到模块变量）
static void gen_enum_module(CodeGen* gen, Ast* ast) {
    // 生成 enum 定义指令
    // 将 enum 名称作为常量
    ObjString* enum_name = str_copy(ast->u.enum_def.name, strlen(ast->u.enum_def.name));
    int name_const = make_constant(gen, val_obj((Object*)enum_name));

    emit_byte(gen, OP_ENUM_DEF, ast->line);
    emit_byte(gen, (name_const >> 8) & 0xff, ast->line);
    emit_byte(gen, name_const & 0xff, ast->line);
    emit_byte(gen, ast->u.enum_def.member_count, ast->line);

    // 为每个成员生成信息
    for (int i = 0; i < ast->u.enum_def.member_count; i++) {
        // 成员名
        ObjString* member_name = str_copy(ast->u.enum_def.member_names[i],
                                          strlen(ast->u.enum_def.member_names[i]));
        int member_name_const = make_constant(gen, val_obj((Object*)member_name));
        emit_byte(gen, (member_name_const >> 8) & 0xff, ast->line);
        emit_byte(gen, member_name_const & 0xff, ast->line);

        // 成员值（作为常量）- 使用val_int_safe支持大整数
        Value member_val = val_int_safe(ast->u.enum_def.member_values[i]);
        int member_val_const = make_constant(gen, member_val);
        emit_byte(gen, (member_val_const >> 8) & 0xff, ast->line);
        emit_byte(gen, member_val_const & 0xff, ast->line);
    }

    // 将 enum 定义存储到模块变量
    emit_bytes_2(gen, OP_SET_MODULE_VAR, ast->u.enum_def.ref.index, ast->line);
    emit_byte(gen, OP_POP, ast->line);
}

// 生成模块块
void gen_block_module(CodeGen* gen, Ast* ast) {
    // 第一遍：为所有函数定义预留位置（包括 export 的函数）
    for (int i = 0; i < ast->u.block.count; i++) {
        Ast* stmt = ast->u.block.items[i];
        Ast* func_ast = NULL;
        
        if (stmt->kind == AST_FUNC_DEF) {
            func_ast = stmt;
        } else if (stmt->kind == AST_EXPORT && stmt->u.export.decl &&
                   stmt->u.export.decl->kind == AST_FUNC_DEF) {
            func_ast = stmt->u.export.decl;
        }
        
        if (func_ast) {
            // 压入 null 作为占位符
            emit_byte(gen, OP_NULL, func_ast->line);
            emit_bytes_2(gen, OP_SET_MODULE_VAR, func_ast->u.func.ref.index, func_ast->line);
            emit_byte(gen, OP_POP, func_ast->line);
        }
    }

    // 第二遍：生成函数定义（包括 export 的函数）
    for (int i = 0; i < ast->u.block.count; i++) {
        Ast* stmt = ast->u.block.items[i];
        Ast* func_ast = NULL;
        
        if (stmt->kind == AST_FUNC_DEF) {
            func_ast = stmt;
        } else if (stmt->kind == AST_EXPORT && stmt->u.export.decl &&
                   stmt->u.export.decl->kind == AST_FUNC_DEF) {
            func_ast = stmt->u.export.decl;
        }
        
        if (func_ast) {
            gen_func_module(gen, func_ast);
        }
    }

    // 第三遍：生成其他语句
    for (int i = 0; i < ast->u.block.count; i++) {
        Ast* stmt = ast->u.block.items[i];
        int is_func = 0;
        
        if (stmt->kind == AST_FUNC_DEF) {
            is_func = 1;
        } else if (stmt->kind == AST_EXPORT && stmt->u.export.decl &&
                   stmt->u.export.decl->kind == AST_FUNC_DEF) {
            is_func = 1;
        }
        
        if (!is_func) {
            gen_stmt_module(gen, stmt);
        }
    }
}

// 生成模块语句
void gen_stmt_module(CodeGen* gen, Ast* ast) {
    if (!ast) return;

    switch (ast->kind) {
        case AST_BLOCK:
            gen_block_module(gen, ast);
            break;
        case AST_IF:
            gen_if(gen, ast);  // 复用普通 if 生成
            break;
        case AST_WHILE:
            gen_while(gen, ast);  // 复用普通 while 生成
            break;
        case AST_FOR:
            gen_for(gen, ast);  // 复用普通 for 生成
            break;
        case AST_SWITCH:
            gen_switch(gen, ast);  // 复用普通 switch 生成
            break;
        case AST_FUNC_DEF:
            // 函数定义在 gen_block_module 中处理
            break;
        case AST_RETURN:
            gen_return(gen, ast);  // 复用普通 return 生成
            break;
        case AST_BREAK:
        case AST_CONTINUE:
            // 复用普通 break/continue 生成
            gen_stmt(gen, ast);
            break;
        case AST_VAR_DECL:
            gen_var_decl_module(gen, ast);
            break;
        case AST_ASSIGN:
            gen_assign(gen, ast);  // 复用普通赋值生成
            break;
        case AST_COMPOUND_ASSIGN:
            gen_compound_assign(gen, ast);  // 复用复合赋值生成
            break;
        case AST_EXPR_STMT:
            gen_expr_stmt(gen, ast);
            break;
        case AST_IMPORT:
            // 模块内可以 import 其他模块
            gen_import_inline(gen, ast);
            break;
        case AST_EXPORT:
            // 导出声明在编译时处理，运行时不需要生成代码
            if (ast->u.export.decl) {
                if (ast->u.export.decl->kind == AST_FUNC_DEF) {
                    gen_func_module(gen, ast->u.export.decl);
                } else if (ast->u.export.decl->kind == AST_VAR_DECL) {
                    gen_var_decl_module(gen, ast->u.export.decl);
                } else if (ast->u.export.decl->kind == AST_STRUCT_DEF) {
                    gen_struct_module(gen, ast->u.export.decl);
                } else if (ast->u.export.decl->kind == AST_ENUM_DEF) {
                    // enum 定义在模块中生成
                    gen_enum_module(gen, ast->u.export.decl);
                } else if (ast->u.export.decl->kind == AST_FACE_DEF) {
                    gen_stmt(gen, ast->u.export.decl);
                } else if (ast->u.export.decl->kind == AST_CSTRUCT_DEF) {
                    gen_stmt(gen, ast->u.export.decl);
                }
            }
            break;
        case AST_TRY:
            // 复用普通 try-catch-finally 生成
            gen_stmt(gen, ast);
            break;
        case AST_THROW:
            gen_stmt(gen, ast);
            break;
        case AST_DEFER:
            gen_stmt(gen, ast);
            break;
        case AST_STRUCT_DEF:
            gen_struct_module(gen, ast);
            break;
        case AST_FACE_DEF:
            gen_stmt(gen, ast);  // face 定义在 gen_stmt 中处理
            break;
        case AST_CSTRUCT_DEF:
            gen_stmt(gen, ast);  // cstruct 定义在 gen_stmt 中处理
            break;
        case AST_CLIB_DEF:
            // clib 定义是纯编译期语法糖，不生成运行时指令
            break;
        case AST_CFUNC_DECL:
            // 纯编译期语法糖，不生成运行时指令
            break;
        case AST_ALIAS:
            if (ast->u.alias.expr) {
                // 值别名：生成表达式求值 + 定义模块变量
                gen_expr(gen, ast->u.alias.expr);
                if (ast->u.alias.ref.kind == SYM_GLOBAL) {
                    emit_define_global(gen, ast->u.alias.ref.index, ast->line);
                } else if (ast->u.alias.ref.kind == SYM_MODULE) {
                    emit_bytes_2(gen, OP_SET_MODULE_VAR, ast->u.alias.ref.index, ast->line);
                    emit_byte(gen, OP_POP, ast->line);
                }
            }
            // 类型别名是纯编译期语法糖，不生成运行时指令
            break;
        case AST_USE:
            // use 语句是编译时指令，不生成运行时指令
            break;
        case AST_ENUM_DEF:
            gen_enum_module(gen, ast);
            break;
        default:
            gen_expr(gen, ast);
            emit_byte(gen, OP_POP, ast->line);
            break;
    }
}
