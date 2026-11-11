#include "parser_internal.h"

// ============================================================================
// 代码块解析
// ============================================================================

// ★ 2026-11-11（P2）：defer 的「退出时求值」陷阱提示
// ----------------------------------------------------------------------------
// defer 表达式在**块退出时**求值（不是注册时，见 parse_defer_stmt 的说明）：
//     var p = ffi.malloc(64)
//     defer ffi.free(p)
//     p = ffi.malloc(128)      // ⇒ 释放的是**新值**，旧缓冲泄漏
// Go 的 defer 是**注册时**求值参数，Leno 不是 —— 而"闭着眼睛写 defer xxx.free()"
// 正是最容易踩到它的写法（外部评审建议把 defer 普及成 RAII，恰好没提这条差异）。
//
// **只提示、不改语义**：改语义会破坏 assert/test_defer.leno 里"退出时求值"的既定
// 契约（那份用例就是专门测它的），要改得单独评估。
//
// 判据**极保守**（宁漏勿误报）：
//   ① 变量名只从 defer 表达式里能静态看到的节点收集（调用/索引/字段/二元/一元/块）；
//      其它形状（闭包、字符串插值等）直接放弃 —— 不猜；
//   ② 只扫**本块顶层语句**里的赋值目标（AST_ASSIGN 的 names[] / AST_COMPOUND_ASSIGN）；
//      嵌套块里的赋值不追 ⇒ 会漏报，但不会误报。
// ============================================================================
#define DEFER_WARN_MAX_VARS 16

static void defer_collect_vars(Ast* a, const char** names, int* count, int cap) {
    if (!a || *count >= cap) return;
    switch (a->kind) {
        case AST_VAR:
            // ⚠ 用 `u.var.name`（parser 阶段**只有**它；`u.var.ref` 是语义分析填的）
            if (a->u.var.name) {
                for (int i = 0; i < *count; i++) {
                    if (strcmp(names[i], a->u.var.name) == 0) return;   // 去重
                }
                names[(*count)++] = a->u.var.name;
            }
            break;
        case AST_CALL:
            defer_collect_vars(a->u.call.callee, names, count, cap);
            for (int i = 0; i < (int)a->u.call.args.count; i++) {
                defer_collect_vars(a->u.call.args.items[i], names, count, cap);
            }
            break;
        case AST_MODULE_CALL:
            // `ffi.free(p)` 走的是模块调用节点（module_name + method_name + args），
            // **不是** AST_CALL —— 漏了这条分支就一个变量都收不到
            // （实测：`defer ffi.free(p)` 完全静默、零警告）。
            for (int i = 0; i < (int)a->u.module_call.args.count; i++) {
                defer_collect_vars(a->u.module_call.args.items[i], names, count, cap);
            }
            break;
        case AST_INDEX:
            defer_collect_vars(a->u.index.obj, names, count, cap);
            defer_collect_vars(a->u.index.index, names, count, cap);
            break;
        case AST_FIELD_ACCESS:
            defer_collect_vars(a->u.field_access.obj, names, count, cap);
            break;
        case AST_BINOP:
            defer_collect_vars(a->u.binop.l, names, count, cap);
            defer_collect_vars(a->u.binop.r, names, count, cap);
            break;
        case AST_UNARY:
            defer_collect_vars(a->u.unary.operand, names, count, cap);
            break;
        case AST_BLOCK:
            for (int i = 0; i < (int)a->u.block.count; i++) {
                defer_collect_vars(a->u.block.items[i], names, count, cap);
            }
            break;
        // ★ 2026-11-11（P2 压测补）：`defer { if p != null { ffi.free(p) } }` —— 块形式
        //   最常见的写法就是带 if 守卫；不认 AST_IF 就一个变量都收不到（实测漏报）。
        case AST_IF:
            defer_collect_vars(a->u.if_.cond, names, count, cap);
            defer_collect_vars(a->u.if_.then, names, count, cap);
            defer_collect_vars(a->u.if_.else_, names, count, cap);
            break;
        case AST_TRY:
            defer_collect_vars(a->u.try_.try_body, names, count, cap);
            defer_collect_vars(a->u.try_.catch_body, names, count, cap);
            defer_collect_vars(a->u.try_.finally_body, names, count, cap);
            break;
        default:
            break;      // 其余形状：不追（宁可漏报）
    }
}

// 块内从 from 号语句起，是否有**顶层**赋值把 name 改了
static int defer_block_assigns_name(Ast* block, int from, const char* name) {
    if (!block || !name) return 0;
    int n = (int)block->u.block.count;
    for (int i = from; i < n; i++) {
        Ast* s = block->u.block.items[i];
        if (!s) continue;
        // 语句位置的赋值被包在 AST_EXPR_STMT 里（`p = ffi.malloc(32)` 实测如此）
        // —— 只看 s->kind 会一条都匹配不上。
        if (s->kind == AST_EXPR_STMT && s->u.expr_stmt.expr) s = s->u.expr_stmt.expr;
        if (s->kind == AST_ASSIGN) {
            for (int k = 0; k < s->u.assign.name_count; k++) {
                if (s->u.assign.names[k] && strcmp(s->u.assign.names[k], name) == 0) return 1;
            }
            // ⚠ parser 阶段赋值的**主数据是 targets[]**（AST_VAR 节点），
            //   `names[]` 可能没填 —— 只看 names 会全漏（实测：一条都不报）。
            if (s->u.assign.targets) {
                for (int k = 0; k < s->u.assign.name_count; k++) {
                    Ast* t = s->u.assign.targets[k];
                    if (t && t->kind == AST_VAR && t->u.var.name &&
                        strcmp(t->u.var.name, name) == 0) return 1;
                }
            }
        } else if (s->kind == AST_COMPOUND_ASSIGN) {
            if (s->u.compound_assign.name && strcmp(s->u.compound_assign.name, name) == 0) return 1;
        }
    }
    return 0;
}

