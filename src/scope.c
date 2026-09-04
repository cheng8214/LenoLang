#include "include/lenolang.h"
#include <stdlib.h>
#include <string.h>

// 全局作用域ID计数器
static int scope_id_counter = 0;

// 内部函数：添加子作用域到父作用域
static void scope_add_child(Scope* parent, Scope* child) {
    if (!parent || !child) return;
    
    // 扩容检查
    if (parent->child_count >= parent->child_capacity) {
        int new_capacity = parent->child_capacity == 0 ? 8 : parent->child_capacity * 2;
        Scope** new_children = (Scope**)realloc(parent->children, sizeof(Scope*) * new_capacity);
        if (!new_children) {
            error_add_at(ERR_RUNTIME, 0, 0, "子作用域数组扩容失败");
            return;
        }
        // 初始化新分配的内存
        for (int i = parent->child_capacity; i < new_capacity; i++) {
            new_children[i] = NULL;
        }
        parent->children = new_children;
        parent->child_capacity = new_capacity;
    }
    
    parent->children[parent->child_count++] = child;
}

Scope* scope_new(Scope* parent, int is_func) {
    Scope* scope = (Scope*)calloc(1, sizeof(Scope));
    if (!scope) {
        error_add_at(ERR_RUNTIME, 0, 0, "内存分配失败");
        return NULL;
    }

    scope->parent = parent;
    scope->is_func = is_func;
    scope->depth = parent ? parent->depth + 1 : 0;
    scope->sym_cnt = 0;
    scope->sym_capacity = 8;  // 初始容量
    scope->local_count = 0;
    scope->param_count = 0;
    
    // 作用域树初始化
    scope->children = NULL;
    scope->child_count = 0;
    scope->child_capacity = 0;
    scope->id = scope_id_counter++;

    // 分配动态符号表数组
    scope->syms = (Symbol**)calloc(scope->sym_capacity, sizeof(Symbol*));
    if (!scope->syms) {
        free(scope);
        error_add_at(ERR_RUNTIME, 0, 0, "符号表内存分配失败");
        return NULL;
    }

    // 如果是子作用域，添加到父作用域的 children 列表
    if (parent) {
        scope_add_child(parent, scope);
    }

    return scope;
}

void scope_free(Scope* scope) {
    if (!scope) return;

    // 递归释放所有子作用域
    for (int i = 0; i < scope->child_count; i++) {
        if (scope->children[i]) {
            // 先断开与父作用域的连接，避免重复释放
            scope->children[i]->parent = NULL;
            scope_free(scope->children[i]);
        }
    }

    // 释放 children 数组并置空
    free(scope->children);
    scope->children = NULL;
    scope->child_count = 0;
    scope->child_capacity = 0;

    for (int i = 0; i < scope->sym_cnt; i++) {
        Symbol* sym = scope->syms[i];
        if (sym) {
            free(sym->name);
            // 释放类型信息
            if (sym->type) {
                type_free(sym->type);
                sym->type = NULL;
            }
            // 释放字典键集合
            if (sym->dict_keys) {
                for (int j = 0; j < sym->dict_key_count; j++) {
                    free(sym->dict_keys[j]);
                }
                free(sym->dict_keys);
            }
            // 释放 cfunc 签名信息
            if (sym->cfunc_param_types) {
                for (int j = 0; j < sym->cfunc_param_count; j++) {
                    type_free(sym->cfunc_param_types[j]);
                }
                free(sym->cfunc_param_types);
            }
            if (sym->cfunc_return_type) {
                type_free(sym->cfunc_return_type);
            }
            // 释放 enum 成员信息
            if (sym->enum_value_names) {
                for (int j = 0; j < sym->enum_value_count; j++) {
                    free(sym->enum_value_names[j]);
                }
                free(sym->enum_value_names);
            }
            if (sym->enum_values) {
                free(sym->enum_values);
            }
            free(sym);
        }
    }

    // 释放符号表数组
    free(scope->syms);
    free(scope);
}

// 添加字典键到符号
void symbol_add_dict_key(Symbol* sym, const char* key) {
    if (!sym || !key) return;
    
    // 检查是否已存在
    for (int i = 0; i < sym->dict_key_count; i++) {
        if (strcmp(sym->dict_keys[i], key) == 0) {
            return;  // 已存在，不重复添加
        }
    }
    
    // 扩容
    if (sym->dict_key_count >= sym->dict_key_capacity) {
        int new_capacity = sym->dict_key_capacity == 0 ? 8 : sym->dict_key_capacity * 2;
        char** new_keys = (char**)realloc(sym->dict_keys, sizeof(char*) * new_capacity);
        if (!new_keys) return;
        sym->dict_keys = new_keys;
        sym->dict_key_capacity = new_capacity;
    }
    
    sym->dict_keys[sym->dict_key_count++] = strdup(key);
}

