// ============================================================================
// 寄存器式 codegen：语句生成
// ============================================================================

#include "codegen.h"

// 前向声明（与 codegen.h 中非 static 声明一致）
void gen_stmt(CodeGen* gen, Ast* ast);
static void gen_var_decl(CodeGen* gen, Ast* ast);
static void gen_return(CodeGen* gen, Ast* ast);
static void gen_return_multi(CodeGen* gen, Ast* ast);
static void gen_while(CodeGen* gen, Ast* ast);
static void gen_for(CodeGen* gen, Ast* ast);
static void gen_for_iter(CodeGen* gen, Ast* ast);
static void gen_switch(CodeGen* gen, Ast* ast);
static void gen_try(CodeGen* gen, Ast* ast);
static void gen_throw(CodeGen* gen, Ast* ast);
static void gen_struct_def(CodeGen* gen, Ast* ast);
static void gen_enum_def(CodeGen* gen, Ast* ast);
static void gen_face_def(CodeGen* gen, Ast* ast);
static void gen_cstruct_def(CodeGen* gen, Ast* ast);
static void gen_clib_def(CodeGen* gen, Ast* ast);
static void gen_cfunc_decl(CodeGen* gen, Ast* ast);
static void gen_alias(CodeGen* gen, Ast* ast);
static void gen_destruct_decl(CodeGen* gen, Ast* ast);
static void gen_import(CodeGen* gen, Ast* ast);
static void gen_export(CodeGen* gen, Ast* ast);
static void gen_use(CodeGen* gen, Ast* ast);

// 表达式声明（codegen_expr.c）
extern void gen_expr_to(CodeGen* gen, Ast* ast, int dst);
extern int gen_expr(CodeGen* gen, Ast* ast);

// ============================================================================
// 赋值/声明处的基本类型 CAST（与栈式 codegen_stmt.c 的 assign_cast_needed 一致）
// ----------------------------------------------------------------------------
// 只有"字面量且静态类型与目标一致"才可省 CAST；其余节点（比较、算术、调用…）
// 的 cached_type 只是推断结果，运行时可能不是该类型 —— 必须保留 CAST 做规范化
// （例：`int i = (k >= 0)` 得到的是 bool，必须 CAST_INT 成 1/0）。
// ============================================================================
static int assign_cast_needed(TypeKind target_kind, Ast* value_ast) {
    if (!value_ast) return 1;
    switch (value_ast->kind) {
        case AST_NUM:
        case AST_STRING:
        case AST_BOOL:
        case AST_NULL:
            return !value_ast->cached_type || value_ast->cached_type->kind != target_kind;
        default:
            return 1;
    }
}

// 若目标声明类型是 int/float/string，就地插入对应的 CAST
static void emit_cast_for_target(CodeGen* gen, TypeKind target_kind, Ast* value_ast, int reg, int line) {
    if (!value_ast) return;
    if (target_kind != TYPE_INT && target_kind != TYPE_FLOAT && target_kind != TYPE_STRING) return;
    if (!assign_cast_needed(target_kind, value_ast)) return;
    if (target_kind == TYPE_INT) emit_cast_int(gen, reg, reg, line);
    else if (target_kind == TYPE_FLOAT) emit_cast_float(gen, reg, reg, line);
    else emit_cast_string(gen, reg, reg, line);
}

// ============================================================================
// ast_default_to_value：把 AST 常量表达式求值为 Value（编译期）
// 寄存器式与栈式语义一致（直接照搬栈式基线实现，供全局变量默认值等场景使用）
// ============================================================================
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

// ============================================================================
// 语句分发
// ============================================================================

