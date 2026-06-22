#include "semantic_internal.h"

// ============================================================================
// 函数表哈希表实现
// ============================================================================

#define FUNC_TABLE_INITIAL_CAPACITY 64    // 初始容量（必须是2的幂）
#define FUNC_TABLE_MAX_LOAD 0.75           // 最大负载因子

// 计算字符串哈希值（FNV-1a算法）
static uint32_t func_hash_string(const char* str) {
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= (unsigned char)(*str);
        hash *= 16777619;
        str++;
    }
    return hash;
}

// 初始化函数表
void func_table_init(FuncTable* table) {
    table->capacity = FUNC_TABLE_INITIAL_CAPACITY;
    table->count = 0;
    table->entries = (FuncEntry**)calloc(table->capacity, sizeof(FuncEntry*));
}

// 释放函数表
void func_table_free(FuncTable* table) {
    if (!table->entries) return;
    
    for (int i = 0; i < table->capacity; i++) {
        FuncEntry* entry = table->entries[i];
        while (entry) {
            FuncEntry* next = entry->next;
            free(entry->name);
            // 不释放 entry->func，因为大部分指向 AST 树中的节点，由 ast_free 统一释放
            // 占位符节点的泄漏很小，可接受
            free(entry);
            entry = next;
        }
    }
    free(table->entries);
    table->entries = NULL;
    table->capacity = 0;
    table->count = 0;
}

// 扩容哈希表
static void func_table_resize(FuncTable* table) {
    int old_capacity = table->capacity;
    FuncEntry** old_entries = table->entries;
    
    // 新容量翻倍
    int new_capacity = old_capacity * 2;
    FuncEntry** new_entries = (FuncEntry**)calloc(new_capacity, sizeof(FuncEntry*));
    if (!new_entries) return;  // 扩容失败，继续使用旧表
    
    // 重新哈希所有条目
    for (int i = 0; i < old_capacity; i++) {
        FuncEntry* entry = old_entries[i];
        while (entry) {
            FuncEntry* next = entry->next;
            // 计算新索引
            uint32_t hash = func_hash_string(entry->name);
            int index = hash & (new_capacity - 1);
            // 插入到新表
            entry->next = new_entries[index];
            new_entries[index] = entry;
            entry = next;
        }
    }
    
    free(old_entries);
    table->entries = new_entries;
    table->capacity = new_capacity;
}

// 添加函数到表（返回1成功，0失败）
int func_table_add(FuncTable* table, const char* name, Ast* func) {
    if (!table->entries) {
        func_table_init(table);
    }
    
    // 检查是否需要扩容
    if (table->count >= table->capacity * FUNC_TABLE_MAX_LOAD) {
        func_table_resize(table);
    }
    
    // 计算哈希索引
    uint32_t hash = func_hash_string(name);
    int index = hash & (table->capacity - 1);
    
    // 检查是否已存在
    FuncEntry* entry = table->entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            // 已存在，更新函数指针
            entry->func = func;
            return 1;
        }
        entry = entry->next;
    }
    
    // 创建新条目
    FuncEntry* new_entry = (FuncEntry*)malloc(sizeof(FuncEntry));
    if (!new_entry) return 0;
    
    new_entry->name = strdup(name);
    new_entry->func = func;
    new_entry->next = table->entries[index];
    table->entries[index] = new_entry;
    table->count++;
    
    return 1;
}

// 查找函数（O(1)）
Ast* func_table_find(FuncTable* table, const char* name) {
    if (!table->entries || table->count == 0) return NULL;
    
    uint32_t hash = func_hash_string(name);
    int index = hash & (table->capacity - 1);
    
    FuncEntry* entry = table->entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            return entry->func;
        }
        entry = entry->next;
    }
    return NULL;
}

// ============================================================================
// 公共 API
// ============================================================================

void semantic_init(Semantic* s, Ast* root) {
    s->root = root;
    s->current = NULL;
    s->root_scope = NULL;
    func_table_init(&s->func_table);
    s->current_func = NULL;
    s->func_stack_depth = 0;
    s->local_index = 0;
    s->imported_module_count = 0;
    s->is_module = 0;
    s->is_lsp_mode = 0;
    
    // 初始化 imported_modules 数组
    memset(s->imported_modules, 0, sizeof(s->imported_modules));
}

