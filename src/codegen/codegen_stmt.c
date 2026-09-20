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

// switch：展开为"比较链 + 跳板 + 各 case 体"。
// 语义与栈式一致（常量 case 用相等比较，命中后执行对应 body 并跳出；
// 无命中走 default；body 之间不 fallthrough）。
// case is Type 模式匹配需要类型检查指令，见后续阶段。
static void gen_switch(CodeGen* gen, Ast* ast) {
    int n = ast->u.switch_.case_count;
    int has_default = ast->u.switch_.default_body != NULL;
    int line = ast->line;

    int tmp = reg_alloc(gen);
    gen_expr_to(gen, ast->u.switch_.expr, tmp);

    // break 支持
    LoopContextNode* node = (LoopContextNode*)malloc(sizeof(LoopContextNode));
    node->prev = gen->loop_head;
    node->ctx.break_count = 0;
    node->ctx.continue_count = 0;
    node->ctx.continue_target = 0;
    gen->loop_head = node;
    gen->loop_count++;

    // --- 比较链：命中就跳到对应跳板 ---
    int cap = 8, cnt = 0;
    int* hit_pos = (int*)malloc(sizeof(int) * cap);
    int* hit_case = (int*)malloc(sizeof(int) * cap);

    for (int i = 0; i < n; i++) {
        // case is Type [,...]：类型匹配（任一命中即进入该 case）
        if (ast->u.switch_.cases[i].is_type_match) {
            int tcount = ast->u.switch_.cases[i].match_type_count;
            for (int k = 0; k < (tcount > 0 ? tcount : 1); k++) {
                TypeInfo* mt = (tcount > 0 && ast->u.switch_.cases[i].match_types)
                                   ? ast->u.switch_.cases[i].match_types[k]
                                   : ast->u.switch_.cases[i].match_type;
                // 就地检查会覆盖 tmp：先复制一份到临时寄存器
                int tr = reg_alloc(gen);
                emit_mov(gen, tr, tmp, line);
                emit_type_check_to(gen, tr, mt, line);
                int jp = emit_jmp_if_true(gen, tr, line);
                if (cnt == cap) {
                    cap *= 2;
                    hit_pos = (int*)realloc(hit_pos, sizeof(int) * cap);
                    hit_case = (int*)realloc(hit_case, sizeof(int) * cap);
                }
                hit_pos[cnt] = jp;
                hit_case[cnt] = i;
                cnt++;
                reg_free(gen, tr);
            }
            continue;
        }
        AstList* vals = &ast->u.switch_.cases[i].values;
        for (int k = 0; k < vals->count; k++) {
            int vr = reg_alloc(gen);
            gen_expr_to(gen, vals->items[k], vr);
            int cr = reg_alloc(gen);
            emit_eq(gen, cr, tmp, vr, line);
            int jp = emit_jmp_if_true(gen, cr, line);
            if (cnt == cap) {
                cap *= 2;
                hit_pos = (int*)realloc(hit_pos, sizeof(int) * cap);
                hit_case = (int*)realloc(hit_case, sizeof(int) * cap);
            }
            hit_pos[cnt] = jp;
            hit_case[cnt] = i;
            cnt++;
            reg_free(gen, cr);
            reg_free(gen, vr);
        }
    }

    // 全部未命中
    int miss_jump = emit_jmp(gen, line);

    // --- 跳板区（连续的无条件跳转）---
    int* pad_pos = (int*)malloc(sizeof(int) * (n > 0 ? n : 1));
    for (int i = 0; i < n; i++) {
        pad_pos[i] = gen->chunk->len;
        emit_jmp(gen, line);
    }
    for (int i = 0; i < cnt; i++) {
        patch_jmp_to(gen, hit_pos[i], pad_pos[hit_case[i]]);
    }

    // --- 各 case 体 ---
    int* body_end_jumps = (int*)malloc(sizeof(int) * (n > 0 ? n : 1));
    for (int i = 0; i < n; i++) {
        int here = gen->chunk->len;
        patch_jmp_to(gen, pad_pos[i], here);
        if (ast->u.switch_.cases[i].body) gen_stmt(gen, ast->u.switch_.cases[i].body);
        body_end_jumps[i] = emit_jmp(gen, line);   // 不 fallthrough
    }

    // --- default ---
    int default_pos = gen->chunk->len;
    if (has_default) {
        gen_stmt(gen, ast->u.switch_.default_body);
    }

    int end_pos = gen->chunk->len;
    patch_jmp_to(gen, miss_jump, has_default ? default_pos : end_pos);
    for (int i = 0; i < n; i++) patch_jmp(gen, body_end_jumps[i]);
    for (int i = 0; i < node->ctx.break_count; i++) patch_jmp(gen, node->ctx.break_jumps[i]);

    gen->loop_head = node->prev;
    free(node);
    gen->loop_count--;

    reg_free(gen, tmp);
    free(hit_pos);
    free(hit_case);
    free(pad_pos);
    free(body_end_jumps);
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

    // 模块变量：值存 module->globals[ref->index]
    if (ref->kind == SYM_MODULE) {
        int r = reg_alloc(gen);
        if (ast->u.var_decl.init) {
            gen_expr_to(gen, ast->u.var_decl.init, r);
        } else {
            emit_loadnil_to(gen, r, ast->line);
        }
        reg_encode_iABx(gen->chunk, OP_SET_MODULE_VAR, r, ref->index, ast->line);
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
                        reg_encode_iABx(gen->chunk, OP_SET_MODULE_VAR, r, ref->index, ast->line);
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

// try / catch / finally 布局（两条路径都汇入 finally，finally 必定执行）：
//   TRY  (catch_ip → CATCH 块)
//   try_body
//   END_TRY                 ← 正常结束：注销 catch（后面不再需要）
//   JMP FINALLY             ← 正常路径也要执行 finally
// CATCH:
//   CATCH A (R[A] = 异常)
//   catch_body              ← 结束后自然落入 FINALLY
// FINALLY:
//   FINALLY (设置 finally_ip，供 return/异常收尾跳回)
//   finally_body
//   END_TRY
// END:
static void gen_try(CodeGen* gen, Ast* ast) {
    int has_finally = ast->u.try_.finally_body != NULL;

    int try_pos = gen->chunk->len;
    reg_encode_iABx(gen->chunk, OP_TRY, 0, 0, ast->line);
    int catch_patch_pos = gen->chunk->len - 2;  // Bx 字段位置

    // try body
    gen_stmt(gen, ast->u.try_.try_body);

    // 正常路径：先注销 catch 注册，再跳到 finally
    reg_encode_iABC(gen->chunk, OP_END_TRY, 0, 0, 0, ast->line);
    int jmp_finally = emit_jmp(gen, ast->line);

    // --- catch 块 ---
    int catch_pos = gen->chunk->len;
    gen->chunk->code[catch_patch_pos] = (uint8_t)(((catch_pos - try_pos) >> 8) & 0xFF);
    gen->chunk->code[catch_patch_pos + 1] = (uint8_t)((catch_pos - try_pos) & 0xFF);

    // catch 块入口：先取异常值（OP_CATCH），再注销本层 try 注册（OP_END_TRY）。
    //   - catch 体内若再次抛异常，必须向上传播，而不是被本层 catch_ip 再抓住
    //     （否则 `catch { ... 越界 ... }` 会无限跳回 catch 自己 → 挂死）。
    //     此前只有 OP_CATCH 顺带清 catch_ip，而 `} catch {`（无变量）根本不发
    //     OP_CATCH，catch_ip 与 has_exception 都不会被清理 —— test_array_bounds
    //     既卡死又把旧异常泄漏给后续调用。
    //   - 顺序必须是 CATCH → END_TRY：END_TRY 会清 vm.exception/has_exception。
    SymRef* cref = &ast->u.try_.catch_var_ref;
    if (ast->u.try_.catch_var && cref->name) {
        if (cref->index >= gen->next_reg) {
            gen->next_reg = cref->index + 1;
            if (gen->next_reg > gen->max_reg) gen->max_reg = gen->next_reg;
        }
        // CATCH iABx: R[A] = 当前异常
        reg_encode_iABx(gen->chunk, OP_CATCH, cref->index, 0, ast->line);
    }
    reg_encode_iABC(gen->chunk, OP_END_TRY, 0, 0, 0, ast->line);

    if (ast->u.try_.catch_body) {
        gen_stmt(gen, ast->u.try_.catch_body);
    }

    // --- finally 块（catch 结束自然落入这里）---
    int fin_pos = gen->chunk->len;
    patch_jmp_to(gen, jmp_finally, fin_pos);
    if (has_finally) {
        reg_encode_iABx(gen->chunk, OP_FINALLY, 0, 0, ast->line);
        gen_stmt(gen, ast->u.try_.finally_body);
        // 清除 finally 注册
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

// 写 2 字节大端常量索引
static void cw_u16(CodeGen* gen, int v, int line) {
    chunk_write(gen->chunk, (uint8_t)((v >> 8) & 0xFF), line);
    chunk_write(gen->chunk, (uint8_t)(v & 0xFF), line);
}
static void cw_u8(CodeGen* gen, int v, int line) {
    chunk_write(gen->chunk, (uint8_t)(v & 0xFF), line);
}

// 结构体定义：指令头 iABx(op, A=0, Bx=名字常量) + 紧随元数据（布局与栈式一致）
static void gen_struct_def(CodeGen* gen, Ast* ast) {
    int method_count = ast->u.struct_def.method_count;

    int* method_name_consts = NULL;
    int* method_func_consts = NULL;
    if (method_count > 0) {
        method_name_consts = (int*)malloc(sizeof(int) * method_count);
        method_func_consts = (int*)malloc(sizeof(int) * method_count);
        for (int i = 0; i < method_count; i++) {
            Ast* m = ast->u.struct_def.methods[i];
            if (m && m->kind == AST_FUNC_DEF) {
                ObjFunction* fn = gen_func_proto(gen, m);
                if (fn) gen_func_closure(gen, m, fn);   // 生成方法体字节码
                method_name_consts[i] = make_constant(
                    gen, val_obj((Object*)str_copy(m->u.func.name, (int)strlen(m->u.func.name))));
                method_func_consts[i] = fn ? make_constant(gen, val_obj((Object*)fn))
                                           : make_constant(gen, val_null());
            } else {
                method_name_consts[i] = make_constant(gen, val_null());
                method_func_consts[i] = make_constant(gen, val_null());
            }
        }
    }

    ObjString* struct_name = str_copy(ast->u.struct_def.name, (int)strlen(ast->u.struct_def.name));
    int name_const = make_constant(gen, val_obj((Object*)struct_name));

    reg_encode_iABx(gen->chunk, OP_STRUCT_DEF, 0, name_const, ast->line);
    cw_u8(gen, ast->u.struct_def.field_count, ast->line);
    cw_u8(gen, method_count, ast->line);

    // impl 声明的 face 名
    cw_u8(gen, ast->u.struct_def.impl_count, ast->line);
    for (int i = 0; i < ast->u.struct_def.impl_count; i++) {
        ObjString* impl_name = str_copy(ast->u.struct_def.impl_names[i],
                                        (int)strlen(ast->u.struct_def.impl_names[i]));
        cw_u16(gen, make_constant(gen, val_obj((Object*)impl_name)), ast->line);
    }

    // 泛型类型参数名
    cw_u8(gen, ast->u.struct_def.type_param_count, ast->line);
    for (int i = 0; i < ast->u.struct_def.type_param_count && ast->u.struct_def.type_params; i++) {
        ObjString* pn = str_copy(ast->u.struct_def.type_params[i],
                                 (int)strlen(ast->u.struct_def.type_params[i]));
        cw_u16(gen, make_constant(gen, val_obj((Object*)pn)), ast->line);
    }

    // 字段
    for (int i = 0; i < ast->u.struct_def.field_count; i++) {
        ObjString* fn = str_copy(ast->u.struct_def.field_names[i],
                                 (int)strlen(ast->u.struct_def.field_names[i]));
        cw_u16(gen, make_constant(gen, val_obj((Object*)fn)), ast->line);

        TypeInfo* ft = ast->u.struct_def.field_types ? ast->u.struct_def.field_types[i] : NULL;
        TypeKind field_type = ft ? ft->kind : TYPE_ANY;
        cw_u8(gen, field_type, ast->line);
        cw_u8(gen, (ft && ft->nullable) ? 1 : 0, ast->line);

        if (field_type == TYPE_STRUCT) {
            const char* stn = ft ? ft->struct_name : NULL;
            if (stn) {
                ObjString* tn = str_copy(stn, (int)strlen(stn));
                cw_u8(gen, 1, ast->line);
                cw_u16(gen, make_constant(gen, val_obj((Object*)tn)), ast->line);
            } else {
                cw_u8(gen, 0, ast->line);
            }
        }
        if (field_type == TYPE_PTR_GENERIC) {
            TypeKind elem = (ft && ft->element_type) ? ft->element_type->kind : TYPE_PTR;
            cw_u8(gen, elem, ast->line);
        }

        Ast* def_expr = ast->u.struct_def.field_defaults ? ast->u.struct_def.field_defaults[i] : NULL;
        if (def_expr) {
            Value dv = ast_default_to_value(def_expr);
            if (val_is_null(dv) && def_expr->kind != AST_NULL) {
                char msg[BUFFER_MEDIUM];
                snprintf(msg, sizeof(msg),
                         "struct 字段 '%s' 的默认值不是常量表达式，请使用构造器初始化",
                         ast->u.struct_def.field_names[i]);
                error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
                cw_u8(gen, 0, ast->line);
            } else {
                cw_u8(gen, 1, ast->line);
                cw_u16(gen, make_constant(gen, dv), ast->line);
            }
        } else {
            cw_u8(gen, 0, ast->line);
        }
    }

    // 方法
    for (int i = 0; i < method_count; i++) {
        cw_u16(gen, method_name_consts[i], ast->line);
        cw_u16(gen, method_func_consts[i], ast->line);
    }

    // 构造 / 析构 标志与下标
    int ctor_idx = -1, dtor_idx = -1;
    for (int i = 0; i < method_count; i++) {
        Ast* m = ast->u.struct_def.methods[i];
        if (m && m->u.func.is_ctor) ctor_idx = i;
        if (m && m->u.func.is_dtor) dtor_idx = i;
    }
    uint8_t flags = 0;
    if (ctor_idx >= 0) flags |= 1;
    if (dtor_idx >= 0) flags |= 2;
    cw_u8(gen, flags, ast->line);
    if (ctor_idx >= 0) cw_u8(gen, ctor_idx, ast->line);
    if (dtor_idx >= 0) cw_u8(gen, dtor_idx, ast->line);

    // 关联常量
    cw_u8(gen, ast->u.struct_def.const_count, ast->line);
    for (int i = 0; i < ast->u.struct_def.const_count; i++) {
        ObjString* cn = str_copy(ast->u.struct_def.const_names[i],
                                 (int)strlen(ast->u.struct_def.const_names[i]));
        cw_u16(gen, make_constant(gen, val_obj((Object*)cn)), ast->line);
        Ast* ce = ast->u.struct_def.const_values[i];
        Value cv = ast_default_to_value(ce);
        cw_u16(gen, make_constant(gen, cv), ast->line);
    }

    if (method_name_consts) free(method_name_consts);
    if (method_func_consts) free(method_func_consts);
}

static void gen_enum_def(CodeGen* gen, Ast* ast) {
    int r = reg_alloc(gen);
    ObjString* nm = str_copy(ast->u.enum_def.name, (int)strlen(ast->u.enum_def.name));
    int name_const = make_constant(gen, val_obj((Object*)nm));

    reg_encode_iABx(gen->chunk, OP_ENUM_DEF, r, name_const, ast->line);
    cw_u8(gen, ast->u.enum_def.member_count, ast->line);
    for (int i = 0; i < ast->u.enum_def.member_count; i++) {
        ObjString* mn = str_copy(ast->u.enum_def.member_names[i],
                                 (int)strlen(ast->u.enum_def.member_names[i]));
        cw_u16(gen, make_constant(gen, val_obj((Object*)mn)), ast->line);
        Value mv = val_int_safe(ast->u.enum_def.member_values[i]);
        cw_u16(gen, make_constant(gen, mv), ast->line);
    }

    // 把 enum 定义对象绑定到它的符号槽位
    SymRef* ref = &ast->u.enum_def.ref;
    if (ref->kind == SYM_GLOBAL) {
        emit_defglobal(gen, r, ref->index, ast->line);
    } else if (ref->kind == SYM_LOCAL || ref->kind == SYM_PARAM) {
        emit_mov(gen, ref->index, r, ast->line);
    } else if (ref->kind == SYM_MODULE) {
        reg_encode_iABx(gen->chunk, OP_SET_MODULE_VAR, r, ref->index, ast->line);
    }
    reg_free(gen, r);
}

static void gen_face_def(CodeGen* gen, Ast* ast) {
    ObjString* nm = str_copy(ast->u.face_def.name, (int)strlen(ast->u.face_def.name));
    int name_const = make_constant(gen, val_obj((Object*)nm));

    reg_encode_iABx(gen->chunk, OP_FACE_DEF, 0, name_const, ast->line);
    cw_u8(gen, ast->u.face_def.method_count, ast->line);
    cw_u8(gen, ast->u.face_def.type_param_count, ast->line);

    for (int i = 0; i < ast->u.face_def.type_param_count && ast->u.face_def.type_params; i++) {
        ObjString* pn = str_copy(ast->u.face_def.type_params[i],
                                 (int)strlen(ast->u.face_def.type_params[i]));
        cw_u16(gen, make_constant(gen, val_obj((Object*)pn)), ast->line);
    }

    for (int i = 0; i < ast->u.face_def.method_count; i++) {
        ObjString* mn = str_copy(ast->u.face_def.method_names[i],
                                 (int)strlen(ast->u.face_def.method_names[i]));
        cw_u16(gen, make_constant(gen, val_obj((Object*)mn)), ast->line);
        int pc = ast->u.face_def.method_param_counts ? ast->u.face_def.method_param_counts[i] : 0;
        cw_u8(gen, pc, ast->line);
        TypeInfo* rt = ast->u.face_def.method_return_types ? ast->u.face_def.method_return_types[i] : NULL;
        cw_u8(gen, rt ? (uint8_t)rt->kind : (uint8_t)TYPE_INFER, ast->line);
        for (int j = 0; j < pc; j++) {
            TypeInfo* pt = ast->u.face_def.method_param_types
                               ? ast->u.face_def.method_param_types[i][j] : NULL;
            cw_u8(gen, pt ? (uint8_t)pt->kind : (uint8_t)TYPE_INFER, ast->line);
        }
    }
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
    int is_native_mod = (strstr(mod, ".leno") == NULL);

    // 别名：显式 alias 优先，否则用模块名（去掉路径与后缀）
    const char* alias = ast->u.import.alias;
    char* extracted = NULL;
    if (!alias) {
        const char* base = strrchr(mod, '/');
        if (!base) base = strrchr(mod, '\\');
        if (!base) base = mod; else base++;
        const char* dot = strrchr(base, '.');
        if (dot && dot > base) {
            extracted = (char*)malloc((size_t)(dot - base) + 1);
            memcpy(extracted, base, (size_t)(dot - base));
            extracted[dot - base] = '\0';
        } else {
            extracted = strdup(base);
        }
        alias = extracted;
    }

    // ★ 登记别名 → 是否原生模块。
    //   调用点（gen_module_call）必须据此选路径：原生模块走 OP_MODULE_CALL，
    //   .leno 模块走「取模块对象 → exports[方法名] → CALL」。
    //   两者的 AST_MODULE_CALL 字段完全一致（module_name 是别名、lib_ref 无信息），
    //   只能在这里留下线索。
    codegen_record_module_alias(gen, alias, mod, is_native_mod);

    if (is_native_mod) {
        int r = reg_alloc(gen);
        int cidx = make_constant(gen, val_obj((Object*)str_new(mod, (int)strlen(mod))));
        reg_encode_iABx(gen->chunk, OP_LOAD_NATIVE_MODULE, r, cidx, ast->line);
        reg_free(gen, r);
        if (extracted) free(extracted);
        return;
    }

    // --- .leno 源码模块 ---
    // 编译期：加载并编译模块（产出 ObjModule + init_chunk）
    // 运行期：OP_INIT_LENOMODULE 执行 init_chunk，再把模块对象绑到别名变量


    const char* current_file = error_get_filename();
    ObjModule* module = load_module_file(mod, current_file, alias);

    if (!module) {
        if (!error_has_any()) {
            char err_msg[BUFFER_MEDIUM];
            snprintf(err_msg, sizeof(err_msg), "无法加载模块 '%s'", mod);
            error_add_at(ERR_SEMANTIC, ast->line, ast->column, err_msg);
        }
        if (extracted) free(extracted);
        return;
    }

    int r = reg_alloc(gen);
    int cidx = make_constant(gen, val_obj((Object*)module));
    emit_loadk_to(gen, r, cidx, ast->line);
    reg_encode_iABC(gen->chunk, OP_INIT_LENOMODULE, r, 0, 0, ast->line);

    // 模块对象绑定到别名（全局 / 模块变量 / 局部）
    Symbol* sym = scope_resolve(gen->sem->root_scope, alias);
    if (sym) {
        if (sym->kind == SYM_GLOBAL) {
            emit_defglobal(gen, r, sym->index, ast->line);
        } else if (sym->kind == SYM_MODULE) {
            reg_encode_iABx(gen->chunk, OP_SET_MODULE_VAR, r, sym->index, ast->line);
        } else if (sym->kind == SYM_LOCAL || sym->kind == SYM_PARAM) {
            if (sym->index >= gen->next_reg) {
                gen->next_reg = sym->index + 1;
                if (gen->next_reg > gen->max_reg) gen->max_reg = gen->next_reg;
            }
            emit_mov(gen, sym->index, r, ast->line);
        }
    }
    reg_free(gen, r);
    if (extracted) free(extracted);
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