void gen_stmt(CodeGen* gen, Ast* ast) {
    if (!ast) return;

    switch (ast->kind) {
        case AST_BLOCK:       gen_block(gen, ast); break;
        case AST_VAR_DECL:   gen_var_decl(gen, ast); break;
        case AST_IF:          gen_if(gen, ast); break;
        case AST_WHILE:       gen_while(gen, ast); break;
        case AST_FOR:         gen_for(gen, ast); break;
        case AST_SWITCH:      gen_switch(gen, ast); break;
        case AST_RETURN:      gen_return(gen, ast); break;
        case AST_RETURN_MULTI: gen_return_multi(gen, ast); break;
        case AST_BREAK: {
            // break: 跳转到循环尾
            if (gen->loop_head) {
                int jmp = emit_jmp(gen, ast->line);
                gen->loop_head->ctx.break_jumps[gen->loop_head->ctx.break_count++] = jmp;
            }
            break;
        }
        case AST_CONTINUE: {
            // continue: 跳回循环头
            if (gen->loop_head) {
                int jmp = emit_jmp(gen, ast->line);
                gen->loop_head->ctx.continue_jumps[gen->loop_head->ctx.continue_count++] = jmp;
            }
            break;
        }
        case AST_ASSIGN:      gen_assign(gen, ast); break;
        case AST_COMPOUND_ASSIGN: gen_compound_assign(gen, ast); break;
        case AST_EXPR_STMT: {
            // 赋值语句会被解析器包在表达式语句里，直接走赋值生成（少一次搬运）
            Ast* e = ast->u.expr_stmt.expr;
            if (e && (e->kind == AST_ASSIGN || e->kind == AST_COMPOUND_ASSIGN)) {
                gen_stmt(gen, e);
                break;
            }
            int r = gen_expr(gen, ast->u.expr_stmt.expr);
            reg_free(gen, r);
            break;
        }
        case AST_FUNC_DEF:    gen_func(gen, ast); break;
        case AST_IMPORT:      gen_import(gen, ast); break;
        case AST_EXPORT:      gen_export(gen, ast); break;
        case AST_USE:         gen_use(gen, ast); break;
        case AST_TRY:         gen_try(gen, ast); break;
        case AST_THROW:       gen_throw(gen, ast); break;
        case AST_STRUCT_DEF:  gen_struct_def(gen, ast); break;
        case AST_ENUM_DEF:    gen_enum_def(gen, ast); break;
        case AST_FACE_DEF:    gen_face_def(gen, ast); break;
        case AST_CSTRUCT_DEF: gen_cstruct_def(gen, ast); break;
        case AST_CLIB_DEF:    gen_clib_def(gen, ast); break;
        case AST_CFUNC_DECL:  gen_cfunc_decl(gen, ast); break;
        case AST_ALIAS:       gen_alias(gen, ast); break;
        case AST_DESTRUCT_DECL: gen_destruct_decl(gen, ast); break;
        default:
            // 未知节点：忽略
            break;
    }
}

// ============================================================================
// block
// ============================================================================

void gen_block(CodeGen* gen, Ast* ast) {
    if (!ast) return;
    int n = ast->u.block.count;
    for (int i = 0; i < n; i++) {
        gen_stmt(gen, ast->u.block.items[i]);
    }
}

// ============================================================================
// if / if-else
// ============================================================================

void gen_if(CodeGen* gen, Ast* ast) {
    gen_if_ex(gen, ast, 0);
}

void gen_if_ex(CodeGen* gen, Ast* ast, int want_value) {
    int dst = -1;
    if (want_value) {
        dst = reg_alloc(gen);
    }

    // 求值条件
    int cond = gen_expr(gen, ast->u.if_.cond);

    // if !cond then jump to else
    int jmp_false = emit_jmp_if_false(gen, cond, ast->line);
    reg_free(gen, cond);

    // then 分支
    if (want_value && ast->u.if_.then) {
        // then 的最后一条表达式写入 dst
        if (ast->u.if_.then->kind == AST_EXPR_STMT) {
            gen_expr_to(gen, ast->u.if_.then->u.expr_stmt.expr, dst);
        } else {
            gen_stmt(gen, ast->u.if_.then);
            if (want_value) emit_loadnil_to(gen, dst, ast->line);
        }
    } else {
        gen_stmt(gen, ast->u.if_.then);
    }

    // if 有 else 分支
    if (ast->u.if_.else_) {
        int jmp_end = emit_jmp(gen, ast->line);
        patch_jmp(gen, jmp_false);

        // else 分支
        if (want_value && ast->u.if_.else_) {
            if (ast->u.if_.else_->kind == AST_EXPR_STMT) {
                gen_expr_to(gen, ast->u.if_.else_->u.expr_stmt.expr, dst);
            } else {
                gen_stmt(gen, ast->u.if_.else_);
                if (want_value) emit_loadnil_to(gen, dst, ast->line);
            }
        } else {
            gen_stmt(gen, ast->u.if_.else_);
        }

        patch_jmp(gen, jmp_end);
    } else {
        patch_jmp(gen, jmp_false);
        if (want_value) {
            emit_loadnil_to(gen, dst, ast->line);
        }
    }
}

// ============================================================================
// while
// ============================================================================

