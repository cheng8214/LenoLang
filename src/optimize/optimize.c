#include "include/lenolang.h"
#include "include/leno_optimize.h"
#include <math.h>
#include <limits.h>

static int is_const_num(Ast* ast) {
    return ast && ast->kind == AST_NUM && !ast->u.num.is_bigint;
}

static int is_const_int(Ast* ast) {
    return is_const_num(ast) && !ast->u.num.is_float;
}

static int is_const_float(Ast* ast) {
    return is_const_num(ast) && ast->u.num.is_float;
}

static int is_const_bool(Ast* ast) {
    return ast && ast->kind == AST_BOOL;
}

static int is_const_string(Ast* ast) {
    return ast && ast->kind == AST_STRING;
}

static void replace_with_num(Ast* ast, double value, int is_float) {
    if (ast->cached_type) {
        type_free(ast->cached_type);
    }
    ast->cached_type = is_float ? type_new(TYPE_FLOAT) : type_new(TYPE_INT);
    ast->kind = AST_NUM;
    ast->u.num.value = value;
    ast->u.num.is_bigint = 0;
    ast->u.num.bigint_str = NULL;
    ast->u.num.is_float = is_float;
}

static void replace_with_bool(Ast* ast, int value) {
    if (ast->cached_type) {
        type_free(ast->cached_type);
    }
    ast->cached_type = type_new(TYPE_BOOL);
    ast->kind = AST_BOOL;
    ast->u.boolean = value;
}

static void replace_with_string(Ast* ast, char* value, int len) {
    if (ast->cached_type) {
        type_free(ast->cached_type);
    }
    ast->cached_type = type_new(TYPE_STRING);
    ast->kind = AST_STRING;
    ast->u.string.value = value;
    ast->u.string.len = len;
}

static void fold_binary(Ast* ast) {
    Ast* l = ast->u.binop.l;
    Ast* r = ast->u.binop.r;
    LenoTokenType op = ast->u.binop.op;

    if (op == TOK_AND || op == TOK_OR) return;

    if (is_const_num(l) && is_const_num(r)) {
        int l_float = l->u.num.is_float;
        int r_float = r->u.num.is_float;
        int result_is_float = l_float || r_float;
        double lv = l->u.num.value;
        double rv = r->u.num.value;
        double result = 0;

        switch (op) {
            case TOK_PLUS:  result = lv + rv; break;
            case TOK_MINUS: result = lv - rv; break;
            case TOK_STAR:  result = lv * rv; break;
            case TOK_SLASH:
                if (rv == 0.0) return;
                if (!result_is_float) {
                    result = (double)((long long)lv / (long long)rv);
                } else {
                    result = lv / rv;
                }
                break;
            case TOK_MOD:
                if (rv == 0.0) return;
                if (!result_is_float) {
                    long long li = (long long)lv;
                    long long ri = (long long)rv;
                    result = (double)(li % ri);
                } else {
                    result = fmod(lv, rv);
                }
                break;
            case TOK_BITAND:
                if (result_is_float) return;
                result = (double)((long long)lv & (long long)rv);
                break;
            case TOK_BITOR:
                if (result_is_float) return;
                result = (double)((long long)lv | (long long)rv);
                break;
            case TOK_BITXOR:
                if (result_is_float) return;
                result = (double)((long long)lv ^ (long long)rv);
                break;
            case TOK_SHL:
                if (result_is_float) return;
                if ((int)rv >= 32) return; // 移位量>=32时不折叠，交给VM的BigInt路径处理
                result = (double)((long long)lv << (int)rv);
                break;
            case TOK_SHR:
                if (result_is_float) return;
                if ((int)rv >= 32) return; // 移位量>=32时不折叠，交给VM的BigInt路径处理
                result = (double)((long long)lv >> (int)rv);
                break;
            case TOK_EQEQ:
                ast_free(l);
                ast_free(r);
                replace_with_bool(ast, lv == rv);
                return;
            case TOK_NEQ:
                ast_free(l);
                ast_free(r);
                replace_with_bool(ast, lv != rv);
                return;
            case TOK_LT:
                ast_free(l);
                ast_free(r);
                replace_with_bool(ast, lv < rv);
                return;
            case TOK_GT:
                ast_free(l);
                ast_free(r);
                replace_with_bool(ast, lv > rv);
                return;
            case TOK_LE:
                ast_free(l);
                ast_free(r);
                replace_with_bool(ast, lv <= rv);
                return;
            case TOK_GE:
                ast_free(l);
                ast_free(r);
                replace_with_bool(ast, lv >= rv);
                return;
            default:
                return;
        }

        ast_free(l);
        ast_free(r);
        replace_with_num(ast, result, result_is_float);
        return;
    }

    if (is_const_bool(l) && is_const_bool(r)) {
        int lv = l->u.boolean;
        int rv = r->u.boolean;

        switch (op) {
            case TOK_EQEQ:
                ast_free(l);
                ast_free(r);
                replace_with_bool(ast, lv == rv);
                return;
            case TOK_NEQ:
                ast_free(l);
                ast_free(r);
                replace_with_bool(ast, lv != rv);
                return;
            default:
                break;
        }
        return;
    }

    if (op == TOK_PLUS && is_const_string(l) && is_const_string(r)) {
        int new_len = l->u.string.len + r->u.string.len;
        char* new_str = (char*)malloc(new_len + 1);
        memcpy(new_str, l->u.string.value, l->u.string.len);
        memcpy(new_str + l->u.string.len, r->u.string.value, r->u.string.len);
        new_str[new_len] = '\0';
        ast_free(l);
        ast_free(r);
        replace_with_string(ast, new_str, new_len);
        return;
    }

    if (is_const_num(l) && is_const_bool(r)) {
        double lv = l->u.num.value;
        int rv = r->u.boolean;
        switch (op) {
            case TOK_EQEQ:
                ast_free(l);
                ast_free(r);
                replace_with_bool(ast, lv == (double)rv);
                return;
            case TOK_NEQ:
                ast_free(l);
                ast_free(r);
                replace_with_bool(ast, lv != (double)rv);
                return;
            default:
                break;
        }
    }

    if (is_const_bool(l) && is_const_num(r)) {
        int lv = l->u.boolean;
        double rv = r->u.num.value;
        switch (op) {
            case TOK_EQEQ:
                ast_free(l);
                ast_free(r);
                replace_with_bool(ast, (double)lv == rv);
                return;
            case TOK_NEQ:
                ast_free(l);
                ast_free(r);
                replace_with_bool(ast, (double)lv != rv);
                return;
            default:
                break;
        }
    }
}

