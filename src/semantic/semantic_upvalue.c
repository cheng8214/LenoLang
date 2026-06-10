#include "semantic_internal.h"

// ============================================================================
// Upvalue 管理
// ============================================================================

// 在函数作用域中分配局部变量索引
int allocate_local_index(Semantic* s) {
    int idx = s->local_index++;
    return idx;
}

// 查找导入的模块信息
ImportedModuleInfo* find_imported_module(Semantic* s, const char* alias) {
    for (int i = 0; i < s->imported_module_count; i++) {
        if (strcmp(s->imported_modules[i].alias, alias) == 0) {
            return &s->imported_modules[i];
        }
    }
    return NULL;
}

// 添加 upvalue 到函数，返回索引
int add_upvalue(Ast* func_ast, const char* name, int index, int is_local, int is_value_capture) {
    if (!func_ast || !name) return -1;
    
    // 首次分配（检查指针是否为NULL）
    if (func_ast->u.func.upvalue_names == NULL) {
        func_ast->u.func.upvalue_names = (char**)calloc(MAX_UPVALUES, sizeof(char*));
        func_ast->u.func.upvalue_indices = (int*)calloc(MAX_UPVALUES, sizeof(int));
        func_ast->u.func.upvalue_is_local = (int*)calloc(MAX_UPVALUES, sizeof(int));
        func_ast->u.func.upvalue_is_value_capture = (int*)calloc(MAX_UPVALUES, sizeof(int));
        func_ast->u.func.upvalue_count = 0;
    }
    
    int count = func_ast->u.func.upvalue_count;
    
    // 检查是否已存在（只比较名字，因为同一函数内同名变量应该是同一个upvalue）
    for (int i = 0; i < count; i++) {
        if (func_ast->u.func.upvalue_names[i] && 
            strcmp(func_ast->u.func.upvalue_names[i], name) == 0) {
            return i;  // 返回已存在的索引
        }
    }
    
    // 扩容检查
    if (count >= MAX_UPVALUES) {
        return -1;
    }
    
    func_ast->u.func.upvalue_names[count] = strdup(name);
    func_ast->u.func.upvalue_indices[count] = index;
    func_ast->u.func.upvalue_is_local[count] = is_local;
    func_ast->u.func.upvalue_is_value_capture[count] = is_value_capture;
    func_ast->u.func.upvalue_count = count + 1;
    
    return count;
}

