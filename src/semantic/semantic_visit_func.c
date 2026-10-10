#include "semantic_internal.h"

// ============================================================================
// 辅助：递归检查 TypeInfo 中是否存在未定义的类型（在 resolve_alias_in_type 之后调用）
// 此时所有 alias 已被解析，剩余的 TYPE_STRUCT 只能是合法 struct 或未定义类型
// ============================================================================
static void check_undefined_type(Semantic* s, TypeInfo* type, int line, int column) {
    if (!type) return;

    // 优先使用 TypeInfo 自身的位置信息（更精确）
    int err_line = (type->line > 0) ? type->line : line;
    int err_column = (type->column > 0) ? type->column : column;

    if (type->kind == TYPE_STRUCT && type->struct_name) {
        // 在作用域中查找
        Symbol* struct_def = scope_resolve_local(s->current, type->struct_name);
        if (!struct_def && s->current) {
            struct_def = scope_resolve(s->current, type->struct_name);
        }
    if (!struct_def && !semantic_native_struct_known(type->struct_name)) {
        // 完全找不到 → 未定义的类型
        //   （native struct 是唯一例外：`DirEntry` / `DirInfo` 不在符号表里，但字段表由
        //     native 规格提供 ⇒ 见 semantic_native_struct_known 的说明）
        char msg[BUFFER_MEDIUM];
        if (type->struct_name && strcmp(type->struct_name, "double") == 0) {
            snprintf(msg, sizeof(msg), "未定义的类型: %s（Leno 中使用 float 代替 double，Leno 的 float 是 64 位双精度浮点数）", type->struct_name);
        } else {
            snprintf(msg, sizeof(msg), "未定义的类型: %s（请检查是否已通过 use 语句导入该类型，如 use module.%s）", type->struct_name, type->struct_name);
        }
        error_add_at(ERR_SEMANTIC, err_line, err_column, msg);
    }
        // 找到了就是合法的 struct 类型（alias 已在 resolve_alias_in_type 中解析）
    }

    // 递归检查子类型（传递子类型自己的位置信息）
    if (type->generic_args) {
        for (int i = 0; i < type->generic_count; i++) {
            check_undefined_type(s, type->generic_args[i], err_line, err_column);
        }
    }
    check_undefined_type(s, type->element_type, err_line, err_column);
    check_undefined_type(s, type->key_type, err_line, err_column);
    check_undefined_type(s, type->value_type, err_line, err_column);
    check_undefined_type(s, type->return_type, err_line, err_column);
    if (type->param_types) {
        for (int i = 0; i < type->param_count; i++) {
            check_undefined_type(s, type->param_types[i], err_line, err_column);
        }
    }
}

// 递归检查 TypeInfo 的**泛型实参/子类型**中是否存在未定义类型（不查顶层——
// 调用方通常已自行检查顶层类型名）。
//   B2 修复：变量声明路径原先只查顶层（`Array[intt]` 的 `intt` 藏在 generic_args
//   里无人检查），直到类型不匹配时才报出误导性的"期望 Array[struct intt]"。
//   现在在声明检查后调用本函数 ⇒ 直接报"未定义的类型: intt"。
//   返回 1 = 报过"未定义"错（调用方应置 declared_type_undefined 跳过后续级联检查）。
int semantic_check_undefined_subtypes(Semantic* s, TypeInfo* type, int line, int column) {
    if (!type) return 0;
    int reported = 0;
    if (type->generic_args) {
        for (int i = 0; i < type->generic_count; i++) {
            int before = error_count();
            check_undefined_type(s, type->generic_args[i], line, column);
            if (error_count() > before) reported = 1;
        }
    }
    int before = error_count();
    check_undefined_type(s, type->element_type, line, column);
    check_undefined_type(s, type->key_type, line, column);
    check_undefined_type(s, type->value_type, line, column);
    if (error_count() > before) reported = 1;
    return reported;
}

// 递归解析 TypeInfo 中的 alias 类型（在 resolve_generic_in_type 之后调用）
// 返回 1 表示有修改，0 表示无修改
int resolve_alias_in_type(Semantic* s, TypeInfo** type_ptr, int line) {
    if (!type_ptr || !*type_ptr) return 0;
    TypeInfo* type = *type_ptr;
    int changed = 0;

    if (type->kind == TYPE_STRUCT && type->struct_name) {
        if (face_def_find(type->struct_name)) {
            type->kind = TYPE_FACE;
            changed = 1;
        } else {
            Symbol* struct_def = scope_resolve_local(s->current, type->struct_name);
            if (!struct_def && s->current) {
                struct_def = scope_resolve(s->current, type->struct_name);
            }
            if (struct_def && struct_def->kind == SYM_TYPE && struct_def->type) {
                // 是 alias（如 FSize = Dict[string,float]），解析为实际类型
                type_free(type);
                *type_ptr = type_copy(struct_def->type);
                changed = 1;
            } else if (struct_def && struct_def->type && struct_def->type->kind == TYPE_CSTRUCT) {
                type->kind = TYPE_CSTRUCT;
                changed = 1;
            } else if (struct_def && struct_def->type && struct_def->type->kind == TYPE_CLIB) {
                type->kind = TYPE_CLIB;
                changed = 1;
            } else if (struct_def && struct_def->type && struct_def->type->kind == TYPE_ENUM) {
                // ★ 枚举名（2026-10-02 补）：parser 不认识 enum（它只认内置类型关键字 / 别名 /
                //   face）⇒ `func f(LE e)` 里的 LE 被记成 TYPE_STRUCT + 名字 ✗；
                //   而作用域里那条符号是 TYPE_ENUM ⇒ 这里与 face/cstruct/clib 同款归一化。
                //   缺了它：`f(LE.L)` 报「期望 struct LE，实际 int」（同文件与跨模块都报 ——
                //   枚举成员在语义期被折叠成 int 字面量，两个名字对不上 ✗）
                type->kind = TYPE_ENUM;
                changed = 1;
            }
        }
    }

    // 递归处理子类型
    TypeInfo* t = *type_ptr;
    if (t->generic_args) {
        for (int i = 0; i < t->generic_count; i++) {
            changed |= resolve_alias_in_type(s, &t->generic_args[i], line);
        }
    }
    changed |= resolve_alias_in_type(s, &t->element_type, line);
    changed |= resolve_alias_in_type(s, &t->key_type, line);
    changed |= resolve_alias_in_type(s, &t->value_type, line);
    changed |= resolve_alias_in_type(s, &t->return_type, line);
    if (t->param_types) {
        for (int i = 0; i < t->param_count; i++) {
            changed |= resolve_alias_in_type(s, &t->param_types[i], line);
        }
    }
    return changed;
}