static void fold_unary(Ast* ast) {
    Ast* operand = ast->u.unary.operand;
    LenoTokenType op = ast->u.unary.op;

    if (is_const_num(operand)) {
        double val = operand->u.num.value;
        int is_float = operand->u.num.is_float;
        double result = 0;

        switch (op) {
            case TOK_MINUS:
                result = -val;
                break;
            case TOK_BITNOT:
                if (is_float) return;
                result = (double)(~(int)val);
                break;
            default:
                return;
        }

        ast_free(operand);
        replace_with_num(ast, result, is_float);
        return;
    }

    if (is_const_bool(operand) && op == TOK_NOT) {
        int val = operand->u.boolean;
        ast_free(operand);
        replace_with_bool(ast, !val);
        return;
    }
}

static void fold_expr(Ast* ast);

static void fold_block(AstList* list) {
    for (int i = 0; i < list->count; i++) {
        fold_expr(list->items[i]);
    }
}

static void fold_expr(Ast* ast) {
    if (!ast) return;

    switch (ast->kind) {
        case AST_NUM:
        case AST_STRING:
        case AST_BOOL:
        case AST_NULL:
            break;

        case AST_BINOP:
            fold_expr(ast->u.binop.l);
            fold_expr(ast->u.binop.r);
            fold_binary(ast);
            break;

        case AST_UNARY:
            fold_expr(ast->u.unary.operand);
            fold_unary(ast);
            break;

        case AST_ARRAY:
            fold_block(&ast->u.array);
            break;

        case AST_DICT:
            for (int i = 0; i < ast->u.dict.count; i++) {
                fold_expr(ast->u.dict.entries[i].key);
                fold_expr(ast->u.dict.entries[i].value);
            }
            break;

        case AST_RANGE:
            fold_expr(ast->u.range.start);
            fold_expr(ast->u.range.end);
            break;

        case AST_VAR:
            break;

        case AST_CALL:
            fold_expr(ast->u.call.callee);
            fold_block(&ast->u.call.args);
            break;

        case AST_INDEX:
            fold_expr(ast->u.index.obj);
            fold_expr(ast->u.index.index);
            break;

        case AST_SLICE:
            fold_expr(ast->u.slice.obj);
            fold_expr(ast->u.slice.start);
            fold_expr(ast->u.slice.end);
            break;

        case AST_INDEX_ASSIGN:
            fold_expr(ast->u.index_assign.obj);
            fold_expr(ast->u.index_assign.index);
            fold_expr(ast->u.index_assign.value);
            break;

        case AST_BLOCK:
            fold_block(&ast->u.block);
            break;

        case AST_IF:
            fold_expr(ast->u.if_.cond);
            fold_expr(ast->u.if_.then);
            fold_expr(ast->u.if_.else_);
            break;

        case AST_WHILE:
            fold_expr(ast->u.while_.cond);
            fold_expr(ast->u.while_.body);
            break;

        case AST_FOR:
            fold_expr(ast->u.for_.start);
            fold_expr(ast->u.for_.end);
            fold_expr(ast->u.for_.step);
            fold_expr(ast->u.for_.body);
            break;

        case AST_SWITCH:
            fold_expr(ast->u.switch_.expr);
            for (int i = 0; i < ast->u.switch_.case_count; i++) {
                fold_block(&ast->u.switch_.cases[i].values);
                fold_expr(ast->u.switch_.cases[i].body);
            }
            fold_expr(ast->u.switch_.default_body);
            break;

        case AST_FUNC_DEF:
            fold_expr(ast->u.func.body);
            break;

        case AST_RETURN:
            fold_expr(ast->u.ret);
            break;

        case AST_ASSIGN:
            fold_expr(ast->u.assign.value);
            break;

        case AST_COMPOUND_ASSIGN:
            fold_expr(ast->u.compound_assign.value);
            break;

        case AST_VAR_DECL:
            fold_expr(ast->u.var_decl.init);
            break;

        case AST_EXPR_STMT:
            fold_expr(ast->u.expr_stmt.expr);
            break;

        case AST_EXPORT:
            fold_expr(ast->u.export.decl);
            break;

        case AST_MODULE_CALL:
            fold_block(&ast->u.module_call.args);
            break;

        case AST_INTERP_STRING:
            for (int i = 0; i < ast->u.interp_string.count - 1; i++) {
                fold_expr(ast->u.interp_string.exprs[i]);
            }
            break;

        case AST_TRY:
            fold_expr(ast->u.try_.try_body);
            fold_expr(ast->u.try_.catch_body);
            fold_expr(ast->u.try_.finally_body);
            break;

        case AST_THROW:
            fold_expr(ast->u.throw_.expr);
            break;

        case AST_TYPE_CHECK:
        case AST_AS_CAST:
            fold_expr(ast->u.type_check.expr);
            break;

        case AST_STRUCT_DEF:
            for (int i = 0; i < ast->u.struct_def.field_count; i++) {
                fold_expr(ast->u.struct_def.field_defaults[i]);
            }
            for (int i = 0; i < ast->u.struct_def.method_count; i++) {
                fold_expr(ast->u.struct_def.methods[i]);
            }
            break;
        case AST_FACE_DEF:
        case AST_ALIAS:
            break;

        case AST_STRUCT_INIT:
            for (int i = 0; i < ast->u.struct_init.field_count; i++) {
                fold_expr(ast->u.struct_init.field_values[i]);
            }
            break;

        case AST_FIELD_ACCESS:
            fold_expr(ast->u.field_access.obj);
            break;

        case AST_AWAIT:
            fold_expr(ast->u.await.expr);
            break;

        case AST_IMPORT:
        case AST_USE:
        case AST_MODULE_ACCESS:
        case AST_BREAK:
        case AST_CONTINUE:
        case AST_ENUM_DEF:
        case AST_CLIB_DEF:
        case AST_CFUNC_DECL:
        case AST_CSTRUCT_DEF:
            break;
    }
}

