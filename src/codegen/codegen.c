// ============================================================================
// 寄存器式 codegen：初始化 / 清理 / 顶层入口
// ============================================================================

#include "codegen.h"

void codegen_init(CodeGen* gen, Chunk* chunk, Semantic* sem) {
    gen->chunk = chunk;
    gen->sem = sem;
    gen->scope_depth = 0;
    gen->loop_head = NULL;
    gen->loop_count = 0;
    gen->finally_depth = 0;
    gen->current_func = NULL;
    // 寄存器分配器初始化
    gen->next_reg = 0;
    gen->max_reg = 0;
    gen->freetop = 0;
    gen->scope_base = 0;
    gen->dtor_entries = NULL;
    gen->dtor_count = 0;
    gen->dtor_capacity = 0;
    gen->dtor_temp_slot = -1;
    gen->suppress_multi_pop = 0;
    gen->mod_alias_count = 0;
}

void codegen_cleanup(CodeGen* gen) {
    while (gen->loop_head) {
        LoopContextNode* node = gen->loop_head;
        gen->loop_head = node->prev;
        free(node);
    }
    gen->loop_count = 0;
    if (gen->dtor_entries) {
        free(gen->dtor_entries);
        gen->dtor_entries = NULL;
    }
    gen->dtor_count = 0;
    gen->dtor_capacity = 0;
}

void codegen_add_dtor_entry(CodeGen* gen, int local_slot) {
    if (gen->dtor_count >= gen->dtor_capacity) {
        int new_cap = gen->dtor_capacity == 0 ? 8 : gen->dtor_capacity * 2;
        gen->dtor_entries = (DtorEntry*)realloc(gen->dtor_entries, sizeof(DtorEntry) * new_cap);
        gen->dtor_capacity = new_cap;
    }
    gen->dtor_entries[gen->dtor_count].local_slot = local_slot;
    gen->dtor_count++;
}

// ============================================================================
// 槽位预扫描：求"顶层代码用到的最大局部变量槽位 + 1"
// ----------------------------------------------------------------------------
// 局部变量槽位号由语义分析分配（可复用、且可能远大于 0）。寄存器式下变量
// 直接用槽位号当寄存器号，临时寄存器必须从所有槽位之上开始分配 —— 否则
// 临时值会覆盖变量（表现为"变量莫名其妙变成别的值"）。
// AST_FUNC_DEF 不递归：函数体有独立的槽位空间（func->local_count）。
// ============================================================================

static void scan_slots_list(AstList* list, int* max_slot);

static void note_ref(SymRef* r, int* max_slot) {
    if (!r || !r->name) return;
    if ((r->kind == SYM_LOCAL || r->kind == SYM_PARAM) && r->index >= 0 && r->index + 1 > *max_slot) {
        *max_slot = r->index + 1;
    }
}

static void note_slot(int slot, int* max_slot) {
    if (slot >= 0 && slot + 1 > *max_slot) *max_slot = slot + 1;
}