// ============================================================================
// 辅助：将 AST 中的 TYPE_STRUCT 泛型参数名转换为 TYPE_GENERIC_PARAM
// ============================================================================

// 解析单个 TypeInfo 中的泛型参数
void resolve_generic_in_type(TypeInfo* type, char** type_params, char** type_param_constraints, int count) {
    if (!type) return;
    if (type->kind == TYPE_STRUCT && type->struct_name) {
        for (int i = 0; i < count; i++) {
            if (strcmp(type->struct_name, type_params[i]) == 0) {
                free(type->struct_name);
                type->struct_name = NULL;
                type->kind = TYPE_GENERIC_PARAM;
                type->type_param_name = strdup(type_params[i]);
                if (type_param_constraints && type_param_constraints[i]) {
                    type->constraint_name = strdup(type_param_constraints[i]);
                }
                return;
            }
        }
    }
    // 递归处理子类型
    resolve_generic_in_type(type->element_type, type_params, type_param_constraints, count);
    resolve_generic_in_type(type->key_type, type_params, type_param_constraints, count);
    resolve_generic_in_type(type->value_type, type_params, type_param_constraints, count);
    resolve_generic_in_type(type->return_type, type_params, type_param_constraints, count);
    if (type->param_types) {
        for (int i = 0; i < type->param_count; i++) {
            resolve_generic_in_type(type->param_types[i], type_params, type_param_constraints, count);
        }
    }
    // 递归处理泛型参数（如 Result[T] 中的 T）
    if (type->generic_args) {
        for (int i = 0; i < type->generic_count; i++) {
            resolve_generic_in_type(type->generic_args[i], type_params, type_param_constraints, count);
        }
    }
}