static void gen_while(CodeGen* gen, Ast* ast) {
    int loop_start = gen->chunk->len;

    // 压入循环上下文
    LoopContextNode* node = (LoopContextNode*)malloc(sizeof(LoopContextNode));
    node->prev = gen->loop_head;
    node->ctx.break_count = 0;
    node->ctx.continue_count = 0;
    node->ctx.continue_target = loop_start;
    gen->loop_head = node;
    gen->loop_count++;

    // 求值条件
    int cond = gen_expr(gen, ast->u.while_.cond);
    int jmp_end = emit_jmp_if_false(gen, cond, ast->line);
    reg_free(gen, cond);

    // 循环体
    gen_stmt(gen, ast->u.while_.body);

    // 回跳
    emit_loop(gen, loop_start, ast->line);

    // patch break 跳转
    int end_pos = gen->chunk->len;
    patch_jmp(gen, jmp_end);
    for (int i = 0; i < node->ctx.break_count; i++) {
        patch_jmp(gen, node->ctx.break_jumps[i]);
    }

    // 弹出循环上下文
    gen->loop_head = node->prev;
    free(node);
    gen->loop_count--;
}

// ============================================================================
// for
// ============================================================================

static void gen_for(CodeGen* gen, Ast* ast) {
    Ast* end_expr = ast->u.for_.end;

    // --- 判断是"容器迭代"还是"数值区间" ---
    // 无 start、有循环变量，且被遍历对象不是数字时按容器迭代处理。
    int is_iter = 0;
    if (!ast->u.for_.start && ast->u.for_.var_name) {
        if (is_string_expr(end_expr) || is_array_expr(end_expr) || is_dict_expr(end_expr)) {
            is_iter = 1;
        } else if (is_var_expr(end_expr) && end_expr->kind == AST_VAR) {
            TypeKind vt = end_expr->u.var.ref.type_kind;
            is_iter = !(vt == TYPE_INT || vt == TYPE_FLOAT);
        }
        // 其余（数字字面量 / 调用 / 未知表达式）按数值区间循环处理：
        // `for 10 to i` 是 0..9，不能当容器遍历。
    }
    if (is_iter) { gen_for_iter(gen, ast); return; }

    // --- 数值区间循环（照栈式：四个独立 slot，用 8 字节 FOR_PREP/FOR_LOOP）---
    int loop_var_slot = ast->u.for_.loop_var_index;
    int end_slot = ast->u.for_.end_index;
    int step_slot = ast->u.for_.step_index;
    int start_slot = ast->u.for_.start_index;
    int counter_slot = ast->u.for_.counter_index;

    // 这些槽位本身就是寄存器号，先把高水位抬到它们之上
    int mx = loop_var_slot;
    if (end_slot > mx) mx = end_slot;
    if (step_slot > mx) mx = step_slot;
    if (start_slot > mx) mx = start_slot;
    if (counter_slot > mx) mx = counter_slot;
    if (mx + 1 > gen->next_reg) gen->next_reg = mx + 1;
    if (gen->next_reg > gen->max_reg) gen->max_reg = gen->next_reg;

    // 起止/步长求值到各自的槽位
    if (ast->u.for_.start) {
        gen_expr_to(gen, ast->u.for_.start, start_slot);
    } else {
        emit_loadi_to(gen, start_slot, 0, ast->line);
    }
    gen_expr_to(gen, ast->u.for_.end, end_slot);
    if (ast->u.for_.step) {
        gen_expr_to(gen, ast->u.for_.step, step_slot);
    } else {
        emit_loadi_to(gen, step_slot, 1, ast->line);
    }

    int has_loop_var = ast->u.for_.var_name != NULL;
    int var_slot = has_loop_var ? loop_var_slot : counter_slot;
    int inclusive = ast->u.for_.inclusive ? 1 : 0;

    LoopContextNode* node = (LoopContextNode*)malloc(sizeof(LoopContextNode));
    node->prev = gen->loop_head;
    node->ctx.break_count = 0;
    node->ctx.continue_count = 0;
    node->ctx.continue_target = 0;
    gen->loop_head = node;
    gen->loop_count++;

    // OP_FOR_PREP: op + start + end + step + [var, inclusive, sBx16] = 8 字节
    int prep_pos = gen->chunk->len;
    reg_encode_iABC(gen->chunk, OP_FOR_PREP, start_slot, end_slot, step_slot, ast->line);
    chunk_write(gen->chunk, (uint8_t)(var_slot & 0xFF), ast->line);
    chunk_write(gen->chunk, (uint8_t)inclusive, ast->line);
    chunk_write(gen->chunk, 0, ast->line);
    chunk_write(gen->chunk, 0, ast->line);

    int body_start = gen->chunk->len;

    if (ast->u.for_.body) gen_stmt(gen, ast->u.for_.body);

    // OP_FOR_LOOP: op + var + step + end + [inclusive, pad, sBx16] = 8 字节
    int loop_insn = gen->chunk->len;
    reg_encode_iABC(gen->chunk, OP_FOR_LOOP, var_slot, step_slot, end_slot, ast->line);
    chunk_write(gen->chunk, (uint8_t)inclusive, ast->line);
    chunk_write(gen->chunk, 0, ast->line);
    chunk_write(gen->chunk, 0, ast->line);
    chunk_write(gen->chunk, 0, ast->line);
    // 回跳目标 = body_start（sBx 相对"下一条指令" = loop_insn + 8）
    patch_sbx_at(gen, loop_insn + 6, body_start - (loop_insn + 8));

    // continue → FOR_LOOP（先自增再判断）
    for (int i = 0; i < node->ctx.continue_count; i++) {
        patch_jmp_to(gen, node->ctx.continue_jumps[i], loop_insn);
    }

    // FOR_PREP 的前跳：初始条件不满足时跳过整个循环
    patch_sbx_at(gen, prep_pos + 6, gen->chunk->len - (prep_pos + 8));

    for (int i = 0; i < node->ctx.break_count; i++) {
        patch_jmp(gen, node->ctx.break_jumps[i]);
    }

    gen->loop_head = node->prev;
    free(node);
    gen->loop_count--;
}