void semantic_analyze(Semantic* s, Ast* ast) {
    // 创建全局作用域
    s->current = scope_new(NULL, 0);
    s->root_scope = s->current;

    // 注册所有模块的 native 函数元信息
    native_register_all_module_metas();
    
    // 注册所有实例方法的元信息（用于编译期参数检查）
    native_register_all_instance_method_metas();

    // 将 native 函数注册到作用域
    int count;
    const NativeFunctionMeta* builtins = native_get_all_functions(&count);
    for (int i = 0; i < count; i++) {
        scope_define(s->current, builtins[i].name, SYM_NATIVE);
    }

    // 预注册所有 struct/face 定义（支持前向引用）
    if (ast && ast->kind == AST_BLOCK) {
        AstList* list = &ast->u.block;
        for (int i = 0; i < list->count; i++) {
            Ast* stmt = list->items[i];
            if (stmt->kind == AST_STRUCT_DEF) {
                // 预注册 struct 类型符号
                Symbol* sym = scope_define(s->current, stmt->u.struct_def.name, SYM_STRUCT);
                if (sym) {
                    sym->type = type_new(TYPE_STRUCT);
                    sym->type->struct_name = strdup(stmt->u.struct_def.name);
                }
                // 同时注册到全局 struct 定义表，确保 type_is_compatible 能找到
                if (stmt->u.struct_def.name && !struct_def_find(stmt->u.struct_def.name)) {
                    ObjStructDef* early_def = struct_def_new(stmt->u.struct_def.name, stmt->u.struct_def.field_count, stmt->u.struct_def.method_count);
                    if (early_def) {
                        early_def->impl_count = stmt->u.struct_def.impl_count;
                        early_def->type_param_count = stmt->u.struct_def.type_param_count;
                        if (early_def->type_param_count > 0 && stmt->u.struct_def.type_params) {
                            early_def->type_param_names = (char**)malloc(sizeof(char*) * early_def->type_param_count);
                            for (int j = 0; j < early_def->type_param_count; j++) {
                                early_def->type_param_names[j] = strdup(stmt->u.struct_def.type_params[j]);
                            }
                        }
                        if (early_def->impl_count > 0) {
                            early_def->impl_names = (char**)malloc(sizeof(char*) * early_def->impl_count);
                            for (int j = 0; j < early_def->impl_count; j++) {
                                early_def->impl_names[j] = strdup(stmt->u.struct_def.impl_names[j]);
                            }
                        }
                        for (int j = 0; j < stmt->u.struct_def.method_count; j++) {
                            early_def->methods[j].name = strdup(stmt->u.struct_def.methods[j]->u.func.name);
                        }
                        struct_def_register(early_def);
                    }
                }
            } else if (stmt->kind == AST_FACE_DEF) {
                // 预注册 face 类型符号
                Symbol* sym = scope_define(s->current, stmt->u.face_def.name, SYM_TYPE);
                if (sym) {
                    sym->type = type_new(TYPE_FACE);
                    sym->type->struct_name = strdup(stmt->u.face_def.name);
                }
                // 同时注册到全局 face 定义表
                if (stmt->u.face_def.name && !face_def_find(stmt->u.face_def.name)) {
                    ObjFaceDef* early_fdef = face_def_new(stmt->u.face_def.name, stmt->u.face_def.method_count);
                    if (early_fdef) {
                        for (int j = 0; j < stmt->u.face_def.method_count; j++) {
                            early_fdef->methods[j].name = strdup(stmt->u.face_def.method_names[j]);
                            early_fdef->methods[j].param_count = stmt->u.face_def.method_param_counts[j];
                        }
                        face_def_register(early_fdef);
                    }
                }
            } else if (stmt->kind == AST_CSTRUCT_DEF) {
                // 预注册 cstruct 类型符号
                Symbol* sym = scope_define(s->current, stmt->u.cstruct_def.name, SYM_CSTRUCT);
                if (sym) {
                    sym->type = type_new(TYPE_CSTRUCT);
                    sym->type->struct_name = strdup(stmt->u.cstruct_def.name);
                }
            } else if (stmt->kind == AST_EXPORT && stmt->u.export.decl) {
                // 预注册 export 中的 struct/face 定义
                Ast* decl = stmt->u.export.decl;
                if (decl->kind == AST_STRUCT_DEF && decl->u.struct_def.name &&
                    !scope_resolve(s->current, decl->u.struct_def.name)) {
                    Symbol* sym = scope_define(s->current, decl->u.struct_def.name, SYM_STRUCT);
                    if (sym) {
                        sym->type = type_new(TYPE_STRUCT);
                        sym->type->struct_name = strdup(decl->u.struct_def.name);
                    }
                    if (!struct_def_find(decl->u.struct_def.name)) {
                        ObjStructDef* early_def = struct_def_new(decl->u.struct_def.name, decl->u.struct_def.field_count, decl->u.struct_def.method_count);
                        if (early_def) {
                            early_def->impl_count = decl->u.struct_def.impl_count;
                            early_def->type_param_count = decl->u.struct_def.type_param_count;
                            if (early_def->type_param_count > 0 && decl->u.struct_def.type_params) {
                                early_def->type_param_names = (char**)malloc(sizeof(char*) * early_def->type_param_count);
                                for (int j = 0; j < early_def->type_param_count; j++) {
                                    early_def->type_param_names[j] = strdup(decl->u.struct_def.type_params[j]);
                                }
                            }
                            if (early_def->impl_count > 0) {
                                early_def->impl_names = (char**)malloc(sizeof(char*) * early_def->impl_count);
                                for (int j = 0; j < early_def->impl_count; j++) {
                                    early_def->impl_names[j] = strdup(decl->u.struct_def.impl_names[j]);
                                }
                            }
                            for (int j = 0; j < decl->u.struct_def.method_count; j++) {
                                early_def->methods[j].name = strdup(decl->u.struct_def.methods[j]->u.func.name);
                            }
                            struct_def_register(early_def);
                        }
                    }
                } else if (decl->kind == AST_FACE_DEF && decl->u.face_def.name &&
                           !scope_resolve(s->current, decl->u.face_def.name)) {
                    Symbol* sym = scope_define(s->current, decl->u.face_def.name, SYM_TYPE);
                    if (sym) {
                        sym->type = type_new(TYPE_FACE);
                        sym->type->struct_name = strdup(decl->u.face_def.name);
                    }
                    if (!face_def_find(decl->u.face_def.name)) {
                        ObjFaceDef* early_fdef = face_def_new(decl->u.face_def.name, decl->u.face_def.method_count);
                        if (early_fdef) {
                            for (int j = 0; j < decl->u.face_def.method_count; j++) {
                                early_fdef->methods[j].name = strdup(decl->u.face_def.method_names[j]);
                                early_fdef->methods[j].param_count = decl->u.face_def.method_param_counts[j];
                            }
                            face_def_register(early_fdef);
                        }
                    }
                }
            }
        }
    }

    // 预注册所有全局函数定义（支持前向引用）
    if (ast && ast->kind == AST_BLOCK) {
        AstList* list = &ast->u.block;
        for (int i = 0; i < list->count; i++) {
            Ast* stmt = list->items[i];
            if (stmt->kind == AST_FUNC_DEF) {
                Symbol* sym = scope_define(s->current, stmt->u.func.name, SYM_GLOBAL_FUNC);
                if (sym) {
                    stmt->u.func.ref.kind = sym->kind;
                    stmt->u.func.ref.index = sym->index;
                    stmt->u.func.ref.name = strdup(sym->name);
                }
                // 将函数添加到函数表（用于参数类型检查）
                func_table_add(&s->func_table, stmt->u.func.name, stmt);
            }
        }
    }

    // 单遍遍历
    if (ast && ast->kind == AST_BLOCK) {
        visit_list(s, &ast->u.block);
    } else {
        visit(s, ast);
    }
}