// 解析代码块 { stmt1; stmt2; ... }
Ast* parse_block_internal(Parser* p) {
    int line = p->lex.current.line;
    consume(p, TOK_LBRACE, "期望 '{'");
    
    Ast* ast = ast_new(AST_BLOCK, line);
    ast_list_init(&ast->u.block);

    // ★ defer 收集（2026-10-22）
    //   语义：**作用域退出时执行**（块级，Swift 风格）——正常结束、return、break、
    //   continue、throw 都会执行，因为降级成 `try { 原块 } finally { 逆序 defer }`。
    //   为什么选块级而不是 Go 的函数级：块级不会在循环里累积（Go 那样容易把内存拖爆）。
    //   为什么用降级而不是新字节码：finally 的语义已经被 try/finally 完整覆盖，零 codegen 改动 ✓
    Ast** defers = (Ast**)malloc(sizeof(Ast*) * 4);
    int* defer_pos = (int*)malloc(sizeof(int) * 4);   // 每个 defer 出现时块内已有的语句数
    int defer_cap = 4;
    int defer_count = 0;

    while (p->lex.current.type != TOK_RBRACE && p->lex.current.type != TOK_EOF) {
        Ast* stmt = parse_statement(p);
        if (stmt && stmt->kind == AST_DEFER) {
            // 不进语句列表，只记下来（块结束时按位置逐层打包进 finally）
            if (defer_count >= defer_cap) {
                defer_cap *= 2;
                defers = (Ast**)realloc(defers, sizeof(Ast*) * defer_cap);
                defer_pos = (int*)realloc(defer_pos, sizeof(int) * defer_cap);
            }
            defer_pos[defer_count] = (int)ast->u.block.count;
            defers[defer_count++] = stmt;
        } else if (stmt) {
            ast_list_add(&ast->u.block, stmt);
        }
        // 错误恢复：同步到块内下一个语句开始位置
        // 跳过混乱的 token 直到找到语句边界或块结束
        if (!stmt && p->lex.current.type != TOK_EOF && p->lex.current.type != TOK_RBRACE) {
            parser_synchronize(p);
            // 在块内，只消费分号，不消费右大括号（它属于当前块）
            if (p->lex.current.type == TOK_SEMI) {
                lexer_next(&p->lex);
            }
        }
    }
    
    // ★ 2026-11-03：**块未闭合**时给可定位的报错。
    //   以前一律 `consume(p, TOK_RBRACE, "期望 '}'")` ⇒ 缺一个 `}` 时，这个块会一直吃到 EOF，
    //   报错落在**文件末尾**（实测：一个函数漏 `}`，结果后面 4 个完全正确的函数全被吞掉，
    //   末尾重复报 4 次"期望 '}'"，用户根本不知道是哪个块的问题）。
    //   现在指出**块的起始行**，并且不硬塞 token（交给上层同步）。
    if (p->lex.current.type != TOK_RBRACE) {
        if (p->lex.current.type == TOK_EOF) {
            error_add_at_fmt(ERR_SYNTAX, ast->line, 1,
                "块没有闭合：第 %d 行开始的 '{' 缺少对应的 '}'（它后面的代码都被当成块内内容了）",
                ast->line);
        } else {
            // 非 EOF：可能是 `}` 前多了东西，仍按"期望 '}'"报，但补上块的起始行便于定位
            error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column,
                "期望 '}'（第 %d 行开始的块没有正确闭合）", ast->line);
        }
    } else {
        lexer_next(&p->lex);       // 正常消费 '}'
    }

    if (defer_count > 0) {
        // ★ 2026-11-11（P2）：先把"defer 里的变量在 defer 之后又被赋值"提示出来
        //   （判据与动机见文件顶部 defer 警告那段的说明）。必须在下面拆解/ast_free 之前。
        for (int i = 0; i < defer_count; i++) {
            const char* dvars[DEFER_WARN_MAX_VARS];
            int dvn = 0;
            defer_collect_vars(defers[i]->u.defer_.expr, dvars, &dvn, DEFER_WARN_MAX_VARS);
            for (int k = 0; k < dvn; k++) {
                if (defer_block_assigns_name(ast, defer_pos[i], dvars[k])) {
                    warning_add_at_fmt(WARN_DEFER_LATE_WRITE, defers[i]->line, 0,
                        "defer 里的 '%s' 在 defer **之后**被重新赋值：defer 的表达式是"
                        "**块退出时**求值（不是注册时）⇒ 这里用的是新值。"
                        "典型后果：`defer ffi.free(p)` 之后 `p = ffi.malloc(...)` 释放的是"
                        "**新缓冲**、旧缓冲泄漏。要锁定注册时的值，先存局部副本："
                        "`var saved = %s` 再 `defer ffi.free(saved)`",
                        dvars[k], dvars[k]);
                }
            }
        }

        // ★ 2026-10-23 修：按位置**逐层嵌套**（原来的"一刀切"有 bug）
        //   旧实现把所有语句分成"第一个 defer 之前/之后"两段、只包一层 finally，
        //   于是第二个 defer 引用"第一个 defer 之后才声明的变量"就会报「未定义变量」
        //   （`defer ffi.free(c)` 里的 c 若写在第一个 defer 之后 ⇒ 看不见）。
        //
        //   现在每个 defer 各包一层，结构如下（defer 按注册顺序 D0..Dk-1）：
        //     {
        //       S[0..p0)
        //       try {                      ← D0 看得到 S[0..p0)
        //         S[p0..p1)
        //         try {                    ← D1 看得到 S[0..p1)
        //           ...
        //           try { S[pk-1..n) } finally { Dk-1 }
        //         } finally { D1 }
        //       } finally { D0 }
        //     }
        //   执行顺序：最内层（最后注册）先跑、最外层后跑 ⇒ **逆序** ✓
        int total = (int)ast->u.block.count;
        Ast* outer = ast_new(AST_BLOCK, line);
        ast_list_init(&outer->u.block);
        Ast* cur = outer;
        int seg = 0;

        for (int i = 0; i < defer_count; i++) {
            int end = defer_pos[i];
            if (end > total) end = total;
            if (end < seg) end = seg;

            // 本段语句（defer i 之前、上一段之后）落到当前层
            for (int s = seg; s < end; s++) {
                ast_list_add(&cur->u.block, ast->u.block.items[s]);
            }

            // 开一层 try，finally = 这个 defer
            Ast* t = ast_new(AST_TRY, line);
            t->u.try_.catch_var = NULL;
            t->u.try_.catch_body = NULL;

            Ast* fin = ast_new(AST_BLOCK, defers[i]->line);
            ast_list_init(&fin->u.block);
            Ast* body = defers[i]->u.defer_.expr;
            if (body && body->kind == AST_BLOCK) {
                // 块形式 `defer { ... }`：整体搬进 finally（保持块内顺序）
                for (int k = 0; k < (int)body->u.block.count; k++) {
                    ast_list_add(&fin->u.block, body->u.block.items[k]);
                }
                body->u.block.count = 0;
                ast_free(body);
            } else if (body) {
                Ast* es = ast_new(AST_EXPR_STMT, defers[i]->line);
                es->u.expr_stmt.expr = body;
                ast_list_add(&fin->u.block, es);
            }
            defers[i]->u.defer_.expr = NULL;   // 防 double free
            t->u.try_.finally_body = fin;

            ast_list_add(&cur->u.block, t);

            // 下一层：本层 try 的 body
            Ast* tb = ast_new(AST_BLOCK, line);
            ast_list_init(&tb->u.block);
            t->u.try_.try_body = tb;
            cur = tb;
            seg = end;
        }

        // 最后一段
        for (int s = seg; s < total; s++) {
            ast_list_add(&cur->u.block, ast->u.block.items[s]);
        }

        ast->u.block.count = 0;     // 元素所有权已转移，别让 ast_free 二次释放
        ast_free(ast);              // 只回收容器（含 items 数组）
        for (int i = 0; i < defer_count; i++) ast_free(defers[i]);
        free(defers);
        free(defer_pos);
        return outer;
    }

    free(defers);
    free(defer_pos);
    return ast;
}

// ============================================================================
// if 语句解析（支持类型守卫和多条件）
// ============================================================================

