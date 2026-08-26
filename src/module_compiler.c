#include "include/lenolang.h"
#include "include/leno_ast.h"
#include "include/leno_parser.h"
#include "include/leno_semantic.h"
#include "include/module_compiler.h"
#include "codegen/codegen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_EXPORTS 256

// 编译模块 - 新实现
// 模块被编译为函数对象，存储在模块的导出表中
ObjModule* compile_module_new(const char* source, const char* module_name,
                               char export_names[][MAX_EXPORT_NAME_LEN], int export_count) {
    // 1. 词法分析 + 语法分析
    // 记录编译前的错误数量，用于区分本模块新增的错误和前序模块遗留的错误
    int errors_before_parse = errors.count;
    Parser parser;
    parser_init(&parser, source);
    if (parser_parse(&parser) < 0) {
        // 只打印本模块解析阶段新增的错误，避免重复打印前序模块的错误
        int saved_count = errors.count;
        errors.count = errors_before_parse;
        error_print_all();
        errors.count = saved_count;
        ast_free(parser.root);
        return NULL;
    }

    // 2. 语义分析（模块模式）
    int errors_before_semantic = errors.count;
    Semantic sem;
    semantic_init(&sem, parser.root);
    semantic_analyze_module(&sem, parser.root);
    // 只关注本模块语义分析新增的错误，不受前序模块错误影响
    if (errors.count > errors_before_semantic) {
        ast_free(parser.root);
        semantic_cleanup(&sem);
        return NULL;
    }

    // 3. 创建模块对象
    ObjModule* module = module_new(module_name);
    if (!module) {
        ast_free(parser.root);
        semantic_cleanup(&sem);
        return NULL;
    }

    // 3.5 收集原生模块引用（从 AST 的 import 语句中）
    {
        int count = 0;
        char** names = NULL;
        if (parser.root && parser.root->kind == AST_BLOCK) {
            for (int j = 0; j < parser.root->u.block.count; j++) {
                Ast* stmt = parser.root->u.block.items[j];
                if (stmt->kind == AST_IMPORT && !strstr(stmt->u.import.module_name, ".leno")) {
                    char** new_names = realloc(names, (count + 1) * sizeof(char*));
                    if (new_names) {
                        names = new_names;
                        names[count] = strdup(stmt->u.import.module_name);
                        count++;
                    }
                }
            }
        }
        module->native_imports = names;
        module->native_import_count = count;
    }

    // 3.6 收集 use 导入的类型名称（运行时需要 re-export 到模块的 exports）
    {
        int reexport_count = 0;
        char** reexport_names = NULL;
        int* reexport_kinds = NULL;
        if (parser.root && parser.root->kind == AST_BLOCK) {
            for (int j = 0; j < parser.root->u.block.count; j++) {
                Ast* stmt = parser.root->u.block.items[j];
                if (stmt->kind == AST_USE) {
                    const char* symbol_name = stmt->u.use.symbol_name;
                    // 检查该名称是否是需要 re-export 的类型（enum/struct/cstruct）
                    Symbol* sym = scope_resolve(sem.root_scope, symbol_name);
                    if (sym && sym->type) {
                        TypeKind kind = sym->type->kind;
                        if (kind == TYPE_ENUM || kind == TYPE_STRUCT) {
                            char** new_names = realloc(reexport_names, (reexport_count + 1) * sizeof(char*));
                            int* new_kinds = realloc(reexport_kinds, (reexport_count + 1) * sizeof(int));
                            if (new_names && new_kinds) {
                                reexport_names = new_names;
                                reexport_kinds = new_kinds;
                                reexport_names[reexport_count] = strdup(symbol_name);
                                reexport_kinds[reexport_count] = (int)kind;
                                reexport_count++;
                            }
                        }
                    }
                } else if (stmt->kind == AST_BLOCK) {
                    // 批量 use 产生的 AST_BLOCK，递归遍历子节点
                    for (int k = 0; k < stmt->u.block.count; k++) {
                        Ast* inner = stmt->u.block.items[k];
                        if (inner && inner->kind == AST_USE) {
                            const char* symbol_name = inner->u.use.symbol_name;
                            Symbol* sym = scope_resolve(sem.root_scope, symbol_name);
                            if (sym && sym->type) {
                                TypeKind kind = sym->type->kind;
                                if (kind == TYPE_ENUM || kind == TYPE_STRUCT) {
                                    char** new_names = realloc(reexport_names, (reexport_count + 1) * sizeof(char*));
                                    int* new_kinds = realloc(reexport_kinds, (reexport_count + 1) * sizeof(int));
                                    if (new_names && new_kinds) {
                                        reexport_names = new_names;
                                        reexport_kinds = new_kinds;
                                        reexport_names[reexport_count] = strdup(symbol_name);
                                        reexport_kinds[reexport_count] = (int)kind;
                                        reexport_count++;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        module->use_reexport_names = reexport_names;
        module->use_reexport_kinds = reexport_kinds;
        module->use_reexport_count = reexport_count;
    }

    // 4. 首先，为模块中的所有函数（包括内部函数和 struct 方法）创建函数对象
    ObjDict* func_dict = dict_new(16);
    
    if (parser.root && parser.root->kind == AST_BLOCK) {
        for (int j = 0; j < parser.root->u.block.count; j++) {
            Ast* stmt = parser.root->u.block.items[j];
            Ast* func_ast = NULL;
            const char* func_name = NULL;
            
            if (stmt->kind == AST_FUNC_DEF) {
                func_ast = stmt;
                func_name = stmt->u.func.name;
            } else if (stmt->kind == AST_EXPORT && stmt->u.export.decl &&
                       stmt->u.export.decl->kind == AST_FUNC_DEF) {
                func_ast = stmt->u.export.decl;
                func_name = stmt->u.export.decl->u.func.name;
            }
            
            if (func_ast && func_name) {
                Chunk func_chunk;
                chunk_init(&func_chunk);
                // 设置函数 chunk 的文件名为当前模块文件名
                const char* current_file = error_get_filename();
                if (current_file) {
                    func_chunk.filename = strdup(current_file);
                }
                
                CodeGen func_gen;
                codegen_init(&func_gen, &func_chunk, &sem);
                
                gen_func(&func_gen, func_ast);
                
                if (!error_has_any() && func_chunk.const_cnt > 0) {
                    Value func_val = func_chunk.constants[0];
                    if (val_is_obj(func_val) && val_as_obj(func_val)->type == OBJ_FUNCTION) {
                        ObjFunction* func = (ObjFunction*)val_as_obj(func_val);
                        func->module = module;
                        
                        ObjString* key = str_copy(func_name, (int)strlen(func_name));
                        dict_set(func_dict, val_obj((Object*)key), func_val);
                    }
                }
                
                codegen_cleanup(&func_gen);
                chunk_free(&func_chunk);
            }
            
            // 处理 struct 定义中的方法
            Ast* struct_def_ast = NULL;
            if (stmt->kind == AST_STRUCT_DEF) {
                struct_def_ast = stmt;
            } else if (stmt->kind == AST_EXPORT && stmt->u.export.decl &&
                       stmt->u.export.decl->kind == AST_STRUCT_DEF) {
                struct_def_ast = stmt->u.export.decl;
            }
            
            if (struct_def_ast && struct_def_ast->u.struct_def.method_count > 0) {
                const char* struct_name = struct_def_ast->u.struct_def.name;
                for (int m = 0; m < struct_def_ast->u.struct_def.method_count; m++) {
                    Ast* method_ast = struct_def_ast->u.struct_def.methods[m];
                    if (method_ast && method_ast->kind == AST_FUNC_DEF) {
                        char method_key[256];
                        snprintf(method_key, sizeof(method_key), "%s::%s", struct_name, method_ast->u.func.name);
                        
                        Chunk method_chunk;
                        chunk_init(&method_chunk);
                        // 设置方法 chunk 的文件名为当前模块文件名
                        const char* current_file = error_get_filename();
                        if (current_file) {
                            method_chunk.filename = strdup(current_file);
                        }
                        
                        CodeGen method_gen;
                        codegen_init(&method_gen, &method_chunk, &sem);
                        
                        ObjFunction* func = gen_func_proto(&method_gen, method_ast);
                        
                        if (!error_has_any() && func) {
                            func->module = module;
                            
                            Value method_val = val_obj((Object*)func);
                            ObjString* key = str_copy(method_key, (int)strlen(method_key));
                            dict_set(func_dict, val_obj((Object*)key), method_val);
                        }
                        
                        codegen_cleanup(&method_gen);
                        chunk_free(&method_chunk);
                    }
                }
            }
        }
    }
    
    // 5. 将导出的项添加到模块导出表
    for (int i = 0; i < export_count; i++) {
        ObjString* key = str_copy(export_names[i], (int)strlen(export_names[i]));
        Value func_val = dict_get(func_dict, val_obj((Object*)key));
        if (!val_is_null(func_val)) {
            dict_set(module->exports, val_obj((Object*)key), func_val);
        } else {
            dict_set(module->exports, val_obj((Object*)key), val_null());
        }
    }

    // 5.1 将 struct 方法也添加到模块导出表（key 为 StructName::methodName）
    // 这样 VM 在 OP_GET_METHOD 中可以通过模块 exports 查找方法
    {
        for (int ei = 0; ei < func_dict->capacity; ei++) {
            ObjDictEntry* entry = &func_dict->entries[ei];
            if (entry && !val_is_null(entry->key) && val_is_obj(entry->key) &&
                val_as_obj(entry->key)->type == OBJ_STRING) {
                ObjString* key = (ObjString*)val_as_obj(entry->key);
                // 只添加包含 :: 的 key（即 struct 方法）
                if (key && key->chars && strstr(key->chars, "::")) {
                    dict_set(module->exports, entry->key, entry->value);
                }
            }
        }
    }

    // 5.5 预分配模块全局变量表空间
    if (sem.root_scope && sem.root_scope->global_var_index > 0) {
        int needed_count = sem.root_scope->global_var_index;
        if (module->global_count < needed_count) {
            Value* new_globals = realloc(module->globals, needed_count * sizeof(Value));
            if (new_globals) {
                for (int k = module->global_count; k < needed_count; k++) {
                    new_globals[k] = val_null();
                }
                module->globals = new_globals;
                module->global_count = needed_count;
                if (needed_count > module->global_capacity) {
                    module->global_capacity = needed_count;
                }
            }
        }
    }

    // 6. 代码生成 - 生成模块初始化代码
    Chunk chunk;
    chunk_init(&chunk);

    CodeGen gen;
    codegen_init(&gen, &chunk, &sem);
    
    // 先将所有函数和 struct 方法添加到模块全局变量表
    for (int j = 0; j < parser.root->u.block.count; j++) {
        Ast* stmt = parser.root->u.block.items[j];
        Ast* func_ast = NULL;
        const char* func_name = NULL;
        
        if (stmt->kind == AST_FUNC_DEF) {
            func_ast = stmt;
            func_name = stmt->u.func.name;
        } else if (stmt->kind == AST_EXPORT && stmt->u.export.decl &&
                   stmt->u.export.decl->kind == AST_FUNC_DEF) {
            func_ast = stmt->u.export.decl;
            func_name = stmt->u.export.decl->u.func.name;
        }
        
        if (func_ast && func_name) {
            ObjString* key = str_copy(func_name, (int)strlen(func_name));
            Value func_val = dict_get(func_dict, val_obj((Object*)key));
            if (!val_is_null(func_val)) {
                int index = func_ast->u.func.ref.index;
                if (index >= 0 && index < 256) {
                    if (module->global_count <= index) {
                        int new_count = index + 1;
                        Value* new_globals = realloc(module->globals, new_count * sizeof(Value));
                        if (new_globals) {
                            for (int k = module->global_count; k < new_count; k++) {
                                new_globals[k] = val_null();
                            }
                            module->globals = new_globals;
                            module->global_count = new_count;
                            if (new_count > module->global_capacity) {
                                module->global_capacity = new_count;
                            }
                        }
                    }
                    if (module->globals) {
                        module->globals[index] = func_val;
                        gc_write_barrier((Object*)module, func_val);
                    }
                }
            }
        }
        
        // 处理 struct 定义中的方法
        Ast* struct_def_ast = NULL;
        if (stmt->kind == AST_STRUCT_DEF) {
            struct_def_ast = stmt;
        } else if (stmt->kind == AST_EXPORT && stmt->u.export.decl &&
                   stmt->u.export.decl->kind == AST_STRUCT_DEF) {
            struct_def_ast = stmt->u.export.decl;
        }
        
        if (struct_def_ast && struct_def_ast->u.struct_def.method_count > 0) {
            const char* struct_name = struct_def_ast->u.struct_def.name;
            for (int m = 0; m < struct_def_ast->u.struct_def.method_count; m++) {
                Ast* method_ast = struct_def_ast->u.struct_def.methods[m];
                if (method_ast && method_ast->kind == AST_FUNC_DEF) {
                    char method_key[256];
                    snprintf(method_key, sizeof(method_key), "%s::%s", struct_name, method_ast->u.func.name);
                    
                    ObjString* key = str_copy(method_key, (int)strlen(method_key));
                    Value method_val = dict_get(func_dict, val_obj((Object*)key));
                    if (!val_is_null(method_val)) {
                        int index = method_ast->u.func.ref.index;
                        if (index >= 0 && index < 256) {
                            if (module->global_count <= index) {
                                int new_count = index + 1;
                                Value* new_globals = realloc(module->globals, new_count * sizeof(Value));
                                if (new_globals) {
                                    for (int k = module->global_count; k < new_count; k++) {
                                        new_globals[k] = val_null();
                                    }
                                    module->globals = new_globals;
                                    module->global_count = new_count;
                                    if (new_count > module->global_capacity) {
                                        module->global_capacity = new_count;
                                    }
                                }
                            }
                            if (module->globals) {
                                module->globals[index] = method_val;
                                gc_write_barrier((Object*)module, method_val);
                            }
                        }
                    }
                }
            }
        }
    }

    codegen_set_func_dict(func_dict);
    codegen_set_module(module);
    int errors_before_codegen = errors.count;
    codegen_module(&gen, parser.root);
    codegen_set_func_dict(NULL);
    codegen_set_module(NULL);
    
    // 调试模式：打印模块字节码
    extern int debugMode;
    if (debugMode) {
        extern void disassembleChunk(Chunk* chunk, const char* name);
        disassembleChunk(&chunk, module_name);
    }
    
    // 只关注本模块代码生成阶段新增的错误
    if (errors.count > errors_before_codegen) {
        codegen_cleanup(&gen);
        ast_free(parser.root);
        semantic_cleanup(&sem);
        chunk_free(&chunk);
        return NULL;
    }

    // 7. 处理导出的变量（直接从 AST 获取初始值）
    for (int i = 0; i < export_count; i++) {
        ObjString* key = str_copy(export_names[i], (int)strlen(export_names[i]));
        Value current_val = dict_get(module->exports, val_obj((Object*)key));
        if (val_is_null(current_val)) {
            for (int j = 0; j < parser.root->u.block.count; j++) {
                Ast* stmt = parser.root->u.block.items[j];
                if (stmt->kind == AST_EXPORT && stmt->u.export.decl) {
                    if (stmt->u.export.decl->kind == AST_VAR_DECL) {
                        Ast* var_decl = stmt->u.export.decl;
                        if (strcmp(var_decl->u.var_decl.name, export_names[i]) == 0) {
                            Ast* init = var_decl->u.var_decl.init;
                            Value var_val = val_null();
                            if (init) {
                                switch (init->kind) {
                                    case AST_NUM:
                                        if (init->u.num.is_bigint) {
                                            var_val = val_bigint_from_string(init->u.num.bigint_str);
                                        } else if (init->u.num.is_float) {
                                            var_val = val_float(init->u.num.value);
                                        } else {
                                            var_val = val_int((int)init->u.num.value);
                                        }
                                        break;
                                    case AST_STRING:
                                        var_val = val_obj((Object*)str_copy(init->u.string.value,
                                            init->u.string.len));
                                        break;
                                    case AST_BOOL:
                                        var_val = val_bool(init->u.boolean);
                                        break;
                                    case AST_NULL:
                                        var_val = val_null();
                                        break;
                                    default:
                                        break;
                                }
                            }
                            if (!val_is_null(var_val)) {
                                dict_set(module->exports, val_obj((Object*)key), var_val);
                                int var_index = var_decl->u.var_decl.ref.index;
                                if (var_index >= 0) {
                                    if (var_index >= module->global_count) {
                                        int new_count = var_index + 1;
                                        Value* new_globals = realloc(module->globals, new_count * sizeof(Value));
                                        if (new_globals) {
                                            for (int k = module->global_count; k < new_count; k++) {
                                                new_globals[k] = val_null();
                                            }
                                            module->globals = new_globals;
                                            module->global_count = new_count;
                                            if (new_count > module->global_capacity) {
                                                module->global_capacity = new_count;
                                            }
                                        }
                                    }
                                    if (var_index < module->global_count) {
                                        module->globals[var_index] = var_val;
                                        gc_write_barrier((Object*)module, var_val);
                                    }
                                }
                            }
                            break;
                        }
                    }
                }
            }
        }
    }

    // 8. 将模块初始化字节码存储到模块对象（延迟到运行时执行，不再编译期调用 VM）
    if (chunk.len > 0) {
        Chunk* stored_chunk = (Chunk*)malloc(sizeof(Chunk));
        if (stored_chunk) {
            *stored_chunk = chunk;
            module->init_chunk = stored_chunk;
        }
    } else {
        chunk_free(&chunk);
    }

    // 9. 构建 export_mappings（导出名 -> 全局变量索引映射，供运行时更新导出值）
    {
        int mapping_count = 0;
        for (int i = 0; i < export_count; i++) {
            for (int j = 0; j < parser.root->u.block.count; j++) {
                Ast* stmt = parser.root->u.block.items[j];
                if (stmt->kind == AST_EXPORT && stmt->u.export.decl) {
                    Ast* decl = stmt->u.export.decl;
                    int global_index = -1;
                    if (decl->kind == AST_VAR_DECL && strcmp(decl->u.var_decl.name, export_names[i]) == 0) {
                        global_index = decl->u.var_decl.ref.index;
                    } else if (decl->kind == AST_DESTRUCT_DECL) {
                        // 解构声明: 检查所有槽位变量名
                        for (int k = 0; k < decl->u.destruct_decl.slot_count; k++) {
                            if (strcmp(decl->u.destruct_decl.names[k], export_names[i]) == 0) {
                                global_index = decl->u.destruct_decl.refs[k].index;
                                break;
                            }
                        }
                    } else if (decl->kind == AST_ENUM_DEF && strcmp(decl->u.enum_def.name, export_names[i]) == 0) {
                        global_index = decl->u.enum_def.ref.index;
                    } else if (decl->kind == AST_STRUCT_DEF && strcmp(decl->u.struct_def.name, export_names[i]) == 0) {
                        global_index = -2; // struct 定义通过 struct_def_find 注册，无全局变量索引
                    }
                    if (global_index != -1) {
                        mapping_count++;
                        break;
                    }
                }
            }
        }
        if (mapping_count > 0) {
            module->export_mappings = (ExportGlobalMapping*)malloc(mapping_count * sizeof(ExportGlobalMapping));
            module->export_mapping_count = 0;
            for (int i = 0; i < export_count; i++) {
                for (int j = 0; j < parser.root->u.block.count; j++) {
                    Ast* stmt = parser.root->u.block.items[j];
                    if (stmt->kind == AST_EXPORT && stmt->u.export.decl) {
                        Ast* decl = stmt->u.export.decl;
                        int global_index = -1;
                        if (decl->kind == AST_VAR_DECL && strcmp(decl->u.var_decl.name, export_names[i]) == 0) {
                            global_index = decl->u.var_decl.ref.index;
                        } else if (decl->kind == AST_DESTRUCT_DECL) {
                            // 解构声明: 检查所有槽位变量名
                            for (int k = 0; k < decl->u.destruct_decl.slot_count; k++) {
                                if (strcmp(decl->u.destruct_decl.names[k], export_names[i]) == 0) {
                                    global_index = decl->u.destruct_decl.refs[k].index;
                                    break;
                                }
                            }
                        } else if (decl->kind == AST_ENUM_DEF && strcmp(decl->u.enum_def.name, export_names[i]) == 0) {
                            global_index = decl->u.enum_def.ref.index;
                        } else if (decl->kind == AST_STRUCT_DEF && strcmp(decl->u.struct_def.name, export_names[i]) == 0) {
                            global_index = -2; // struct 定义通过 struct_def_find 注册
                        } else if (decl->kind == AST_CSTRUCT_DEF && strcmp(decl->u.cstruct_def.name, export_names[i]) == 0) {
                            global_index = decl->u.cstruct_def.ref.index; // cstruct 通过 global var 注册
                        }
                        if (global_index != -1) {
                            int mi = module->export_mapping_count;
                            module->export_mappings[mi].name = strdup(export_names[i]);
                            module->export_mappings[mi].global_index = global_index;
                            module->export_mapping_count++;
                            break;
                        }
                    }
                }
            }
        }
    }

    // 10. 释放 AST 和语义分析资源（init_chunk 已转移到模块对象，不再释放）
    codegen_cleanup(&gen);
    ast_free(parser.root);
    semantic_cleanup(&sem);
    return module;
}
