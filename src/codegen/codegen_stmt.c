// ============================================================================
// 寄存器式 codegen：语句生成
// ============================================================================

#include "codegen.h"

// 前向声明（与 codegen.h 中非 static 声明一致）
void gen_stmt(CodeGen* gen, Ast* ast);
void gen_index_assign(CodeGen* gen, Ast* ast, int dst);
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
        // 独立索引赋值：parser 对 `c.count += 5`（AST_MODULE_ACCESS + 复合赋值）与
        // 语义阶段把方法内 `count = v` 的 AST_ASSIGN 就地改写成 AST_INDEX_ASSIGN，
        // 这两类节点都是**裸**的（不挂在 AST_ASSIGN.targets 下）⇒ 此前无人生成 ✗
        case AST_INDEX_ASSIGN: gen_index_assign(gen, ast, -1); break;
        case AST_EXPR_STMT: {
            // 赋值语句会被解析器包在表达式语句里，直接走赋值生成（少一次搬运）
            Ast* e = ast->u.expr_stmt.expr;
            if (e && (e->kind == AST_ASSIGN || e->kind == AST_COMPOUND_ASSIGN ||
                      e->kind == AST_INDEX_ASSIGN)) {
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
            // ★ 兜底：语句位置出现**裸表达式节点**时求值后丢弃（值不要了就 reg_free）。
            //   必须与栈式 gen_stmt 的 default 同口径（那边是 gen_expr + OP_POP）——
            //   优化器的 DCE 会把 `if true then X else Y` **原地替换**成 X
            //   （optimize.c：memcpy(ast, then_branch, sizeof(Ast))），语句位置于是
            //   可能出现 AST_CALL / AST_NUM 这类表达式节点；没有这一支就**静默丢语句**
            //   （实测 examples/验证/repro_if_expr.leno 末行 `print(99)` 不执行，
            //    整份输出少一行且零诊断）。
            {
                int r = gen_expr(gen, ast);
                reg_free(gen, r);
            }
            break;
    }
}

// ============================================================================
// block
// ============================================================================

// 逆序对 [from, gen->dtor_count) 区间内的条目发射 OP_DTOR_LOCAL
static void emit_dtors_from(CodeGen* gen, int from, int line, int nrvo_skip) {
    for (int i = gen->dtor_count - 1; i >= from; i--) {
        int slot = gen->dtor_entries[i].local_slot;
        if (slot == nrvo_skip) continue;   // NRVO：直接返回该局部变量时不析构
        reg_encode_iABC(gen->chunk, OP_DTOR_LOCAL, slot, 0, 0, line);
    }
}

// 可提升的局部函数：非循环体内定义的 SYM_LOCAL/SYM_PARAM 函数定义。
//   循环体内的（is_in_loop）**不提升** —— 它们的语义是每轮迭代重新创建（值捕获），
//   提到循环外生成会改掉捕获语义。
static int is_hoistable_local_func(Ast* st) {
    return st && st->kind == AST_FUNC_DEF && st->u.func.ref.name &&
           (st->u.func.ref.kind == SYM_LOCAL || st->u.func.ref.kind == SYM_PARAM) &&
           !st->u.func.is_in_loop;
}

void gen_block(CodeGen* gen, Ast* ast) {
    if (!ast) return;
    int dtor_at_entry = gen->dtor_count;
    int n = ast->u.block.count;

    // ★ 局部函数**提升**（照栈式 gen_block 的两遍策略）：函数体内先调用、后声明
    //   （`test(); func test() { ... }`）必须能跑通 —— 按语句顺序生成的话调用点在
    //   槽位还是 null 时就执行了，运行期报「函数未定义」（examples/func/前向引用.leno、
    //   func 闭包.leno 就是这条；栈式靠同样的两遍策略支持"前向引用"）。
    //   ① 先把这些槽位预置 null（槽位存在、值为空，与栈式一致）；
    //   ② 再在**其它语句之前**生成它们的闭包。
    int has_hoist = 0;
    for (int i = 0; i < n; i++) {
        if (is_hoistable_local_func(ast->u.block.items[i])) { has_hoist = 1; break; }
    }
    if (has_hoist) {
        for (int i = 0; i < n; i++) {
            Ast* st = ast->u.block.items[i];
            if (is_hoistable_local_func(st)) emit_loadnil_to(gen, st->u.func.ref.index, st->line);
        }
        for (int i = 0; i < n; i++) {
            Ast* st = ast->u.block.items[i];
            if (is_hoistable_local_func(st)) gen_func(gen, st);
        }
    }

    for (int i = 0; i < n; i++) {
        Ast* st = ast->u.block.items[i];
        if (has_hoist && is_hoistable_local_func(st)) continue;   // 已在上面生成
        // ★ 语句边界回收临时寄存器高水位（reg_scope_enter/exit 从未被调用过：
        //   next_reg 只升不降 ⇒ 函数越长顶得越高。SDL 的 Window.run / process 被顶到
        //   655 / 563，而指令里的寄存器号只有 8 位（MAX_REG=256）⇒ `OP_MOV A=70 B=320`
        //   里的 B 被 &0xFF 截断成 64，把 R64 的值当成了 `i`（恰好是 null ⇒
        //   `_runEvts[i]` 报「数组索引必须是数字」）。
        //   语句内的临时寄存器在本语句结束时全部失效（跨语句存活的值都在**变量槽位**里），
        //   所以退回 _nr_before 是安全的；被丢弃的空闲项由 reg_alloc 自行跳过。
        int _nr_before = gen->next_reg;
        int _ft_before = gen->freetop;
        gen_stmt(gen, st);
        if (gen->next_reg > _nr_before) {
            gen->next_reg = _nr_before;
            if (gen->freetop > _ft_before) gen->freetop = _ft_before;
        }
    }
    // 块结束：逆序析构本层声明的带析构函数 struct 局部变量（栈式同一语义）
    emit_dtors_from(gen, dtor_at_entry, ast->line, -1);
    if (gen->dtor_count > dtor_at_entry) gen->dtor_count = dtor_at_entry;
}

// ============================================================================
// if / if-else
// ============================================================================

void gen_if(CodeGen* gen, Ast* ast) {
    gen_if_ex(gen, ast, 0, -1);
}