static void scan_slots(Ast* ast, int* max_slot) {
    if (!ast) return;
    switch (ast->kind) {
        case AST_FUNC_DEF: return;   // 函数体独立生成
        case AST_VAR: note_ref(&ast->u.var.ref, max_slot); return;
        case AST_VAR_DECL:
            note_ref(&ast->u.var_decl.ref, max_slot);
            scan_slots(ast->u.var_decl.init, max_slot);
            return;
        case AST_BLOCK: scan_slots_list(&ast->u.block, max_slot); return;
        case AST_EXPR_STMT: scan_slots(ast->u.expr_stmt.expr, max_slot); return;
        case AST_BINOP:
            scan_slots(ast->u.binop.l, max_slot);
            scan_slots(ast->u.binop.r, max_slot);
            return;
        case AST_UNARY: scan_slots(ast->u.unary.operand, max_slot); return;
        case AST_CALL:
            scan_slots(ast->u.call.callee, max_slot);
            scan_slots_list(&ast->u.call.args, max_slot);
            return;
        case AST_INDEX:
            scan_slots(ast->u.index.obj, max_slot);
            scan_slots(ast->u.index.index, max_slot);
            return;
        case AST_SLICE:
            scan_slots(ast->u.slice.obj, max_slot);
            scan_slots(ast->u.slice.start, max_slot);
            scan_slots(ast->u.slice.end, max_slot);
            return;
        case AST_INDEX_ASSIGN:
            scan_slots(ast->u.index_assign.obj, max_slot);
            scan_slots(ast->u.index_assign.index, max_slot);
            scan_slots(ast->u.index_assign.value, max_slot);
            return;
        case AST_ASSIGN:
            for (int i = 0; i < ast->u.assign.name_count; i++) scan_slots(ast->u.assign.targets[i], max_slot);
            if (ast->u.assign.refs) {
                for (int i = 0; i < ast->u.assign.name_count; i++) note_ref(&ast->u.assign.refs[i], max_slot);
            }
            scan_slots(ast->u.assign.value, max_slot);
            return;
        case AST_COMPOUND_ASSIGN:
            note_ref(&ast->u.compound_assign.ref, max_slot);
            scan_slots(ast->u.compound_assign.value, max_slot);
            return;
        case AST_IF:
            scan_slots(ast->u.if_.cond, max_slot);
            scan_slots(ast->u.if_.then, max_slot);
            scan_slots(ast->u.if_.else_, max_slot);
            note_slot(ast->u.if_.guard_bind_index, max_slot);
            scan_slots(ast->u.if_.guard_bind_expr, max_slot);
            return;
        case AST_WHILE:
            scan_slots(ast->u.while_.cond, max_slot);
            scan_slots(ast->u.while_.body, max_slot);
            return;
        case AST_FOR:
            scan_slots(ast->u.for_.start, max_slot);
            scan_slots(ast->u.for_.end, max_slot);
            scan_slots(ast->u.for_.step, max_slot);
            note_slot(ast->u.for_.loop_var_index, max_slot);
            note_slot(ast->u.for_.end_index, max_slot);
            note_slot(ast->u.for_.step_index, max_slot);
            note_slot(ast->u.for_.start_index, max_slot);
            note_slot(ast->u.for_.counter_index, max_slot);
            note_slot(ast->u.for_.index_var_index, max_slot);
            scan_slots(ast->u.for_.body, max_slot);
            return;
        case AST_SWITCH:
            scan_slots(ast->u.switch_.expr, max_slot);
            for (int i = 0; i < ast->u.switch_.case_count; i++) {
                scan_slots_list(&ast->u.switch_.cases[i].values, max_slot);
                scan_slots(ast->u.switch_.cases[i].body, max_slot);
                note_slot(ast->u.switch_.cases[i].guard_bind_index, max_slot);
                if (ast->u.switch_.cases[i].destructure_indices) {
                    for (int d = 0; d < ast->u.switch_.cases[i].destructure_count; d++) {
                        note_slot(ast->u.switch_.cases[i].destructure_indices[d], max_slot);
                    }
                }
            }
            scan_slots(ast->u.switch_.default_body, max_slot);
            return;
        case AST_RETURN: scan_slots(ast->u.ret, max_slot); return;
        case AST_RETURN_MULTI:
            for (int i = 0; i < ast->u.ret_multi.count; i++) scan_slots(ast->u.ret_multi.exprs[i], max_slot);
            return;
        case AST_THROW: scan_slots(ast->u.throw_.expr, max_slot); return;
        case AST_TRY:
            scan_slots(ast->u.try_.try_body, max_slot);
            scan_slots(ast->u.try_.catch_body, max_slot);
            scan_slots(ast->u.try_.finally_body, max_slot);
            note_ref(&ast->u.try_.catch_var_ref, max_slot);
            return;
        case AST_TYPE_CHECK:
        case AST_AS_CAST:
            scan_slots(ast->u.type_check.expr, max_slot);
            return;
        case AST_AWAIT: scan_slots(ast->u.await.expr, max_slot); return;
        case AST_INTERP_STRING:
            for (int i = 0; i < ast->u.interp_string.count; i++) scan_slots(ast->u.interp_string.exprs[i], max_slot);
            return;
        case AST_MODULE_CALL: scan_slots_list(&ast->u.module_call.args, max_slot); return;
        case AST_ARRAY:
            for (int i = 0; i < ast->u.array.count; i++) scan_slots(ast->u.array.items[i], max_slot);
            return;
        case AST_DICT:
            for (int i = 0; i < ast->u.dict.count; i++) {
                scan_slots(ast->u.dict.entries[i].key, max_slot);
                scan_slots(ast->u.dict.entries[i].value, max_slot);
            }
            return;
        case AST_STRUCT_INIT:
            for (int i = 0; i < ast->u.struct_init.field_count; i++) scan_slots(ast->u.struct_init.field_values[i], max_slot);
            return;
        case AST_FIELD_ACCESS: scan_slots(ast->u.field_access.obj, max_slot); return;
        case AST_ADDRESS_OF: scan_slots(ast->u.address_of.operand, max_slot); return;
        case AST_SAFE_ACCESS:
            scan_slots(ast->u.safe_access.obj, max_slot);
            scan_slots_list(&ast->u.safe_access.args, max_slot);
            return;
        case AST_DESTRUCT_DECL:
            if (ast->u.destruct_decl.refs) {
                for (int i = 0; i < ast->u.destruct_decl.slot_count; i++) {
                    note_ref(&ast->u.destruct_decl.refs[i], max_slot);
                }
            }
            scan_slots(ast->u.destruct_decl.init, max_slot);
            return;
        case AST_EXPORT: scan_slots(ast->u.export.decl, max_slot); return;
        case AST_ALIAS: scan_slots(ast->u.alias.expr, max_slot); return;
        default: return;
    }
}