Ast* parse_if_stmt(Parser* p) {
    int line = p->lex.current.line;
    Lexer if_start_lex = p->lex;   // 保存 if 关键字位置，供 if 表达式形式回退
    // 支持 if 和 eif 关键字
    int was_eif = (p->lex.current.type == TOK_EIF);
    if (p->lex.current.type == TOK_IF || p->lex.current.type == TOK_EIF) {
        lexer_next(&p->lex); // 消费 if/eif
    } else {
        error_add_at_fmt(ERR_SYNTAX, line, p->lex.current.column, "期望 if 或 eif 关键字");
        return NULL;
    }

    // ★ 2026-11-05：`eif { … }`（漏写条件）此前会被当成**字典字面量**去解析 ⇒
    //   首条报「期望 ':'」，完全指不到"少了条件" ✗（实测还会再刷两条级联）。
    //   eif 不可能作为表达式开头（它是 else if 的简写）⇒ 见到块起止符号直接定向报错，
    //   并把这一整块跳掉，避免后续连锁。
    // ★ 2026-11-07：`if { … }`（同样漏写条件）也并进来 —— 此前首条同样是「期望 ':'」，
    //   后跟「if 语句体必须用大括号」+「期望表达式」共 3 条 ✗。if 也不可能是字典字面量开头 ✓
    if (p->lex.current.type == TOK_LBRACE || p->lex.current.type == TOK_RBRACE) {
        if (was_eif) {
            error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column,
                "eif 后面必须跟条件表达式（eif 是 else if 的简写，不能直接接 '{'）—— "
                "例：`if a { … } eif b { … }`");
        } else {
            error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column,
                "if 后面必须跟条件表达式（不能直接接 '{'）—— 例：`if a { … }`；"
                "无条件分支请直接写 `{ … }` 代码块 ✓");
        }
        if (p->lex.current.type == TOK_LBRACE) {
            Ast* skipped = parse_block_internal(p);
            if (skipped) ast_free(skipped);
        }
        return NULL;
    }

    // 检查是否是类型守卫语法：if var is type
    // 注意：让 parse_expression 自己处理括号，不要在外部干预
    // 这样可以正确解析 if (i + 1) % 100 == 0 这样的表达式
    char* guard_var = NULL;
    TypeInfo* guard_type = NULL;
    Ast* cond = NULL;
    TypeGuardList guard_conds;
    int has_guard_conds = 0;  // 标记是否有类型守卫条件列表
    char* guard_bind_var = NULL;     // => 绑定变量名（函数级声明，守卫路径和外部都可访问）
    Ast* guard_bind_expr = NULL;    // => 绑定的表达式

    if (p->lex.current.type == TOK_IDENT) {
        // 保存变量名位置
        char* var_name = copy_string(p->lex.current.text, p->lex.current.len);
        int var_line = p->lex.current.line;

        // 向前看，检查是否是类型守卫语法
        Lexer saved_lex = p->lex;
        lexer_next(&p->lex); // 消费变量名

        // 检查是否是属性访问守卫：obj.field is Type
        char* field_name = NULL;
        if (p->lex.current.type == TOK_DOT) {
            Lexer dot_saved_lex = p->lex;
            lexer_next(&p->lex); // 消费 "."
            if (p->lex.current.type == TOK_IDENT) {
                // 保存字段名，继续看后面是否是 "is"
                field_name = copy_string(p->lex.current.text, p->lex.current.len);
                lexer_next(&p->lex); // 消费字段名
                if (p->lex.current.type != TOK_IS && p->lex.current.type != TOK_NOT) {
                    // 不是类型守卫，恢复到 "." 之前
                    free(field_name);
                    field_name = NULL;
                    p->lex = dot_saved_lex;
                    error_set_column(dot_saved_lex.current.column);
                }
            } else {
                // "." 后面不是标识符，恢复
                p->lex = dot_saved_lex;
                error_set_column(dot_saved_lex.current.column);
            }
        }

        // 检查是否是 "not is"（否定类型守卫）
        int is_not_is = 0;
        if (field_name == NULL) {
            if (p->lex.current.type == TOK_NOT) {
                Lexer not_saved_lex = p->lex;
                lexer_next(&p->lex); // 消费 "not"
                if (p->lex.current.type == TOK_IS) {
                    is_not_is = 1;
                } else {
                    p->lex = not_saved_lex;
                    error_set_column(not_saved_lex.current.column);
                }
            }
        } else {
            if (p->lex.current.type == TOK_NOT) {
                Lexer not_saved_lex = p->lex;
                lexer_next(&p->lex); // 消费 "not"
                if (p->lex.current.type == TOK_IS) {
                    is_not_is = 1;
                } else {
                    p->lex = not_saved_lex;
                    error_set_column(not_saved_lex.current.column);
                }
            }
        }

        // 是类型守卫语法：var is Type 或 var.field is Type
        if ((field_name == NULL && (p->lex.current.type == TOK_IS || is_not_is)) ||
            (field_name != NULL && (p->lex.current.type == TOK_IS || is_not_is))) {
            lexer_next(&p->lex); // 消费 "is"

            // 使用新的类型解析函数解析类型（支持 Array[int], Dict[string, int] 等）
            TypeInfo* type_info = parse_type(p);
            if (!type_info) {
                error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column, "类型守卫期望类型名（如 int, Array[int], Dict[string, int] 等）");
                free(var_name);
                if (field_name) free(field_name);
                return NULL;
            }
            // 模块限定类型名（`if v is a.Point`）：此前 misparse ⇒ 报"if 语句体必须用大括号 {}"
            if (!parser_reject_module_qualified_type(p, type_info)) {
                free(var_name);
                if (field_name) free(field_name);
                return NULL;
            }

            guard_var = strdup(var_name);
            guard_type = type_info;

            // 创建条件表达式
            // 对于 "not is"，创建否定表达式
            if (is_not_is) {
                // 创建一元非表达式：!(type_check)
                Ast* type_check = ast_new(AST_TYPE_CHECK, var_line);
                if (field_name) {
                    // 属性访问守卫：s.age not is int
                    type_check->u.type_check.expr = ast_new(AST_FIELD_ACCESS, var_line);
                    type_check->u.type_check.expr->u.field_access.obj = ast_new(AST_VAR, var_line);
                    type_check->u.type_check.expr->u.field_access.obj->u.var.name = strdup(var_name);
                    type_check->u.type_check.expr->u.field_access.field_name = strdup(field_name);
                    type_check->u.type_check.expr->u.field_access.field_index = -1;
                } else {
                    // 简单变量守卫：x not is int
                    type_check->u.type_check.expr = ast_new(AST_VAR, var_line);
                    type_check->u.type_check.expr->u.var.name = strdup(var_name);
                }
                type_check->u.type_check.type = type_copy(type_info);
                
                cond = ast_new(AST_UNARY, var_line);
                cond->u.unary.op = TOK_NOT;
                cond->u.unary.operand = type_check;
            } else {
                cond = ast_new(AST_TYPE_CHECK, var_line);
                if (field_name) {
                    // 属性访问守卫：s.age is int
                    cond->u.type_check.expr = ast_new(AST_FIELD_ACCESS, var_line);
                    cond->u.type_check.expr->u.field_access.obj = ast_new(AST_VAR, var_line);
                    cond->u.type_check.expr->u.field_access.obj->u.var.name = strdup(var_name);
                    cond->u.type_check.expr->u.field_access.field_name = strdup(field_name);
                    cond->u.type_check.expr->u.field_access.field_index = -1;
                } else {
                    // 简单变量守卫：x is int
                    cond->u.type_check.expr = ast_new(AST_VAR, var_line);
                    cond->u.type_check.expr->u.var.name = strdup(var_name);
                }
                cond->u.type_check.type = type_copy(type_info);
            }

            // 初始化类型守卫条件列表，添加第一个条件
            type_guard_list_init(&guard_conds);
            has_guard_conds = 1;
            TypeGuardCond first_cond;
            memset(&first_cond, 0, sizeof(first_cond));
            first_cond.var_name = strdup(var_name);
            first_cond.field_name = field_name ? strdup(field_name) : NULL;
            first_cond.guard_type = type_copy(type_info);
            type_guard_list_add(&guard_conds, first_cond);

            free(var_name);
            if (field_name) free(field_name);

            // ===== 先检查 => var 绑定语法 =====
            // 语法：if expr is Type => var [and/or ...] { ... }
            // => 绑定必须在 and/or 循环之前处理，因为 => 出现在 and 之前
            if (p->lex.current.type == TOK_FAT_ARROW && cond && cond->kind == AST_TYPE_CHECK) {
                lexer_next(&p->lex); // 消费 "=>"
                if (p->lex.current.type != TOK_IDENT) {
                    error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column, "=> 后面期望标识符作为绑定变量名");
                    return NULL;
                }
                guard_bind_var = (char*)malloc(p->lex.current.len + 1);
                memcpy(guard_bind_var, p->lex.current.text, p->lex.current.len);
                guard_bind_var[p->lex.current.len] = '\0';
                lexer_next(&p->lex); // 消费绑定变量名

                // 提取被检查的表达式（从 AST_TYPE_CHECK 节点）
                guard_bind_expr = cond->u.type_check.expr;
            }

            // 检查是否有 or/and 连接多个类型守卫条件
            // 如果 or/and 后不是类型守卫，则回退并用普通表达式解析余下部分
            int fallback_to_expr = 0;
            LenoTokenType fallback_op = TOK_AND;  // 记住 fallback 时的操作符
            while (p->lex.current.type == TOK_OR || p->lex.current.type == TOK_AND) {
                LenoTokenType op = p->lex.current.type;
                lexer_next(&p->lex); // 消费 or/and
                Lexer saved_after_op = p->lex; // or/and 之后的位置

                // 解析下一个类型守卫条件
                if (p->lex.current.type == TOK_IDENT) {
                    char* next_var_name = copy_string(p->lex.current.text, p->lex.current.len);
                    lexer_next(&p->lex); // 消费变量名

                    // 检查是否是属性访问守卫
                    char* next_field_name = NULL;
                    if (p->lex.current.type == TOK_DOT) {
                        Lexer dot_saved = p->lex;
                        lexer_next(&p->lex); // 消费 "."
                        if (p->lex.current.type == TOK_IDENT) {
                            next_field_name = copy_string(p->lex.current.text, p->lex.current.len);
                            lexer_next(&p->lex); // 消费字段名
                            if (p->lex.current.type != TOK_IS) {
                                free(next_field_name);
                                next_field_name = NULL;
                                p->lex = dot_saved;
                                error_set_column(dot_saved.current.column);
                            }
                        } else {
                            p->lex = dot_saved;
                            error_set_column(dot_saved.current.column);
                        }
                    }

                    if (p->lex.current.type == TOK_IS) {
                        lexer_next(&p->lex); // 消费 "is"

                        // 使用新的类型解析函数
                        TypeInfo* next_type_info = parse_type(p);
                        if (!next_type_info) {
                            error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column, "期望类型名");
                            free(next_var_name);
                            if (next_field_name) free(next_field_name);
                            type_guard_list_free(&guard_conds);
                            return NULL;
                        }
                        // 模块限定类型名：同类型守卫（and/or 链上的后续条件）
                        if (!parser_reject_module_qualified_type(p, next_type_info)) {
                            free(next_var_name);
                            if (next_field_name) free(next_field_name);
                            type_guard_list_free(&guard_conds);
                            return NULL;
                        }

                        // 添加到类型守卫条件列表
                        // 注意：or 条件不安全（不收窄），不添加到 guard_conds
                        TypeGuardCond next_cond;
                        memset(&next_cond, 0, sizeof(next_cond));
                        next_cond.var_name = next_var_name;
                        next_cond.field_name = next_field_name;
                        next_cond.guard_type = next_type_info;
                        int is_or_cond = (op == TOK_OR);
                        if (!is_or_cond) {
                            type_guard_list_add(&guard_conds, next_cond);
                        }

                        // 创建下一个类型守卫的 AST_TYPE_CHECK 节点
                        // （不能用 AST_BOOL(true) 占位，因为 or + true = 恒真）
                        Ast* next_cond_ast = ast_new(AST_TYPE_CHECK, var_line);
                        if (next_field_name) {
                            next_cond_ast->u.type_check.expr = ast_new(AST_FIELD_ACCESS, var_line);
                            next_cond_ast->u.type_check.expr->u.field_access.obj = ast_new(AST_VAR, var_line);
                            next_cond_ast->u.type_check.expr->u.field_access.obj->u.var.name = strdup(next_var_name);
                            next_cond_ast->u.type_check.expr->u.field_access.field_name = strdup(next_field_name);
                            next_cond_ast->u.type_check.expr->u.field_access.field_index = -1;
                        } else {
                            next_cond_ast->u.type_check.expr = ast_new(AST_VAR, var_line);
                            next_cond_ast->u.type_check.expr->u.var.name = strdup(next_var_name);
                        }
                        next_cond_ast->u.type_check.type = type_copy(next_type_info);

                        // or 条件不收窄，释放 next_cond 的独有资源（AST 节点已 strdup/type_copy）
                        if (is_or_cond) {
                            free(next_var_name);
                            if (next_field_name) free(next_field_name);
                            type_free(next_type_info);
                        }

                        // 创建二元操作节点
                        Ast* binop = ast_new(AST_BINOP, var_line);
                        binop->u.binop.l = cond;
                        binop->u.binop.r = next_cond_ast;
                        binop->u.binop.op = (op == TOK_OR) ? TOK_OR : TOK_AND;
                        cond = binop;
                    } else {
                        // or/and 后不是类型守卫，回退到 or/and 之后用普通表达式解析
                        free(next_var_name);
                        if (next_field_name) free(next_field_name);
                        p->lex = saved_after_op;
            error_set_column(saved_after_op.current.column);  // 恢复列号
                        fallback_op = op;
                        fallback_to_expr = 1;
                        break;
                    }
                } else {
                    // or/and 后不是标识符，回退用普通表达式解析
                    p->lex = saved_after_op;
            error_set_column(saved_after_op.current.column);  // 恢复列号
                    fallback_op = op;
                    fallback_to_expr = 1;
                    break;
                }
            }

            // 回退：or/and 后跟的是普通表达式
            // 两种情况：
            //   A. 无 => 绑定：恢复到 saved_lex，用 parse_expression 重新解析整个条件
            //   B. 有 => 绑定：不能恢复（会丢失 =>），改用从 and/or 之后继续解析
            if (fallback_to_expr) {
                if (guard_bind_var != NULL) {
                    // 情况 B：有 => 绑定 + and/or 后跟普通表达式
                    // 不恢复 lexer，从当前位置继续解析余下表达式
                    // 构建 AST_BINOP(cond, op, right)
                    LenoTokenType saved_op = fallback_op;
                    Ast* right = parse_expression(p);
                    if (right) {
                        Ast* binop = ast_new(AST_BINOP, var_line);
                        binop->u.binop.l = cond;
                        binop->u.binop.r = right;
                        binop->u.binop.op = saved_op;
                        cond = binop;
                    }
                    // 清除 guard_var/guard_conds，让 visit_control 的回退提取逻辑
                    // 从 AST_BINOP 中统一提取所有守卫条件（包括 and 右侧的 a[0] is float）
                    // guard_bind_var/guard_bind_expr 保留不受影响
                    type_guard_list_free(&guard_conds);
                    if (guard_var) { free(guard_var); guard_var = NULL; }
                    if (guard_type) { type_free(guard_type); guard_type = NULL; }
                    has_guard_conds = 0;
                } else {
                    // 情况 A：无 => 绑定，完全回退
                    type_guard_list_free(&guard_conds);
                    if (guard_var) { free(guard_var); guard_var = NULL; }
                    if (guard_type) { type_free(guard_type); guard_type = NULL; }
                    has_guard_conds = 0;
                    // 回退会丢弃已建好的守卫条件 AST（AST_TYPE_CHECK + 其 expr 子树）
                    //   ⇒ 必须显式释放，否则每个 `if a is T and <非守卫>` 都漏一棵条件树
                    if (cond) { ast_free(cond); cond = NULL; }
                    p->lex = saved_lex;
            error_set_column(saved_lex.current.column);  // 恢复列号
                    cond = parse_expression(p);
                }
            }
        } else {
            // 不是类型守卫，恢复 lexer 位置并正常解析表达式
            if (field_name) free(field_name);
            p->lex = saved_lex;
            error_set_column(saved_lex.current.column);  // 恢复列号
            free(var_name);
            cond = parse_expression(p);
        }
    } else {
        // 正常解析条件表达式
        // parse_expression 会自己处理括号
        cond = parse_expression(p);
    }

    // 检查是否是 => var 绑定语法：if expr is Type => var { ... }
    // 条件必须是 AST_TYPE_CHECK（expr is Type 形式）
    // 仅当守卫路径未处理 => 时才检查（如走的是 parse_expression 路径）
    if (guard_bind_var == NULL && p->lex.current.type == TOK_FAT_ARROW && cond && cond->kind == AST_TYPE_CHECK) {
        lexer_next(&p->lex); // 消费 "=>"
        if (p->lex.current.type != TOK_IDENT) {
            error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column, "=> 后面期望标识符作为绑定变量名");
            return NULL;
        }
        guard_bind_var = (char*)malloc(p->lex.current.len + 1);
        memcpy(guard_bind_var, p->lex.current.text, p->lex.current.len);
        guard_bind_var[p->lex.current.len] = '\0';
        lexer_next(&p->lex); // 消费变量名

        // 提取被检查的表达式（从 AST_TYPE_CHECK 节点）
        guard_bind_expr = cond->u.type_check.expr;

        // 如果 guard_bind_expr 是简单变量（AST_VAR），且与 guard_var 相同，
        // 则 => var 只是给已有变量取别名，不需要额外存储 expr
        // 但对于复杂表达式（函数调用等），需要保存 expr 供 codegen 生成求值代码
        // 标记此 if 为绑定模式：guard_var 保持为 NULL（没有已存在的变量需要收窄），
        // 但 guard_bind_var/guard_bind_expr 有值
        // 注意：如果原来有 guard_var（var is Type => var 形式），
        // 仍保留 guard_var 用于兼容旧路径，同时设 guard_bind_var
    }

    // ★ C2 护栏（2026-11-02）：`if x => 1 { }` 以前是语法错误，有了箭头函数后会被
    //   解析成"条件 = 函数值"（恒真）⇒ 在这里拦掉，避免静默走另一条路。
    check_arrow_in_cond(cond);

    // if 表达式形式：if cond then expr1 else expr2
    // 出现在语句位置时（如函数体最后一行、独立表达式语句），回退交给
    // parse_if_expr 解析。codegen 的 gen_if 已按 then/else 节点 kind 区分
    // 语句块与表达式，因此表达式形式的 AST_IF 在语句位置同样可编译。
    if (p->lex.current.type == TOK_THEN) {
        // cond 是上面按表达式解析出来的，但 then/else 形式要整段回退重解析 ⇒ 必须释放，
        // 否则每个 `if x then a else b` 都会漏一个条件 AST 节点。
        // guard_bind_var 同理：这一路径不会建 AST，没人接手它的所有权。
        ast_free(cond);
        if (guard_bind_var) { free(guard_bind_var); guard_bind_var = NULL; }
        guard_bind_expr = NULL;
        p->lex = if_start_lex;
        error_set_column(if_start_lex.current.column);
        return parse_if_expr(p);
    }

    // then 分支必须是代码块 {}
    if (p->lex.current.type != TOK_LBRACE) {
        error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column, "if 语句体必须用大括号 {} 包裹");
        return NULL;
    }
    Ast* then_branch = parse_block_internal(p);

    Ast* else_branch = NULL;

    // 支持 else if 和 eif
    if (match(p, TOK_ELSE)) {
        // 检查是否是 else if
        if (p->lex.current.type == TOK_IF) {
            // else if 情况：递归解析 if 语句
            else_branch = parse_if_stmt(p);
        } else if (p->lex.current.type == TOK_EIF) {
            // else eif 不合法，报错
            error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column, "else 后面不能直接跟 eif，请使用 else if 或单独的 eif");
            return NULL;
        } else {
            // else 分支必须是代码块 {}
            if (p->lex.current.type != TOK_LBRACE) {
                error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column, "else 语句体必须用大括号 {} 包裹");
                return NULL;
            }
            else_branch = parse_block_internal(p);
        }
    } else if (p->lex.current.type == TOK_EIF) {
        // eif 情况：递归解析 if 语句
        else_branch = parse_if_stmt(p);
    }

    Ast* ast = ast_new(AST_IF, line);
    ast->u.if_.cond = cond;
    ast->u.if_.then = then_branch;
    ast->u.if_.else_ = else_branch;
    ast->u.if_.guard_var = guard_var;
    ast->u.if_.guard_type = guard_type;
    if (has_guard_conds) {
        ast->u.if_.guard_conds = guard_conds;
    } else {
        type_guard_list_init(&ast->u.if_.guard_conds);
    }
    ast->u.if_.guard_bind_var = guard_bind_var;
    ast->u.if_.guard_bind_index = -1;
    ast->u.if_.guard_bind_expr = guard_bind_expr;
    return ast;
}