// 检查字典键是否存在
int symbol_has_dict_key(Symbol* sym, const char* key) {
    if (!sym || !key) return 0;
    
    for (int i = 0; i < sym->dict_key_count; i++) {
        if (strcmp(sym->dict_keys[i], key) == 0) {
            return 1;
        }
    }
    return 0;
}

Symbol* scope_define(Scope* s, const char* name, SymKind kind) {
    if (!s) {
        error_add_at(ERR_SEMANTIC, 0, 0, "作用域为空");
        return NULL;
    }
    
    if (!name) {
        error_add_at(ERR_SEMANTIC, 0, 0, "符号名称为空");
        return NULL;
    }

    // 检查重复定义（只在当前作用域）
    for (int i = 0; i < s->sym_cnt; i++) {
        if (strcmp(s->syms[i]->name, name) == 0) {
            // 重复定义错误由调用方报告，以便提供更准确的上下文信息
            return NULL;
        }
    }

    // 动态扩容符号表
    if (s->sym_cnt >= s->sym_capacity) {
        int new_capacity = s->sym_capacity * 2;
        Symbol** new_syms = (Symbol**)realloc(s->syms, sizeof(Symbol*) * new_capacity);
        if (!new_syms) {
            error_add_at(ERR_RUNTIME, 0, 0, "符号表扩容失败");
            return NULL;
        }
        // 初始化新分配的内存
        for (int i = s->sym_capacity; i < new_capacity; i++) {
            new_syms[i] = NULL;
        }
        s->syms = new_syms;
        s->sym_capacity = new_capacity;
    }

    Symbol* sym = (Symbol*)calloc(1, sizeof(Symbol));
    if (!sym) {
        error_add_at(ERR_RUNTIME, 0, 0, "内存分配失败");
        return NULL;
    }

    sym->name = strdup(name);
    sym->kind = kind;
    sym->scope = s;
    sym->is_captured = 0;
    sym->next = NULL;

    // 分配索引
    if (kind == SYM_LOCAL) {
        sym->index = s->local_count++;
    } else if (kind == SYM_PARAM) {
        sym->index = s->param_count++;
    } else if (kind == SYM_GLOBAL || kind == SYM_MODULE) {
        // 全局变量和模块变量使用独立的索引
        sym->index = s->global_var_index++;
    } else if (kind == SYM_GLOBAL_FUNC) {
        // 全局函数使用独立的索引
        sym->index = s->global_func_index++;
    } else if (kind == SYM_NATIVE || kind == SYM_TYPE || kind == SYM_STRUCT || kind == SYM_CSTRUCT || kind == SYM_ENUM || kind == SYM_FUNC_ALIAS) {
        // native 函数和类型定义不占用运行时索引
        sym->index = -1;
    } else {
        sym->index = s->sym_cnt;
    }

    s->syms[s->sym_cnt++] = sym;
    return sym;
}

Symbol* scope_resolve_local(Scope* s, const char* name) {
    if (!s) return NULL;
    
    for (int i = 0; i < s->sym_cnt; i++) {
        if (strcmp(s->syms[i]->name, name) == 0) {
            return s->syms[i];
        }
    }
    return NULL;
}

Symbol* scope_resolve(Scope* s, const char* name) {
    if (!s) return NULL;
    
    // 先在当前作用域查找
    Symbol* sym = scope_resolve_local(s, name);
    if (sym) return sym;
    
    // 向上查找（闭包处理）
    if (s->parent) {
        sym = scope_resolve(s->parent, name);
        if (sym) {
            // 标记为被捕获，但排除全局变量、native 函数和模块变量
            // 全局变量通过全局变量表访问，模块变量通过模块变量表访问，都不需要 upvalue
            // 类型定义也不需要 upvalue
            if (sym->kind != SYM_GLOBAL && sym->kind != SYM_NATIVE && sym->kind != SYM_GLOBAL_FUNC && sym->kind != SYM_MODULE &&
                sym->kind != SYM_TYPE && sym->kind != SYM_STRUCT && sym->kind != SYM_CSTRUCT && sym->kind != SYM_ENUM) {
                sym->is_captured = 1;
            }
        }
        return sym;
    }
    
    return NULL;
}

// ============================================================================
// 作用域树遍历 API
// ============================================================================

// 获取根作用域
Scope* scope_get_root(Scope* s) {
    if (!s) return NULL;
    while (s->parent) {
        s = s->parent;
    }
    return s;
}

// 获取第 n 个子作用域
Scope* scope_get_child(Scope* s, int index) {
    if (!s || index < 0 || index >= s->child_count) return NULL;
    return s->children[index];
}

// 遍历作用域树（前序遍历）
void scope_traverse_preorder(Scope* s, void (*callback)(Scope* scope, void* userdata), void* userdata) {
    if (!s || !callback) return;
    
    callback(s, userdata);
    
    for (int i = 0; i < s->child_count; i++) {
        scope_traverse_preorder(s->children[i], callback, userdata);
    }
}

// 遍历作用域树（后序遍历）
void scope_traverse_postorder(Scope* s, void (*callback)(Scope* scope, void* userdata), void* userdata) {
    if (!s || !callback) return;
    
    for (int i = 0; i < s->child_count; i++) {
        scope_traverse_postorder(s->children[i], callback, userdata);
    }
    
    callback(s, userdata);
}