static void scan_slots_list(AstList* list, int* max_slot) {
    for (int i = 0; i < list->count; i++) scan_slots(list->items[i], max_slot);
}

// ============================================================================
// T33：字符串自追加特化 —— 判定"哪些局部槽位可以安全地原地追加"
// ----------------------------------------------------------------------------
// 背景：`s = s + e` 是 O(n²)（每轮都重新分配并复制整个前缀）。CPython 靠
//   "refcount == 1 就原地 realloc" 的特例把它摊平成 O(n)；而 Leno 的 GC **没有引用
//   计数**，运行时无法判断"这个串是否还有别名"，所以只能在**编译期**证明：
//   该槽位在函数体内的**全部使用**都是自追加形态。
//
// 判据（保守，宁可放弃优化）—— 对函数体遍历一次，统计每个槽位：
//     decls    : AST_VAR_DECL 次数（只认 SYM_LOCAL）
//     appear   : 该槽位的 AST_VAR 出现次数（读、写都算）
//     appends  : `s = s + e` 次数（每次贡献 2 处 appear：左值 + 右值左侧）
//     compounds: `s += e` 次数（每次贡献 1 处 appear：ref）
//   可特化 ⟺ !bail && decls == 1 && (appends + compounds) > 0
//             && appear == 1 + 2*appends + compounds
//     （1 = 声明本身。**任何**其它使用 —— print(s) / t = s / f(s) / return s /
//       `s == x` / arr.add(s) —— 都会让 appear 多出来 ⇒ 判定失败 ⇒ 不特化 ✓）
//
// ⚠ fail-safe：遍历的 default 分支一律置 bail（整个函数放弃特化）。这样"将来新增
//   AST 节点类型"只会让优化失效，**绝不会**被误判成"无别名"（后者会静默改坏别人的串）。
// ⚠ 运行时还有第二道闸（OP_STR_APPEND 只对 capacity > len+1 的串原地改），所以即使
//   这里判断有误，最多退化成"多复制一次"，不会破坏语义。
// ============================================================================
typedef struct {
    int appear[MAX_REG];
    // 其中"**不可能泄漏引用**"的读取：`s.len()` / `s[i]` / `s == x`。它们只是**观察**当前
    //   内容（返回 int/bool/元素），不会让别人持有这个串 ⇒ 不阻止原地追加。
    //   允许它们很关键：现实里"循环累加完再取一次 len / 比较一下"极其常见，
    //   若一律算作"使用"就会让绝大多数累加代码都白放过。
    int soft[MAX_REG];
    int decls[MAX_REG];
    int appends[MAX_REG];
    int compounds[MAX_REG];
    int bail;
} StrAppendScan;