// 递归遍历 AST 树，解析所有类型中的泛型参数
static void resolve_generic_in_ast(Semantic* s, Ast* ast, char** type_params, char** type_param_constraints, int count) {
    if (!ast) return;
    
    switch (ast->kind) {
        case AST_VAR_DECL:
            resolve_generic_in_type(ast->u.var_decl.type, type_params, type_param_constraints, count);
            if (ast->u.var_decl.init) resolve_generic_in_ast(s, ast->u.var_decl.init, type_params, type_param_constraints, count);
            break;
        case AST_FUNC_DEF:
            // 嵌套函数：解析其参数和返回类型中的泛型
            for (int i = 0; i < ast->u.func.pcnt; i++) {
                resolve_generic_in_type(ast->u.func.param_types[i], type_params, type_param_constraints, count);
            }
            resolve_generic_in_type(ast->u.func.return_type, type_params, type_param_constraints, count);
            if (ast->u.func.body) resolve_generic_in_ast(s, ast->u.func.body, type_params, type_param_constraints, count);
            break;
        case AST_IF:
            if (ast->u.if_.guard_type) {
                resolve_generic_in_type(ast->u.if_.guard_type, type_params, type_param_constraints, count);
            }
            if (ast->u.if_.guard_conds.count > 0) {
                for (int gi = 0; gi < ast->u.if_.guard_conds.count; gi++) {
                    resolve_generic_in_type(ast->u.if_.guard_conds.items[gi].guard_type, type_params, type_param_constraints, count);
                }
            }
            if (ast->u.if_.cond) resolve_generic_in_ast(s, ast->u.if_.cond, type_params, type_param_constraints, count);
            if (ast->u.if_.then) resolve_generic_in_ast(s, ast->u.if_.then, type_params, type_param_constraints, count);
            if (ast->u.if_.else_) resolve_generic_in_ast(s, ast->u.if_.else_, type_params, type_param_constraints, count);
            break;
        case AST_SWITCH:
            if (ast->u.switch_.expr) resolve_generic_in_ast(s, ast->u.switch_.expr, type_params, type_param_constraints, count);
            for (int i = 0; i < ast->u.switch_.case_count; i++) {
                if (ast->u.switch_.cases[i].match_type) {
                    resolve_generic_in_type(ast->u.switch_.cases[i].match_type, type_params, type_param_constraints, count);
                }
                if (ast->u.switch_.cases[i].body) {
                    resolve_generic_in_ast(s, ast->u.switch_.cases[i].body, type_params, type_param_constraints, count);
                }
            }
            if (ast->u.switch_.default_body) resolve_generic_in_ast(s, ast->u.switch_.default_body, type_params, type_param_constraints, count);
            break;
        case AST_BLOCK:
            for (int i = 0; i < ast->u.block.count; i++) {
                resolve_generic_in_ast(s, ast->u.block.items[i], type_params, type_param_constraints, count);
            }
            break;
case AST_RETURN:
if (ast->u.ret) resolve_generic_in_ast(s, ast->u.ret, type_params, type_param_constraints, count);
break;
case AST_RETURN_MULTI:
for (int i = 0; i < ast->u.ret_multi.count; i++) {
resolve_generic_in_ast(s, ast->u.ret_multi.exprs[i], type_params, type_param_constraints, count);
}
break;
        case AST_CALL: {
            Ast* callee = ast->u.call.callee;
            if (callee) resolve_generic_in_ast(s, callee, type_params, type_param_constraints, count);
            for (int i = 0; i < ast->u.call.args.count; i++) {
                resolve_generic_in_ast(s, ast->u.call.args.items[i], type_params, type_param_constraints, count);
            }
            break;
        }
        case AST_EXPR_STMT:
            resolve_generic_in_ast(s, ast->u.expr_stmt.expr, type_params, type_param_constraints, count);
            break;
        case AST_BINOP:
            if (ast->u.binop.l) resolve_generic_in_ast(s, ast->u.binop.l, type_params, type_param_constraints, count);
            if (ast->u.binop.r) resolve_generic_in_ast(s, ast->u.binop.r, type_params, type_param_constraints, count);
            break;
        case AST_UNARY:
            if (ast->u.unary.operand) resolve_generic_in_ast(s, ast->u.unary.operand, type_params, type_param_constraints, count);
            break;
        case AST_FOR:
            if (ast->u.for_.start) resolve_generic_in_ast(s, ast->u.for_.start, type_params, type_param_constraints, count);
            if (ast->u.for_.end) resolve_generic_in_ast(s, ast->u.for_.end, type_params, type_param_constraints, count);
            if (ast->u.for_.body) resolve_generic_in_ast(s, ast->u.for_.body, type_params, type_param_constraints, count);
            break;
        case AST_TRY:
            if (ast->u.try_.try_body) resolve_generic_in_ast(s, ast->u.try_.try_body, type_params, type_param_constraints, count);
            if (ast->u.try_.catch_body) resolve_generic_in_ast(s, ast->u.try_.catch_body, type_params, type_param_constraints, count);
            if (ast->u.try_.finally_body) resolve_generic_in_ast(s, ast->u.try_.finally_body, type_params, type_param_constraints, count);
            break;
        case AST_ASSIGN:
            if (ast->u.assign.value) resolve_generic_in_ast(s, ast->u.assign.value, type_params, type_param_constraints, count);
            break;
        case AST_DESTRUCT_DECL: {
            for (int i = 0; i < ast->u.destruct_decl.slot_count; i++) {
                resolve_generic_in_type(ast->u.destruct_decl.slot_types[i], type_params, type_param_constraints, count);
            }
            if (ast->u.destruct_decl.init) resolve_generic_in_ast(s, ast->u.destruct_decl.init, type_params, type_param_constraints, count);
            break;
        }
        default:
            // 对于其他节点（字面量等），不需要处理
            break;
    }
}

// ============================================================================
// 函数处理 - 单作用域
// ============================================================================

void visit_func_impl(Semantic* s, Ast* ast, int is_struct_method);

void visit_func(Semantic* s, Ast* ast) {
    visit_func_impl(s, ast, 0);
}

void visit_func_as_struct_method(Semantic* s, Ast* ast) {
    visit_func_impl(s, ast, 1);
}

// ★★ 2026-10-10 新增：**漏写 return 检查**用的「这条语句是否必然离开函数」判定 ✓
//   为什么要有它：体检实测（用例 A4）—— 函数声明 `: int` 却漏了 return 时，编译器
//     **一声不吭** ✓、运行期**静默返回 null** ✗（调用点拿到 null 接着跑 ✓ 极难查 ✓）。
//   口径（**保守**：只认确凿的终止语句，宁可漏报也不误报 ✗）：
//     return / return a,b,c / throw                ⇒ 必然离开 ✓
//     块（AST_BLOCK）：看**最后一条**有效语句 ✓（空项跳过 ✓）
//     if：**有 else** 且两支都必然离开 ✓
//     switch：**有 default**，且每个 case 体与 default 都必然离开 ✓
//     其余（while / for / 赋值 / 表达式 …）一律算「可能走到底」✓ —— 含 `while true {}` ✗
//       （那是误报之源 ✓ 本仓的常驻循环本来就都带终止条件 ⇒ 不报更划算 ✓）
static int stmt_always_exits(Ast* st) {
    if (!st) return 0;
    switch (st->kind) {
        case AST_RETURN:
        case AST_RETURN_MULTI:
        case AST_THROW:
            return 1;
        case AST_BLOCK: {
            // ★ 正序扫：块里**任意一条**必然离开 ⇒ 整个块必然离开 ✓
            //   ⚠ 不能只看"最后一条" ✗：`func f(): int { return 1; print("never") }` 这种
            //     "return 之后还压着一句死代码"的写法，最后一条是 print ⇒ 只看末尾就会
            //     误报"漏写 return" ✗（实测 C1 用例正是这个形状 ✓ 已修 ✓）
            //     （紧跟的那句死代码由 visit_block.inc 的 WARN_UNREACHABLE 单独提示 ✓ 不冲突 ✓）
            for (int i = 0; i < st->u.block.count; i++) {
                Ast* it = st->u.block.items[i];
                if (!it) continue;
                if (stmt_always_exits(it)) return 1;
            }
            return 0;
        }
        case AST_IF:
            if (st->u.if_.else_ && stmt_always_exits(st->u.if_.then) &&
                stmt_always_exits(st->u.if_.else_)) {
                return 1;
            }
            return 0;
        case AST_TRY: {
            // ★ try / catch 两支都必然离开 ⇒ 整体必然离开 ✓
            //   为什么必须认这一条：本仓大量函数写成
            //     `try { ...; return x } catch { return y }`
            //   （如 sdl_table 的 to_float / is_number_str ✓）⇒ 不认它就会把这种
            //   完全正确的写法全报成"漏写 return" ✗（实测就是它们把警告数从个位数顶上去的 ✓）
            //   ⚠ finally：它自己若必然离开（`finally { return }`）也算 ✓；否则只是收尾 ✓
            if (st->u.try_.finally_body && stmt_always_exits(st->u.try_.finally_body)) return 1;
            if (!stmt_always_exits(st->u.try_.try_body)) return 0;
            if (st->u.try_.catch_body && stmt_always_exits(st->u.try_.catch_body)) return 1;
            return 0;
        }
        case AST_SWITCH: {
            if (!st->u.switch_.default_body) return 0;
            if (!stmt_always_exits(st->u.switch_.default_body)) return 0;
            for (int i = 0; i < st->u.switch_.case_count; i++) {
                if (!stmt_always_exits(st->u.switch_.cases[i].body)) return 0;
            }
            return 1;
        }
        default:
            return 0;
    }
}