// ============================================================================
// while 语句解析（支持多条件组合）
// ============================================================================

Ast* parse_while_stmt(Parser* p) {
    int line = p->lex.current.line;
    lexer_next(&p->lex); // while

    // ★ 2026-11-07：`while { … }`（漏写条件）此前会被当成字典字面量 ⇒
    //   「期望 ':'」+「while 循环体必须用大括号」+「期望表达式」3 条级联 ✗
    if (p->lex.current.type == TOK_LBRACE || p->lex.current.type == TOK_RBRACE) {
        error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column,
            "while 后面必须跟条件表达式（不能直接接 '{'）—— 无限循环写 `while true { … }` ✓");
        if (p->lex.current.type == TOK_LBRACE) {
            Ast* skipped = parse_block_internal(p);
            if (skipped) ast_free(skipped);
        }
        return NULL;
    }
    
    // 让 parse_expression 自己处理括号
    // 这样可以正确解析 while (i + 1) % 100 == 0 这样的表达式
    Ast* cond = parse_expression(p);

    // ★ C2 护栏（2026-11-02）：同 if，`while x => 1 { }` 不能被当成"条件恒真"
    check_arrow_in_cond(cond);

    // while 循环体必须是代码块 {}
    if (p->lex.current.type != TOK_LBRACE) {
        error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column, "while 循环体必须用大括号 {} 包裹");
        return NULL;
    }
    Ast* body = parse_block_internal(p);
    
    Ast* ast = ast_new(AST_WHILE, line);
    ast->u.while_.cond = cond;
    ast->u.while_.body = body;
    return ast;
}