void semantic_cleanup(Semantic* s) {
    // 注意：AST 类型缓存在 AST 节点被释放时会自动清理
    // 这里只清理函数表和其他资源
    func_table_free(&s->func_table);
    
    for (int i = 0; i < s->imported_module_count; i++) {
        free(s->imported_modules[i].alias);
        free(s->imported_modules[i].file_path);
        // 释放模块符号表
        if (s->imported_modules[i].sym_table) {
            module_symbol_table_destroy(s->imported_modules[i].sym_table);
            s->imported_modules[i].sym_table = NULL;
        }
    }
    s->imported_module_count = 0;

    s->root_scope = NULL;
    s->current = NULL;
    s->is_module = 0;
    s->is_lsp_mode = 0;
}

// 模块语义分析 - 将模块级别的符号标记为 SYM_MODULE
void semantic_analyze_module(Semantic* s, Ast* ast) {
    // 标记为模块模式
    s->is_module = 1;

    // 创建模块作用域
    s->current = scope_new(NULL, 0);
    s->root_scope = s->current;
    
    // 初始化函数表
    func_table_init(&s->func_table);

    // 注册所有模块的 native 函数元信息
    native_register_all_module_metas();
    
    // 注册所有实例方法的元信息（用于编译期参数检查）
    native_register_all_instance_method_metas();

    // 将 native 函数注册到作用域
    int count;
    const NativeFunctionMeta* builtins = native_get_all_functions(&count);
    for (int i = 0; i < count; i++) {
        scope_define(s->current, builtins[i].name, SYM_NATIVE);
    }

    // 预注册所有模块类型定义（struct/cstruct/face）和函数定义（使用 SYM_MODULE 类型）
    // 同时提前注册 struct/face 到全局定义表，确保函数体中能通过 struct_def_find/face_def_find 找到
    if (ast && ast->kind == AST_BLOCK) {
        AstList* list = &ast->u.block;
        for (int i = 0; i < list->count; i++) {
            Ast* stmt = list->items[i];
            if (!stmt) continue;

            if (stmt->kind == AST_STRUCT_DEF) {
                if (stmt->u.struct_def.name && !scope_resolve(s->current, stmt->u.struct_def.name)) {
                    Symbol* sym = scope_define(s->current, stmt->u.struct_def.name, SYM_STRUCT);
                    if (sym) {
                        sym->type = type_new(TYPE_STRUCT);
                        sym->type->struct_name = strdup(stmt->u.struct_def.name);
                    }
                }
                if (!struct_def_find(stmt->u.struct_def.name)) {
                    ObjStructDef* early_def = struct_def_new(stmt->u.struct_def.name, stmt->u.struct_def.field_count, stmt->u.struct_def.method_count);
                    if (early_def) {
                        early_def->impl_count = stmt->u.struct_def.impl_count;
                        if (early_def->impl_count > 0) {
                            early_def->impl_names = (char**)malloc(sizeof(char*) * early_def->impl_count);
                            for (int j = 0; j < early_def->impl_count; j++) {
                                early_def->impl_names[j] = strdup(stmt->u.struct_def.impl_names[j]);
                            }
                        }
                        for (int j = 0; j < stmt->u.struct_def.method_count; j++) {
                            early_def->methods[j].name = strdup(stmt->u.struct_def.methods[j]->u.func.name);
                        }
                        struct_def_register(early_def);
                    }
                }
            } else if (stmt->kind == AST_FACE_DEF) {
                if (stmt->u.face_def.name && !scope_resolve(s->current, stmt->u.face_def.name)) {
                    Symbol* sym = scope_define(s->current, stmt->u.face_def.name, SYM_TYPE);
                    if (sym) {
                        sym->type = type_new(TYPE_FACE);
                        sym->type->struct_name = strdup(stmt->u.face_def.name);
                    }
                }
                if (!face_def_find(stmt->u.face_def.name)) {
                    ObjFaceDef* early_fdef = face_def_new(stmt->u.face_def.name, stmt->u.face_def.method_count);
                    if (early_fdef) {
                        for (int j = 0; j < stmt->u.face_def.method_count; j++) {
                            early_fdef->methods[j].name = strdup(stmt->u.face_def.method_names[j]);
                            early_fdef->methods[j].param_count = stmt->u.face_def.method_param_counts[j];
                        }
                        face_def_register(early_fdef);
                    }
                }
            } else if (stmt->kind == AST_CSTRUCT_DEF) {
                if (stmt->u.cstruct_def.name && !scope_resolve(s->current, stmt->u.cstruct_def.name)) {
                    Symbol* sym = scope_define(s->current, stmt->u.cstruct_def.name, SYM_CSTRUCT);
                    if (sym) {
                        sym->type = type_new(TYPE_CSTRUCT);
                        sym->type->struct_name = strdup(stmt->u.cstruct_def.name);
                    }
                }
            } else if (stmt->kind == AST_EXPORT && stmt->u.export.decl) {
                Ast* decl = stmt->u.export.decl;
                if (decl->kind == AST_STRUCT_DEF && decl->u.struct_def.name &&
                    !scope_resolve(s->current, decl->u.struct_def.name)) {
                    Symbol* sym = scope_define(s->current, decl->u.struct_def.name, SYM_STRUCT);
                    if (sym) {
                        sym->type = type_new(TYPE_STRUCT);
                        sym->type->struct_name = strdup(decl->u.struct_def.name);
                    }
                    if (!struct_def_find(decl->u.struct_def.name)) {
                        ObjStructDef* early_def = struct_def_new(decl->u.struct_def.name, decl->u.struct_def.field_count, decl->u.struct_def.method_count);
                        if (early_def) {
                            early_def->impl_count = decl->u.struct_def.impl_count;
                            early_def->type_param_count = decl->u.struct_def.type_param_count;
                            if (early_def->type_param_count > 0 && decl->u.struct_def.type_params) {
                                early_def->type_param_names = (char**)malloc(sizeof(char*) * early_def->type_param_count);
                                for (int j = 0; j < early_def->type_param_count; j++) {
                                    early_def->type_param_names[j] = strdup(decl->u.struct_def.type_params[j]);
                                }
                            }
                            if (early_def->impl_count > 0) {
                                early_def->impl_names = (char**)malloc(sizeof(char*) * early_def->impl_count);
                                for (int j = 0; j < early_def->impl_count; j++) {
                                    early_def->impl_names[j] = strdup(decl->u.struct_def.impl_names[j]);
                                }
                            }
                            for (int j = 0; j < decl->u.struct_def.method_count; j++) {
                                early_def->methods[j].name = strdup(decl->u.struct_def.methods[j]->u.func.name);
                            }
                            struct_def_register(early_def);
                        }
                    }
                } else if (decl->kind == AST_FACE_DEF && decl->u.face_def.name &&
                           !scope_resolve(s->current, decl->u.face_def.name)) {
                    Symbol* sym = scope_define(s->current, decl->u.face_def.name, SYM_TYPE);
                    if (sym) {
                        sym->type = type_new(TYPE_FACE);
                        sym->type->struct_name = strdup(decl->u.face_def.name);
                    }
                    if (!face_def_find(decl->u.face_def.name)) {
                        ObjFaceDef* early_fdef = face_def_new(decl->u.face_def.name, decl->u.face_def.method_count);
                        if (early_fdef) {
                            for (int j = 0; j < decl->u.face_def.method_count; j++) {
                                early_fdef->methods[j].name = strdup(decl->u.face_def.method_names[j]);
                                early_fdef->methods[j].param_count = decl->u.face_def.method_param_counts[j];
                            }
                            face_def_register(early_fdef);
                        }
                    }
                } else if (decl->kind == AST_CSTRUCT_DEF && decl->u.cstruct_def.name &&
                           !scope_resolve(s->current, decl->u.cstruct_def.name)) {
                    Symbol* sym = scope_define(s->current, decl->u.cstruct_def.name, SYM_CSTRUCT);
                    if (sym) {
                        sym->type = type_new(TYPE_CSTRUCT);
                        sym->type->struct_name = strdup(decl->u.cstruct_def.name);
                    }
                } else if (decl->kind == AST_FUNC_DEF && decl->u.func.name) {
                    Symbol* sym = scope_define(s->current, decl->u.func.name, SYM_MODULE);
                    if (sym) {
                        decl->u.func.ref.kind = sym->kind;
                        decl->u.func.ref.index = sym->index;
                        decl->u.func.ref.name = strdup(sym->name);
                        sym->type = type_function(NULL, NULL, 0);
                    }
                }
            } else if (stmt->kind == AST_FUNC_DEF) {
                if (stmt->u.func.name) {
                    Symbol* sym = scope_define(s->current, stmt->u.func.name, SYM_MODULE);
                    if (sym) {
                        stmt->u.func.ref.kind = sym->kind;
                        stmt->u.func.ref.index = sym->index;
                        stmt->u.func.ref.name = strdup(sym->name);
                        sym->type = type_function(NULL, NULL, 0);
                    }
                }
            }
        }
    }

    // 单遍遍历
    if (ast && ast->kind == AST_BLOCK) {
        visit_list(s, &ast->u.block);
    } else {
        visit(s, ast);
    }
}