// 容器迭代：for arr to v [, idx] / for "abc" to ch / for d to k, v
static void gen_for_iter(CodeGen* gen, Ast* ast) {
    int lv = ast->u.for_.loop_var_index;
    int iv = ast->u.for_.index_var_index;
    int obj_slot = ast->u.for_.end_index;      // 被遍历对象
    int idx_slot = ast->u.for_.counter_index;  // 运行时索引
    // ⚠ 只有真的写了索引变量（index_var_name 非空）才有 index_var_index；
    //   后者未分配时是脏值（0），按 >=0 判断会把索引写进别人的槽位。
    int has_idx_var = (ast->u.for_.index_var_name != NULL);

    int mx = obj_slot;
    if (idx_slot > mx) mx = idx_slot;
    if (lv > mx) mx = lv;
    if (has_idx_var && iv > mx) mx = iv;
    if (mx + 1 > gen->next_reg) gen->next_reg = mx + 1;
    if (gen->next_reg > gen->max_reg) gen->max_reg = gen->next_reg;

    gen_expr_to(gen, ast->u.for_.end, obj_slot);
    emit_loadi_to(gen, idx_slot, 0, ast->line);

    LoopContextNode* node = (LoopContextNode*)malloc(sizeof(LoopContextNode));
    node->prev = gen->loop_head;
    node->ctx.break_count = 0;
    node->ctx.continue_count = 0;
    node->ctx.continue_target = 0;
    gen->loop_head = node;
    gen->loop_count++;

    int loop_start = gen->chunk->len;

    // 条件：idx < len(obj)
    int len_reg = reg_alloc(gen);
    reg_encode_iABC(gen->chunk, OP_LEN, len_reg, obj_slot, 0, ast->line);
    int cond = reg_alloc(gen);
    emit_lt(gen, cond, idx_slot, len_reg, ast->line);
    int exit_jump = emit_jmp_if_false(gen, cond, ast->line);
    reg_free(gen, cond);
    reg_free(gen, len_reg);

    // 元素 / 键 → 循环变量
    if (ast->u.for_.var_name) {
        reg_encode_iABC(gen->chunk, OP_ITER_GET, lv, obj_slot, idx_slot, ast->line);
    }
    if (has_idx_var) {
        emit_mov(gen, iv, idx_slot, ast->line);
    }

    if (ast->u.for_.body) gen_stmt(gen, ast->u.for_.body);

    // continue 目标 = 索引自增处
    int inc_pos = gen->chunk->len;
    emit_inc(gen, idx_slot, idx_slot, ast->line);
    emit_loop(gen, loop_start, ast->line);

    patch_jmp(gen, exit_jump);

    for (int i = 0; i < node->ctx.continue_count; i++) {
        patch_jmp_to(gen, node->ctx.continue_jumps[i], inc_pos);
    }
    for (int i = 0; i < node->ctx.break_count; i++) {
        patch_jmp(gen, node->ctx.break_jumps[i]);
    }

    gen->loop_head = node->prev;
    free(node);
    gen->loop_count--;
}