void visit_func_impl(Semantic* s, Ast* ast, int is_struct_method) {
    if (ast->u.func.local_count > 0) {
        return;
    }

    // 将解析为 struct 的泛型类型参数转换为 TYPE_GENERIC_PARAM（递归处理嵌套类型如 Array[T]）
    if (ast->u.func.type_param_count > 0 && ast->u.func.type_params) {
        for (int i = 0; i < ast->u.func.pcnt; i++) {
            resolve_generic_in_type(ast->u.func.param_types[i], ast->u.func.type_params, ast->u.func.type_param_constraints, ast->u.func.type_param_count);
        }
        resolve_generic_in_type(ast->u.func.return_type, ast->u.func.type_params, ast->u.func.type_param_constraints, ast->u.func.type_param_count);
    }

    // struct 方法：从 self 参数类型获取 struct 的泛型参数并解析
    if (is_struct_method && ast->u.func.pcnt > 0 && ast->u.func.param_types[0]) {
        TypeInfo* self_type = ast->u.func.param_types[0];
        if (self_type->kind == TYPE_STRUCT && self_type->generic_count > 0 && self_type->generic_args) {
            // 构建泛型参数名数组
            int gp_count = self_type->generic_count;
            char** gp_names = (char**)malloc(sizeof(char*) * gp_count);
            for (int i = 0; i < gp_count; i++) {
                if (self_type->generic_args[i]->kind == TYPE_GENERIC_PARAM && self_type->generic_args[i]->type_param_name) {
                    gp_names[i] = strdup(self_type->generic_args[i]->type_param_name);
                } else {
                    gp_names[i] = NULL;
                }
            }
            // 解析方法参数和返回类型中的泛型参数
            for (int i = 1; i < ast->u.func.pcnt; i++) {
                resolve_generic_in_type(ast->u.func.param_types[i], gp_names, NULL, gp_count);
            }
            resolve_generic_in_type(ast->u.func.return_type, gp_names, NULL, gp_count);
            // 保存到 ast 以便后续 resolve_generic_in_ast 使用
            if (!ast->u.func.type_params && !ast->u.func.type_param_count) {
                ast->u.func.type_param_count = gp_count;
                ast->u.func.type_params = gp_names;
            } else {
                for (int i = 0; i < gp_count; i++) free(gp_names[i]);
                free(gp_names);
            }
        }
    }

    // ========== 解析 alias 类型 + 检查未定义类型 ==========
    // 在 resolve_generic_in_type 之后执行，此时泛型参数 T/K/V 已转换为 TYPE_GENERIC_PARAM
    // 剩余的 TYPE_STRUCT 只能是合法 struct、alias 或未定义类型

    // 1. 解析参数类型中的 alias（如 FSize = Dict[string,float]）
    for (int i = 0; i < ast->u.func.pcnt; i++) {
        resolve_alias_in_type(s, &ast->u.func.param_types[i], ast->line);
    }
    // 2. 解析返回类型中的 alias
    resolve_alias_in_type(s, &ast->u.func.return_type, ast->line);

    // 3. 同步更新符号的函数类型（返回类型 + **形参**）
    //    ★ 2026-10-10：形参此前**漏了同步** —— `resolve_alias_in_type` 只改了 AST 上的
    //    `param_types`，符号里那份拷贝仍是 TYPE_STRUCT ⇒ 入口模块编译时，形参是 clib 的函数
    //    在调用点被读成「期望 struct X」，而实参（ffi.load 结果 / 同类型变量）是 clib ⇒
    //    报「期望 struct k32_charset, 实际 clib k32_charset」✗（web_charset.leno 5 处；
    //    被 import 时该参数检查整段跳过 ⇒ 长期没暴露）。返回类型早有同步 ⇒ 这里补齐对称 ✓
    if (ast->u.func.return_type || ast->u.func.pcnt > 0) {
        Symbol* sym = scope_resolve_local(s->current, ast->u.func.name);
        if (!sym && s->current) sym = scope_resolve(s->current, ast->u.func.name);
        if (sym && sym->type && sym->type->kind == TYPE_FUNCTION) {
            if (ast->u.func.return_type && sym->type->return_type) {
                type_free(sym->type->return_type);
                sym->type->return_type = type_copy(ast->u.func.return_type);
            }
            if (ast->u.func.pcnt > 0 && sym->type->param_types) {
                for (int i = 0; i < ast->u.func.pcnt && i < sym->type->param_count; i++) {
                    if (!ast->u.func.param_types[i]) continue;
                    type_free(sym->type->param_types[i]);
                    sym->type->param_types[i] = type_copy(ast->u.func.param_types[i]);
                }
            }
        }
    }

    // 4. 检查参数类型中是否有未定义的类型
    for (int i = 0; i < ast->u.func.pcnt; i++) {
        check_undefined_type(s, ast->u.func.param_types[i], ast->line, ast->column);
    }
    // 5. 检查返回类型中是否有未定义的类型
    check_undefined_type(s, ast->u.func.return_type, ast->line, ast->column);

    // 5.5 检查参数类型和返回类型是否使用了 C 布局类型（i32/u8/f32 等）
    // C 布局类型只能在 clib 声明、cstruct 字段、Ptr[T] 中使用
    // 在 clib/cfunc 上下文中的函数定义不检查（clib 内部允许）
    if (!s->in_clib) {
        for (int i = 0; i < ast->u.func.pcnt; i++) {
            if (ast->u.func.param_types[i] && is_c_layout_type(ast->u.func.param_types[i]->kind)) {
                char msg[BUFFER_MEDIUM];
                const char* type_str = type_kind_to_string(ast->u.func.param_types[i]->kind);
                snprintf(msg, sizeof(msg), "C 布局类型 '%s' 不能用于函数参数，请使用 Leno 类型（如 int/float/string）", type_str);
                error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
            }
        }
        if (ast->u.func.return_type && is_c_layout_type(ast->u.func.return_type->kind)) {
            char msg[BUFFER_MEDIUM];
            const char* type_str = type_kind_to_string(ast->u.func.return_type->kind);
            snprintf(msg, sizeof(msg), "C 布局类型 '%s' 不能用于函数返回值，请使用 Leno 类型（如 int/float/string）", type_str);
            error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
        }
    }

    // 5.7 main 入口函数返回类型检查：返回值将作为进程退出码，只能是 int
    //    与 codegen 的 find_main_function 保持一致：仅全局作用域的 main 是入口函数
    int is_entry_main = (!is_struct_method && ast->u.func.name &&
                         strcmp(ast->u.func.name, "main") == 0 &&
                         s->current && s->current->parent == NULL);
    if (is_entry_main && ast->u.func.return_type &&
        ast->u.func.return_type->kind != TYPE_INFER &&
        ast->u.func.return_type->kind != TYPE_INT) {
        char msg[BUFFER_MEDIUM];
        snprintf(msg, sizeof(msg),
            "main 函数的返回值将作为进程退出码，只能返回 int，不能返回 %s",
            type_kind_to_string(ast->u.func.return_type->kind));
        error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
    }

    // 6. 检查参数类型和返回类型中是否将 var 用作了类型参数
    for (int i = 0; i < ast->u.func.pcnt; i++) {
        if (ast->u.func.param_types[i] && ast->u.func.param_types[i]->kind != TYPE_INFER) {
            TypeKind parent_kind = TYPE_UNKNOWN;
            if (type_has_infer_as_param(ast->u.func.param_types[i], &parent_kind)) {
                char msg[BUFFER_MEDIUM];
                const char* parent_name = type_kind_to_string(parent_kind);
                snprintf(msg, sizeof(msg), "var 不能用作类型参数，请改用 any（如 %s[any]）", parent_name);
                error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
            }
        }
    }
    if (ast->u.func.return_type && ast->u.func.return_type->kind != TYPE_INFER) {
        TypeKind parent_kind = TYPE_UNKNOWN;
        if (type_has_infer_as_param(ast->u.func.return_type, &parent_kind)) {
            char msg[BUFFER_MEDIUM];
            const char* parent_name = type_kind_to_string(parent_kind);
            snprintf(msg, sizeof(msg), "var 不能用作类型参数，请改用 any（如 %s[any]）", parent_name);
            error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
        }
    }

    // 7. 检查参数是否使用了 var 类型（参数位置不允许 var，请改用 any）
    for (int i = 0; i < ast->u.func.pcnt; i++) {
        if (ast->u.func.param_types[i] && ast->u.func.param_types[i]->kind == TYPE_INFER) {
            char msg[BUFFER_MEDIUM];
            snprintf(msg, sizeof(msg), "参数 '%s' 不能使用 var 类型，请改用 any", ast->u.func.params[i]);
            error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
        }
    }

    // ========== 默认参数语义检查 ==========
    int found_default = 0;  // 标记是否已遇到有默认值的参数
    for (int i = 0; i < ast->u.func.pcnt; i++) {
        Ast* default_expr = ast->u.func.param_defaults ? ast->u.func.param_defaults[i] : NULL;
        
        if (default_expr) {
            found_default = 1;
            
            // 2.2 检查默认值类型：支持字面量、全局变量引用（如 Global.ALL）、常量表达式
            int is_literal = (default_expr->kind == AST_NUM || 
                             default_expr->kind == AST_STRING || 
                             default_expr->kind == AST_BOOL || 
                             default_expr->kind == AST_NULL);
            int is_const_expr = (default_expr->kind == AST_BINOP ||
                                default_expr->kind == AST_UNARY ||
                                default_expr->kind == AST_VAR ||
                                default_expr->kind == AST_MODULE_ACCESS);
            if (!is_literal && !is_const_expr) {
                char msg[BUFFER_MEDIUM];
                snprintf(msg, sizeof(msg), "参数 '%s' 的默认值必须是字面量常量或编译期常量表达式", ast->u.func.params[i]);
                error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
            }
            
            // 对非字面量表达式执行语义分析（设置 ref、折叠 enum 常量等）
            // visit 可能将 AST_MODULE_ACCESS（如 Flags.ALL）折叠为 AST_NUM
            if (!is_literal) {
                visit(s, default_expr);
            }
            
            // visit 后重新检查：如果被折叠为字面量，按字面量处理
            if (default_expr->kind == AST_NUM || default_expr->kind == AST_STRING ||
                default_expr->kind == AST_BOOL || default_expr->kind == AST_NULL) {
                is_literal = 1;
            }
            
            // 2.3 & 2.4 检查：默认值类型与参数类型匹配
            TypeInfo* param_type = ast->u.func.param_types[i];
            if (param_type && param_type->kind != TYPE_INFER) {
                // 推断默认值类型
                TypeKind default_kind = TYPE_ANY;
                if (default_expr->kind == AST_NUM) {
                    default_kind = default_expr->u.num.is_float ? TYPE_FLOAT : TYPE_INT;
                } else if (default_expr->kind == AST_STRING) {
                    default_kind = TYPE_STRING;
                } else if (default_expr->kind == AST_BOOL) {
                    default_kind = TYPE_BOOL;
                } else if (default_expr->kind == AST_NULL) {
                    default_kind = TYPE_NULL;
                } else if (default_expr->kind == AST_VAR || default_expr->kind == AST_MODULE_ACCESS ||
                           default_expr->kind == AST_BINOP || default_expr->kind == AST_UNARY) {
                    // 全局变量引用或常量表达式：尝试推断类型
                    TypeInfo* expr_type = infer_expr_type(s, default_expr);
                    if (expr_type) {
                        default_kind = expr_type->kind;
                        type_free(expr_type);
                    }
                }
                
                // 检查类型兼容性（null 默认值也要检查，只有 nullable/指针类型才能接受 null）
                if (default_kind != TYPE_ANY) {
                    if (!type_is_compatible(param_type, type_new(default_kind))) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "参数 '%s' 的默认值类型 '%s' 与参数类型 '%s' 不匹配",
                                 ast->u.func.params[i],
                                 type_kind_to_string(default_kind),
                                 type_kind_to_string(param_type->kind));
                        error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, msg);
                    }
                }
            } else if (param_type && param_type->kind == TYPE_INFER) {
                // 2.4 var 参数：根据默认值推断类型
                TypeKind inferred_kind = TYPE_ANY;
                if (default_expr->kind == AST_NUM) {
                    inferred_kind = default_expr->u.num.is_float ? TYPE_FLOAT : TYPE_INT;
                } else if (default_expr->kind == AST_STRING) {
                    inferred_kind = TYPE_STRING;
                } else if (default_expr->kind == AST_BOOL) {
                    inferred_kind = TYPE_BOOL;
                } else if (default_expr->kind == AST_NULL) {
                    inferred_kind = TYPE_NULL;
                } else if (default_expr->kind == AST_VAR || default_expr->kind == AST_MODULE_ACCESS ||
                           default_expr->kind == AST_BINOP || default_expr->kind == AST_UNARY) {
                    TypeInfo* expr_type = infer_expr_type(s, default_expr);
                    if (expr_type) {
                        inferred_kind = expr_type->kind;
                        type_free(expr_type);
                    }
                }
                
                // 更新参数类型为推断的类型
                type_free(param_type);
                ast->u.func.param_types[i] = type_new(inferred_kind);
            }
        } else {
            // 2.1 检查：无默认值的参数不能在有默认值的参数之后
            if (found_default) {
                char msg[BUFFER_MEDIUM];
                snprintf(msg, sizeof(msg), "参数 '%s' 没有默认值，但它位于有默认值的参数之后。所有有默认值的参数必须放在参数列表末尾",
                         ast->u.func.params[i]);
                error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
            }
        }
    }

    // 将函数添加到函数表（哈希表自动处理重复）
    // 跳过 struct 方法的注册，因为它们已经在 semantic_visit_ast.c 中以 struct_name::method_name 格式注册
    // 全局函数和局部函数都注册到 func_table，以便代码生成器查找函数定义（默认参数填充等）
    // 局部函数会覆盖同名的全局函数定义（实现遮蔽语义，同时确保默认参数信息可用）
    // ⚠ 注意 codegen **不能只靠 func_table 判 async**：本表是按名字的全局表、无作用域信息，
    //   局部函数（别的作用域里的同名函数）会覆盖全局条目，而 codegen 在语义遍**之后**才
    //   查它 ⇒ 会拿到被污染的定义。async 判定必须优先用语义遍**当场**捕获的
    //   `ast->u.call.callee_is_async`（见 visitinc/visit_expr.inc），栈式正是这么做的。
    if (!is_struct_method) {
        func_table_add(&s->func_table, ast->u.func.name, ast);
    }

    // 在父作用域注册函数名（如果还没有注册）
    // 注意：函数名可能已经在 AST_BLOCK 的预扫描中注册了
    // 在创建函数作用域之前注册，以便正确判断是否为全局函数
    // 跳过 struct 方法的注册，因为它们不应该在 struct 外部作用域中可见
    if (s->current && !is_struct_method) {
        Symbol* existing = scope_resolve_local(s->current, ast->u.func.name);
        if (!existing) {
            // 判断是否为全局函数：当前作用域的父作用域为 NULL 时表示全局作用域
            SymKind kind = (s->current->parent == NULL) ? SYM_GLOBAL_FUNC : SYM_LOCAL;
            Symbol* sym = scope_define(s->current, ast->u.func.name, kind);
            if (sym) {
                ast->u.func.ref.kind = sym->kind;
                // 局部函数使用函数级别的local_index分配索引，与变量一致
                if (kind == SYM_LOCAL) {
                    sym->index = allocate_local_index(s);
                }
                ast->u.func.ref.index = sym->index;
                ast->u.func.ref.name = strdup(sym->name);
                // 设置函数符号的类型为函数类型
                TypeInfo* return_type = (ast->u.func.return_type && ast->u.func.return_type->kind != TYPE_INFER)
                    ? type_copy(ast->u.func.return_type) : NULL;
                TypeInfo** param_types = NULL;
                if (ast->u.func.pcnt > 0) {
                    param_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * ast->u.func.pcnt);
                    for (int i = 0; i < ast->u.func.pcnt; i++) {
                        param_types[i] = (ast->u.func.param_types[i] && ast->u.func.param_types[i]->kind != TYPE_INFER)
                            ? type_copy(ast->u.func.param_types[i]) : NULL;
                    }
                }
                sym->type = type_function(return_type, param_types, ast->u.func.pcnt);
                if (param_types) {
                    for (int i = 0; i < ast->u.func.pcnt; i++) {
                        type_free(param_types[i]);
                    }
                    free(param_types);
                }
                ast->u.func.ref.type_kind = TYPE_FUNCTION;
            }
        } else {
            // 使用已注册的符号（可能来自预扫描）
            ast->u.func.ref.kind = existing->kind;
            ast->u.func.ref.index = existing->index;
            if (!ast->u.func.ref.name) {
                ast->u.func.ref.name = strdup(existing->name);
            }
            // 设置函数类型（即使预扫描阶段已设置，也要更新为完整的类型信息）
            TypeInfo* return_type = (ast->u.func.return_type && ast->u.func.return_type->kind != TYPE_INFER)
                ? type_copy(ast->u.func.return_type) : NULL;
            TypeInfo** param_types = NULL;
            if (ast->u.func.pcnt > 0) {
                param_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * ast->u.func.pcnt);
                for (int i = 0; i < ast->u.func.pcnt; i++) {
                    param_types[i] = (ast->u.func.param_types[i] && ast->u.func.param_types[i]->kind != TYPE_INFER)
                        ? type_copy(ast->u.func.param_types[i]) : NULL;
                }
            }
            if (existing->type) type_free(existing->type);
            existing->type = type_function(return_type, param_types, ast->u.func.pcnt);
            if (param_types) {
                for (int i = 0; i < ast->u.func.pcnt; i++) {
                    type_free(param_types[i]);
                }
                free(param_types);
            }
            ast->u.func.ref.type_kind = TYPE_FUNCTION;
        }
    }
    
    // 保存状态
    int prev_local_index = s->local_index;
    int prev_in_main_func = s->in_main_func;
    s->in_main_func = is_entry_main;

    // 将当前函数压入栈（所有函数都入栈，用于建立upvalue链）
    s->func_stack[s->func_stack_depth++] = ast;
    s->current_func = ast;
    s->local_index = ast->u.func.pcnt;

    // ★ 槽位回收：本函数（层级 = func_stack_depth-1）的记账初始化。
    //   func_pinned 初值 = pcnt：**参数槽位 0..pcnt-1 不得被回退复用**（它们整个函数期内有效，
    //   且可能被闭包捕获）；func_max_index 初值同样 = pcnt（参数也是寄存器高水位的一部分）。
    //   ⚠ 这两个数组按层级各存一份：分析内层函数时外层函数的状态是"挂起"的，
    //     各自的计数器/钉住下界必须分开记账（内层捕获外层槽位时写的是外层那一格）。
    if (s->func_stack_depth > 0 && s->func_stack_depth <= 64) {
        int lv = s->func_stack_depth - 1;
        s->func_pinned[lv] = ast->u.func.pcnt;
        s->func_max_index[lv] = ast->u.func.pcnt;
    }
    
    // 创建单一函数作用域（同时包含参数和局部变量）
    Scope* func_scope = scope_new(s->current, 1);
    s->current = func_scope;

    // 注册泛型类型参数到函数作用域（用于类型解析）
    if (ast->u.func.type_param_count > 0 && ast->u.func.type_params) {
        for (int i = 0; i < ast->u.func.type_param_count; i++) {
            // 注册为 TYPE 符号，使类型名在函数体内可解析
            Symbol* tp_sym = scope_define(s->current, ast->u.func.type_params[i], SYM_TYPE);
            if (tp_sym) {
                tp_sym->type = type_generic_param(ast->u.func.type_params[i]);
                tp_sym->index = -1;  // 类型符号不占运行时索引
            }
        }
    }

    // 定义参数
    for (int i = 0; i < ast->u.func.pcnt; i++) {
        // B4：重复参数名检查——此前 scope_define 静默覆盖，后者遮蔽前者
        for (int j = 0; j < i; j++) {
            if (ast->u.func.params[i] && ast->u.func.params[j] &&
                strcmp(ast->u.func.params[i], ast->u.func.params[j]) == 0) {
                char msg[BUFFER_SMALL];
                snprintf(msg, sizeof(msg), "重复的参数名 '%s'（参数 %d 与参数 %d 重名）",
                         ast->u.func.params[i], j + 1, i + 1);
                error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
            }
        }
        Symbol* sym = scope_define(s->current, ast->u.func.params[i], SYM_PARAM);
        if (sym) {
            sym->index = i;
            // 设置参数类型
            TypeInfo* param_type = ast->u.func.param_types[i];
            if (param_type) {
                if (param_type->kind == TYPE_INFER) {
                    // var 参数默认为 any
                    sym->type = type_new(TYPE_ANY);
                } else {
                    // 包括函数类型（无返回类型签名时 return_type 为 NULL，表示 void）
                    sym->type = type_copy(param_type);
                }
            } else {
                // 没有类型信息，默认为 any
                sym->type = type_new(TYPE_ANY);
            }
        }
    }

    // 预注册嵌套函数名（支持前向引用）
    if (ast->u.func.body && ast->u.func.body->kind == AST_BLOCK) {
        AstList* block = &ast->u.func.body->u.block;
        for (int i = 0; i < block->count; i++) {
            Ast* stmt = block->items[i];
            if (stmt->kind == AST_FUNC_DEF) {
                Symbol* sym = scope_define(s->current, stmt->u.func.name, SYM_LOCAL);
                if (sym) {
                    sym->index = allocate_local_index(s);
                    stmt->u.func.ref.kind = sym->kind;
                    stmt->u.func.ref.index = sym->index;
                    free(stmt->u.func.ref.name);
                    stmt->u.func.ref.name = strdup(sym->name);
                }
            }
        }
    }

    // 将函数体中的泛型类型参数名解析为 TYPE_GENERIC_PARAM
    if (ast->u.func.type_param_count > 0) {
        resolve_generic_in_ast(s, ast->u.func.body, ast->u.func.type_params, ast->u.func.type_param_constraints, ast->u.func.type_param_count);
    }

    // ★ 泛型需求收集的上下文（B 方案，2026-10-02）：函数体内把 T 用在 native 的**具体类型形参**上时，
    //   需求记到本函数头上（随后由**调用点**用类型实参校验 ✓）；非泛型函数体设为 NULL
    //   ⇒ 那种情况下 native 检查点会退回"就地报错"的兜底（比静默放过去好 ✓）
    Ast* saved_generic_func = s->cur_generic_func;
    s->cur_generic_func = (ast->u.func.type_param_count > 0) ? ast : NULL;

    // 处理函数体（单遍完成所有分析）
    // 注意：使用 visit 而不是 visit_list，以确保 AST_BLOCK 的预扫描逻辑被执行
    visit(s, ast->u.func.body);

    // ★★ 2026-10-10 新增：**漏写 return 检查** ✓
    //   实测（体检用例 A4）：函数声明了返回值却漏 return 时，原先**完全不报** ✓，
    //   运行期静默返回 null ✗（调用点拿到 null 继续跑 ⇒ 极难查 ✓）。
    //   ⚠ 只对"声明了返回类型"的函数查 ✓（`return_type == NULL` = void ⇒ 跳过 ✓，
    //     见上方形参处理的注释：无返回类型签名时 return_type 为 NULL ✓）；
    //     判定口径见 stmt_always_exits 的注释（保守 ⇒ 宁可漏报不误报 ✓）
    //   ⚠ 判据必须同时排除 **TYPE_INFER**：没写返回类型的函数（void ✓）在 AST 里
    //     `return_type` 并不是 NULL 而是 **TYPE_INFER 占位** ✗ —— 只看 NULL 会让所有
    //     void 函数统统报"漏写 return"（实测：SDL3 / sdl_layout 各刷出 250+ 条全误报 ✗✗）。
    //     同文件 249 行那份判据就是 `return_type && kind != TYPE_INFER` ✓ 照它写 ✓
    if (ast->u.func.return_type && ast->u.func.return_type->kind != TYPE_INFER &&
        ast->u.func.body && !stmt_always_exits(ast->u.func.body)) {
        char mrmsg[BUFFER_MEDIUM];
        snprintf(mrmsg, sizeof(mrmsg),
                 "函数 '%s' 声明了返回值，但存在没有 return 的路径 ⇒ 漏写 return 时调用点会"
                 "**静默拿到 null**（实测如此 ✓，请补 return 或在末尾 return 兜底值）",
                 ast->u.func.name ? ast->u.func.name : "?");
        warning_add_at(WARN_MISSING_RETURN, ast->line, ast->column, mrmsg);
    }

    s->cur_generic_func = saved_generic_func;

    // 保存局部变量数量
    //   ★ 必须是**历史最高水位**（func_max_index），不能取 local_index 的当前值：
    //     槽位回收（语句/作用域边界回退）后当前值会比真实用到的最高槽位小 ⇒
    //     codegen 的临时寄存器起点（codegen_func.c 里 base_reg 取 func->local_count）
    //     偏低 ⇒ 临时值盖掉变量（静默错值）。
    {
        int lv = sem_local_level(s);
        int hw = s->func_max_index[lv];
        if (hw < s->local_index) hw = s->local_index;   // 保险：至少覆盖当前位置
        ast->u.func.local_count = hw;
    }

    // 恢复状态
    s->current = func_scope->parent;

    // 在 LSP 模式下保留函数作用域，以便后续符号查询
    if (!s->is_lsp_mode) {
        scope_detach_child(s->current, func_scope);
        scope_free(func_scope);
    }

    // 弹出函数栈
    s->func_stack_depth--;
    s->current_func = (s->func_stack_depth > 0) ? s->func_stack[s->func_stack_depth - 1] : NULL;
    s->local_index = prev_local_index;
    s->in_main_func = prev_in_main_func;
}