// ============================================================================
// 死代码消除（Dead Code Elimination, DCE）
//
// 在常量折叠之后执行，利用已折叠的常量条件进行更激进的消除。
//
// 优化规则：
//   1. 常量条件消除：
//      - if true  { A } else { B } → A（消除条件和 else 分支）
//      - if false { A } else { B } → B（消除条件和 then 分支）
//      - while false { A }        → null（消除整个循环）
//
//   2. 终止语句后死代码消除：
//      - return / break / continue / throw 之后的语句全部消除
//      - 例如：return 42; print("dead") → return 42;
//
//   3. 节点原地替换技术：
//      - 将 if 节点替换为其存活分支时，使用 memcpy 原地替换
//      - 避免修改父节点的指针，保持 AST 树结构完整
// ============================================================================

// 判断语句是否是终止语句
// 终止语句执行后，控制流不会继续到下一条语句，因此后续代码都是死代码
static int is_terminator(Ast* ast) {
    if (!ast) return 0;
    switch (ast->kind) {
        case AST_RETURN:     // return：函数返回，后续代码不可达
        case AST_BREAK:      // break：跳出循环，后续代码不可达
        case AST_CONTINUE:   // continue：跳到循环下一轮，后续代码不可达
        case AST_THROW:      // throw：抛出异常，后续代码不可达
            return 1;
        default:
            return 0;
    }
}