// ============================================================================
// switch
// ============================================================================

static void gen_switch(CodeGen* gen, Ast* ast) {
    // TODO: 完整 switch 实现
    // 简化：生成默认分支
    if (ast->u.switch_.default_body) {
        gen_stmt(gen, ast->u.switch_.default_body);
    }
}

// ============================================================================
// return
// ============================================================================

static void gen_return(CodeGen* gen, Ast* ast) {
    if (ast->u.ret) {
        int r = gen_expr(gen, ast->u.ret);
        emit_return(gen, r, 1, ast->line);
        reg_free(gen, r);
    } else {
        int r = reg_alloc(gen);
        emit_loadnil_to(gen, r, ast->line);
        emit_return(gen, r, 1, ast->line);
        reg_free(gen, r);
    }
}

static void gen_return_multi(CodeGen* gen, Ast* ast) {
    // 多值返回：求值每个返回值到连续寄存器
    int count = ast->u.ret_multi.count;
    if (count == 0) {
        int r = reg_alloc(gen);
        emit_loadnil_to(gen, r, ast->line);
        emit_return(gen, r, 1, ast->line);
        reg_free(gen, r);
        return;
    }

    // 求值第一个返回值
    int base = gen_expr(gen, ast->u.ret_multi.exprs[0]);
    // 其余返回值紧随
    for (int i = 1; i < count; i++) {
        int r = reg_alloc(gen);
        gen_expr_to(gen, ast->u.ret_multi.exprs[i], r);
    }

    // RETURN_MULTI: A = base, B = count_reg
    int count_reg = reg_alloc(gen);
    emit_loadi_to(gen, count_reg, count, ast->line);
    emit_return_multi(gen, base, count_reg, ast->line);
}

// ============================================================================
// var_decl
// ============================================================================

static void gen_var_decl(CodeGen* gen, Ast* ast) {
    SymRef* ref = &ast->u.var_decl.ref;
    if (!ref->name) return;

    // 全局变量：值存 vm.globals[ref->index]，寄存器只做中转
    if (ref->kind == SYM_GLOBAL) {
        int r = reg_alloc(gen);
        if (ast->u.var_decl.init) {
            gen_expr_to(gen, ast->u.var_decl.init, r);
        } else {
            emit_loadnil_to(gen, r, ast->line);
        }
        emit_defglobal(gen, r, ref->index, ast->line);
        reg_free(gen, r);
        return;
    }

    // 局部变量 / 参数：寄存器号 = ref->index（语义分析决定的固定槽位）
    int dst = ref->index;
    if (dst >= gen->next_reg) {
        gen->next_reg = dst + 1;
        if (gen->next_reg > gen->max_reg) gen->max_reg = gen->next_reg;
    }

    if (ast->u.var_decl.init) {
        gen_expr_to(gen, ast->u.var_decl.init, dst);
        TypeInfo* vt = ast->u.var_decl.type;
        if (vt) emit_cast_for_target(gen, vt->kind, ast->u.var_decl.init, dst, ast->line);
    } else {
        emit_loadnil_to(gen, dst, ast->line);
    }

    // 如果有析构函数，添加追踪
    TypeInfo* t = ast->u.var_decl.type;
    if (t && t->kind == TYPE_STRUCT && t->struct_name) {
        codegen_add_dtor_entry(gen, dst);
    }
}

// ============================================================================
// assign
// ============================================================================

// 取赋值目标的符号引用：语义分析把结果写在 assign.refs[i]（不是 targets[i]->u.var.ref）
static SymRef* assign_target_ref(Ast* ast, int i) {
    Ast* target = ast->u.assign.targets[i];
    if (ast->u.assign.refs && ast->u.assign.refs[i].name) {
        return &ast->u.assign.refs[i];
    }
    return &target->u.var.ref;
}