// 分支生成（唯一实现）：
//   语句位置：块 / `eif` 链（AST_IF）按语句生成；普通表达式求值后丢弃。
//   表达式位置：块按语句生成（值补 null）；AST_IF（三元链的 else if）按表达式
//   递归生成，值写入 dst；其余表达式直接写入 dst。
// ⚠ 此前表达式位置只认 AST_EXPR_STMT，三元 `if c then a else b` 的 then/else
//   是普通表达式（AST_VAR 等），走 gen_stmt 被静默忽略 ⇒ 结果永远是 null ✗
static void gen_if_branch_to(CodeGen* gen, Ast* branch, int want_value, int dst, int line) {
    if (!branch) {
        if (want_value) emit_loadnil_to(gen, dst, line);
        return;
    }
    if (branch->kind == AST_BLOCK) {
        gen_stmt(gen, branch);
        if (want_value) emit_loadnil_to(gen, dst, line);
        return;
    }
    if (!want_value) {
        // 语句位置：AST_IF（eif 链）与表达式语句都按语句生成
        if (branch->kind == AST_IF || branch->kind == AST_EXPR_STMT) {
            gen_stmt(gen, branch);
            return;
        }
        int r = gen_expr(gen, branch);
        reg_free(gen, r);
        return;
    }
    // 表达式位置
    if (branch->kind == AST_EXPR_STMT) {
        gen_expr_to(gen, branch->u.expr_stmt.expr, dst);
        return;
    }
    gen_expr_to(gen, branch, dst);
}

