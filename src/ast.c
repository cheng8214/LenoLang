#include "include/lenolang.h"
#include "include/leno_ast.h"
#include <stdlib.h>

Ast* ast_new(AstKind kind, int line) {
    Ast* ast = (Ast*)calloc(1, sizeof(Ast));
    if (!ast) {
        error_add_at_fmt(ERR_RUNTIME, line, 0, "内存分配失败");
        return NULL;
    }
    ast->kind = kind;
    ast->line = line;
    ast->column = error_get_column();
    return ast;
}

void ast_list_init(AstList* list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void ast_list_add(AstList* list, Ast* ast) {
    if (list->count >= list->capacity) {
        int new_capacity = list->capacity == 0 ? 8 : list->capacity * 2;
        Ast** new_items = (Ast**)realloc(list->items, new_capacity * sizeof(Ast*));
        if (!new_items) {
            error_add_at_fmt(ERR_RUNTIME, 0, 0, "内存分配失败");
            return;
        }
        list->items = new_items;
        list->capacity = new_capacity;
    }
    list->items[list->count++] = ast;
}

// 类型守卫列表操作
void type_guard_list_init(TypeGuardList* list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void type_guard_list_add(TypeGuardList* list, TypeGuardCond cond) {
    if (list->count >= list->capacity) {
        int new_capacity = list->capacity == 0 ? 4 : list->capacity * 2;
        TypeGuardCond* new_items = (TypeGuardCond*)realloc(list->items, new_capacity * sizeof(TypeGuardCond));
        if (!new_items) {
            error_add_at_fmt(ERR_RUNTIME, 0, 0, "内存分配失败");
            return;
        }
        list->items = new_items;
        list->capacity = new_capacity;
    }
    list->items[list->count++] = cond;
}

void type_guard_list_free(TypeGuardList* list) {
    for (int i = 0; i < list->count; i++) {
        free(list->items[i].var_name);
        if (list->items[i].field_name) {
            free(list->items[i].field_name);
        }
        if (list->items[i].index_key) {
            free(list->items[i].index_key);
        }
        if (list->items[i].guard_type) {
            type_free(list->items[i].guard_type);
        }
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void free_ast_list(AstList* list) {
    for (int i = 0; i < list->count; i++) {
        ast_free(list->items[i]);
    }
    free(list->items);
}

void ast_free(Ast* ast) {
    if (!ast) return;

    // 注解（2026-10-31）：挂在声明上、与具体 kind 无关 ⇒ 统一在这里释放，
    //   不必往下面那个大 switch 的每个分支里各加一份 ✓
    if (ast->annos) {
        for (int i = 0; i < ast->anno_count; i++) {
            free(ast->annos[i].name);
            free(ast->annos[i].arg_str);
            if (ast->annos[i].arg_list) {
                for (int k = 0; k < ast->annos[i].arg_list_len; k++) {
                    free(ast->annos[i].arg_list[k]);
                }
                free(ast->annos[i].arg_list);
            }
        }
        free(ast->annos);
        ast->annos = NULL;
        ast->anno_count = 0;
    }

    switch (ast->kind) {
        case AST_VAR:
            free(ast->u.var.name);
            free(ast->u.var.ref.name);
            free(ast->u.var.ref.struct_name);
            break;
        case AST_NUM:
            free(ast->u.num.bigint_str);
            break;
        case AST_STRING:
            free(ast->u.string.value);
            break;
        case AST_ARRAY:
            free_ast_list(&ast->u.array);
            break;
        case AST_DICT:
            for (int i = 0; i < ast->u.dict.count; i++) {
                ast_free(ast->u.dict.entries[i].key);
                ast_free(ast->u.dict.entries[i].value);
            }
            free(ast->u.dict.entries);
            break;
        case AST_BINOP:
            ast_free(ast->u.binop.l);
            ast_free(ast->u.binop.r);
            break;
        case AST_UNARY:
            ast_free(ast->u.unary.operand);
            break;
        case AST_CALL:
            ast_free(ast->u.call.callee);
            free_ast_list(&ast->u.call.args);
            // 命名实参的求值顺序表（只借节点指针，节点归 args 所有 ⇒ 只释放数组本身）
            free(ast->u.call.arg_eval);
            free(ast->u.call.arg_eval_slots);
            ast->u.call.arg_eval = NULL;
            ast->u.call.arg_eval_slots = NULL;
            // 泛型调用的实参/名字（`foo[int](...)`）
            if (ast->u.call.generic_type_args) {
                for (int i = 0; i < ast->u.call.generic_type_count; i++) {
                    if (ast->u.call.generic_type_args[i]) type_free(ast->u.call.generic_type_args[i]);
                }
                free(ast->u.call.generic_type_args);
            }
            if (ast->u.call.generic_type_names) {
                for (int i = 0; i < ast->u.call.generic_type_count; i++) {
                    free(ast->u.call.generic_type_names[i]);
                }
                free(ast->u.call.generic_type_names);
            }
            break;
        case AST_INDEX:
            ast_free(ast->u.index.obj);
            ast_free(ast->u.index.index);
            break;
        case AST_INDEX_ASSIGN:
            ast_free(ast->u.index_assign.obj);
            ast_free(ast->u.index_assign.index);
            ast_free(ast->u.index_assign.value);
            break;
        case AST_BLOCK:
            free_ast_list(&ast->u.block);
            break;
        case AST_IF:
            ast_free(ast->u.if_.cond);
            ast_free(ast->u.if_.then);
            ast_free(ast->u.if_.else_);
            free(ast->u.if_.guard_var);
            free(ast->u.if_.guard_var_ref.name);
            free(ast->u.if_.guard_var_ref.struct_name);
            if (ast->u.if_.guard_type) {
                type_free(ast->u.if_.guard_type);
            }
            // guard_bind_var 是 parser 里 malloc 出来的变量名（`if x is T => y` 的 y）
            //   ⇒ 必须释放（guard_bind_expr 是 cond 的子节点，随 cond 一起释放，不在此处重复释放）
            free(ast->u.if_.guard_bind_var);
            type_guard_list_free(&ast->u.if_.guard_conds);
            break;
        case AST_WHILE:
            ast_free(ast->u.while_.cond);
            ast_free(ast->u.while_.body);
            break;
        case AST_FOR:
            ast_free(ast->u.for_.start);
            ast_free(ast->u.for_.end);
            ast_free(ast->u.for_.step);
            free(ast->u.for_.var_name);
            free(ast->u.for_.index_var_name);
            ast_free(ast->u.for_.body);
            break;
        case AST_SWITCH:
            ast_free(ast->u.switch_.expr);
            for (int i = 0; i < ast->u.switch_.case_count; i++) {
                free_ast_list(&ast->u.switch_.cases[i].values);
                ast_free(ast->u.switch_.cases[i].body);
                // 释放 match_types 数组（逗号合并时多个类型）
                if (ast->u.switch_.cases[i].match_types) {
                    for (int mi = 0; mi < ast->u.switch_.cases[i].match_type_count; mi++) {
                        type_free(ast->u.switch_.cases[i].match_types[mi]);
                    }
                    free(ast->u.switch_.cases[i].match_types);
                } else if (ast->u.switch_.cases[i].match_type) {
                    type_free(ast->u.switch_.cases[i].match_type);
                }
                free(ast->u.switch_.cases[i].guard_var);
                free(ast->u.switch_.cases[i].guard_bind_var);
                free(ast->u.switch_.cases[i].guard_var_ref.name);
                free(ast->u.switch_.cases[i].guard_var_ref.struct_name);
                // `case is Point(x, y)` 的解构变量名（parser 的 var_names 数组 + 每个名字）
                if (ast->u.switch_.cases[i].destructure_vars) {
                    for (int vi = 0; vi < ast->u.switch_.cases[i].destructure_count; vi++) {
                        free(ast->u.switch_.cases[i].destructure_vars[vi]);
                    }
                    free(ast->u.switch_.cases[i].destructure_vars);
                }
                // 解构的语义产物（visit_control 里按 destructure_count 数组分配）：
                //   索引数组 + 字段名数组（每个字段名 strdup）
                free(ast->u.switch_.cases[i].destructure_indices);
                if (ast->u.switch_.cases[i].destructure_field_names) {
                    for (int vi = 0; vi < ast->u.switch_.cases[i].destructure_count; vi++) {
                        free(ast->u.switch_.cases[i].destructure_field_names[vi]);
                    }
                    free(ast->u.switch_.cases[i].destructure_field_names);
                }
            }
            free(ast->u.switch_.cases);
            ast_free(ast->u.switch_.default_body);
            break;
        case AST_FUNC_DEF:
            free(ast->u.func.name);
            free(ast->u.func.ref.name);
            free(ast->u.func.ref.struct_name);
            for (int i = 0; i < ast->u.func.pcnt; i++) {
                if (ast->u.func.params && ast->u.func.params[i])
                    free(ast->u.func.params[i]);
                if (ast->u.func.param_types && ast->u.func.param_types[i])
                    type_free(ast->u.func.param_types[i]);
                if (ast->u.func.param_defaults && ast->u.func.param_defaults[i])
                    ast_free(ast->u.func.param_defaults[i]);
            }
            free(ast->u.func.params);
            free(ast->u.func.param_types);
            free(ast->u.func.param_defaults);
            if (ast->u.func.return_type) type_free(ast->u.func.return_type);
            ast_free(ast->u.func.body);
            // 释放闭包信息
            if (ast->u.func.upvalue_names) {
                for (int i = 0; i < ast->u.func.upvalue_count; i++) {
                    free(ast->u.func.upvalue_names[i]);
                }
            }
            free(ast->u.func.upvalue_names);
            free(ast->u.func.upvalue_indices);
            free(ast->u.func.upvalue_is_local);
            free(ast->u.func.upvalue_is_value_capture);
            // 泛型形参的三组平行数组（parser 分配，此前漏释放）
            if (ast->u.func.type_params) {
                for (int i = 0; i < ast->u.func.type_param_count; i++) {
                    free(ast->u.func.type_params[i]);
                }
                free(ast->u.func.type_params);
            }
            if (ast->u.func.type_param_constraints) {
                for (int i = 0; i < ast->u.func.type_param_count; i++) {
                    free(ast->u.func.type_param_constraints[i]);
                }
                free(ast->u.func.type_param_constraints);
            }
            if (ast->u.func.type_param_defaults) {
                for (int i = 0; i < ast->u.func.type_param_count; i++) {
                    free(ast->u.func.type_param_defaults[i]);
                }
                free(ast->u.func.type_param_defaults);
            }
            break;
case AST_RETURN:
ast_free(ast->u.ret);
break;
case AST_RETURN_MULTI:
for (int i = 0; i < ast->u.ret_multi.count; i++) {
ast_free(ast->u.ret_multi.exprs[i]);
}
free(ast->u.ret_multi.exprs);
break;
        case AST_ASSIGN:
            for (int i = 0; i < ast->u.assign.name_count; i++) {
                free(ast->u.assign.names[i]);
                free(ast->u.assign.refs[i].name);
                free(ast->u.assign.refs[i].struct_name);
            }
            free(ast->u.assign.names);
            free(ast->u.assign.refs);
            // 释放 targets 数组中的 AST 节点
            // ★ targets 全部由 parser 为本次赋值**新建**（parser_expr.c 的单赋值、
            //   parser_func.c 的并行赋值），没有任何外部共享 ⇒ 必须走 ast_free 递归释放。
            //   此前只 `free(target)`：AST_VAR 目标的 u.var.name / ref.name、以及
            //   `arr[i] = v` 这类 INDEX 目标的 obj/index 子树全部泄漏（LeakSanitizer
            //   实测每个简单赋值漏掉一个标识符字符串）。
            if (ast->u.assign.targets) {
                for (int i = 0; i < ast->u.assign.name_count; i++) {
                    if (ast->u.assign.targets[i]) {
                        ast_free(ast->u.assign.targets[i]);
                    }
                }
                free(ast->u.assign.targets);
            }
            ast_free(ast->u.assign.value);
            break;
        case AST_COMPOUND_ASSIGN:
            free(ast->u.compound_assign.name);
            free(ast->u.compound_assign.ref.name);
            free(ast->u.compound_assign.ref.struct_name);
            ast_free(ast->u.compound_assign.value);
            break;
    case AST_VAR_DECL:
        free(ast->u.var_decl.name);
        free(ast->u.var_decl.ref.name);
        free(ast->u.var_decl.ref.struct_name);
        ast_free(ast->u.var_decl.init);
        type_free(ast->u.var_decl.type);
        break;
    case AST_DEFER:
        // parser 阶段就会把它降级成 try/finally，正常不会留到这里；
        // 留着这条是为了解析失败时的错误清理路径不泄漏
        ast_free(ast->u.defer_.expr);
        break;
    case AST_DESTRUCT_DECL: {
        for (int i = 0; i < ast->u.destruct_decl.slot_count; i++) {
            type_free(ast->u.destruct_decl.slot_types[i]);
            if (ast->u.destruct_decl.slot_keys) {
                free(ast->u.destruct_decl.slot_keys[i]);
            }
            free(ast->u.destruct_decl.names[i]);
            if (ast->u.destruct_decl.refs[i].name) {
                free(ast->u.destruct_decl.refs[i].name);
            }
            free(ast->u.destruct_decl.refs[i].struct_name);
            if (ast->u.destruct_decl.slot_defaults) {
                ast_free(ast->u.destruct_decl.slot_defaults[i]);   // 可空
            }
        }
        free(ast->u.destruct_decl.slot_defaults);
        free(ast->u.destruct_decl.slot_types);
        free(ast->u.destruct_decl.slot_keys);
        free(ast->u.destruct_decl.names);
        free(ast->u.destruct_decl.refs);
        ast_free(ast->u.destruct_decl.init);
        break;
    }
    case AST_EXPR_STMT:
            ast_free(ast->u.expr_stmt.expr);
            break;
        case AST_IMPORT:
            free(ast->u.import.module_name);
            free(ast->u.import.alias);
            free(ast->u.import.file_path);
            break;
        case AST_USE:
            free(ast->u.use.module_name);
            free(ast->u.use.symbol_name);
            break;
        case AST_EXPORT:
            ast_free(ast->u.export.decl);
            break;
        case AST_MODULE_CALL:
            free(ast->u.module_call.module_name);
            free(ast->u.module_call.method_name);
            free(ast->u.module_call.lib_ref.name);
            free(ast->u.module_call.lib_ref.struct_name);
            free_ast_list(&ast->u.module_call.args);
            if (ast->u.module_call.generic_type_args) {
                for (int i = 0; i < ast->u.module_call.generic_type_count; i++) {
                    if (ast->u.module_call.generic_type_args[i])
                        type_free(ast->u.module_call.generic_type_args[i]);
                }
                free(ast->u.module_call.generic_type_args);
            }
            if (ast->u.module_call.generic_type_names) {
                for (int i = 0; i < ast->u.module_call.generic_type_count; i++) {
                    free(ast->u.module_call.generic_type_names[i]);
                }
                free(ast->u.module_call.generic_type_names);
            }
            if (ast->u.module_call.clib_return_type)
                type_free(ast->u.module_call.clib_return_type);
            break;
        case AST_MODULE_ACCESS:
            free(ast->u.module_access.module_name);
            free(ast->u.module_access.member_name);
            free(ast->u.module_access.ref.name);
            free(ast->u.module_access.ref.struct_name);
            break;
        case AST_INTERP_STRING:
            // parts 数组大小为 count，exprs 数组大小为 count - 1
            for (int i = 0; i < ast->u.interp_string.count; i++) {
                free(ast->u.interp_string.parts[i]);
            }
            for (int i = 0; i < ast->u.interp_string.count - 1; i++) {
                ast_free(ast->u.interp_string.exprs[i]);
            }
            free(ast->u.interp_string.parts);
            free(ast->u.interp_string.exprs);
            break;
        case AST_TRY:
            ast_free(ast->u.try_.try_body);
            free(ast->u.try_.catch_var);
            free(ast->u.try_.catch_var_ref.name);
            free(ast->u.try_.catch_var_ref.struct_name);
            ast_free(ast->u.try_.catch_body);
            ast_free(ast->u.try_.finally_body);
            break;
        case AST_THROW:
            ast_free(ast->u.throw_.expr);
            break;
        case AST_TYPE_CHECK:
        case AST_AS_CAST:
            ast_free(ast->u.type_check.expr);
            type_free(ast->u.type_check.type);
            break;
        case AST_RANGE:
            ast_free(ast->u.range.start);
            ast_free(ast->u.range.end);
            break;
        case AST_SLICE:
            ast_free(ast->u.slice.obj);
            ast_free(ast->u.slice.start);
            ast_free(ast->u.slice.end);
            break;
        case AST_EXTEND_DEF:
            // extend 声明（2026-11-02）：方法被**移动**进目标 struct 的 methods 数组
            //   （语义层做完合并后会把这里的指针清空，避免二次释放 ✓），未合并的由这里释放
            free(ast->u.extend_def.module_name);
            free(ast->u.extend_def.type_name);
            for (int i = 0; i < ast->u.extend_def.method_count; i++) {
                ast_free(ast->u.extend_def.methods[i]);
            }
            free(ast->u.extend_def.methods);
            break;
        case AST_STRUCT_DEF:
            free(ast->u.struct_def.name);
            for (int i = 0; i < ast->u.struct_def.field_count; i++) {
                free(ast->u.struct_def.field_names[i]);
                type_free(ast->u.struct_def.field_types[i]);
                ast_free(ast->u.struct_def.field_defaults[i]);
            }
            free(ast->u.struct_def.field_names);
            free(ast->u.struct_def.field_types);
            free(ast->u.struct_def.field_defaults);
            free(ast->u.struct_def.field_lines);     // 字段声明行数组（与 field_names 同长 ✓）
            free(ast->u.struct_def.field_private);   // pri 标志数组（与 field_names 同长 ✓）
            for (int i = 0; i < ast->u.struct_def.method_count; i++) {
                ast_free(ast->u.struct_def.methods[i]);
            }
            free(ast->u.struct_def.methods);
            for (int i = 0; i < ast->u.struct_def.impl_count; i++) {
                free(ast->u.struct_def.impl_names[i]);
            }
            free(ast->u.struct_def.impl_names);
            // impl 声明的泛型实参（parser_func.c 为每个 impl 单独分配的 TypeInfo 数组）
            if (ast->u.struct_def.impl_type_args) {
                for (int i = 0; i < ast->u.struct_def.impl_count; i++) {
                    if (!ast->u.struct_def.impl_type_args[i]) continue;
                    int argc = ast->u.struct_def.impl_type_arg_counts
                                   ? ast->u.struct_def.impl_type_arg_counts[i] : 0;
                    for (int j = 0; j < argc; j++) {
                        type_free(ast->u.struct_def.impl_type_args[i][j]);
                    }
                    free(ast->u.struct_def.impl_type_args[i]);
                }
                free(ast->u.struct_def.impl_type_args);
            }
            free(ast->u.struct_def.impl_type_arg_counts);
            // 泛型类型参数名 / 约束 / 默认值
            if (ast->u.struct_def.type_params) {
                for (int i = 0; i < ast->u.struct_def.type_param_count; i++) {
                    free(ast->u.struct_def.type_params[i]);
                    if (ast->u.struct_def.type_param_constraints)
                        free(ast->u.struct_def.type_param_constraints[i]);
                    if (ast->u.struct_def.type_param_defaults)
                        free(ast->u.struct_def.type_param_defaults[i]);
                }
            }
            free(ast->u.struct_def.type_params);
            free(ast->u.struct_def.type_param_constraints);
            free(ast->u.struct_def.type_param_defaults);
            // 释放关联常量
            for (int i = 0; i < ast->u.struct_def.const_count; i++) {
                free(ast->u.struct_def.const_names[i]);
                ast_free(ast->u.struct_def.const_values[i]);
            }
            free(ast->u.struct_def.const_names);
            free(ast->u.struct_def.const_values);
            break;
        case AST_FACE_DEF:
            free(ast->u.face_def.name);
            for (int i = 0; i < ast->u.face_def.method_count; i++) {
                free(ast->u.face_def.method_names[i]);
                if (ast->u.face_def.method_return_types[i])
                    type_free(ast->u.face_def.method_return_types[i]);
                for (int j = 0; j < ast->u.face_def.method_param_counts[i]; j++) {
                    type_free(ast->u.face_def.method_param_types[i][j]);
                }
                free(ast->u.face_def.method_param_types[i]);
            }
            free(ast->u.face_def.method_names);
            free(ast->u.face_def.method_return_types);
            free(ast->u.face_def.method_param_types);
            free(ast->u.face_def.method_param_counts);
            if (ast->u.face_def.type_params) {
                for (int i = 0; i < ast->u.face_def.type_param_count; i++) {
                    free(ast->u.face_def.type_params[i]);
                    if (ast->u.face_def.type_param_constraints)
                        free(ast->u.face_def.type_param_constraints[i]);
                }
            }
            free(ast->u.face_def.type_params);
            free(ast->u.face_def.type_param_constraints);
            break;
        case AST_CSTRUCT_DEF:
            free(ast->u.cstruct_def.name);
            for (int i = 0; i < ast->u.cstruct_def.field_count; i++) {
                free(ast->u.cstruct_def.field_names[i]);
                type_free(ast->u.cstruct_def.field_types[i]);
            }
            free(ast->u.cstruct_def.field_names);
            free(ast->u.cstruct_def.field_types);
            free(ast->u.cstruct_def.field_offsets);
            free(ast->u.cstruct_def.field_array_dims);
            free(ast->u.cstruct_def.ref.name);
            free(ast->u.cstruct_def.ref.struct_name);
            break;
        case AST_ALIAS:
            free(ast->u.alias.name);
            type_free(ast->u.alias.type);
            if (ast->u.alias.expr) ast_free(ast->u.alias.expr);
            free(ast->u.alias.ref.name);
            free(ast->u.alias.ref.struct_name);
            // 泛型别名的类型参数名（`alias MyBox[T, U] = ...` 的 T/U，parser 逐项 strdup）
            if (ast->u.alias.type_params) {
                for (int i = 0; i < ast->u.alias.type_param_count; i++) {
                    free(ast->u.alias.type_params[i]);
                }
                free(ast->u.alias.type_params);
            }
            break;

        case AST_ENUM_DEF:
            free(ast->u.enum_def.name);
            for (int i = 0; i < ast->u.enum_def.member_count; i++) {
                free(ast->u.enum_def.member_names[i]);
                // ★ 2026-11-10（P4，步 1）：带载荷成员的参数名/类型
                if (ast->u.enum_def.member_param_names && ast->u.enum_def.member_param_names[i]) {
                    for (int j = 0; j < ast->u.enum_def.member_param_counts[i]; j++) {
                        free(ast->u.enum_def.member_param_names[i][j]);
                        if (ast->u.enum_def.member_param_types[i][j]) {
                            type_free(ast->u.enum_def.member_param_types[i][j]);
                        }
                    }
                    free(ast->u.enum_def.member_param_names[i]);
                    free(ast->u.enum_def.member_param_types[i]);
                }
            }
            free(ast->u.enum_def.member_names);
            free(ast->u.enum_def.member_values);
            free(ast->u.enum_def.member_param_names);
            free(ast->u.enum_def.member_param_types);
            free(ast->u.enum_def.member_param_counts);
            free(ast->u.enum_def.member_payload_flags);
            free(ast->u.enum_def.ref.name);
            free(ast->u.enum_def.ref.struct_name);
            break;
        case AST_STRUCT_INIT:
            free(ast->u.struct_init.struct_name);
            for (int i = 0; i < ast->u.struct_init.field_count; i++) {
                free(ast->u.struct_init.field_names[i]);
                ast_free(ast->u.struct_init.field_values[i]);
            }
            free(ast->u.struct_init.field_names);
            free(ast->u.struct_init.field_values);
            // `new Box[int](...)` 的泛型实参（parser 分配，此前未释放）
            if (ast->u.struct_init.generic_type_args) {
                for (int i = 0; i < ast->u.struct_init.generic_type_count; i++) {
                    if (ast->u.struct_init.generic_type_args[i])
                        type_free(ast->u.struct_init.generic_type_args[i]);
                }
                free(ast->u.struct_init.generic_type_args);
            }
            break;
        case AST_FIELD_ACCESS:
            ast_free(ast->u.field_access.obj);
            free(ast->u.field_access.field_name);
            break;
        case AST_ADDRESS_OF:
            ast_free(ast->u.address_of.operand);
            break;
        case AST_AWAIT:
            ast_free(ast->u.await.expr);
            break;
        case AST_SAFE_ACCESS:
            ast_free(ast->u.safe_access.obj);
            free(ast->u.safe_access.name);
            for (int i = 0; i < ast->u.safe_access.args.count; i++) {
                ast_free(ast->u.safe_access.args.items[i]);
            }
            free(ast->u.safe_access.args.items);
            if (ast->u.safe_access.ref.name) free(ast->u.safe_access.ref.name);
            if (ast->u.safe_access.ref.struct_name) free(ast->u.safe_access.ref.struct_name);
            if (ast->u.safe_access.generic_type_args) {
                for (int i = 0; i < ast->u.safe_access.generic_type_count; i++) {
                    if (ast->u.safe_access.generic_type_args[i])
                        type_free(ast->u.safe_access.generic_type_args[i]);
                }
                free(ast->u.safe_access.generic_type_args);
            }
            if (ast->u.safe_access.generic_type_names) {
                for (int i = 0; i < ast->u.safe_access.generic_type_count; i++) {
                    free(ast->u.safe_access.generic_type_names[i]);
                }
                free(ast->u.safe_access.generic_type_names);
            }
            break;
        case AST_CLIB_DEF:
            free(ast->u.clib_def.name);
            for (int i = 0; i < ast->u.clib_def.func_count; i++) {
                free(ast->u.clib_def.func_names[i]);
                type_free(ast->u.clib_def.func_return_types[i]);
                for (int j = 0; j < ast->u.clib_def.func_param_counts[i]; j++) {
                    type_free(ast->u.clib_def.func_param_types[i][j]);
                }
                free(ast->u.clib_def.func_param_types[i]);
            }
            free(ast->u.clib_def.func_names);
            free(ast->u.clib_def.func_return_types);
            free(ast->u.clib_def.func_param_types);
            free(ast->u.clib_def.func_param_counts);
            free(ast->u.clib_def.ref.name);
            free(ast->u.clib_def.ref.struct_name);
            break;
        case AST_CFUNC_DECL:
            free(ast->u.cfunc_decl.name);
            for (int i = 0; i < ast->u.cfunc_decl.param_count; i++) {
                type_free(ast->u.cfunc_decl.param_types[i]);
                free(ast->u.cfunc_decl.param_names[i]);
            }
            free(ast->u.cfunc_decl.param_types);
            free(ast->u.cfunc_decl.param_names);
            if (ast->u.cfunc_decl.return_type) type_free(ast->u.cfunc_decl.return_type);
            free(ast->u.cfunc_decl.ref.name);
            free(ast->u.cfunc_decl.ref.struct_name);
            break;
        // AST_BREAK, AST_CONTINUE, AST_BOOL, AST_NULL 不需要特殊处理（没有动态分配的成员）
        default:
            break;
    }

    // 释放缓存的类型信息
    if (ast->cached_type) {
        type_free(ast->cached_type);
        ast->cached_type = NULL;
    }

    free(ast);
}

// ★ 2026-11-10（P4 enum 载荷）：`progress(done,total);finished(path)`
//   为什么要有它：运行期的 `==`（按值比较）与 `jsons.encode`（成员式输出）手里只有
//   ObjStructDef（字段名是 p0/p1），而"第 k 个槽对应哪个参数名"只在 AST 里 ⇒ 编成一段
//   可解析的短文本随 OP_STRUCT_DEF 带过去。不是带载荷 enum ⇒ NULL（编 0 个字节即可）。
char* enum_payload_signature_text(Ast* ed) {
    if (!ed || ed->kind != AST_ENUM_DEF) return NULL;
    if (!ed->u.enum_def.member_param_counts) return NULL;
    int mc = ed->u.enum_def.member_count;
    if (mc <= 0) return NULL;

    size_t cap = 64;
    for (int m = 0; m < mc; m++) {
        const char* mn = (ed->u.enum_def.member_names && ed->u.enum_def.member_names[m])
                             ? ed->u.enum_def.member_names[m] : "m";
        cap += strlen(mn) + 3;
        int pc = ed->u.enum_def.member_param_counts[m];
        for (int k = 0; k < pc; k++) {
            const char* pn = (ed->u.enum_def.member_param_names &&
                              ed->u.enum_def.member_param_names[m] &&
                              ed->u.enum_def.member_param_names[m][k])
                                 ? ed->u.enum_def.member_param_names[m][k] : "p";
            cap += strlen(pn) + 2;
        }
    }
    char* out = (char*)malloc(cap + 1);
    if (!out) return NULL;
    size_t off = 0;
    for (int m = 0; m < mc; m++) {
        const char* mn = (ed->u.enum_def.member_names && ed->u.enum_def.member_names[m])
                             ? ed->u.enum_def.member_names[m] : "m";
        int pc = ed->u.enum_def.member_param_counts[m];
        int w = snprintf(out + off, cap + 1 - off, "%s%s", m == 0 ? "" : ";", mn);
        if (w < 0) break;
        off += (size_t)w;
        if (pc <= 0) continue;
        w = snprintf(out + off, cap + 1 - off, "(");
        if (w < 0) break;
        off += (size_t)w;
        for (int k = 0; k < pc; k++) {
            const char* pn = (ed->u.enum_def.member_param_names &&
                              ed->u.enum_def.member_param_names[m] &&
                              ed->u.enum_def.member_param_names[m][k])
                                 ? ed->u.enum_def.member_param_names[m][k] : "p";
            w = snprintf(out + off, cap + 1 - off, "%s%s", k == 0 ? "" : ",", pn);
            if (w < 0) break;
            off += (size_t)w;
        }
        w = snprintf(out + off, cap + 1 - off, ")");
        if (w < 0) break;
        off += (size_t)w;
    }
    out[off < cap ? off : cap] = '\0';
    return out;
}