// `s = s + <expr>` 形态？（target 与 value 左侧是同一个 SYM_LOCAL 槽位）
// 非 static：codegen_stmt.c 的发射点要复用它（单一来源，避免两处判断分叉）
int ast_is_self_append(SymRef* target_ref, Ast* value) {
    // ⚠ 目标**不能**用 `target->u.var.ref`：赋值目标位置的 AST_VAR 在语义阶段**没有**被解析
    //   （实测其 kind 仍是 SYM_GLOBAL 这个默认值）⇒ 必须用调用方给的、已解析的 SymRef
    //   （gen_assign 传 assign_target_ref，分析遍历传 u.assign.refs[i]）。
    //   右值里的同一个变量是**正常引用**，解析正确 ⇒ 拿它做比对是可靠的。
    if (!target_ref || !target_ref->name || target_ref->kind != SYM_LOCAL) return 0;
    int slot = target_ref->index;
    if (slot < 0 || slot >= MAX_REG) return 0;
    if (!value || value->kind != AST_BINOP || value->u.binop.op != TOK_PLUS) return 0;
    Ast* l = value->u.binop.l;
    if (!l || l->kind != AST_VAR) return 0;
    return l->u.var.ref.kind == SYM_LOCAL && l->u.var.ref.index == slot;
}

static void sa_note(StrAppendScan* s, SymRef* r) {
    if (s->bail || !r || !r->name) return;
    if (r->kind == SYM_LOCAL && r->index >= 0 && r->index < MAX_REG) s->appear[r->index]++;
}

// 记一次"安全的读取"（不泄漏引用）：计入 appear，但同时也计入 soft ⇒ 判定时被扣掉
static void sa_note_soft(StrAppendScan* s, int slot) {
    if (s->bail || slot < 0 || slot >= MAX_REG) return;
    s->appear[slot]++;
    s->soft[slot]++;
}

// 编译器内部生成的槽位号（loop var / guard 绑定 / 解构 / for 的 start·end·step·counter）
//
// ⚠ 这里**故意什么都不做**。原因：这些"槽位编号"字段在**没有对应绑定变量**时的值是 0，
//   而 0 是完全合法的用户变量槽位 ⇒ 一旦计数就会污染那个变量。实测 `for 20000 to i`
//   的 `index_var_index` 就是 0，直接把"槽 0 的累加变量"判成不可特化（这正是第一版
//   死活不命中的原因）。
//   而"绑定变量真的被读取"这件事**不需要**在这里补：它在 AST 里就是一个 AST_VAR，
//   已由 sa_note 正常计入 ⇒ 语义不受影响 ✓
static void sa_note_slot(StrAppendScan* s, int slot) {
    (void)s;
    (void)slot;
}

static void sa_scan_list(StrAppendScan* s, AstList* list);