// 在作用域树中查找符号（广度优先）
Symbol* scope_resolve_tree_bfs(Scope* root, const char* name) {
    if (!root || !name) return NULL;

    // 动态队列初始容量
    int queue_capacity = 256;
    Scope** queue = (Scope**)malloc(sizeof(Scope*) * queue_capacity);
    if (!queue) return NULL;

    int front = 0, rear = 0;
    queue[rear++] = root;

    while (front < rear) {
        Scope* current = queue[front++];

        // 在当前作用域查找
        Symbol* sym = scope_resolve_local(current, name);
        if (sym) {
            free(queue);
            return sym;
        }

        // 将子作用域加入队列（动态扩容）
        for (int i = 0; i < current->child_count; i++) {
            // 检查是否需要扩容
            if (rear >= queue_capacity) {
                int new_capacity = queue_capacity * 2;
                Scope** new_queue = (Scope**)realloc(queue, sizeof(Scope*) * new_capacity);
                if (!new_queue) {
                    free(queue);
                    return NULL;
                }
                queue = new_queue;
                queue_capacity = new_capacity;
            }
            queue[rear++] = current->children[i];
        }
    }

    free(queue);
    return NULL;
}

// 获取作用域的完整路径（用于调试）
char* scope_get_path(Scope* s) {
    if (!s) return strdup("<null>");

    // 先计算深度
    int depth = 0;
    Scope* current = s;
    while (current) {
        depth++;
        current = current->parent;
    }

    // 分配足够空间：每个ID最多20位数字 + "/" + \0
    int len = depth * 32;

    char* path = (char*)malloc(len);
    if (!path) return strdup("<error>");

    path[0] = '\0';
    current = s;
    char temp[32];

    // 构建路径（从根到当前）
    while (current) {
        snprintf(temp, sizeof(temp), "%d", current->id);
        // 在当前路径前插入
        char* new_path = (char*)malloc(len);
        if (current->parent) {
            snprintf(new_path, len, "%s/%s", temp, path);
        } else {
            snprintf(new_path, len, "/%s", temp);
        }
        free(path);
        path = new_path;
        current = current->parent;
    }

    return path;
}

// 打印作用域树（用于调试）
void scope_print_tree(Scope* s, int indent) {
    if (!s) return;
    
    // 打印缩进
    for (int i = 0; i < indent; i++) {
        printf("  ");
    }
    
    // 打印作用域信息
    printf("Scope #%d (depth=%d, is_func=%d, symbols=%d, children=%d)\n",
           s->id, s->depth, s->is_func, s->sym_cnt, s->child_count);
    
    // 打印符号
    for (int i = 0; i < s->sym_cnt; i++) {
        for (int j = 0; j < indent + 1; j++) {
            printf("  ");
        }
        Symbol* sym = s->syms[i];
        const char* kind_str = "UNKNOWN";
        switch (sym->kind) {
            case SYM_GLOBAL: kind_str = "GLOBAL"; break;
            case SYM_GLOBAL_FUNC: kind_str = "GLOBAL_FUNC"; break;
            case SYM_LOCAL: kind_str = "LOCAL"; break;
            case SYM_PARAM: kind_str = "PARAM"; break;
            case SYM_UPVALUE: kind_str = "UPVALUE"; break;
            case SYM_NATIVE: kind_str = "NATIVE"; break;
            case SYM_MODULE: kind_str = "MODULE"; break;
            case SYM_TYPE: kind_str = "TYPE"; break;
            case SYM_STRUCT: kind_str = "STRUCT"; break;
            case SYM_CSTRUCT: kind_str = "CSTRUCT"; break;
            case SYM_CLIB: kind_str = "CLIB"; break;
            case SYM_CFUNC: kind_str = "CFUNC"; break;
            case SYM_ENUM: kind_str = "ENUM"; break;
            case SYM_FUNC_ALIAS: kind_str = "FUNC_ALIAS"; break;
        }
        printf("- %s [%s, idx=%d, captured=%d]\n", 
               sym->name, kind_str, sym->index, sym->is_captured);
    }
    
    // 递归打印子作用域
    for (int i = 0; i < s->child_count; i++) {
        scope_print_tree(s->children[i], indent + 1);
    }
}

// 从父作用域中移除子作用域（不释放子作用域）
// 用于临时作用域，避免递归释放时重复释放
void scope_detach_child(Scope* parent, Scope* child) {
    if (!parent || !child) return;
    
    // 在 children 数组中查找并移除
    for (int i = 0; i < parent->child_count; i++) {
        if (parent->children[i] == child) {
            // 将后面的元素前移
            for (int j = i; j < parent->child_count - 1; j++) {
                parent->children[j] = parent->children[j + 1];
            }
            parent->child_count--;
            // 清除 parent 指针
            child->parent = NULL;
            return;
        }
    }
}