void gen_assign(CodeGen* gen, Ast* ast) {
    int n = ast->u.assign.name_count;
    if (n <= 0 || !ast->u.assign.targets) return;

    for (int i = 0; i < n; i++) {
        Ast* target = ast->u.assign.targets[i];
        if (!target) continue;

        // 右侧值：单目标直接取 value；多目标时 value 通常是数组字面量，取第 i 项
        Ast* value = ast->u.assign.value;
        if (n > 1 && value && value->kind == AST_ARRAY && i < value->u.array.count) {
            value = value->u.array.items[i];
        }

        switch (target->kind) {
            case AST_VAR: {
                SymRef* ref = assign_target_ref(ast, i);
                switch (ref->kind) {
                    case SYM_LOCAL:
                    case SYM_PARAM:
                        gen_expr_to(gen, value, ref->index);
                        emit_cast_for_target(gen, ref->type_kind, value, ref->index, ast->line);
                        break;
                    case SYM_GLOBAL: {
                        int r = gen_expr(gen, value);
                        emit_cast_for_target(gen, ref->type_kind, value, r, ast->line);
                        emit_setglobal(gen, r, ref->index, ast->line);
                        reg_free(gen, r);
                        break;
                    }
                    case SYM_UPVALUE: {
                        int r = gen_expr(gen, value);
                        emit_cast_for_target(gen, ref->type_kind, value, r, ast->line);
                        emit_setupval(gen, r, ref->index, ast->line);
                        reg_free(gen, r);
                        break;
                    }
                    case SYM_MODULE: {
                        int r = gen_expr(gen, value);
                        emit_cast_for_target(gen, ref->type_kind, value, r, ast->line);
                        reg_encode_iABC(gen->chunk, OP_SET_MODULE_VAR, r, ref->index, 0, ast->line);
                        reg_free(gen, r);
                        break;
                    }
                    default:
                        break;
                }
                break;
            }

            // 索引赋值：arr[i] = val / dict["k"] = val
            case AST_INDEX: {
                int obj_reg = gen_expr(gen, target->u.index.obj);
                int idx_reg = gen_expr(gen, target->u.index.index);
                int val_reg = gen_expr(gen, value);
                // INDEX_SET: R[B][R[C]] = R[A]
                reg_encode_iABC(gen->chunk, OP_INDEX_SET, val_reg, obj_reg, idx_reg, ast->line);
                reg_free(gen, val_reg);
                reg_free(gen, idx_reg);
                reg_free(gen, obj_reg);
                break;
            }

            case AST_INDEX_ASSIGN: {
                int obj_reg = gen_expr(gen, target->u.index_assign.obj);
                int idx_reg = gen_expr(gen, target->u.index_assign.index);
                int val_reg = gen_expr(gen, value);
                reg_encode_iABC(gen->chunk, OP_INDEX_SET, val_reg, obj_reg, idx_reg, ast->line);
                reg_free(gen, val_reg);
                reg_free(gen, idx_reg);
                reg_free(gen, obj_reg);
                break;
            }

            // 字段赋值：obj.field = val
            case AST_FIELD_ACCESS: {
                int obj_reg = gen_expr(gen, target->u.field_access.obj);
                int val_reg = gen_expr(gen, value);
                // SET_FIELD: R[B].field(C) = R[A]
                reg_encode_iABC(gen->chunk, OP_SET_FIELD, val_reg, obj_reg,
                                target->u.field_access.field_index, ast->line);
                reg_free(gen, val_reg);
                reg_free(gen, obj_reg);
                break;
            }

            default:
                break;
        }
    }
}

// ============================================================================
// compound_assign
// ============================================================================