static void sa_scan(StrAppendScan* s, Ast* ast) {
    if (!ast || s->bail) return;
    switch (ast->kind) {
        case AST_FUNC_DEF: return;   // 嵌套函数独立分析；其函数体里的引用是 upvalue
        // 字面量叶子：不含变量引用。**必须显式列出** —— 否则会落进 default 的 fail-safe
        // 分支，把整个函数判成"不可特化"（`for 20000 to i` 里的 `20000` 正是这种节点，
        // 实测就是它让第一个版本完全没命中）。
        case AST_NUM:
        case AST_STRING:
        case AST_BOOL:
        case AST_NULL:
        case AST_RANGE:
            return;
        case AST_VAR: sa_note(s, &ast->u.var.ref); return;
        case AST_VAR_DECL:
            if (ast->u.var_decl.ref.kind == SYM_LOCAL && ast->u.var_decl.ref.index >= 0 &&
                ast->u.var_decl.ref.index < MAX_REG) {
                s->decls[ast->u.var_decl.ref.index]++;
            }
            sa_note(s, &ast->u.var_decl.ref);
            sa_scan(s, ast->u.var_decl.init);
            return;
        case AST_BLOCK: sa_scan_list(s, &ast->u.block); return;
        case AST_EXPR_STMT: sa_scan(s, ast->u.expr_stmt.expr); return;
        case AST_BINOP:
            // `s == x` / `s != x`：比较返回 bool，不会让别人持有这个串 ⇒ 安全读取
            if ((ast->u.binop.op == TOK_EQEQ || ast->u.binop.op == TOK_NEQ) &&
                ast->u.binop.l && ast->u.binop.l->kind == AST_VAR &&
                ast->u.binop.l->u.var.ref.kind == SYM_LOCAL) {
                sa_note_soft(s, ast->u.binop.l->u.var.ref.index);
            } else {
                sa_scan(s, ast->u.binop.l);
            }
            sa_scan(s, ast->u.binop.r);
            return;
        case AST_UNARY: sa_scan(s, ast->u.unary.operand); return;
        case AST_CALL: {
            // `s.len()` / `s.byte_len()` / `s.char_len()`：返回 int，不会让别人持有 s ⇒ 安全读取
            //   （注意：`s.len()` 在 AST 里其实是 `CALL{callee: INDEX{obj: s, index: "len"}}`
            //     —— 方法名被当成"索引"，所以真正生效的是下面 AST_INDEX 那条分支）
            Ast* callee = ast->u.call.callee;
            // ⚠ **不能**要求"接收者的 ref 已解析成 SYM_LOCAL"：接收者位置的 AST_VAR 在语义
            //   阶段同样可能没被填 kind（与赋值目标那个坑同源）⇒ 加了这条会漏掉
            //   `print("... " + acc.len())` 这类（实测 soft=0、特化完全不命中）。
            //   `len/byte_len/char_len` 无论接收者是谁都**不泄漏引用**，所以无条件放行是安全的；
            //   槽号无效（-1）时 sa_note_soft 自己会忽略 ⇒ 只是少记一次，不影响正确性。
            if (callee && callee->kind == AST_FIELD_ACCESS && callee->u.field_access.obj &&
                callee->u.field_access.obj->kind == AST_VAR &&
                callee->u.field_access.field_name) {
                const char* fn = callee->u.field_access.field_name;
                if (strcmp(fn, "len") == 0 || strcmp(fn, "byte_len") == 0 || strcmp(fn, "char_len") == 0) {
                    sa_note_soft(s, callee->u.field_access.obj->u.var.ref.index);
                    // 参数里若再出现它 ⇒ 照常算硬使用（`f(s, s.len())` 是真泄漏）
                    sa_scan_list(s, &ast->u.call.args);
                    return;
                }
            }
            sa_scan(s, callee);
            sa_scan_list(s, &ast->u.call.args);
            return;
        }
        case AST_INDEX:
            // `s[i]`（取元素）与 `s.len()`（方法名在此形态里就是索引）都不会泄漏 s 本身。
            //   ⚠ 与 AST_CALL 同一条教训：**不能**要求接收者的 ref 已解析成 SYM_LOCAL ——
            //     接收者位置的 AST_VAR 常常没被填 kind，加这条会让 `acc.len()` 判成"硬使用"，
            //     于是 `for {...} { acc = acc + "x" }` 这种最典型的累加全都特化不了。
            //     槽号无效（-1）时 sa_note_soft 自己忽略 ✓
            if (ast->u.index.obj && ast->u.index.obj->kind == AST_VAR) {
                sa_note_soft(s, ast->u.index.obj->u.var.ref.index);
            } else {
                sa_scan(s, ast->u.index.obj);
            }
            sa_scan(s, ast->u.index.index);
            return;
        case AST_SLICE:
            sa_scan(s, ast->u.slice.obj);
            sa_scan(s, ast->u.slice.start);
            sa_scan(s, ast->u.slice.end);
            return;
        case AST_INDEX_ASSIGN:
            sa_scan(s, ast->u.index_assign.obj);
            sa_scan(s, ast->u.index_assign.index);
            sa_scan(s, ast->u.index_assign.value);
            return;
        case AST_ASSIGN: {
            // 自追加形态单独记账：**不能**再走下面的通用递归，否则右值左侧会被多算一次
            if (ast->u.assign.name_count == 1 && ast->u.assign.targets && ast->u.assign.refs) {
                SymRef* tref = &ast->u.assign.refs[0];   // 语义阶段解析后的目标引用
                Ast* v = ast->u.assign.value;
                if (ast_is_self_append(tref, v)) {
                    s->appends[tref->index]++;
                    sa_note(s, tref);                       // 左值
                    sa_note(s, &v->u.binop.l->u.var.ref);   // 右值左侧
                    sa_scan(s, v->u.binop.r);               // 右侧子表达式照常递归
                    return;
                }
            }
            for (int i = 0; i < ast->u.assign.name_count; i++) sa_scan(s, ast->u.assign.targets[i]);
            if (ast->u.assign.refs) {
                for (int i = 0; i < ast->u.assign.name_count; i++) sa_note(s, &ast->u.assign.refs[i]);
            }
            sa_scan(s, ast->u.assign.value);
            return;
        }
        case AST_COMPOUND_ASSIGN:
            if (ast->u.compound_assign.op == TOK_PLUSEQ &&
                ast->u.compound_assign.ref.kind == SYM_LOCAL &&
                ast->u.compound_assign.ref.index >= 0 && ast->u.compound_assign.ref.index < MAX_REG) {
                s->compounds[ast->u.compound_assign.ref.index]++;
            }
            sa_note(s, &ast->u.compound_assign.ref);
            sa_scan(s, ast->u.compound_assign.value);
            return;
        case AST_IF:
            sa_scan(s, ast->u.if_.cond);
            sa_scan(s, ast->u.if_.then);
            sa_scan(s, ast->u.if_.else_);
            sa_note_slot(s, ast->u.if_.guard_bind_index);
            sa_scan(s, ast->u.if_.guard_bind_expr);
            return;
        case AST_WHILE:
            sa_scan(s, ast->u.while_.cond);
            sa_scan(s, ast->u.while_.body);
            return;
        case AST_FOR:
            sa_scan(s, ast->u.for_.start);
            sa_scan(s, ast->u.for_.end);
            sa_scan(s, ast->u.for_.step);
            // ⚠ 只把**用户可见**的绑定（`for n to i` 的 i、`for arr to v, idx` 的 idx）计入；
            //   start/end/step/counter 是**编译器内部临时槽**，编号由寄存器分配器决定、可能
            //   与用户变量同号 —— 实测 `start_index` 就是 0，会把"槽 0 的那个用户变量"的
            //   计数污染掉，让特化完全不命中（第一版就是栽在这）。
            sa_note_slot(s, ast->u.for_.loop_var_index);
            sa_note_slot(s, ast->u.for_.index_var_index);
            sa_scan(s, ast->u.for_.body);
            return;
        case AST_SWITCH:
            sa_scan(s, ast->u.switch_.expr);
            for (int i = 0; i < ast->u.switch_.case_count; i++) {
                sa_scan_list(s, &ast->u.switch_.cases[i].values);
                sa_scan(s, ast->u.switch_.cases[i].body);
                sa_note_slot(s, ast->u.switch_.cases[i].guard_bind_index);
                if (ast->u.switch_.cases[i].destructure_indices) {
                    for (int d = 0; d < ast->u.switch_.cases[i].destructure_count; d++) {
                        sa_note_slot(s, ast->u.switch_.cases[i].destructure_indices[d]);
                    }
                }
            }
            sa_scan(s, ast->u.switch_.default_body);
            return;
        case AST_RETURN: sa_scan(s, ast->u.ret); return;
        case AST_RETURN_MULTI:
            for (int i = 0; i < ast->u.ret_multi.count; i++) sa_scan(s, ast->u.ret_multi.exprs[i]);
            return;
        case AST_THROW: sa_scan(s, ast->u.throw_.expr); return;
        case AST_TRY:
            sa_scan(s, ast->u.try_.try_body);
            sa_scan(s, ast->u.try_.catch_body);
            sa_scan(s, ast->u.try_.finally_body);
            sa_note(s, &ast->u.try_.catch_var_ref);
            return;
        case AST_TYPE_CHECK:
        case AST_AS_CAST:
            sa_scan(s, ast->u.type_check.expr);
            return;
        case AST_AWAIT: sa_scan(s, ast->u.await.expr); return;
        case AST_INTERP_STRING:
            for (int i = 0; i < ast->u.interp_string.count; i++) sa_scan(s, ast->u.interp_string.exprs[i]);
            return;
        case AST_MODULE_CALL: sa_scan_list(s, &ast->u.module_call.args); return;
        case AST_ARRAY:
            for (int i = 0; i < ast->u.array.count; i++) sa_scan(s, ast->u.array.items[i]);
            return;
        case AST_DICT:
            for (int i = 0; i < ast->u.dict.count; i++) {
                sa_scan(s, ast->u.dict.entries[i].key);
                sa_scan(s, ast->u.dict.entries[i].value);
            }
            return;
        case AST_STRUCT_INIT:
            for (int i = 0; i < ast->u.struct_init.field_count; i++) sa_scan(s, ast->u.struct_init.field_values[i]);
            return;
        case AST_FIELD_ACCESS: sa_scan(s, ast->u.field_access.obj); return;
        case AST_ADDRESS_OF: sa_scan(s, ast->u.address_of.operand); return;
        case AST_SAFE_ACCESS:
            sa_scan(s, ast->u.safe_access.obj);
            sa_scan_list(s, &ast->u.safe_access.args);
            return;
        case AST_DESTRUCT_DECL:
            if (ast->u.destruct_decl.refs) {
                for (int i = 0; i < ast->u.destruct_decl.slot_count; i++) {
                    sa_note(s, &ast->u.destruct_decl.refs[i]);
                }
            }
            sa_scan(s, ast->u.destruct_decl.init);
            return;
        case AST_EXPORT: sa_scan(s, ast->u.export.decl); return;
        case AST_ALIAS: sa_scan(s, ast->u.alias.expr); return;
        default:
            // fail-safe：不认识这个节点 ⇒ 无法保证"没漏读"，整个函数放弃特化
            s->bail = 1;
            return;
    }
}