// ============================================================================
// for 语句解析（支持多种语法形式）
// ============================================================================

Ast* parse_for_stmt(Parser* p) {
    int line = p->lex.current.line;
    lexer_next(&p->lex); // for

    // ★ 2026-11-07：`for { … }`（漏写范围/容器）此前被当成字典字面量 ⇒ 3 条级联 ✗
    if (p->lex.current.type == TOK_LBRACE || p->lex.current.type == TOK_RBRACE) {
        error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column,
            "for 后面必须跟范围或容器表达式（不能直接接 '{'）—— "
            "例：`for 5 to i { … }`、`for arr to x { … }` ✓");
        if (p->lex.current.type == TOK_LBRACE) {
            Ast* skipped = parse_block_internal(p);
            if (skipped) ast_free(skipped);
        }
        return NULL;
    }

    // ★ 2026-10-14：C / Java / JS 风格 `for (int i = 0; i < n; i++)` ⇒ **定向报错**
    //   判据用「括号里出现 `;`」而**不是**「见到 `(` 就报」—— 因为 `for (-5):(-1)` 这种
    //   **括号边界**是 Leno 的合法写法（见下面"接受可作范围起点的 token"的注释 ✓）
    //   ⇒ 一见 `(` 就报会误伤 ✗；而 C 风格的三段式必然含 `;` ⇒ 判据既准又不误伤 ✓
    //   报完把 `( … )` 与紧跟的 `{ }` 体整块跳掉 ⇒ **只留这一条错**（否则后面会刷一串级联 ✗）
    if (p->lex.current.type == TOK_LPAREN) {
        Lexer saved = p->lex;
        int depth = 0, saw_semi = 0;
        while (p->lex.current.type != TOK_EOF) {
            if (p->lex.current.type == TOK_LPAREN) {
                depth++;
            } else if (p->lex.current.type == TOK_RPAREN) {
                depth--;
                if (depth == 0) break;
            } else if (p->lex.current.type == TOK_SEMI && depth == 1) {
                saw_semi = 1;
                break;
            }
            lexer_next(&p->lex);
        }
        p->lex = saved;
        if (saw_semi) {
            error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column,
                "Leno 的 for **不是 C 风格**（没有 `for (初始化; 条件; 步进)`）✗ —— "
                "数次数写 `for 5 to i { }`、含端点区间写 `for 2:5 to i { }`、"
                "步进/倒序写 `for 0:5:2 to i { }` / `for 5:0:-1 to i { }`、"
                "遍历容器写 `for arr to x { }`（带索引再加一个变量：`for arr to x, i`）✓");
            int d = 0;
            while (p->lex.current.type != TOK_EOF) {
                if (p->lex.current.type == TOK_LPAREN) {
                    d++;
                } else if (p->lex.current.type == TOK_RPAREN) {
                    d--;
                    if (d == 0) { lexer_next(&p->lex); break; }
                }
                lexer_next(&p->lex);
            }
            if (p->lex.current.type == TOK_LBRACE) {
                Ast* skipped_body = parse_block_internal(p);
                if (skipped_body) ast_free(skipped_body);
            }
            return NULL;
        }
    }

    Ast* start = NULL;
    Ast* end = NULL;
    Ast* step = NULL;
    int inclusive = 0;
    char* var_name = NULL;

    // 检查是否是简单循环次数形式: for 5 { } 或 for expr { }
    // 接受可作范围起点的 token：数字、标识符、负号/正号（负数边界如 for -5:-1）、
    // 左括号（括号边界如 for (-5):(-1)）。解析后通过 : / to / { 区分范围形式与简单次数形式
    if (p->lex.current.type == TOK_NUM || p->lex.current.type == TOK_IDENT ||
        p->lex.current.type == TOK_MINUS || p->lex.current.type == TOK_PLUS ||
        p->lex.current.type == TOK_LPAREN) {
        // 保存当前位置
        Lexer saved_lex = p->lex;
        
        // 先尝试解析表达式作为可能的 start
        start = parse_expression(p);
        
        // 检查解析后遇到的是 : 还是 to 或 {
        if (p->lex.current.type == TOK_COLON) {
            // for start:end 形式
            inclusive = 1;
            lexer_next(&p->lex); // :
            
            // 解析结束值
            end = parse_expression(p);
            
            // 检查是否有步进值
            if (p->lex.current.type == TOK_COLON) {
                lexer_next(&p->lex); // :
                step = parse_expression(p);
            }
        } else if (p->lex.current.type == TOK_LBRACE || p->lex.current.type == TOK_TO) {
            // for 5 { } 或 for 5 to var { } 形式 - 只有结束值，从0开始
            end = start;
            start = NULL; // 表示从0开始
        } else {
            // 其他情况，无法继续解析，报错并恢复
            ast_free(start);
            p->lex = saved_lex;
            error_set_column(saved_lex.current.column);  // 恢复列号
            start = NULL;
            end = parse_expression(p);
        }
    } else if (p->lex.current.type == TOK_COLON) {
        // for :5 { } 形式 - 从0到5（包含）
        inclusive = 1;
        lexer_next(&p->lex); // :
        end = parse_expression(p);
        start = NULL;
    } else {
        // 其他表达式作为结束值
        end = parse_expression(p);
        start = NULL;
    }

    // 解析可选的 to varname
    char* index_var_name = NULL;
    if (p->lex.current.type == TOK_TO) {
        lexer_next(&p->lex); // to
        
        // 可选的 var 关键字
        if (p->lex.current.type == TOK_VAR) {
            lexer_next(&p->lex); // var
        }
        
        // 期望标识符作为变量名
        if (p->lex.current.type == TOK_IDENT) {
            var_name = (char*)malloc(p->lex.current.len + 1);
            if (var_name) {
                memcpy(var_name, p->lex.current.text, p->lex.current.len);
                var_name[p->lex.current.len] = '\0';
            }
            lexer_next(&p->lex);
        } else {
            error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column, "for 循环变量名必须是标识符");
            return NULL;
        }
        
        // 解析可选的索引变量: to item, index
        if (match(p, TOK_COMMA)) {
            // 可选的 var 关键字
            if (p->lex.current.type == TOK_VAR) {
                lexer_next(&p->lex); // var
            }
            
            // 期望标识符作为索引变量名
            if (p->lex.current.type == TOK_IDENT) {
                index_var_name = (char*)malloc(p->lex.current.len + 1);
                if (index_var_name) {
                    memcpy(index_var_name, p->lex.current.text, p->lex.current.len);
                    index_var_name[p->lex.current.len] = '\0';
                }
                lexer_next(&p->lex);
            } else {
                error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column, "for 循环索引变量名必须是标识符");
                return NULL;
            }
        }
    }

    // for 循环体必须是代码块 {}
    if (p->lex.current.type != TOK_LBRACE) {
        // ★ 2026-11-08：`for d as k, v { … }` / `for arr, i { … }`（别的语言的遍历写法）
        //   此前只报「循环体必须用大括号」，完全看不出是**遍历语法**写错 ✗
        if (p->lex.current.type == TOK_COMMA || p->lex.current.type == TOK_AS) {
            error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column,
                "for 的循环变量要用 `to` 引出（不是 `as` / 只用逗号）—— "
                "例：`for 容器 to 值 { … }`、`for 字典 to 键, 值 { … }`、"
                "`for 数组 to 值, 下标 { … }` ✓");
            return NULL;
        }
        error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column, "for 循环体必须用大括号 {} 包裹");
        return NULL;
    }
    Ast* body = parse_block_internal(p);

    Ast* ast = ast_new(AST_FOR, line);
    ast->u.for_.start = start;
    ast->u.for_.end = end;
    ast->u.for_.step = step;
    ast->u.for_.inclusive = inclusive;
    ast->u.for_.var_name = var_name;
    ast->u.for_.index_var_name = index_var_name;
    ast->u.for_.body = body;
    return ast;
}