void gen_compound_assign(CodeGen* gen, Ast* ast) {
    // a += expr → R[dst] = R[dst] + expr
    SymRef* ref = &ast->u.compound_assign.ref;
    if (!ref->name) return;

    int dst;
    int is_local = (ref->kind == SYM_LOCAL || ref->kind == SYM_PARAM);
    if (is_local) {
        dst = ref->index;
    } else {
        // 全局 / upvalue：先读到临时寄存器，算完再写回
        dst = reg_alloc(gen);
        if (ref->kind == SYM_GLOBAL) {
            emit_getglobal_to(gen, dst, ref->index, ast->line);
        } else if (ref->kind == SYM_UPVALUE) {
            emit_getupval_to(gen, dst, ref->index, ast->line);
        } else {
            emit_loadnil_to(gen, dst, ast->line);
        }
    }

    int r = gen_expr(gen, ast->u.compound_assign.value);

    LenoTokenType op = ast->u.compound_assign.op;
    switch (op) {
        case TOK_PLUSEQ:  emit_add(gen, dst, dst, r, ast->line); break;
        case TOK_MINUSEQ:  emit_sub(gen, dst, dst, r, ast->line); break;
        case TOK_STAREQ:   emit_mul(gen, dst, dst, r, ast->line); break;
        case TOK_SLASHEQ:  emit_div(gen, dst, dst, r, ast->line); break;
        case TOK_MODEQ:    emit_mod(gen, dst, dst, r, ast->line); break;
        case TOK_BITANDEQ: emit_bitand(gen, dst, dst, r, ast->line); break;
        case TOK_BITOREQ:  emit_bitor(gen, dst, dst, r, ast->line); break;
        case TOK_BITXOREQ: emit_bitxor(gen, dst, dst, r, ast->line); break;
        case TOK_SHLEQ:    emit_shl(gen, dst, dst, r, ast->line); break;
        case TOK_SHREQ:   emit_shr(gen, dst, dst, r, ast->line); break;
        case TOK_USHREQ:   emit_ushr(gen, dst, dst, r, ast->line); break;
        default: break;
    }
    reg_free(gen, r);

    if (!is_local) {
        if (ref->kind == SYM_GLOBAL) {
            emit_setglobal(gen, dst, ref->index, ast->line);
        } else if (ref->kind == SYM_UPVALUE) {
            emit_setupval(gen, dst, ref->index, ast->line);
        }
        reg_free(gen, dst);
    }
}

// ============================================================================
// try-catch-finally
// ============================================================================

static void gen_try(CodeGen* gen, Ast* ast) {
    // TRY: 设置 catch_ip
    int try_pos = gen->chunk->len;
    // TRY iABx: Bx = catch 偏移（占位）
    reg_encode_iABx(gen->chunk, OP_TRY, 0, 0, ast->line);
    int catch_patch_pos = gen->chunk->len - 2;  // Bx 字段位置

    // try body
    gen_stmt(gen, ast->u.try_.try_body);

    // END_TRY
    int jmp_end = emit_jmp(gen, ast->line);
    reg_encode_iABC(gen->chunk, OP_END_TRY, 0, 0, 0, ast->line);

    // CATCH: patch catch_ip
    int catch_pos = gen->chunk->len;
    gen->chunk->code[catch_patch_pos] = (uint8_t)(((catch_pos - try_pos) >> 8) & 0xFF);
    gen->chunk->code[catch_patch_pos + 1] = (uint8_t)((catch_pos - try_pos) & 0xFF);

    if (ast->u.try_.catch_var) {
        // 分配 catch 变量寄存器
        Symbol* sym = scope_resolve(gen->sem->current, ast->u.try_.catch_var);
        if (sym) {
            // CATCH iABx: R[A] = exception
            reg_encode_iABx(gen->chunk, OP_CATCH, sym->index, 0, ast->line);
        }
    }

    if (ast->u.try_.catch_body) {
        gen_stmt(gen, ast->u.try_.catch_body);
    }

    patch_jmp(gen, jmp_end);

    // FINALLY
    if (ast->u.try_.finally_body) {
        // FINALLY iABx: 设置 finally_ip
        int fin_pos = gen->chunk->len;
        reg_encode_iABx(gen->chunk, OP_FINALLY, 0, 0, ast->line);
        gen_stmt(gen, ast->u.try_.finally_body);
        // END_TRY 清除 finally
        reg_encode_iABC(gen->chunk, OP_END_TRY, 0, 0, 0, ast->line);
    }
}

// ============================================================================
// throw
// ============================================================================

static void gen_throw(CodeGen* gen, Ast* ast) {
    int r = gen_expr(gen, ast->u.throw_.expr);
    // THROW iABC: throw R[A]
    reg_encode_iABC(gen->chunk, OP_THROW, r, 0, 0, ast->line);
    reg_free(gen, r);
}

// ============================================================================
// 类型定义语句（简化实现）
// ============================================================================

static void gen_struct_def(CodeGen* gen, Ast* ast) {
    // STRUCT_DEF iABx: Bx = struct 定义常量索引
    // 完整实现需要序列化 struct 元数据到常量表
    reg_encode_iABx(gen->chunk, OP_STRUCT_DEF, 0, 0, ast->line);

    // 生成方法函数
    for (int i = 0; i < ast->u.struct_def.method_count; i++) {
        gen_func(gen, ast->u.struct_def.methods[i]);
    }
}

static void gen_enum_def(CodeGen* gen, Ast* ast) {
    reg_encode_iABx(gen->chunk, OP_ENUM_DEF, 0, 0, ast->line);
}