// 解析变量并处理 upvalue
// 返回符号，并在需要时自动注册 upvalue
Symbol* resolve_variable_with_upvalue(Semantic* s, const char* name, SymRef* ref) {
    if (!s->current) {
        return scope_resolve(s->current, name);
    }
    
    // 先在当前作用域查找
    Symbol* local_sym = scope_resolve_local(s->current, name);
    if (local_sym) {
        ref->kind = local_sym->kind;
        ref->index = local_sym->index;
        ref->name = strdup(local_sym->name);
        ref->type_kind = local_sym->type ? local_sym->type->kind : TYPE_ANY;
        ref->struct_name = (local_sym->type && local_sym->type->struct_name) ? strdup(local_sym->type->struct_name) : NULL;
        return local_sym;
    }
    
    // 找到当前函数的函数作用域
    Scope* current_func_scope = s->current;
    while (current_func_scope && !current_func_scope->is_func) {
        current_func_scope = current_func_scope->parent;
    }
    
    // 向上层查找，确定变量在哪一层
    Scope* scope = s->current->parent;
    Scope* var_scope = NULL;  // 变量定义所在的作用域
    Symbol* target_sym = NULL;
    int func_boundary_count = 0;  // 经过的函数边界数
    
    while (scope) {
        Symbol* sym = scope_resolve_local(scope, name);
        if (sym) {
            target_sym = sym;
            var_scope = scope;
            // 如果找到的是全局变量、全局函数、native函数、模块变量或类型定义，直接返回，不作为 upvalue 处理
            if (sym->kind == SYM_GLOBAL || sym->kind == SYM_GLOBAL_FUNC || sym->kind == SYM_NATIVE || sym->kind == SYM_MODULE ||
                sym->kind == SYM_TYPE || sym->kind == SYM_STRUCT || sym->kind == SYM_CSTRUCT || sym->kind == SYM_ENUM) {
                ref->kind = sym->kind;
                ref->index = sym->index;
                ref->name = strdup(sym->name);
                ref->type_kind = sym->type ? sym->type->kind : TYPE_ANY;
                ref->struct_name = (sym->type && sym->type->struct_name) ? strdup(sym->type->struct_name) : NULL;
                return sym;
            }
            // 如果变量定义在当前函数的函数作用域中，不是 upvalue
            if (scope == current_func_scope) {
                ref->kind = target_sym->kind;
                ref->index = target_sym->index;
                ref->name = strdup(target_sym->name);
                ref->type_kind = target_sym->type ? target_sym->type->kind : TYPE_ANY;
                ref->struct_name = (target_sym->type && target_sym->type->struct_name) ? strdup(target_sym->type->struct_name) : NULL;
                return target_sym;
            }
            // 检查变量定义的作用域是否在当前函数内部（如while/for循环中）
            // 从var_scope向上查找，如果找到current_func_scope，说明变量在当前函数内
            if (var_scope && current_func_scope) {
                Scope* check_scope = var_scope->parent;
                int crossed_func = 0;
                while (check_scope) {
                    if (check_scope == current_func_scope) {
                        ref->kind = target_sym->kind;
                        ref->index = target_sym->index;
                        ref->name = strdup(target_sym->name);
                        ref->type_kind = target_sym->type ? target_sym->type->kind : TYPE_ANY;
                        ref->struct_name = (target_sym->type && target_sym->type->struct_name) ? strdup(target_sym->type->struct_name) : NULL;
                        return target_sym;
                    }
                    if (check_scope->is_func) {
                        crossed_func++;
                        break;  // 跨越了函数边界
                    }
                    check_scope = check_scope->parent;
                }
            }
            // 变量在外层函数中，需要增加 func_boundary_count
            if (scope->is_func) {
                func_boundary_count++;
            }
            break;
        }
        // 检查是否跨越函数边界
        if (scope->is_func) {
            func_boundary_count++;
        }
        scope = scope->parent;
    }
    
    // 如果变量定义在非函数作用域（如while/for循环），需要继续向上找到函数作用域
    if (var_scope && !var_scope->is_func) {
        scope = var_scope->parent;
        while (scope) {
            if (scope->is_func) {
                func_boundary_count++;
                break;
            }
            scope = scope->parent;
        }
    }
    
    if (!target_sym) {
        // 最后尝试全局查找
        Symbol* global_sym = scope_resolve(s->current, name);
        if (global_sym) {
            ref->kind = global_sym->kind;
            ref->index = global_sym->index;
            ref->name = strdup(global_sym->name);
            ref->type_kind = global_sym->type ? global_sym->type->kind : TYPE_ANY;
            ref->struct_name = (global_sym->type && global_sym->type->struct_name) ? strdup(global_sym->type->struct_name) : NULL;
            return global_sym;
        }
        return NULL;
    }
    
    // 标记为被捕获
    target_sym->is_captured = 1;
    
    // 建立 upvalue 链
    // func_boundary_count 表示变量定义在往上第几层函数中
    // 0 = 直接外层, 1 = 外两层, 等等
    
    // 计算起始函数在栈中的位置
    // start_level 是变量定义的函数层级
    // upvalue 应该从 start_level+1 开始建立（外层函数引用内层变量）
    int start_level = s->func_stack_depth - 1 - func_boundary_count;
    if (start_level < 0) start_level = 0;
    
    // 从变量定义层的下一层开始，向外到当前层，逐层建立 upvalue
    int upvalue_index = target_sym->index;
    int is_local = 1;  // 第一层引用的是局部变量
    int upvalue_added = 0;  // 标记是否添加了 upvalue
    // 如果变量在循环作用域内定义，使用值捕获
    int is_value_capture = target_sym->is_in_loop;
    
    // 从 start_level+1 开始，因为 start_level 是变量定义的函数，不需要 upvalue
    for (int i = start_level + 1; i < s->func_stack_depth; i++) {
        Ast* func = s->func_stack[i];
        int idx = add_upvalue(func, name, upvalue_index, is_local, is_value_capture);
        if (idx < 0) return NULL;
        
        // 下一层将引用这一层的 upvalue
        upvalue_index = idx;
        is_local = 0;  // 后续层引用的是 upvalue
        upvalue_added = 1;
        // 后续层继承值捕获标记
        is_value_capture = is_value_capture;
    }
    
    // 只有实际建立了 upvalue 链，才设置为 SYM_UPVALUE
    // 否则保持原始类型（局部变量或参数）
    if (upvalue_added) {
        ref->kind = SYM_UPVALUE;
        ref->index = upvalue_index;
    } else {
        ref->kind = target_sym->kind;
        ref->index = target_sym->index;
    }
    free(ref->name);
    ref->name = strdup(name);
    free(ref->struct_name);
    ref->type_kind = target_sym->type ? target_sym->type->kind : TYPE_ANY;
    ref->struct_name = (target_sym->type && target_sym->type->struct_name) ? strdup(target_sym->type->struct_name) : NULL;
    return target_sym;
}