// 错误恢复：跳到 switch 的 '}' 并消费掉。
// 不这么做，剩下的 case/default 会掉到语句层被重新解析 ⇒ 级联报
// 「default 只能写在 switch 语句内部」/「期望表达式」✗
static void skip_to_switch_close(Parser* p) {
    int depth = 0;
    while (p->lex.current.type != TOK_EOF) {
        if (p->lex.current.type == TOK_LBRACE) {
            depth++;
        } else if (p->lex.current.type == TOK_RBRACE) {
            if (depth == 0) { lexer_next(&p->lex); break; }   // 消费 switch 的 '}'
            depth--;
        }
        lexer_next(&p->lex);
    }
}

// ============================================================================
// switch 语句解析（支持多值 case）
// ============================================================================

Ast* parse_switch_stmt(Parser* p) {
    int line = p->lex.current.line;
    lexer_next(&p->lex); // switch

    // ★ 2026-11-07：`switch { … }`（漏写表达式）此前被当成字典字面量 ⇒
    //   「字典键必须是字符串…」+「switch 语句体必须用大括号」+「期望表达式」3 条级联 ✗
    //   ⚠ 只拦 '{'（不拦 '}'）：`switch` 后换行再接 '}' 仍走 parse_expression，
    //     保留"表达式解析失败返回 NULL 不解引用"的健壮性回归路径 ✓
    if (p->lex.current.type == TOK_LBRACE) {
        error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column,
            "switch 后面必须跟要匹配的表达式（不能直接接 '{'）—— "
            "例：`switch x { case 1 { … } default { … } }` ✓");
        if (p->lex.current.type == TOK_LBRACE) {
            Ast* skipped = parse_block_internal(p);
            if (skipped) ast_free(skipped);
        }
        return NULL;
    }
    
    // 解析 switch 表达式
    Ast* expr = parse_expression(p);
    // ★ 2026-10-30：表达式解析失败会返回 NULL（已报「期望表达式」）⇒ 不能继续解引用。
    if (!expr) return NULL;
    
    // 提取 switch 表达式的变量名（用于 case is Type 的类型收窄）
    char* switch_var_name = NULL;
    if (expr->kind == AST_VAR) {
        switch_var_name = strdup(expr->u.var.name);
    }
    
    // switch 体必须用大括号包裹
    if (p->lex.current.type != TOK_LBRACE) {
        error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column, "switch 语句体必须用大括号 {} 包裹");
        free(switch_var_name);
        return NULL;
    }
    lexer_next(&p->lex); // 消费 '{'
    
    // 分配 case 数组（初始容量 8）
    int case_capacity = 8;
    struct SwitchCase* cases = malloc(sizeof(struct SwitchCase) * case_capacity);
    int case_count = 0;
    Ast* default_body = NULL;
    
    // 解析 case 和 default
    while (p->lex.current.type != TOK_RBRACE && p->lex.current.type != TOK_EOF) {
        if (p->lex.current.type == TOK_CASE) {
            int case_line = p->lex.current.line;
            int case_column = p->lex.current.column;
            lexer_next(&p->lex); // case
            
            // 扩展 case 数组
            if (case_count >= case_capacity) {
                case_capacity *= 2;
                cases = realloc(cases, sizeof(struct SwitchCase) * case_capacity);
            }
            
            // 初始化 case 字段
            ast_list_init(&cases[case_count].values);
            cases[case_count].line = case_line;
            cases[case_count].column = case_column;
            cases[case_count].is_type_match = 0;
            cases[case_count].match_type = NULL;
            cases[case_count].match_types = NULL;
            cases[case_count].match_type_count = 0;
            cases[case_count].guard_var = NULL;
            cases[case_count].guard_bind_var = NULL;
            cases[case_count].guard_bind_index = -1;
            cases[case_count].destructure_vars = NULL;
            cases[case_count].destructure_count = 0;
            cases[case_count].destructure_indices = NULL;
            cases[case_count].destructure_field_names = NULL;
            memset(&cases[case_count].guard_var_ref, 0, sizeof(SymRef));
            
            // 检查是否是 case is Type 模式
            if (p->lex.current.type == TOK_IS) {
                lexer_next(&p->lex); // is
                
                // 解析类型（支持逗号合并：case is Type1, Type2, Type3）
                int mt_cap = 4;
                int mt_count = 0;
                TypeInfo** mt_arr = malloc(sizeof(TypeInfo*) * mt_cap);

                TypeInfo* type_info = parse_type(p);
                if (!type_info) {
                    error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column, "case is 后期望类型名");
                    free(mt_arr);
                    free(switch_var_name);
                    free(cases);
                    return NULL;
                }
                // 模块限定类型名：同类型守卫（case is a.Point）
                if (!parser_reject_module_qualified_type(p, type_info)) {
                    free(mt_arr);
                    free(switch_var_name);
                    free(cases);
                    return NULL;
                }
                mt_arr[mt_count++] = type_info;

                // 逗号合并：解析后续类型
                while (p->lex.current.type == TOK_COMMA) {
                    lexer_next(&p->lex); // ,
                    TypeInfo* extra_type = parse_type(p);
                    if (!extra_type) {
                        error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column, "case is 逗号后期望类型名");
                        for (int t = 0; t < mt_count; t++) type_free(mt_arr[t]);
                        free(mt_arr);
                        free(switch_var_name);
                        free(cases);
                        return NULL;
                    }
                    if (mt_count >= mt_cap) {
                        mt_cap *= 2;
                        mt_arr = realloc(mt_arr, sizeof(TypeInfo*) * mt_cap);
                    }
                    mt_arr[mt_count++] = extra_type;
                }

                cases[case_count].is_type_match = 1;
                cases[case_count].match_types = mt_arr;
                cases[case_count].match_type_count = mt_count;
                cases[case_count].match_type = mt_arr[0];  // 首类型，用于类型窄化和解构
                if (switch_var_name) {
                    cases[case_count].guard_var = strdup(switch_var_name);
                }

                // 解构变量：case is Point(x, y)（仅单类型时允许）
                if (p->lex.current.type == TOK_LPAREN) {
                    if (mt_count > 1) {
                        error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column,
                            "case is 逗号合并不支持解构（case is A, B 不允许 (x, y)）");
                    } else {
                        lexer_next(&p->lex); // (
                        int var_cap = 4;
                        int var_count = 0;
                        char** var_names = malloc(sizeof(char*) * var_cap);

                        while (p->lex.current.type != TOK_RPAREN && p->lex.current.type != TOK_EOF) {
                            if (p->lex.current.type != TOK_IDENT) {
                                error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column, "解构变量名期望标识符");
                                free(var_names);
                                free(switch_var_name);
                                free(cases);
                                return NULL;
                            }
                            if (var_count >= var_cap) {
                                var_cap *= 2;
                                var_names = realloc(var_names, sizeof(char*) * var_cap);
                            }
                            {
                                char* name = malloc(p->lex.current.len + 1);
                                memcpy(name, p->lex.current.text, p->lex.current.len);
                                name[p->lex.current.len] = '\0';
                                var_names[var_count++] = name;
                            }
                            lexer_next(&p->lex);
                            if (p->lex.current.type == TOK_COMMA) {
                                lexer_next(&p->lex); // ,
                            }
                        }
                        consume(p, TOK_RPAREN, "期望 ')'");
                        cases[case_count].destructure_vars = var_names;
                        cases[case_count].destructure_count = var_count;
                    }
                }

                // ===== 检查 => 绑定语法：case is Type => var =====
                // 语法：case is Type => var { ... use(var) ... }
                // var 在 case 体内即为 Type 类型
                if (p->lex.current.type == TOK_FAT_ARROW) {
                    // 逗号合并不允许 => 绑定
                    if (mt_count > 1) {
                        error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column,
                            "case is 逗号合并不支持 => 绑定（case is A, B 不允许 => var）");
                    } else {
                        lexer_next(&p->lex); // 消费 "=>"
                        if (p->lex.current.type != TOK_IDENT) {
                            error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column,
                                "=> 后面期望标识符作为绑定变量名");
                            skip_to_switch_close(p);
                            free(switch_var_name);
                            free(cases);
                            return NULL;
                        }
                        char* bvar = malloc(p->lex.current.len + 1);
                        memcpy(bvar, p->lex.current.text, p->lex.current.len);
                        bvar[p->lex.current.len] = '\0';
                        cases[case_count].guard_bind_var = bvar;
                        lexer_next(&p->lex); // 消费绑定变量名

                        // switch case 不支持 and 守卫（switch 的 codegen 结构无法 fallthrough）
                        // 如需"类型匹配 + 额外条件"，请用 if-else if 链
                        if (p->lex.current.type == TOK_AND) {
                            error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column,
                                "switch case 不支持 and 守卫（case is Type => var and ...），请改用 if-else if 链");
                            skip_to_switch_close(p);
                            free(switch_var_name);
                            free(cases);
                            return NULL;
                        }
                    }
                }
            } else {
                // 普通值匹配：解析 case 值（支持多个值，用逗号分隔）
                do {
                    Ast* value = parse_expression(p);
                    // ★ 2026-11-03：**区间 case 用 `:`** —— 与切片 `arr[2:8]`、
                    //   范围表达式 `x in 0:9` 同符号、同口径（**闭区间**，两端都含）。
                    //   `:` 才是本语言既有的区间符号；给"区间"再造一个词（`to`）等于
                    //   两套等价写法，与"单一事实来源"的纪律冲突。
                    if (value && p->lex.current.type == TOK_COLON) {
                        // ★ 2026-11-08：`case 1: { … }`（C / Java / JS 风格的"冒号 + 块"）
                        //   此前把 ':' 当成**区间分隔符**，再去解析 `{ … }` 当区间终点 ⇒
                        //   先报「字典键后面必须跟 ':'」，再报「case 语句体必须用大括号」✗
                        //   判据：':' 后面紧跟 '{' ⇒ 那显然是想写块体，不是区间 ✓
                        Lexer look = p->lex;
                        lexer_next(&p->lex);
                        int colon_starts_body = (p->lex.current.type == TOK_LBRACE);
                        p->lex = look;
                        error_set_column(look.current.column);
                        if (colon_starts_body) {
                            error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column,
                                "case 的语句体不要写冒号 —— 直接 `case 1 { … }`；"
                                "`:` 在 case 里表示**区间**（如 `case 1:3`）");
                            lexer_next(&p->lex);   // 吃掉多余的那个 ':'
                        } else {
                            int range_line = value->line;
                            lexer_next(&p->lex);                 // ':'
                            Ast* end = parse_expression(p);
                            Ast* range = ast_new(AST_RANGE, range_line);
                            range->column = value->column;
                            range->u.range.start = value;
                            range->u.range.end = end;
                            range->u.range.inclusive = 1;        // 闭区间，与切片同口径
                            value = range;
                        }
                    } else if (value && p->lex.current.type == TOK_TO) {
                        // 更早的临时写法 `case 1 to 3`：报错并**照旧构造区间**，
                        // 这样只报一条、不会把后面的 `{` 也带崩。
                        int range_line = value->line;
                        error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column,
                            "case 的区间写法是 `case 1:3`（与切片 arr[1:3] 同口径，闭区间）—— "
                            "`case 1 to 3` 已统一成 `:`，把中间那个 `to` 换成 `:` 即可");
                        lexer_next(&p->lex);                 // 'to'
                        Ast* end = parse_expression(p);
                        Ast* range = ast_new(AST_RANGE, range_line);
                        range->column = value->column;
                        range->u.range.start = value;
                        range->u.range.end = end;
                        range->u.range.inclusive = 1;
                        value = range;
                    }
                    if (value) {
                        // ★ C2 护栏（2026-11-02）：`case x => …` 与条件位置同理，
                        //   以前是语法错误，现在会变成"case 值是函数"（永不命中）⇒ 拦掉
                        check_arrow_in_cond(value);
                        ast_list_add(&cases[case_count].values, value);
                    }
                } while (match(p, TOK_COMMA));
            }
            
            // case 体必须用大括号包裹
            if (p->lex.current.type != TOK_LBRACE) {
                error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column, "case 语句体必须用大括号 {} 包裹");
                // ★ 2026-10-24：报错后要**跳到 switch 的 `}` 并消费掉**再返回。
                //   原来直接 return NULL ⇒ 剩下的 case 体（`print("one")`）和
                //   `default:` 会被当成表达式继续解析 ⇒ 级联报「期望表达式」，
                //   把真错淹没（实测 case 1: print(...) 会额外多报 2 条）。
                skip_to_switch_close(p);
                free(switch_var_name);
                free(cases);
                return NULL;
            }
            cases[case_count].body = parse_block_internal(p);
            case_count++;
        } else if (p->lex.current.type == TOK_DEFAULT) {
            int default_line = p->lex.current.line;
            int default_col = p->lex.current.column;
            lexer_next(&p->lex); // default
            
            // default 体必须用大括号包裹
            if (p->lex.current.type == TOK_COLON) {
                // ★ 2026-11-08：`default: { … }`（C / Java / JS 风格）此前只报
                //   「default 语句体必须用大括号」，看不出是多了个冒号 ✗
                error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column,
                    "default 的语句体不要写冒号 —— 直接 `default { … }`");
                lexer_next(&p->lex);   // 吃掉多余的 ':'
            }
            if (p->lex.current.type != TOK_LBRACE) {
                error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column, "default 语句体必须用大括号 {} 包裹");
                free(switch_var_name);
                free(cases);
                return NULL;
            }
            // ★ 2026-11-06：多个 default 此前**零诊断** ✗（只有第一个会生效，后面的永远死代码）
            if (default_body) {
                error_add_at_fmt(ERR_SEMANTIC, default_line, default_col,
                    "switch 里出现了多个 default —— 只有第一个会生效，后面的分支永远不会执行；请删掉多余的");
                Ast* dup = parse_block_internal(p);
                if (dup) ast_free(dup);
                continue;
            }
            default_body = parse_block_internal(p);
        } else {
            error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column, "switch 语句中期望 case 或 default");
            free(switch_var_name);
            free(cases);
            return NULL;
        }
    }
    
    consume(p, TOK_RBRACE, "期望 '}'");
    
    // 创建 AST 节点
    Ast* ast = ast_new(AST_SWITCH, line);
    ast->u.switch_.expr = expr;
    ast->u.switch_.cases = cases;
    ast->u.switch_.case_count = case_count;
    ast->u.switch_.default_body = default_body;
    
    free(switch_var_name);
    return ast;
}