void gen_if_ex(CodeGen* gen, Ast* ast, int want_value, int dst) {
    int need_bind = (ast->u.if_.guard_bind_var && ast->u.if_.guard_bind_index >= 0);
    Ast* cond_ast = ast->u.if_.cond;

    // 绑定赋值（`if x is T => name`）：把被检查的表达式/变量写入绑定变量槽位
    #define IF_DO_BIND()                                                          \
        do {                                                                      \
            if (need_bind) {                                                      \
                int _slot = ast->u.if_.guard_bind_index;                          \
                if (_slot >= gen->next_reg) {                                     \
                    gen->next_reg = _slot + 1;                                    \
                    if (gen->next_reg > gen->max_reg) gen->max_reg = gen->next_reg; \
                }                                                                 \
                if (ast->u.if_.guard_bind_expr) {                                 \
                    int _br = gen_expr(gen, ast->u.if_.guard_bind_expr);          \
                    if (_br != _slot) emit_mov(gen, _slot, _br, ast->line);       \
                    reg_free(gen, _br);                                           \
                } else if (ast->u.if_.guard_var_ref.name) {                       \
                    SymRef* _vr = &ast->u.if_.guard_var_ref;                      \
                    if (_vr->kind == SYM_LOCAL || _vr->kind == SYM_PARAM) {       \
                        if (_vr->index != _slot) emit_mov(gen, _slot, _vr->index, ast->line); \
                    } else if (_vr->kind == SYM_GLOBAL) {                         \
                        emit_getglobal_to(gen, _slot, _vr->index, ast->line);     \
                    } else if (_vr->kind == SYM_UPVALUE) {                        \
                        emit_getupval_to(gen, _slot, _vr->index, ast->line);      \
                    }                                                             \
                }                                                                 \
            }                                                                     \
        } while (0)

    int jmp_false = -1;
    int jmp_false2 = -1;

    // 条件求值。`x is T => a and a[0] is int` 这种链式守卫里，
    // 绑定必须发生在**第一个条件成立之后、第二个条件求值之前** ——
    // 否则右半边用到的绑定变量还是 null（实测报「下标访问: 对象不支持索引」）。
    if (need_bind && cond_ast && cond_ast->kind == AST_BINOP &&
        cond_ast->u.binop.op == TOK_AND) {
        int c1 = gen_expr(gen, cond_ast->u.binop.l);
        jmp_false = emit_jmp_if_false(gen, c1, ast->line);
        reg_free(gen, c1);

        IF_DO_BIND();

        int c2 = gen_expr(gen, cond_ast->u.binop.r);
        jmp_false2 = emit_jmp_if_false(gen, c2, ast->line);
        reg_free(gen, c2);
    } else {
        int cond = gen_expr(gen, cond_ast);
        jmp_false = emit_jmp_if_false(gen, cond, ast->line);
        reg_free(gen, cond);

        IF_DO_BIND();
    }
    #undef IF_DO_BIND

    // then 分支
    gen_if_branch_to(gen, ast->u.if_.then, want_value, dst, ast->line);

    // if 有 else 分支
    if (ast->u.if_.else_) {
        int jmp_end = emit_jmp(gen, ast->line);
        patch_jmp(gen, jmp_false);
        if (jmp_false2 >= 0) patch_jmp(gen, jmp_false2);

        // else 分支
        gen_if_branch_to(gen, ast->u.if_.else_, want_value, dst, ast->line);

        patch_jmp(gen, jmp_end);
    } else {
        patch_jmp(gen, jmp_false);
        if (jmp_false2 >= 0) patch_jmp(gen, jmp_false2);
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
    // ① 以**类型推断**为主（比栈式更准）：`var x = 10; for x to i` 推断出 INT ⇒ 数值区间，
    //    栈式只看符号 type_kind（TYPE_INFER）会误判成迭代；
    // ② 推断拿不准时按**变量类表达式 → 保守迭代**（与栈式 gen_for 的 is_var_expr 分支同口径，
    //    见 D:\CLeno\Leno 的 codegen_utils.c 注释：属性/字段访问 `d.statements` 也算）。
    //    ⚠ 判错的代价不对称：把**容器**当数值上界 ⇒ FOR_PREP 的 value_to_double(容器)=0
    //    ⇒ 循环体一次都不进（**静默**错值，minilang 例子整段不执行就是这么来的）；
    //    反过来数字被当容器，OP_LEN(数字)=0 同样直观。总之不能靠"猜"。
    int is_iter = 0;
    if (!ast->u.for_.start && ast->u.for_.var_name) {
        TypeInfo* et = infer_expr_type(gen->sem, end_expr);
        TypeKind ek = et ? et->kind : TYPE_UNKNOWN;
        if (et) type_free(et);
        if (ek == TYPE_STRING || ek == TYPE_ARRAY || ek == TYPE_DICT || ek == TYPE_STRUCT) {
            is_iter = 1;                       // 明确是容器 ⇒ 迭代
        } else if (ek == TYPE_INT || ek == TYPE_FLOAT) {
            is_iter = 0;                       // 明确是数字 ⇒ 数值区间
        } else if (is_string_expr(end_expr) || is_array_expr(end_expr) || is_dict_expr(end_expr)) {
            is_iter = 1;                       // 字面量容器 ⇒ 迭代
        } else if (is_var_expr(end_expr)) {
            // 变量 / 属性访问 / 索引 / 模块成员：只有**能证明是数字**（符号 type_kind 明确为
            // INT/FLOAT）才走数值区间，否则保守按迭代（栈式同此）。
            if (end_expr->kind == AST_VAR) {
                TypeKind vt = end_expr->u.var.ref.type_kind;
                is_iter = !(vt == TYPE_INT || vt == TYPE_FLOAT);
            } else {
                is_iter = 1;
            }
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
    //   第一个循环变量：数组/字符串 → 元素；dict/struct → 键（字段名）
    //   第二个循环变量：数组/字符串 → 下标；dict/struct → **值**（OP_ITER_GET_VALUE）
    //   （此前第二个变量一律当下标 ⇒ `for d to k, v` 的 v 拿到的是序号 ✗）
    if (ast->u.for_.var_name) {
        reg_encode_iABC(gen->chunk, OP_ITER_GET, lv, obj_slot, idx_slot, ast->line);
    }
    if (has_idx_var) {
        TypeInfo* ot = infer_expr_type(gen->sem, ast->u.for_.end);
        int is_kv_container = (ot && (ot->kind == TYPE_DICT || ot->kind == TYPE_STRUCT));
        if (ot) type_free(ot);
        if (is_kv_container) {
            reg_encode_iABC(gen->chunk, OP_ITER_GET_VALUE, iv, obj_slot, idx_slot, ast->line);
        } else {
            emit_mov(gen, iv, idx_slot, ast->line);
        }
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

        // `case is T => name`：匹配成功（正好跳到这里）后把 switch 表达式的值
        // 绑定到新局部变量。switch 表达式的值一直保存在 tmp 里（函数末尾才回收），
        // 所以这里直接 MOV 即可。
        if (ast->u.switch_.cases[i].guard_bind_var &&
            ast->u.switch_.cases[i].guard_bind_index >= 0) {
            int slot = ast->u.switch_.cases[i].guard_bind_index;
            if (slot >= gen->next_reg) {
                gen->next_reg = slot + 1;
                if (gen->next_reg > gen->max_reg) gen->max_reg = gen->next_reg;
            }
            if (slot != tmp) emit_mov(gen, slot, tmp, line);
        }

        // `case is Point(x, y)` 的解构：从匹配到的值里按字段名取出各分量
        {
            int dc = ast->u.switch_.cases[i].destructure_count;
            int* dindices = ast->u.switch_.cases[i].destructure_indices;
            char** dfields = ast->u.switch_.cases[i].destructure_field_names;
            for (int k = 0; k < dc && dindices && dfields; k++) {
                int slot = dindices[k];
                const char* fname = dfields[k];
                if (slot < 0 || !fname) continue;
                if (slot >= gen->next_reg) {
                    gen->next_reg = slot + 1;
                    if (gen->next_reg > gen->max_reg) gen->max_reg = gen->next_reg;
                }
                int ireg = reg_alloc(gen);
                int c = make_constant(gen, val_obj((Object*)str_copy(fname, (int)strlen(fname))));
                emit_loadk_to(gen, ireg, c, line);
                reg_encode_iABC(gen->chunk, OP_INDEX, slot, tmp, ireg, line);
                reg_free(gen, ireg);
            }
        }

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

// 返回值按**声明类型**规范化（C1）
// ----------------------------------------------------------------------------
// `func f(): float { return 1 }` 此前返回值完全不 CAST ⇒ 运行期拿到 int：
//   `type(f())` = "int"、`f() / 2` 走 int 除法得 0（应为 0.5）；
// 跨模块调用同样错（调用点只信声明类型，值本身是 int）。**栈式侧也这样**，是共有缺口。
// 口径与变量声明 / 赋值完全一致（emit_cast_for_target）：只对 int/float/string 目标插 CAST；
// 构造函数（func StructName() 返回 self）与多返回值不走这里。
static void emit_cast_for_return(CodeGen* gen, Ast* ret_ast, int reg, int line) {
    if (!ret_ast) return;
    Ast* fn = gen->current_func_ast;
    if (!fn || fn->kind != AST_FUNC_DEF) return;
    if (fn->u.func.is_ctor) return;
    TypeInfo* rt = fn->u.func.return_type;
    if (!rt) return;
    emit_cast_for_target(gen, rt->kind, ret_ast, reg, line);
}

static void gen_return(CodeGen* gen, Ast* ast) {
    // 有带析构的局部变量：先把返回值存进临时寄存器 → 逆序析构 → 再 RETURN。
    //   （先析构会把返回值本身销毁 ✗）
    if (gen->dtor_count > 0) {
        int r = reg_alloc(gen);
        if (ast->u.ret) {
            gen_expr_to(gen, ast->u.ret, r);
            emit_cast_for_return(gen, ast->u.ret, r, ast->line);   // C1
        } else {
            emit_loadnil_to(gen, r, ast->line);
        }
        int nrvo_skip = -1;
        if (ast->u.ret && ast->u.ret->kind == AST_VAR) {
            SymRef* rref = &ast->u.ret->u.var.ref;
            if (rref->kind == SYM_LOCAL || rref->kind == SYM_PARAM) nrvo_skip = rref->index;
        }
        emit_dtors_from(gen, 0, ast->line, nrvo_skip);
        emit_return(gen, r, 1, ast->line);
        reg_free(gen, r);
        return;
    }

    if (ast->u.ret) {
        int r = gen_expr(gen, ast->u.ret);
        emit_cast_for_return(gen, ast->u.ret, r, ast->line);   // C1：返回值按声明类型规范化
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

    // 结果区必须连号：整块分配 base .. base+count-1，count 寄存器紧随其后
    int base = reg_alloc_block(gen, count + 1);
    for (int i = 0; i < count; i++) {
        gen_expr_to(gen, ast->u.ret_multi.exprs[i], base + i);
    }

    // RETURN_MULTI: A = base, B = count_reg
    int count_reg = base + count;
    emit_loadi_to(gen, count_reg, count, ast->line);

    if (gen->dtor_count > 0) {
        int nrvo_skip[16];
        int nrvo_count = 0;
        for (int j = 0; j < count && j < 16; j++) {
            Ast* e = ast->u.ret_multi.exprs[j];
            if (e && e->kind == AST_VAR) {
                SymRef* rref = &e->u.var.ref;
                if (rref->kind == SYM_LOCAL || rref->kind == SYM_PARAM) nrvo_skip[nrvo_count++] = rref->index;
            }
        }
        for (int i = gen->dtor_count - 1; i >= 0; i--) {
            int slot = gen->dtor_entries[i].local_slot;
            int skip = 0;
            for (int j = 0; j < nrvo_count; j++) if (slot == nrvo_skip[j]) { skip = 1; break; }
            if (skip) continue;
            reg_encode_iABC(gen->chunk, OP_DTOR_LOCAL, slot, 0, 0, ast->line);
        }
    }

    emit_return_multi(gen, base, count_reg, ast->line);
    reg_free_block(gen, base);
}

// ============================================================================
// var_decl
// ============================================================================

// 声明类型要求"运行时类型标注"时，在值已写进 R[reg] 之后补发设置指令：
//   Ptr[u32] / Ptr[u8] …  → OP_SET_PTR_ELEM_TYPE（ffi.read_at/write_at 靠它算元素宽度）
//   face 类型（Speaker 等）→ OP_SET_DECLARED_FACE（type()/数组类型推断把实例当 face 看）
// ⚠ 照栈式 gen_var_decl 的做法（它在赋值前后各发一次）。寄存器式的这两条此前是**空实现**
//   且从未发射 ⇒ `Ptr[u32] p = ffi.malloc(20); ffi.write_at(p, 0, 10)` 直接抛
//   「write_at 需要 Ptr[T] 类型指针（如 Ptr[u32]），不支持无类型 Ptr」。
static void emit_decl_runtime_type(CodeGen* gen, int reg, TypeInfo* t, int line) {
    if (!t) return;
    if (t->kind == TYPE_PTR_GENERIC && t->element_type) {
        reg_encode_iABC(gen->chunk, OP_SET_PTR_ELEM_TYPE, reg, (int)t->element_type->kind, 0, line);
    } else if (t->kind == TYPE_FACE && t->struct_name) {
        ObjString* face_name = str_copy(t->struct_name, (int)strlen(t->struct_name));
        int name_const = make_constant(gen, val_obj((Object*)face_name));
        reg_encode_iABx(gen->chunk, OP_SET_DECLARED_FACE, reg, name_const, line);
    }
}

// "声明了但没初始化"时的**类型默认值**（照栈式 gen_var_decl / gen_var_decl_module）：
//   Array[T] → 空数组、Dict → 空字典、其余 → null。
//   ⚠ 不能一律 null：`Array[int] res` + `res.add(x)` 是极常见写法，null 上调用方法
//   会直接抛「类型 object 上不存在方法 'add'」（实测 examples/type类型/
//   处理 非数值的健壮函数.leno；栈式同一段代码正常返回 [10]）。
static void emit_typed_default_to(CodeGen* gen, int dst, TypeInfo* t, int line) {
    if (t && t->kind == TYPE_ARRAY) {
        reg_encode_iABC(gen->chunk, OP_NEWARRAY, dst, 0, 0, line);   // A=dst C=元素个数(0)
        return;
    }
    if (t && t->kind == TYPE_DICT) {
        reg_encode_iABC(gen->chunk, OP_NEWDICT, dst, 0, 0, line);
        return;
    }
    emit_loadnil_to(gen, dst, line);
}

static void gen_var_decl(CodeGen* gen, Ast* ast) {
    SymRef* ref = &ast->u.var_decl.ref;
    if (!ref->name) return;

    // 全局变量：值存 vm.globals[ref->index]，寄存器只做中转
    if (ref->kind == SYM_GLOBAL) {
        int r = reg_alloc(gen);
        if (ast->u.var_decl.init) {
            gen_expr_to(gen, ast->u.var_decl.init, r);
            // ★ 声明类型是 float/int/string 时同样要规范化（与下面的局部变量分支一致）。
            //   漏掉会让 `float g = 1` 在全局里**存成 int**：print 出 `1` 而不是 `1.0`，
            //   后续算术也走 int 语义 —— 静默错值（实测 examples/测试/export array or dict：
            //   `float val = m.mixedArr[0]` 打印 1、栈式打印 1.0）。
            TypeInfo* gvt = ast->u.var_decl.type;
            if (gvt) emit_cast_for_target(gen, gvt->kind, ast->u.var_decl.init, r, ast->line);
        } else {
            emit_typed_default_to(gen, r, ast->u.var_decl.type, ast->line);
        }
        emit_decl_runtime_type(gen, r, ast->u.var_decl.type, ast->line);
        emit_defglobal(gen, r, ref->index, ast->line);
        reg_free(gen, r);
        return;
    }

    // 模块变量：值存 module->globals[ref->index]
    if (ref->kind == SYM_MODULE) {
        int r = reg_alloc(gen);
        if (ast->u.var_decl.init) {
            gen_expr_to(gen, ast->u.var_decl.init, r);
            TypeInfo* mvt = ast->u.var_decl.type;   // 同上：模块变量也要 CAST
            if (mvt) emit_cast_for_target(gen, mvt->kind, ast->u.var_decl.init, r, ast->line);
        } else {
            emit_typed_default_to(gen, r, ast->u.var_decl.type, ast->line);
        }
        emit_decl_runtime_type(gen, r, ast->u.var_decl.type, ast->line);
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
        emit_typed_default_to(gen, dst, ast->u.var_decl.type, ast->line);
    }
    // Ptr[T] / face 的运行时类型标注（局部变量同样需要：ffi.read_at 等要靠它）
    emit_decl_runtime_type(gen, dst, ast->u.var_decl.type, ast->line);

    // 追踪带析构的 struct 局部变量：作用域结束 / return 时发 OP_DTOR_LOCAL。
    // ⚠ `var r = new Resource()` 的声明类型是 `var`（u.var_decl.type 不是 TYPE_STRUCT），
    //   必须回退到**初始化表达式的推断类型**，否则析构整类不触发 ✗
    TypeInfo* t = ast->u.var_decl.type;
    int is_struct_local = (t && t->kind == TYPE_STRUCT && t->struct_name);
    if (!is_struct_local && ast->u.var_decl.init) {
        TypeInfo* it = infer_expr_type(gen->sem, ast->u.var_decl.init);
        if (it && it->kind == TYPE_STRUCT && it->struct_name) is_struct_local = 1;
        if (it) type_free(it);
    }
    if (is_struct_local) {
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

// 索引赋值（裸 AST_INDEX_ASSIGN）：R[obj][R[idx]] = R[val]
//   dst >= 0 时把被赋的值也写入 dst（表达式位置）；dst < 0 表示语句位置（丢弃结果）
void gen_index_assign(CodeGen* gen, Ast* ast, int dst) {
    Ast* obj_ast = ast->u.index_assign.obj;
    Ast* idx_ast = ast->u.index_assign.index;
    Ast* val_ast = ast->u.index_assign.value;
    int obj_reg = obj_ast ? gen_expr(gen, obj_ast) : -1;
    int idx_reg = idx_ast ? gen_expr(gen, idx_ast) : -1;
    int val_reg = val_ast ? gen_expr(gen, val_ast) : -1;
    if (val_reg < 0) {
        val_reg = reg_alloc(gen);
        emit_loadnil_to(gen, val_reg, ast->line);
    }
    // INDEX_SET: R[B][R[C]] = R[A]
    reg_encode_iABC(gen->chunk, OP_INDEX_SET, val_reg, obj_reg, idx_reg, ast->line);
    if (dst >= 0 && dst != val_reg) emit_mov(gen, dst, val_reg, ast->line);
    reg_free(gen, val_reg);
    if (idx_reg >= 0) reg_free(gen, idx_reg);
    if (obj_reg >= 0) reg_free(gen, obj_reg);
}

// 第 i 个右侧值：多目标时 parser 把 RHS 包成 AST_ARRAY（`a, b = b, a`）
static Ast* assign_rhs_value(Ast* ast, int i) {
    Ast* value = ast->u.assign.value;
    if (ast->u.assign.name_count > 1 && value && value->kind == AST_ARRAY &&
        i < value->u.array.count) {
        return value->u.array.items[i];
    }
    return value;
}

void gen_assign(CodeGen* gen, Ast* ast) {
    int n = ast->u.assign.name_count;
    if (n <= 0 || !ast->u.assign.targets) return;

    // ★ 并行赋值（`a, b = b, a`）必须**先把所有右侧值求到临时寄存器**，再逐个写回。
    //   求一个写一个时，第一次写回就污染了第二次还要读的变量 ——
    //   实测 `a, b = b, a` 得到 b 的新值（两个都变成 b）✗
    int* pre = NULL;
    if (n > 1 && ast->u.assign.value) {
        pre = (int*)malloc(sizeof(int) * (size_t)n);
        if (pre) {
            for (int i = 0; i < n; i++) {
                pre[i] = gen_expr(gen, assign_rhs_value(ast, i));
            }
        }
    }

    for (int i = 0; i < n; i++) {
        Ast* target = ast->u.assign.targets[i];
        if (!target) continue;

        Ast* value = assign_rhs_value(ast, i);
        int pre_reg = pre ? pre[i] : -1;

        // 取值到 val_reg：并行赋值时直接用已求值的临时寄存器
        #define ASSIGN_VAL() (pre_reg >= 0 ? pre_reg : gen_expr(gen, value))
        #define ASSIGN_FREE_VAL(_r) do { if (pre_reg < 0) reg_free(gen, (_r)); } while (0)

        switch (target->kind) {
            case AST_VAR: {
                SymRef* ref = assign_target_ref(ast, i);
                switch (ref->kind) {
                    case SYM_LOCAL:
                    case SYM_PARAM:
                        if (pre_reg >= 0) {
                            emit_cast_for_target(gen, ref->type_kind, value, pre_reg, ast->line);
                            if (ref->index != pre_reg) emit_mov(gen, ref->index, pre_reg, ast->line);
                        } else {
                            gen_expr_to(gen, value, ref->index);
                            emit_cast_for_target(gen, ref->type_kind, value, ref->index, ast->line);
                        }
                        break;
                    case SYM_GLOBAL: {
                        int r = ASSIGN_VAL();
                        emit_cast_for_target(gen, ref->type_kind, value, r, ast->line);
                        emit_setglobal(gen, r, ref->index, ast->line);
                        ASSIGN_FREE_VAL(r);
                        break;
                    }
                    case SYM_UPVALUE: {
                        int r = ASSIGN_VAL();
                        emit_cast_for_target(gen, ref->type_kind, value, r, ast->line);
                        emit_setupval(gen, r, ref->index, ast->line);
                        ASSIGN_FREE_VAL(r);
                        break;
                    }
                    case SYM_MODULE: {
                        int r = ASSIGN_VAL();
                        emit_cast_for_target(gen, ref->type_kind, value, r, ast->line);
                        reg_encode_iABx(gen->chunk, OP_SET_MODULE_VAR, r, ref->index, ast->line);
                        ASSIGN_FREE_VAL(r);
                        break;
                    }
                    default:
                        break;
                }
                break;
            }

            // 索引赋值：arr[i] = val / dict["k"] = val
            case AST_INDEX:
            case AST_INDEX_ASSIGN: {
                Ast* obj_ast = (target->kind == AST_INDEX) ? target->u.index.obj
                                                           : target->u.index_assign.obj;
                Ast* idx_ast = (target->kind == AST_INDEX) ? target->u.index.index
                                                           : target->u.index_assign.index;
                int obj_reg = gen_expr(gen, obj_ast);
                int idx_reg = gen_expr(gen, idx_ast);
                int val_reg = ASSIGN_VAL();
                // INDEX_SET: R[B][R[C]] = R[A]
                reg_encode_iABC(gen->chunk, OP_INDEX_SET, val_reg, obj_reg, idx_reg, ast->line);
                ASSIGN_FREE_VAL(val_reg);
                reg_free(gen, idx_reg);
                reg_free(gen, obj_reg);
                break;
            }

            // 字段赋值：obj.field = val
            case AST_FIELD_ACCESS: {
                int obj_reg = gen_expr(gen, target->u.field_access.obj);
                int val_reg = ASSIGN_VAL();
                // SET_FIELD: R[B].field(C) = R[A]
                reg_encode_iABC(gen->chunk, OP_SET_FIELD, val_reg, obj_reg,
                                target->u.field_access.field_index, ast->line);
                ASSIGN_FREE_VAL(val_reg);
                reg_free(gen, obj_reg);
                break;
            }

            default:
                break;
        }
        #undef ASSIGN_VAL
        #undef ASSIGN_FREE_VAL
    }

    if (pre) {
        for (int i = 0; i < n; i++) reg_free(gen, pre[i]);
        free(pre);
    }
}

// ============================================================================
// compound_assign
// ============================================================================

// 复合赋值的运算部分：R[dst] op= R[r]
static void emit_compound_op(CodeGen* gen, LenoTokenType op, int dst, int r, int line) {
    switch (op) {
        case TOK_PLUSEQ:  emit_add(gen, dst, dst, r, line); break;
        case TOK_MINUSEQ:  emit_sub(gen, dst, dst, r, line); break;
        case TOK_STAREQ:   emit_mul(gen, dst, dst, r, line); break;
        case TOK_SLASHEQ:  emit_div(gen, dst, dst, r, line); break;
        case TOK_MODEQ:    emit_mod(gen, dst, dst, r, line); break;
        case TOK_BITANDEQ: emit_bitand(gen, dst, dst, r, line); break;
        case TOK_BITOREQ:  emit_bitor(gen, dst, dst, r, line); break;
        case TOK_BITXOREQ: emit_bitxor(gen, dst, dst, r, line); break;
        case TOK_SHLEQ:    emit_shl(gen, dst, dst, r, line); break;
        case TOK_SHREQ:    emit_shr(gen, dst, dst, r, line); break;
        case TOK_USHREQ:   emit_ushr(gen, dst, dst, r, line); break;
        default: break;
    }
}

void gen_compound_assign(CodeGen* gen, Ast* ast) {
    // a += expr → R[dst] = R[dst] + expr
    SymRef* ref = &ast->u.compound_assign.ref;
    if (!ref->name) return;

    // ★ struct 方法体内的字段复合赋值（`count += n`）：语义阶段把目标改写成
    //   ref.name = "__self_field__"、ref.index = 字段索引（self 固定在 R0）。
    //   此前这种标记被当成普通变量槽位用（读到 R0/R<字段索引> 的垃圾）⇒ null 参与运算 ✗
    if (strcmp(ref->name, "__self_field__") == 0) {
        int field_idx = ref->index;
        int slot = reg_alloc(gen);
        // GET_FIELD: R[slot] = R[0].field(field_idx)
        reg_encode_iABC(gen->chunk, OP_GET_FIELD, slot, 0, field_idx, ast->line);
        int r = gen_expr(gen, ast->u.compound_assign.value);
        emit_compound_op(gen, ast->u.compound_assign.op, slot, r, ast->line);
        reg_free(gen, r);
        // SET_FIELD: R[0].field(field_idx) = R[slot]
        reg_encode_iABC(gen->chunk, OP_SET_FIELD, slot, 0, field_idx, ast->line);
        reg_free(gen, slot);
        return;
    }

    int dst;
    int is_local = (ref->kind == SYM_LOCAL || ref->kind == SYM_PARAM);
    if (is_local) {
        dst = ref->index;
    } else {
        // 全局 / upvalue / **模块变量**：先读到临时寄存器，算完再写回
        //   ⚠ SYM_MODULE 漏了会退化成"读成 null"（下面那个 else 分支发 LOADNIL）
        //   ⇒ 模块级 `var _windowCount = 0` 的 `_windowCount -= 1` 变成 `null - 1`
        //   （实测：SDL 的 Window.run 收尾报「减法: null 不能参与运算」）；
        //   而且即便不报错也从不回写。gen_assign 有 SYM_MODULE 分支，这里必须对齐。
        dst = reg_alloc(gen);
        if (ref->kind == SYM_GLOBAL) {
            emit_getglobal_to(gen, dst, ref->index, ast->line);
        } else if (ref->kind == SYM_UPVALUE) {
            emit_getupval_to(gen, dst, ref->index, ast->line);
        } else if (ref->kind == SYM_MODULE) {
            reg_encode_iABx(gen->chunk, OP_GET_MODULE_VAR, dst, ref->index, ast->line);
        } else {
            emit_loadnil_to(gen, dst, ast->line);
        }
    }

    int r = gen_expr(gen, ast->u.compound_assign.value);

    emit_compound_op(gen, ast->u.compound_assign.op, dst, r, ast->line);
    reg_free(gen, r);

    if (!is_local) {
        if (ref->kind == SYM_GLOBAL) {
            emit_setglobal(gen, dst, ref->index, ast->line);
        } else if (ref->kind == SYM_UPVALUE) {
            emit_setupval(gen, dst, ref->index, ast->line);
        } else if (ref->kind == SYM_MODULE) {
            reg_encode_iABx(gen->chunk, OP_SET_MODULE_VAR, dst, ref->index, ast->line);
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
    int catch_fin_patch = -1;
    if (ast->u.try_.catch_var && cref->name) {
        if (cref->index >= gen->next_reg) {
            gen->next_reg = cref->index + 1;
            if (gen->next_reg > gen->max_reg) gen->max_reg = gen->next_reg;
        }
        // CATCH iABx: R[A] = 当前异常；Bx = finally 块相对本指令的偏移（0 = 无 finally）
        reg_encode_iABx(gen->chunk, OP_CATCH, cref->index, 0, ast->line);
        catch_fin_patch = gen->chunk->len - 2;   // Bx 字段位置
    }
    // A=1 表示"catch 块入口"这次 END_TRY：保留 catch_finally_ip（catch 体期间有效）
    reg_encode_iABC(gen->chunk, OP_END_TRY, 1, 0, 0, ast->line);

    if (ast->u.try_.catch_body) {
        gen_stmt(gen, ast->u.try_.catch_body);
    }

    // --- finally 块（catch 结束自然落入这里）---
    int fin_pos = gen->chunk->len;
    patch_jmp_to(gen, jmp_finally, fin_pos);
    // 回填 CATCH 的 Bx：catch 体内再次 throw 时，仍要先执行本层 finally 再向外传播
    //   （`try { throw "first" } catch e { throw "second" } finally { ... }`）
    if (catch_fin_patch >= 0) {
        int rel = has_finally ? (fin_pos - catch_pos) : 0;
        gen->chunk->code[catch_fin_patch] = (uint8_t)((rel >> 8) & 0xFF);
        gen->chunk->code[catch_fin_patch + 1] = (uint8_t)(rel & 0xFF);
    }
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

// cstruct 定义：iABx(op, A=目标寄存器, Bx=名字常量) + 紧随元数据
//   field_count(1) total_size(2) alignment(1) is_packed(1) explicit_align(1)
//   每字段：name_const(2) type(1) offset(2) array_dim(2) struct_name_const(2, 0xFFFF=无)
//           + TYPE_PTR_GENERIC 时再 1 字节元素类型
// VM 侧 OP_CSTRUCT_DEF 按同样顺序读（与栈式 op_cstruct.inc 的布局一致）
static void gen_cstruct_def(CodeGen* gen, Ast* ast) {
    int r = reg_alloc(gen);
    ObjString* cstruct_name = str_copy(ast->u.cstruct_def.name,
                                       (int)strlen(ast->u.cstruct_def.name));
    int name_const = make_constant(gen, val_obj((Object*)cstruct_name));

    reg_encode_iABx(gen->chunk, OP_CSTRUCT_DEF, r, name_const, ast->line);
    cw_u8(gen, ast->u.cstruct_def.field_count, ast->line);
    cw_u16(gen, ast->u.cstruct_def.total_size, ast->line);
    cw_u8(gen, ast->u.cstruct_def.alignment, ast->line);
    cw_u8(gen, ast->u.cstruct_def.is_packed ? 1 : 0, ast->line);
    cw_u8(gen, (uint8_t)ast->u.cstruct_def.explicit_align, ast->line);

    for (int i = 0; i < ast->u.cstruct_def.field_count; i++) {
        ObjString* fn = str_copy(ast->u.cstruct_def.field_names[i],
                                 (int)strlen(ast->u.cstruct_def.field_names[i]));
        cw_u16(gen, make_constant(gen, val_obj((Object*)fn)), ast->line);

        TypeInfo* ft = ast->u.cstruct_def.field_types ? ast->u.cstruct_def.field_types[i] : NULL;
        TypeKind fk = ft ? ft->kind : TYPE_ANY;
        cw_u8(gen, fk, ast->line);
        cw_u16(gen, ast->u.cstruct_def.field_offsets ? ast->u.cstruct_def.field_offsets[i] : 0, ast->line);
        cw_u16(gen, ast->u.cstruct_def.field_array_dims ? ast->u.cstruct_def.field_array_dims[i] : 0, ast->line);

        if (fk == TYPE_CSTRUCT && ft && ft->struct_name) {
            cw_u16(gen, make_constant(gen, val_obj((Object*)str_copy(
                          ft->struct_name, (int)strlen(ft->struct_name)))), ast->line);
        } else {
            cw_u16(gen, 0xFFFF, ast->line);
        }
        if (fk == TYPE_PTR_GENERIC) {
            TypeKind elem = (ft && ft->element_type) ? ft->element_type->kind : TYPE_PTR;
            cw_u8(gen, elem, ast->line);
        }
    }

    // 定义对象绑定到符号槽位（与 enum/struct 定义一致）
    SymRef* ref = &ast->u.cstruct_def.ref;
    if (ref->kind == SYM_GLOBAL) {
        emit_defglobal(gen, r, ref->index, ast->line);
    } else if (ref->kind == SYM_LOCAL || ref->kind == SYM_PARAM) {
        emit_mov(gen, ref->index, r, ast->line);
    } else if (ref->kind == SYM_MODULE) {
        reg_encode_iABx(gen->chunk, OP_SET_MODULE_VAR, r, ref->index, ast->line);
    }
    reg_free(gen, r);
}

static void gen_clib_def(CodeGen* gen, Ast* ast) {
    // 简化：clib 定义不生成字节码
}

static void gen_cfunc_decl(CodeGen* gen, Ast* ast) {
    // 简化：cfunc 声明不生成字节码
}

static void gen_alias(CodeGen* gen, Ast* ast) {
    // 纯类型别名（`alias Handler = func(Event):bool`）：编译期语法糖，不发指令
    if (!ast->u.alias.expr) return;

    // 值别名（`alias Disabled = Enum.Member`）：必须求值并绑定到符号，
    // 否则别名在使用处是 null（枚举值别名整类失效 ✗）
    int r = gen_expr(gen, ast->u.alias.expr);
    SymRef* ref = &ast->u.alias.ref;
    if (ref->kind == SYM_GLOBAL) {
        emit_defglobal(gen, r, ref->index, ast->line);
    } else if (ref->kind == SYM_MODULE) {
        reg_encode_iABx(gen->chunk, OP_SET_MODULE_VAR, r, ref->index, ast->line);
    } else if (ref->kind == SYM_LOCAL || ref->kind == SYM_PARAM) {
        if (ref->index >= gen->next_reg) {
            gen->next_reg = ref->index + 1;
            if (gen->next_reg > gen->max_reg) gen->max_reg = gen->next_reg;
        }
        emit_mov(gen, ref->index, r, ast->line);
    }
    reg_free(gen, r);
}

// ============================================================================
// 解构声明
// ============================================================================

// 解构声明：把目标变量的值写到它的槽位。
//   变量槽位来自语义分析写好的 refs（别再 scope_resolve —— codegen 阶段的作用域
//   已不对应声明点）。
// 两种形态：
//   1) 多返回值调用：`var[int,string](a, b) = f()` → CALL 的 nresults = 槽位数，
//      结果落在连续寄存器，再逐个搬到变量槽。
//   2) 数组/字典解构：`var[int,int](a, b) = arr` / `var[...](x) = d["k"]`
//      → 对源对象做 OP_INDEX（字典用键名，数组用序号）。
static void gen_destruct_decl(CodeGen* gen, Ast* ast) {
    int n = ast->u.destruct_decl.slot_count;
    Ast* init = ast->u.destruct_decl.init;
    if (n <= 0 || !init) return;

    SymRef* refs = ast->u.destruct_decl.refs;
    int line = ast->line;

    // 把值 value_reg 写入第 i 个目标变量
    #define DESTRUCT_STORE(_i, _val_reg)                                          \
        do {                                                                      \
            SymRef* _r = (refs && refs[(_i)].name) ? &refs[(_i)] : NULL;          \
            if (_r) {                                                             \
                if (_r->kind == SYM_LOCAL || _r->kind == SYM_PARAM) {             \
                    if (_r->index != (_val_reg)) emit_mov(gen, _r->index, (_val_reg), line); \
                } else if (_r->kind == SYM_GLOBAL) {                              \
                    emit_setglobal(gen, (_val_reg), _r->index, line);             \
                } else if (_r->kind == SYM_UPVALUE) {                             \
                    emit_setupval(gen, (_val_reg), _r->index, line);              \
                } else if (_r->kind == SYM_MODULE) {                              \
                    reg_encode_iABx(gen->chunk, OP_SET_MODULE_VAR, (_val_reg), _r->index, line); \
                }                                                                 \
            }                                                                     \
        } while (0)

    // ★ 只有**确实返回多个值**的调用才能按"结果区连续"处理：
    //   `make_array()` 返回 Array[int]（一个值），若也按多返回值收发就会把数组本身
    //   当成第 1 个返回值、其余槽位读到垃圾 ✗（实测 test_destruct 得 [object]）
    int init_is_multi = (init->cached_type && init->cached_type->kind == TYPE_MULTI_RET);

    // --- 情形 1：多返回值调用 ---
    //   注意 `var{"code": int, "msg": string}(c, m) = f()` 也是这种形态：
    //   花括号里的键名只是"带键名的多返回值类型"标注，并不是字典解构。
    //   所以这里不看 is_dict，只看 init 是不是一次调用。
    //   跨模块的 `mod.f()` 是 AST_MODULE_CALL，同样要按"结果区连续"处理。
    if (init->kind == AST_MODULE_CALL && n > 1 && init_is_multi) {
        // 与普通跨模块调用**共用**同一套准备（模块对象解析 / callee / 实参 / 默认参数）。
        // ⚠ 此前这里自带一份简化实现：模块对象只认 lib_ref 与 root_scope 兜底，
        //   于是"模块 A 的方法里调用模块 B 的多返回值函数"解析不到模块对象 ⇒ 发 nil
        //   ⇒ `nil["f"]` 报「下标访问: 对象不支持索引」（LenoSDL3 sdl_titlebar.leno:331）。
        int expected = 0;
        int base = gen_module_call_prep(gen, init, n, &expected);
        emit_call(gen, base, expected, n, line);
        for (int i = 0; i < n; i++) {
            DESTRUCT_STORE(i, base + i);
        }
        reg_free_block(gen, base);
        return;
    }

    if (init->kind == AST_CALL && n > 1 && init_is_multi) {
        // 走公共的多返回值调用生成：方法调用（GET_METHOD）、默认参数补齐都一致
        int base = gen_call_multi(gen, init, n, line);
        for (int i = 0; i < n; i++) {
            DESTRUCT_STORE(i, base + i);
        }
        reg_free_block(gen, base);
        return;
    }

    // --- 情形 2：数组 / 字典解构 ---
    int src = gen_expr(gen, init);
    for (int i = 0; i < n; i++) {
        SymRef* r = (refs && refs[i].name) ? &refs[i] : NULL;
        if (!r || (!r->name)) continue;

        int idx_reg = reg_alloc(gen);
        if (ast->u.destruct_decl.is_dict) {
            const char* key = (ast->u.destruct_decl.slot_keys && ast->u.destruct_decl.slot_keys[i])
                                  ? ast->u.destruct_decl.slot_keys[i] : NULL;
            if (!key) { reg_free(gen, idx_reg); continue; }
            int c = make_constant(gen, val_obj((Object*)str_copy(key, (int)strlen(key))));
            emit_loadk_to(gen, idx_reg, c, line);
        } else {
            emit_loadi_to(gen, idx_reg, i, line);
        }

        // 目标不是局部槽位时，先取到临时再写回（OP_INDEX 的目标不能跨到全局）
        int is_local_slot = (r->kind == SYM_LOCAL || r->kind == SYM_PARAM);
        if (is_local_slot && r->index != src) {
            reg_encode_iABC(gen->chunk, OP_INDEX, r->index, src, idx_reg, line);
        } else {
            int tmp = reg_alloc(gen);
            reg_encode_iABC(gen->chunk, OP_INDEX, tmp, src, idx_reg, line);
            DESTRUCT_STORE(i, tmp);
            reg_free(gen, tmp);
        }
        reg_free(gen, idx_reg);
    }
    reg_free(gen, src);
    #undef DESTRUCT_STORE
}

// ============================================================================
// import / export / use
// ============================================================================

static void gen_import(CodeGen* gen, Ast* ast) {
    const char* mod = ast->u.import.module_name;
    if (!mod) return;

    // 原生模块（times / io / jsons ...）：运行时加载，确保方法表已注册
    // 是否原生模块：用原生注册表探测。
    // 不能靠"名字里有没有 .leno"来判断 —— `import multi_ret_mod`（不带后缀）
    // 同样是 .leno 源码模块，会被误当作原生模块加载（模块对象为 null，
    // 后续 mod.func() 全部失败）。
    extern int native_init_module(const char* name);
    int is_native_mod = (native_init_module(mod) == 0);

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


    // .leno 模块文件：没写后缀就补 .leno（`import multi_ret_mod` 是合法写法）
    char mod_path_buf[512];
    const char* mod_path = mod;
    if (!strstr(mod, ".leno") && strlen(mod) + 6 < sizeof(mod_path_buf)) {
        snprintf(mod_path_buf, sizeof(mod_path_buf), "%s.leno", mod);
        mod_path = mod_path_buf;
    }

    const char* current_file = error_get_filename();
    ObjModule* module = load_module_file(mod_path, current_file, alias);

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
