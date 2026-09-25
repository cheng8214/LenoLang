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
    // ---------------------------------------------------------------------------
    // ⚠ 本段已于 2026-09-25 **整段移除**（编译时间优化；体积侧在此之前已被"结构去重"吃干净）。
    //   它原来把每个顶层函数**完整编译一遍**（gen_func，含函数体）存进 func_dict，
    //   而那个字典只有三个去处，现在全都不需要了：
    //     ① 第 5 步用它把函数值写进 exports —— 2026-09-24 起 exports 只留 null 占位，
    //        真身由运行期 init_chunk 跑完后从 globals[slot] 补填（见第 5 步说明）；
    //     ② 第 6 步用它把函数值写进 module->globals[slot] —— 同期已移除
    //        （唯一那份函数体由 init_chunk 的 OP_DEFINE_MODULE_FUNC 在运行期写入）；
    //     ③ 为 struct 方法建**空壳原型**（只有 gen_func_proto、chunk 为空）存进 func_dict，
    //        供第 6 步写进方法槽位 —— 那批槽位实测只有 8 个、577 B，本次一并不要
    //        （读方法槽从"空壳函数"变成 null；两种都不可用，null 报错更响亮）。
    //   ⇒ 模块函数体现在**只被第 6 步的 codegen_module 编译一次**（原来两遍）。
    //   配套删除：func_dict 变量、codegen_set_func_dict（**dead API**，没有任何读取点）。
    //   被删掉的代码见 git 历史。
    // ---------------------------------------------------------------------------

    // 5. 将导出的项添加到模块导出表
    // ---------------------------------------------------------------------------
    // ⚠ 2026-09-24（.lenb 体积优化）：这里**只登记名字占位（null）**。
    //   原因：当初顶层函数被编译了两遍（第 4 步一份 → 写进 globals[]/exports{}；
    //   第 6 步 init_chunk 里还有一份完整体）。globals[] 里那份 76525 B + exports{}
    //   里对它的重复引用 64595 B = 141 KB（hello_window 实测），全是 init_chunk 那份
    //   的冗余拷贝 ⇒ 不再写入即省下这 141 KB（pvz 实测省 164 KB）。
    //   （第 4 步那遍编译本身后来也整段删除 —— 见上面的说明，现在模块函数体全局只有一份。）
    //   运行期怎么拿到函数值：init_chunk 执行时 OP_DEFINE_MODULE_FUNC 会把那份唯一的
    //   函数写进 module->globals[slot]，随后 OP_INIT_LENOMODULE 的"导出补填"再把它
    //   填进 exports（上面 val_is_null 判空 ⇒ 占位为 null 时正好补上）。
    //   补填依赖第 9 步 export_mappings 里新增的 **FUNC_DEF** 条目（原来只有
    //   VAR/DESTRUCT/ENUM/STRUCT/CSTRUCT）——两处 if 链都要同步加。
    //   ⚠ 已知行为差异：模块顶层若在函数定义**之前**就引用该函数值
    //     （如顶层 `var f = laterFunc`，laterFunc 在文件后面定义），补填发生在
    //     init_chunk 跑完之后 ⇒ 那一刻读到的是 null（以前是第 4 步那份预填值）。
    //     全仓（含 SDL3 lib 44 文件）扫过，没有这种写法；真要支持得把函数定义提到
    //     顶层语句之前（hoisting），那属于另一个改动。
    // ---------------------------------------------------------------------------
    for (int i = 0; i < export_count; i++) {
        ObjString* key = str_copy(export_names[i], (int)strlen(export_names[i]));
        dict_set(module->exports, val_obj((Object*)key), val_null());
    }

    // 5.1 将 struct 方法也添加到模块导出表（key 为 StructName::methodName）
    // 这样 VM 在 OP_GET_METHOD 中可以通过模块 exports 查找方法
    // ---------------------------------------------------------------------------
    // ⚠ 本段已于 2026-09-24 移除（.lenb 体积优化，实测 minilang 与 SDL3 游戏同受益）：
    //   写进 exports 的 Value 是**第 4 步 func_dict 里那个"只有原型、chunk 为空"的
    //   方法对象**（方法体在 init_chunk 里另有一份），1434 个方法 × ≈73 B ≈ 105 KB，
    //   全部进 .lenb 却**运行期从不被读取**：
    //     · struct 方法分派走实例的 struct def 方法表 —— vm.c 的 struct_method_lookup
    //       是**规则唯一来源**（op_struct.inc 的 OP_GET_METHOD struct 分支同口径）；
    //     · VM 里没有任何地方按 "类型::方法" 这个键查 exports（全仓 grep `"::"` 只
    //       出现在编译器侧：semantic / module_compiler / codegen 的 func_table）。
    //   上面两行原注释描述的"通过模块 exports 查找方法"与现行 VM 实现不符（历史遗留）；
    //   保留原文备查，但不要再据此恢复本段。
    //   ⚠ 步骤 4 当初还顺带为方法建了原型对象、由第 6 步写进 module->globals 的方法槽位
    //     （未导出方法也要占槽）—— 那半段与第 4 步一起删除（2026-09-25，见上面第 4 步说明）。
    //     （被删掉的代码见 git 历史：5.1 的 for 循环 + dict_set(module->exports, ...)）
    // ---------------------------------------------------------------------------

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
    // ---------------------------------------------------------------------------
    // ⚠ 2026-09-25：本段已整段删除（原来是"顶层函数 + struct 方法"两半，都靠第 4 步的
    //   func_dict 取值写进 module->globals[]）：
    //     · 顶层函数那半段 2026-09-24 已移除（与 init_chunk 里那份完整体重复，白占体积）；
    //     · 方法那半段随第 4 步一起删除（那批槽位实测只有 8 个、577 B，且槽位里放的
    //       只是 chunk 为空的空壳原型）。
    //   函数/方法槽位空间由 5.5 按 sem.root_scope->global_var_index 预分配；函数体的
    //   唯一那份由 init_chunk 的 OP_DEFINE_MODULE_FUNC 在运行期写进对应槽位。
    // ---------------------------------------------------------------------------

    codegen_set_module(module);
    int errors_before_codegen = errors.count;
    codegen_module(&gen, parser.root);
    codegen_set_module(NULL);
    
    // 调试模式：字节码统一输出由 main.c 在编译完成后处理
    // （此处不输出，因为缓存命中的模块不走此路径，会导致输出不完整）
    
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
                            // ★ 声明类型的种类：字面量建值时必须尊重它，否则
                            //   `export float modFloat = 1` 会在 exports 里**预登记成 int 1** ——
                            //   随后模块初始化末尾"从 globals 补填 exports"那一步看到
                            //   `val_is_null(current_val)` 为假就**跳过**了，
                            //   于是跨模块永远读到 int（本模块内部读自己的槽位是 float，
                            //   两端不一致）⇒ `t.modFloat / 2` 走 int 除法得 0（应为 0.5）。
                            //   （历史：栈式实现当年同样如此 —— 栈式已弃用，此处仅作来源注记 ✓）
                            TypeKind decl_kind = var_decl->u.var_decl.type
                                                     ? var_decl->u.var_decl.type->kind : TYPE_UNKNOWN;
                            Value var_val = val_null();
                            if (init) {
                                switch (init->kind) {
                                    case AST_NUM:
                                        if (init->u.num.is_bigint) {
                                            var_val = val_bigint_from_string(init->u.num.bigint_str);
                                        } else if (init->u.num.is_float || decl_kind == TYPE_FLOAT) {
                                            // 声明是 float ⇒ 整数字面量也按浮点存
                                            var_val = val_float(init->u.num.value);
                                        } else {
                                            // ⚠ 别写 `val_int((int)init->u.num.value)`：int 字面量只要
                                            //   超过 int32（但仍 ≤ int48 下界内，比如 3000000000）就会被
                                            //   截断/成 INT32_MIN ⇒ 跨模块读到的是 -2147483648 ✗
                                            //   （实测：`export var BIG = 3000000000` 导出成 -2147483648；
                                            //    而 3000000000000000 因为被判成 bigint 反而没事，所以这坑很隐蔽 ✓）。
                                            //   ⇒ 走 `val_int_safe`（与 vm.c 里 bigint→int 的处理同一约定 ✓，
                                            //     该处也是"只在 int32 内才压成 int" —— 这里语义不同：本处要的是
                                            //     字面量**原值**，所以直接按 int64 建值，超出 int48 自动成 bigint ✓）
                                            var_val = val_int_safe((int64_t)init->u.num.value);
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
                    } else if (decl->kind == AST_FUNC_DEF && strcmp(decl->u.func.name, export_names[i]) == 0) {
                        // ★ 导出函数：第 5 步只写了 null 占位 ⇒ 靠这里在 init_chunk 跑完
                        //   之后用 globals[slot]（OP_DEFINE_MODULE_FUNC 写入的唯一函数体）
                        //   补填 exports。槽位就是模块全局槽 ref.index（与
                        //   OP_DEFINE_MODULE_FUNC 编码的 Bx 同源）。
                        global_index = decl->u.func.ref.index;
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
                        } else if (decl->kind == AST_FUNC_DEF && strcmp(decl->u.func.name, export_names[i]) == 0) {
                            // ★ 与上面计数循环同源：导出函数靠 globals[slot] 补填 exports
                            global_index = decl->u.func.ref.index;
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
