#include "semantic_internal.h"

// ============================================================================
// struct 方法字段访问转换
// ============================================================================

// 将方法体中对字段名和方法名的访问转换为 self.字段名 / StructName::method(self, args)
// 这是实现 struct 方法的核心：方法体内可以直接写字段名访问字段，直接写方法名调用同 struct 方法
void transform_method_body(Ast* ast, char** field_names, int field_count, char** method_names, int method_count, const char* struct_name) {
    if (!ast) return;
    
    switch (ast->kind) {
        case AST_VAR: {
            // 检查是否是字段名
            for (int i = 0; i < field_count; i++) {
                if (strcmp(ast->u.var.name, field_names[i]) == 0) {
                    // 保存行号
                    int line = ast->line;

                    // 释放原节点的变量数据
                    free(ast->u.var.name);
                    if (ast->u.var.ref.name) free(ast->u.var.ref.name);

                    // 将当前节点转换为 INDEX 节点：self["field_name"]
                    ast->kind = AST_INDEX;

                    // 创建 self 变量节点
                    Ast* self_var = ast_new(AST_VAR, line);
                    self_var->u.var.name = strdup("self");
                    self_var->u.var.ref.name = strdup("self");
                    self_var->u.var.ref.kind = SYM_PARAM;  // self 是参数
                    self_var->u.var.ref.index = 0;         // self 是第一个参数
                    ast->u.index.obj = self_var;

                    // 创建字段名字符串节点
                    Ast* field_str = ast_new(AST_STRING, line);
                    field_str->u.string.value = strdup(field_names[i]);
                    field_str->u.string.len = (int)strlen(field_names[i]);
                    ast->u.index.index = field_str;

                    break;
                }
            }
            break;
        }
        case AST_ASSIGN: {
            // 先处理 value 中的字段访问（在转换前保存 value 引用）
            Ast* value = ast->u.assign.value;
            transform_method_body(value, field_names, field_count, method_names, method_count, struct_name);

            // 处理赋值左侧的字段名
            for (int i = 0; i < ast->u.assign.name_count; i++) {
                for (int j = 0; j < field_count; j++) {
                    if (strcmp(ast->u.assign.names[i], field_names[j]) == 0) {
                        // 将字段名赋值转换为 self.字段名 = value
                        // 保存原始 value 引用
                        Ast* original_value = ast->u.assign.value;

                        // 替换当前 AST 节点为 INDEX_ASSIGN
                        ast->kind = AST_INDEX_ASSIGN;

                        // 创建 self 变量节点
                        Ast* self_var = ast_new(AST_VAR, ast->line);
                        self_var->u.var.name = strdup("self");
                        self_var->u.var.ref.name = strdup("self");
                        self_var->u.var.ref.kind = SYM_PARAM;  // self 是参数
                        self_var->u.var.ref.index = 0;         // self 是第一个参数
                        ast->u.index_assign.obj = self_var;

                        // 创建字段名字符串节点
                        Ast* field_str = ast_new(AST_STRING, ast->line);
                        field_str->u.string.value = strdup(field_names[j]);
                        field_str->u.string.len = (int)strlen(field_names[j]);
                        ast->u.index_assign.index = field_str;

                        // 设置 value（已经处理过其中的字段访问）
                        ast->u.index_assign.value = original_value;

                        // 不需要 break，因为已经转换了整个赋值语句
                        return;
                    }
                }
            }
            break;
        }
        case AST_BINOP:
            transform_method_body(ast->u.binop.l, field_names, field_count, method_names, method_count, struct_name);
            transform_method_body(ast->u.binop.r, field_names, field_count, method_names, method_count, struct_name);
            break;
        case AST_UNARY:
            transform_method_body(ast->u.unary.operand, field_names, field_count, method_names, method_count, struct_name);
            break;
        case AST_CALL: {
            // 检查 callee 是否是同 struct 的方法名调用
            int is_struct_method_call = 0;
            if (ast->u.call.callee->kind == AST_VAR && struct_name) {
                const char* callee_name = ast->u.call.callee->u.var.name;
                for (int i = 0; i < method_count; i++) {
                    if (strcmp(callee_name, method_names[i]) == 0) {
                        int line = ast->u.call.callee->line;
                        // 先保存方法名，再释放 callee
                        char* saved_method_name = strdup(callee_name);
                        // 将 callee 从 AST_VAR 改为 AST_INDEX(self, "method_name")
                        // 这样 codegen 会用 OP_GET_METHOD 处理
                        free(ast->u.call.callee->u.var.name);
                        if (ast->u.call.callee->u.var.ref.name) free(ast->u.call.callee->u.var.ref.name);
                        ast->u.call.callee->kind = AST_INDEX;
                        Ast* self_var = ast_new(AST_VAR, line);
                        self_var->u.var.name = strdup("self");
                        self_var->u.var.ref.name = strdup("self");
                        self_var->u.var.ref.kind = SYM_PARAM;
                        self_var->u.var.ref.index = 0;
                        ast->u.call.callee->u.index.obj = self_var;
                        Ast* method_str = ast_new(AST_STRING, line);
                        method_str->u.string.value = saved_method_name;
                        method_str->u.string.len = (int)strlen(saved_method_name);
                        ast->u.call.callee->u.index.index = method_str;
                        is_struct_method_call = 1;
                        break;
                    }
                }
            }
            transform_method_body(ast->u.call.callee, field_names, field_count, method_names, method_count, struct_name);
            
            // 如果是同 struct 方法调用，需要将 self 作为第一个参数插入
            if (is_struct_method_call) {
                // 创建新的参数列表，self 作为第一个参数
                int new_count = ast->u.call.args.count + 1;
                Ast** new_items = (Ast**)malloc(sizeof(Ast*) * new_count);
                
                // 第一个参数是 self
                Ast* self_arg = ast_new(AST_VAR, ast->line);
                self_arg->u.var.name = strdup("self");
                self_arg->u.var.ref.name = strdup("self");
                self_arg->u.var.ref.kind = SYM_PARAM;
                self_arg->u.var.ref.index = 0;
                new_items[0] = self_arg;
                
                // 复制原有参数
                for (int i = 0; i < ast->u.call.args.count; i++) {
                    new_items[i + 1] = ast->u.call.args.items[i];
                    transform_method_body(new_items[i + 1], field_names, field_count, method_names, method_count, struct_name);
                }
                
                // 释放旧列表，使用新列表
                free(ast->u.call.args.items);
                ast->u.call.args.items = new_items;
                ast->u.call.args.count = new_count;
            } else {
                // 普通调用，正常处理参数
                for (int i = 0; i < ast->u.call.args.count; i++) {
                    transform_method_body(ast->u.call.args.items[i], field_names, field_count, method_names, method_count, struct_name);
                }
            }
            break;
        }
        case AST_INDEX:
            transform_method_body(ast->u.index.obj, field_names, field_count, method_names, method_count, struct_name);
            transform_method_body(ast->u.index.index, field_names, field_count, method_names, method_count, struct_name);
            break;
        case AST_INDEX_ASSIGN:
            transform_method_body(ast->u.index_assign.obj, field_names, field_count, method_names, method_count, struct_name);
            transform_method_body(ast->u.index_assign.index, field_names, field_count, method_names, method_count, struct_name);
            transform_method_body(ast->u.index_assign.value, field_names, field_count, method_names, method_count, struct_name);
            break;
        case AST_IF: {
            transform_method_body(ast->u.if_.cond, field_names, field_count, method_names, method_count, struct_name);
            transform_method_body(ast->u.if_.then, field_names, field_count, method_names, method_count, struct_name);
            transform_method_body(ast->u.if_.else_, field_names, field_count, method_names, method_count, struct_name);
            break;
        }
        case AST_WHILE: {
            transform_method_body(ast->u.while_.cond, field_names, field_count, method_names, method_count, struct_name);
            transform_method_body(ast->u.while_.body, field_names, field_count, method_names, method_count, struct_name);
            break;
        }
        case AST_FOR: {
            transform_method_body(ast->u.for_.start, field_names, field_count, method_names, method_count, struct_name);
            transform_method_body(ast->u.for_.end, field_names, field_count, method_names, method_count, struct_name);
            transform_method_body(ast->u.for_.step, field_names, field_count, method_names, method_count, struct_name);
            transform_method_body(ast->u.for_.body, field_names, field_count, method_names, method_count, struct_name);
            break;
        }
        case AST_BLOCK: {
            for (int i = 0; i < ast->u.block.count; i++) {
                transform_method_body(ast->u.block.items[i], field_names, field_count, method_names, method_count, struct_name);
            }
            break;
        }
        case AST_RETURN:
            transform_method_body(ast->u.ret, field_names, field_count, method_names, method_count, struct_name);
            break;
        case AST_VAR_DECL:
            transform_method_body(ast->u.var_decl.init, field_names, field_count, method_names, method_count, struct_name);
            break;
        case AST_EXPR_STMT:
            transform_method_body(ast->u.expr_stmt.expr, field_names, field_count, method_names, method_count, struct_name);
            break;
        case AST_MODULE_CALL: {
            // 检查 module_name 是否是字段名（如 scores.len() 中的 scores）
            int is_field = 0;
            for (int i = 0; i < field_count; i++) {
                if (strcmp(ast->u.module_call.module_name, field_names[i]) == 0) {
                    is_field = 1;
                    break;
                }
            }
            
            // 检查是否是同 struct 的方法调用
            int is_method = 0;
            for (int i = 0; i < method_count; i++) {
                if (strcmp(ast->u.module_call.method_name, method_names[i]) == 0) {
                    is_method = 1;
                    break;
                }
            }
            
            if (is_field) {
                // 将 field.method() 转换为 self["field"]["method"]()
                // 即：把 MODULE_CALL 转换为 CALL，callee 是 INDEX(self["field"], "method")
                int line = ast->line;
                
                // 保存原始信息
                char* field_name = strdup(ast->u.module_call.module_name);
                char* method_name = strdup(ast->u.module_call.method_name);
                int arg_count = ast->u.module_call.args.count;
                Ast** arg_items = ast->u.module_call.args.items;
                
                // 释放 module_name 和 method_name
                free(ast->u.module_call.module_name);
                free(ast->u.module_call.method_name);
                
                // 转换节点类型
                ast->kind = AST_CALL;
                
                // 创建 callee: self["field"]["method"]
                Ast* callee = ast_new(AST_INDEX, line);
                
                // 创建 self["field"]
                Ast* self_field = ast_new(AST_INDEX, line);
                Ast* self_var = ast_new(AST_VAR, line);
                self_var->u.var.name = strdup("self");
                self_var->u.var.ref.name = strdup("self");
                self_var->u.var.ref.kind = SYM_PARAM;
                self_var->u.var.ref.index = 0;
                self_field->u.index.obj = self_var;
                Ast* field_str = ast_new(AST_STRING, line);
                field_str->u.string.value = field_name;
                field_str->u.string.len = (int)strlen(field_name);
                self_field->u.index.index = field_str;

                // 创建 ["method"]
                callee->u.index.obj = self_field;
                Ast* method_str = ast_new(AST_STRING, line);
                method_str->u.string.value = method_name;
                method_str->u.string.len = (int)strlen(method_name);
                callee->u.index.index = method_str;
                
                ast->u.call.callee = callee;
                ast->u.call.args.items = arg_items;
                ast->u.call.args.count = arg_count;
                ast->u.call.args.capacity = arg_count;
                
                // 处理参数
                for (int i = 0; i < arg_count; i++) {
                    transform_method_body(ast->u.call.args.items[i], field_names, field_count, method_names, method_count, struct_name);
                }
            // 仅当 module_name 为 "self" 时才转换为同 struct 方法调用
            // 否则可能是外部模块调用（如 sm.getValue()），不应被转换为 self 调用
            } else if (is_method && strcmp(ast->u.module_call.module_name, "self") == 0) {
                // 同 struct 方法调用：method(args) -> self["method"](self, args)
                int line = ast->line;
                char* method_name = strdup(ast->u.module_call.method_name);
                int arg_count = ast->u.module_call.args.count;
                Ast** arg_items = ast->u.module_call.args.items;
                
                free(ast->u.module_call.module_name);
                free(ast->u.module_call.method_name);
                
                ast->kind = AST_CALL;
                
                // 创建 callee: self["method"]
                Ast* callee = ast_new(AST_INDEX, line);
                Ast* self_var = ast_new(AST_VAR, line);
                self_var->u.var.name = strdup("self");
                self_var->u.var.ref.name = strdup("self");
                self_var->u.var.ref.kind = SYM_PARAM;
                self_var->u.var.ref.index = 0;
                callee->u.index.obj = self_var;
                Ast* method_str = ast_new(AST_STRING, line);
                method_str->u.string.value = method_name;
                method_str->u.string.len = (int)strlen(method_name);
                callee->u.index.index = method_str;

                ast->u.call.callee = callee;
                
                // 创建新的参数列表，self 作为第一个参数
                int new_count = arg_count + 1;
                Ast** new_items = (Ast**)malloc(sizeof(Ast*) * new_count);
                
                Ast* self_arg = ast_new(AST_VAR, line);
                self_arg->u.var.name = strdup("self");
                self_arg->u.var.ref.name = strdup("self");
                self_arg->u.var.ref.kind = SYM_PARAM;
                self_arg->u.var.ref.index = 0;
                new_items[0] = self_arg;

                for (int i = 0; i < arg_count; i++) {
                    new_items[i + 1] = arg_items[i];
                    transform_method_body(new_items[i + 1], field_names, field_count, method_names, method_count, struct_name);
                }
                
                free(arg_items);
                ast->u.call.args.items = new_items;
                ast->u.call.args.count = new_count;
                ast->u.call.args.capacity = new_count;
            } else {
                // 普通模块调用，只处理参数
                for (int i = 0; i < ast->u.module_call.args.count; i++) {
                    transform_method_body(ast->u.module_call.args.items[i], field_names, field_count, method_names, method_count, struct_name);
                }
            }
            break;
        }
        case AST_COMPOUND_ASSIGN: {
            // 检查是否是字段名的复合赋值
            for (int j = 0; j < field_count; j++) {
                if (strcmp(ast->u.compound_assign.name, field_names[j]) == 0) {
                    // 将字段复合赋值转换为 self.字段名 的复合赋值
                    free(ast->u.compound_assign.name);
                    ast->u.compound_assign.name = strdup(field_names[j]);
                    // 标记为需要通过 self 访问（使用 ref.name 存储标记）
                    free(ast->u.compound_assign.ref.name);
                    ast->u.compound_assign.ref.name = strdup("__self_field__");
                    // 存储字段索引，供代码生成器使用（优化：避免运行时线性搜索）
                    ast->u.compound_assign.ref.index = j;
                    break;
                }
            }
            transform_method_body(ast->u.compound_assign.value, field_names, field_count, method_names, method_count, struct_name);
            break;
        }
        case AST_SWITCH: {
            transform_method_body(ast->u.switch_.expr, field_names, field_count, method_names, method_count, struct_name);
            for (int i = 0; i < ast->u.switch_.case_count; i++) {
                for (int j = 0; j < ast->u.switch_.cases[i].values.count; j++) {
                    transform_method_body(ast->u.switch_.cases[i].values.items[j], field_names, field_count, method_names, method_count, struct_name);
                }
                transform_method_body(ast->u.switch_.cases[i].body, field_names, field_count, method_names, method_count, struct_name);
            }
            transform_method_body(ast->u.switch_.default_body, field_names, field_count, method_names, method_count, struct_name);
            break;
        }
        case AST_TRY: {
            transform_method_body(ast->u.try_.try_body, field_names, field_count, method_names, method_count, struct_name);
            transform_method_body(ast->u.try_.catch_body, field_names, field_count, method_names, method_count, struct_name);
            transform_method_body(ast->u.try_.finally_body, field_names, field_count, method_names, method_count, struct_name);
            break;
        }
        case AST_THROW:
            transform_method_body(ast->u.throw_.expr, field_names, field_count, method_names, method_count, struct_name);
            break;
        case AST_ARRAY: {
            for (int i = 0; i < ast->u.array.count; i++) {
                transform_method_body(ast->u.array.items[i], field_names, field_count, method_names, method_count, struct_name);
            }
            break;
        }
        case AST_DICT: {
            for (int i = 0; i < ast->u.dict.count; i++) {
                transform_method_body(ast->u.dict.entries[i].value, field_names, field_count, method_names, method_count, struct_name);
            }
            break;
        }
        case AST_INTERP_STRING: {
            for (int i = 0; i < ast->u.interp_string.count - 1; i++) {
                transform_method_body(ast->u.interp_string.exprs[i], field_names, field_count, method_names, method_count, struct_name);
            }
            break;
        }
        case AST_TYPE_CHECK:
            transform_method_body(ast->u.type_check.expr, field_names, field_count, method_names, method_count, struct_name);
            break;
        case AST_MODULE_ACCESS: {
            // 检查 module_name 是否是字段名（如 head.next 中的 head）
            for (int i = 0; i < field_count; i++) {
                if (strcmp(ast->u.module_access.module_name, field_names[i]) == 0) {
                    // 将 module_access 转换为 index: self["field"].member
                    int line = ast->line;
                    char* member_name = ast->u.module_access.member_name;

                    // 释放原 module_name
                    free(ast->u.module_access.module_name);

                    // 转换为 INDEX 节点
                    ast->kind = AST_INDEX;

                    // 创建 self["field"] 作为 obj
                    Ast* self_var = ast_new(AST_VAR, line);
                    self_var->u.var.name = strdup("self");
                    self_var->u.var.ref.name = strdup("self");
                    self_var->u.var.ref.kind = SYM_PARAM;
                    self_var->u.var.ref.index = 0;

                    Ast* field_str = ast_new(AST_STRING, line);
                    field_str->u.string.value = strdup(field_names[i]);
                    field_str->u.string.len = (int)strlen(field_names[i]);

                    Ast* index_obj = ast_new(AST_INDEX, line);
                    index_obj->u.index.obj = self_var;
                    index_obj->u.index.index = field_str;

                    ast->u.index.obj = index_obj;

                    // 创建 member_name 作为 index
                    Ast* member_str = ast_new(AST_STRING, line);
                    member_str->u.string.value = member_name;
                    member_str->u.string.len = (int)strlen(member_name);
                    ast->u.index.index = member_str;

                    break;
                }
            }
            break;
        }
        case AST_FUNC_DEF:
            // 处理嵌套函数：转换函数体内的字段访问和方法调用
            transform_method_body(ast->u.func.body, field_names, field_count, method_names, method_count, struct_name);
            break;
        case AST_AWAIT:
            // 处理 await 表达式中的字段访问
            transform_method_body(ast->u.await.expr, field_names, field_count, method_names, method_count, struct_name);
            break;
        case AST_CLIB_DEF:
        default:
            break;
    }
}