// 判断块是否以终止语句结尾（用于向上传播终止信息）
static int block_ends_with_terminator(AstList* list) {
    if (list->count == 0) return 0;
    return is_terminator(list->items[list->count - 1]);
}

// 消除块中终止语句之后的死代码
// 例如：{ return 1; x = 2; print(x); } → { return 1; }
static void dce_block(AstList* list) {
    for (int i = 0; i < list->count; i++) {
        if (is_terminator(list->items[i]) && i + 1 < list->count) {
            // 找到终止语句，释放之后的所有死代码
            for (int j = i + 1; j < list->count; j++) {
                ast_free(list->items[j]);
            }
            list->count = i + 1;
            break;
        }
    }
}

// 判断 AST 是否为编译期常量 true
// 包括：true 字面量、非零整数（常量折叠后可能产生）
static int is_always_true(Ast* ast) {
    if (!ast) return 0;
    if (ast->kind == AST_BOOL && ast->u.boolean) return 1;
    if (is_const_int(ast) && (int)ast->u.num.value != 0) return 1;
    return 0;
}

// 判断 AST 是否为编译期常量 false
// 包括：false 字面量、null、0、0.0（均为假值）
static int is_always_false(Ast* ast) {
    if (!ast) return 0;
    if (ast->kind == AST_BOOL && !ast->u.boolean) return 1;
    if (ast->kind == AST_NULL) return 1;
    if (is_const_int(ast) && (int)ast->u.num.value == 0) return 1;
    if (is_const_float(ast) && ast->u.num.value == 0.0) return 1;
    return 0;
}

