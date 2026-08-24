#include "include/lenolang.h"
#include "include/leno_ast.h"
#include <stdlib.h>

Ast* ast_new(AstKind kind, int line) {
    Ast* ast = (Ast*)calloc(1, sizeof(Ast));
    if (!ast) {
        error_add_at(ERR_RUNTIME, line, 0, "内存分配失败");
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
            error_add_at(ERR_RUNTIME, 0, 0, "内存分配失败");
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
            error_add_at(ERR_RUNTIME, 0, 0, "内存分配失败");
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
            if (ast->u.if_.guard_type) {
                type_free(ast->u.if_.guard_type);
            }
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
            }
            free(ast->u.switch_.cases);
            ast_free(ast->u.switch_.default_body);
            break;
        case AST_FUNC_DEF:
            free(ast->u.func.name);
            free(ast->u.func.ref.name);
            for (int i = 0; i < ast->u.func.pcnt; i++) {
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
            break;
        case AST_RETURN:
            ast_free(ast->u.ret);
            break;
        case AST_ASSIGN:
            for (int i = 0; i < ast->u.assign.name_count; i++) {
                free(ast->u.assign.names[i]);
                free(ast->u.assign.refs[i].name);
            }
            free(ast->u.assign.names);
            free(ast->u.assign.refs);
            // 释放 targets 数组中的 AST 节点
            if (ast->u.assign.targets) {
                for (int i = 0; i < ast->u.assign.name_count; i++) {
                    if (ast->u.assign.targets[i]) {
                        // 注意：targets 中的节点不应该递归释放子节点
                        // 因为它们可能是共享的
                        free(ast->u.assign.targets[i]);
                    }
                }
                free(ast->u.assign.targets);
            }
            ast_free(ast->u.assign.value);
            break;
        case AST_COMPOUND_ASSIGN:
            free(ast->u.compound_assign.name);
            free(ast->u.compound_assign.ref.name);
            ast_free(ast->u.compound_assign.value);
            break;
        case AST_VAR_DECL:
            free(ast->u.var_decl.name);
            free(ast->u.var_decl.ref.name);
            ast_free(ast->u.var_decl.init);
            type_free(ast->u.var_decl.type);
            break;
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
            free_ast_list(&ast->u.module_call.args);
            break;
        case AST_MODULE_ACCESS:
            free(ast->u.module_access.module_name);
            free(ast->u.module_access.member_name);
            free(ast->u.module_access.ref.name);
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
            for (int i = 0; i < ast->u.struct_def.method_count; i++) {
                ast_free(ast->u.struct_def.methods[i]);
            }
            free(ast->u.struct_def.methods);
            for (int i = 0; i < ast->u.struct_def.impl_count; i++) {
                free(ast->u.struct_def.impl_names[i]);
            }
            free(ast->u.struct_def.impl_names);
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
            break;
        case AST_ALIAS:
            free(ast->u.alias.name);
            type_free(ast->u.alias.type);
            if (ast->u.alias.expr) ast_free(ast->u.alias.expr);
            free(ast->u.alias.ref.name);
            break;

        case AST_ENUM_DEF:
            free(ast->u.enum_def.name);
            for (int i = 0; i < ast->u.enum_def.member_count; i++) {
                free(ast->u.enum_def.member_names[i]);
            }
            free(ast->u.enum_def.member_names);
            free(ast->u.enum_def.member_values);
            free(ast->u.enum_def.ref.name);
            break;
        case AST_STRUCT_INIT:
            free(ast->u.struct_init.struct_name);
            for (int i = 0; i < ast->u.struct_init.field_count; i++) {
                free(ast->u.struct_init.field_names[i]);
                ast_free(ast->u.struct_init.field_values[i]);
            }
            free(ast->u.struct_init.field_names);
            free(ast->u.struct_init.field_values);
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
            if (ast->u.safe_access.generic_type_args) free(ast->u.safe_access.generic_type_args);
            if (ast->u.safe_access.generic_type_names) free(ast->u.safe_access.generic_type_names);
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