static void gen_face_def(CodeGen* gen, Ast* ast) {
    reg_encode_iABx(gen->chunk, OP_FACE_DEF, 0, 0, ast->line);
}

static void gen_cstruct_def(CodeGen* gen, Ast* ast) {
    reg_encode_iABx(gen->chunk, OP_CSTRUCT_DEF, 0, 0, ast->line);
}

static void gen_clib_def(CodeGen* gen, Ast* ast) {
    // 简化：clib 定义不生成字节码
}

static void gen_cfunc_decl(CodeGen* gen, Ast* ast) {
    // 简化：cfunc 声明不生成字节码
}

static void gen_alias(CodeGen* gen, Ast* ast) {
    // 简化：别名不生成字节码
}

// ============================================================================
// 解构声明
// ============================================================================

static void gen_destruct_decl(CodeGen* gen, Ast* ast) {
    // TODO: 完整解构实现
    // 简化：求值 init 表达式，然后逐个赋值
    if (ast->u.destruct_decl.init) {
        int src = gen_expr(gen, ast->u.destruct_decl.init);
        for (int i = 0; i < ast->u.destruct_decl.slot_count; i++) {
            Symbol* sym = scope_resolve(gen->sem->current, ast->u.destruct_decl.names[i]);
            if (sym) {
                // 从 src 中取第 i 个元素
                int idx_reg = reg_alloc(gen);
                emit_loadi_to(gen, idx_reg, i, ast->line);
                reg_encode_iABC(gen->chunk, OP_INDEX, sym->index, src, idx_reg, ast->line);
                reg_free(gen, idx_reg);
            }
        }
        reg_free(gen, src);
    }
}

// ============================================================================
// import / export / use
// ============================================================================

static void gen_import(CodeGen* gen, Ast* ast) {
    const char* mod = ast->u.import.module_name;
    if (!mod) return;

    // 原生模块（times / io / jsons ...）：运行时加载，确保方法表已注册
    if (!strstr(mod, ".leno")) {
        int r = reg_alloc(gen);
        int cidx = make_constant(gen, val_obj((Object*)str_new(mod, (int)strlen(mod))));
        reg_encode_iABx(gen->chunk, OP_LOAD_NATIVE_MODULE, r, cidx, ast->line);
        reg_free(gen, r);
        return;
    }
    // .leno 模块：编译期已由 load_module_file 加载（后续阶段接入运行时初始化）
}

static void gen_export(CodeGen* gen, Ast* ast) {
    // 导出：生成声明的代码
    if (ast->u.export.decl) {
        gen_stmt(gen, ast->u.export.decl);
    }
}

static void gen_use(CodeGen* gen, Ast* ast) {
    // use 语句不生成字节码
}

// ============================================================================
// 模块语句生成
// ============================================================================

void gen_stmt_module(CodeGen* gen, Ast* ast) {
    // 简化：复用 gen_stmt
    gen_stmt(gen, ast);
}

void gen_block_module(CodeGen* gen, Ast* ast) {
    if (!ast) return;
    int n = ast->u.block.count;
    for (int i = 0; i < n; i++) {
        gen_stmt_module(gen, ast->u.block.items[i]);
    }
}

// ============================================================================
// 默认值生成
// ============================================================================

void gen_default_value(CodeGen* gen, Ast* default_expr) {
    // 简化：求值默认表达式
    gen_expr(gen, default_expr);
}

// ============================================================================
// 数组 append（公共函数）
// ============================================================================

void gen_array_add(CodeGen* gen, Ast* receiver_ast, Ast* arg_ast, int need_result, int line) {
    int obj_reg = gen_expr(gen, receiver_ast);
    int val_reg = gen_expr(gen, arg_ast);
    // ARRAY_APPEND: append R[A] to R[B], R[A] = new len
    reg_encode_iABC(gen->chunk, OP_ARRAY_APPEND, val_reg, obj_reg, need_result, line);
    reg_free(gen, val_reg);
    reg_free(gen, obj_reg);
}

void gen_array_add_by_symbol(CodeGen* gen, Symbol* var_sym, Ast* arg_ast, int need_result, int line) {
    int val_reg = gen_expr(gen, arg_ast);
    // ARRAY_APPEND: append R[A] to R[B]
    reg_encode_iABC(gen->chunk, OP_ARRAY_APPEND, val_reg, var_sym->index, need_result, line);
    reg_free(gen, val_reg);
}