static void sa_scan_list(StrAppendScan* s, AstList* list) {
    if (!list) return;
    for (int i = 0; i < list->count; i++) sa_scan(s, list->items[i]);
}

// 分析入口：每个函数体生成之前调一次，重填 gen->str_append_ok
void str_append_analyze(CodeGen* gen, Ast* func_body) {
    StrAppendScan s;
    memset(&s, 0, sizeof(s));
    sa_scan(&s, func_body);
    for (int i = 0; i < MAX_REG; i++) {
        int uses = s.appends[i] + s.compounds[i];
        int need = 1 + 2 * s.appends[i] + s.compounds[i];
        // hard = 会泄漏引用的使用次数（扣掉 len/索引/比较这类"只看一眼"的读取）
        int hard = s.appear[i] - s.soft[i];
        gen->str_append_ok[i] = (!s.bail && s.decls[i] == 1 && uses > 0 && hard == need) ? 1 : 0;
    }
}

void codegen(CodeGen* gen, Ast* ast) {
    if (!ast) return;

    const char* current_file = error_get_filename();
    if (current_file && gen->chunk) {
        gen->chunk->filename = strdup(current_file);
    }

    // 临时寄存器必须从所有局部变量槽位之上开始（否则会覆盖变量）
    {
        int max_slot = 0;
        scan_slots(ast, &max_slot);
        if (max_slot > gen->next_reg) gen->next_reg = max_slot;
        if (gen->next_reg > gen->max_reg) gen->max_reg = gen->next_reg;
    }

    if (ast->kind == AST_BLOCK) {
        gen_block(gen, ast);
    } else {
        gen_stmt(gen, ast);
    }

    // 顶层 main 函数调用
    MainFuncInfo main_info = find_main_function(gen->sem);
    if (main_info.has_main) {
        // GETGLOBALFUNC R0, main_index
        // CALL R0, 0 args, 1 result
        // RETURN R0, 1 result
        int r = reg_alloc(gen);
        emit_getglobalfunc_to(gen, r, main_info.main_index, ast->line);
        emit_call(gen, r, 0, 1, ast->line);
        emit_return(gen, r, 1, ast->line);
        reg_free(gen, r);
    } else {
        // 无 main：发 RETURN NIL
        int r = reg_alloc(gen);
        emit_loadnil_to(gen, r, ast->line);
        emit_return(gen, r, 1, ast->line);
        reg_free(gen, r);
    }

    // 寄存器高水位写回 chunk->local_count（GC 依赖）
    if (gen->current_func) {
        gen->current_func->local_count = gen->max_reg;
    }
    gen->chunk->local_count = gen->max_reg;
}

// 模块代码生成
void codegen_module(CodeGen* gen, Ast* ast) {
    if (!ast) return;

    const char* current_file = error_get_filename();
    if (current_file && gen->chunk) {
        gen->chunk->filename = strdup(current_file);
    }

    if (ast->kind == AST_BLOCK) {
        gen_block_module(gen, ast);
    } else {
        gen_stmt_module(gen, ast);
    }

    // ★ 模块 chunk 末尾必须补 RETURN：模块由 vm_run_chunk 单独执行一段字节码，
    //   没有 return 就会越过 chunk 末尾继续执行（表现为莫名其妙的崩溃/乱跑）。
    {
        int r = reg_alloc(gen);
        emit_loadnil_to(gen, r, ast->line);
        emit_return(gen, r, 1, ast->line);
        reg_free(gen, r);
    }

    gen->chunk->local_count = gen->max_reg;
}