// 递归消除死代码
// 返回值：当前节点是否是终止语句（用于向上传播终止信息）
static int dce_expr(Ast* ast) {
    if (!ast) return 0;

    switch (ast->kind) {
        // 叶子节点：无子引用，不是终止语句
        case AST_NUM:
        case AST_STRING:
        case AST_BOOL:
        case AST_NULL:
        case AST_VAR:
        case AST_BREAK:
        case AST_CONTINUE:
            return 0;

        // return：递归处理返回值表达式，本身是终止语句
        case AST_RETURN:
            if (ast->u.ret) dce_expr(ast->u.ret);
            return 1;

        // throw：递归处理异常表达式，本身是终止语句
        case AST_THROW:
            if (ast->u.throw_.expr) dce_expr(ast->u.throw_.expr);
            return 1;

        // 块：递归处理所有子语句，然后消除终止语句后的死代码
        case AST_BLOCK: {
            AstList* list = &ast->u.block;
            for (int i = 0; i < list->count; i++) {
                dce_expr(list->items[i]);
            }
            dce_block(list);
            return block_ends_with_terminator(list);
        }

        // if 语句：核心优化目标
        case AST_IF: {
            // 先递归处理条件和分支（可能产生更多常量条件）
            dce_expr(ast->u.if_.cond);
            dce_expr(ast->u.if_.then);
            dce_expr(ast->u.if_.else_);

            // 优化1：if true { then } else { else_ } → then
            // 消除条件判断和 else 分支，保留 then 分支
            if (is_always_true(ast->u.if_.cond)) {
                Ast* then_branch = ast->u.if_.then;
                // 先将指针置空，防止 ast_free 释放子节点
                ast->u.if_.then = NULL;
                ast->u.if_.cond = NULL;
                ast->u.if_.else_ = NULL;
                int is_term = is_terminator(then_branch);
                // 释放缓存的类型信息
                if (ast->cached_type) { type_free(ast->cached_type); ast->cached_type = NULL; }
                // 原地替换：将 then 分支的内容复制到当前 if 节点
                // 这样不需要修改父节点的指针
                memcpy(ast, then_branch, sizeof(Ast));
                free(then_branch);
                return is_term;
            }

            // 优化2：if false { then } else { else_ } → else_ 或 null
            // 消除条件判断和 then 分支，保留 else 分支
            if (is_always_false(ast->u.if_.cond)) {
                Ast* else_branch = ast->u.if_.else_;
                ast->u.if_.then = NULL;
                ast->u.if_.cond = NULL;
                ast->u.if_.else_ = NULL;
                if (else_branch) {
                    // 有 else 分支：用 else 替换整个 if
                    int is_term = is_terminator(else_branch);
                    if (ast->cached_type) { type_free(ast->cached_type); ast->cached_type = NULL; }
                    memcpy(ast, else_branch, sizeof(Ast));
                    free(else_branch);
                    return is_term;
                } else {
                    // 无 else 分支：替换为 null 节点（表达式语句中的空操作）
                    ast->kind = AST_NULL;
                    if (ast->cached_type) { type_free(ast->cached_type); ast->cached_type = NULL; }
                    return 0;
                }
            }

            // 条件非常量：保留 if 语句，不做消除
            return 0;
        }

        // while 循环：消除不可达循环
        case AST_WHILE: {
            dce_expr(ast->u.while_.cond);
            dce_expr(ast->u.while_.body);

            // while false { body } → null（循环体永远不会执行）
            if (is_always_false(ast->u.while_.cond)) {
                ast_free(ast->u.while_.cond);
                ast_free(ast->u.while_.body);
                if (ast->cached_type) { type_free(ast->cached_type); ast->cached_type = NULL; }
                ast->kind = AST_NULL;
                return 0;
            }

            // while true { body }：保留（可能包含 break/return）
            return 0;
        }

        // for 循环：递归处理子节点
        case AST_FOR: {
            if (ast->u.for_.start) dce_expr(ast->u.for_.start);
            dce_expr(ast->u.for_.end);
            if (ast->u.for_.step) dce_expr(ast->u.for_.step);
            dce_expr(ast->u.for_.body);
            return 0;
        }

        // switch：递归处理表达式和各 case 分支
        case AST_SWITCH: {
            dce_expr(ast->u.switch_.expr);
            for (int i = 0; i < ast->u.switch_.case_count; i++) {
                for (int j = 0; j < ast->u.switch_.cases[i].values.count; j++) {
                    dce_expr(ast->u.switch_.cases[i].values.items[j]);
                }
                dce_expr(ast->u.switch_.cases[i].body);
            }
            dce_expr(ast->u.switch_.default_body);
            return 0;
        }

        // 函数定义：递归处理函数体
        case AST_FUNC_DEF: {
            dce_expr(ast->u.func.body);
            return 0;
        }

        // 二元运算：递归处理左右操作数
        case AST_BINOP:
            dce_expr(ast->u.binop.l);
            dce_expr(ast->u.binop.r);
            return 0;

        // 一元运算：递归处理操作数
        case AST_UNARY:
            dce_expr(ast->u.unary.operand);
            return 0;

        // 数组字面量：递归处理所有元素
        case AST_ARRAY:
            for (int i = 0; i < ast->u.array.count; i++) {
                dce_expr(ast->u.array.items[i]);
            }
            return 0;

        // 字典字面量：递归处理所有键和值
        case AST_DICT:
            for (int i = 0; i < ast->u.dict.count; i++) {
                dce_expr(ast->u.dict.entries[i].key);
                dce_expr(ast->u.dict.entries[i].value);
            }
            return 0;

        // 范围表达式：递归处理起止值
        case AST_RANGE:
            dce_expr(ast->u.range.start);
            dce_expr(ast->u.range.end);
            return 0;

        // 函数调用：递归处理被调用者和参数
        case AST_CALL:
            dce_expr(ast->u.call.callee);
            for (int i = 0; i < ast->u.call.args.count; i++) {
                dce_expr(ast->u.call.args.items[i]);
            }
            return 0;

        // 索引访问：递归处理对象和索引
        case AST_INDEX:
            dce_expr(ast->u.index.obj);
            dce_expr(ast->u.index.index);
            return 0;

        // 切片：递归处理对象和起止索引
        case AST_SLICE:
            dce_expr(ast->u.slice.obj);
            if (ast->u.slice.start) dce_expr(ast->u.slice.start);
            if (ast->u.slice.end) dce_expr(ast->u.slice.end);
            return 0;

        // 索引赋值：递归处理对象、索引和值
        case AST_INDEX_ASSIGN:
            dce_expr(ast->u.index_assign.obj);
            dce_expr(ast->u.index_assign.index);
            dce_expr(ast->u.index_assign.value);
            return 0;

        // 变量赋值：递归处理赋值表达式
        case AST_ASSIGN:
            dce_expr(ast->u.assign.value);
            return 0;

        // 复合赋值：递归处理赋值表达式
        case AST_COMPOUND_ASSIGN:
            dce_expr(ast->u.compound_assign.value);
            return 0;

        // 变量声明：递归处理初始化表达式
        case AST_VAR_DECL:
            if (ast->u.var_decl.init) dce_expr(ast->u.var_decl.init);
            return 0;

        // 表达式语句：递归处理表达式
        case AST_EXPR_STMT:
            dce_expr(ast->u.expr_stmt.expr);
            return 0;

        // 导出声明：递归处理被导出的声明
        case AST_EXPORT:
            dce_expr(ast->u.export.decl);
            return 0;

        // 模块调用：递归处理参数
        case AST_MODULE_CALL:
            for (int i = 0; i < ast->u.module_call.args.count; i++) {
                dce_expr(ast->u.module_call.args.items[i]);
            }
            return 0;

        // 字符串插值：递归处理所有插值表达式
        case AST_INTERP_STRING:
            for (int i = 0; i < ast->u.interp_string.count - 1; i++) {
                dce_expr(ast->u.interp_string.exprs[i]);
            }
            return 0;

        // try-catch-finally：递归处理各部分
        case AST_TRY:
            dce_expr(ast->u.try_.try_body);
            if (ast->u.try_.catch_body) dce_expr(ast->u.try_.catch_body);
            if (ast->u.try_.finally_body) dce_expr(ast->u.try_.finally_body);
            return 0;

        // 类型检查：递归处理表达式
        case AST_TYPE_CHECK:
        case AST_AS_CAST:
            dce_expr(ast->u.type_check.expr);
            return 0;

        // 结构体定义：递归处理字段默认值和方法
        case AST_STRUCT_DEF:
            for (int i = 0; i < ast->u.struct_def.field_count; i++) {
                if (ast->u.struct_def.field_defaults[i])
                    dce_expr(ast->u.struct_def.field_defaults[i]);
            }
            for (int i = 0; i < ast->u.struct_def.method_count; i++) {
                dce_expr(ast->u.struct_def.methods[i]);
            }
            return 0;

        case AST_FACE_DEF:
        case AST_ALIAS:
            return 0;

        // 结构体实例化：递归处理字段值
        case AST_STRUCT_INIT:
            for (int i = 0; i < ast->u.struct_init.field_count; i++) {
                dce_expr(ast->u.struct_init.field_values[i]);
            }
            return 0;

        // 字段访问：递归处理对象
        case AST_FIELD_ACCESS:
            dce_expr(ast->u.field_access.obj);
            return 0;

        // await：递归处理表达式
        case AST_AWAIT:
            dce_expr(ast->u.await.expr);
            return 0;

        // 以下节点无子表达式需要 DCE 处理
        case AST_IMPORT:
        case AST_USE:
        case AST_MODULE_ACCESS:
        case AST_ENUM_DEF:
        case AST_CLIB_DEF:
        case AST_CFUNC_DECL:
        case AST_CSTRUCT_DEF:
            return 0;
    }

    return 0;
}

// 死代码消除入口函数
// 在常量折叠之后调用，利用已折叠的常量条件进行更激进的消除
void optimize_dead_code_elimination(Ast* ast) {
    dce_expr(ast);
}

void optimize_constant_fold(Ast* ast) {
    fold_expr(ast);
}
