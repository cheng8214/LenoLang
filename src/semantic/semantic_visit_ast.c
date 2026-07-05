#include "semantic_internal.h"

// ============================================================================
// 泛型类型推断辅助
// ============================================================================
#include "visitinc/visit_generic.inc"

// ============================================================================
// use 导入 alias 时，递归导入底层类型依赖
// 当 use 导入 alias（如 EventHandler = func(Event):bool）时，
// alias 底层类型引用的其他类型（如 Event）不会自动带入当前作用域。
// 此函数递归扫描 TypeInfo，将引用的 struct/cstruct/face/enum/alias 自动导入。
// ============================================================================
static void import_type_deps(Semantic* s, ImportedModuleInfo* module_info, TypeInfo* type_info) {
    if (!type_info || !module_info || !module_info->sym_table) return;

    // 递归处理子类型
    switch (type_info->kind) {
        case TYPE_FUNCTION:
            // 函数类型：递归处理参数类型和返回类型
            if (type_info->param_types) {
                for (int i = 0; i < type_info->param_count; i++) {
                    import_type_deps(s, module_info, type_info->param_types[i]);
                }
            }
            if (type_info->return_type) {
                import_type_deps(s, module_info, type_info->return_type);
            }
            break;
        case TYPE_ARRAY:
            if (type_info->element_type) {
                import_type_deps(s, module_info, type_info->element_type);
            }
            break;
        case TYPE_DICT:
            if (type_info->key_type) {
                import_type_deps(s, module_info, type_info->key_type);
            }
            if (type_info->value_type) {
                import_type_deps(s, module_info, type_info->value_type);
            }
            break;
        case TYPE_PTR_GENERIC:
            if (type_info->element_type) {
                import_type_deps(s, module_info, type_info->element_type);
            }
            break;
        case TYPE_STRUCT:
        case TYPE_CSTRUCT:
        case TYPE_FACE:
        case TYPE_ENUM: {
            // 这些类型有 struct_name，需要检查是否已在当前作用域
            if (!type_info->struct_name) break;
            // 如果当前作用域已有该类型，无需重复导入
            Symbol* existing = scope_resolve_local(s->current, type_info->struct_name);
            if (existing) break;

            // 从源模块符号表中查找并导入
            const char* dep_name = type_info->struct_name;

            // 尝试 struct/cstruct
            ModuleStructSymbol* ssym = module_symbol_table_find_struct(module_info->sym_table, dep_name);
            if (ssym) {
                SymKind kind = ssym->is_cstruct ? SYM_CSTRUCT : SYM_STRUCT;
                Symbol* sym = scope_define(s->current, dep_name, kind);
                if (sym) {
                    TypeKind tk = ssym->is_cstruct ? TYPE_CSTRUCT : TYPE_STRUCT;
                    sym->type = type_new(tk);
                    sym->type->struct_name = strdup(dep_name);
                    // 复制字段信息（与方法与正常 use struct 一致）
                    sym->struct_field_count = ssym->field_count;
                    sym->struct_field_names = (char**)malloc(sizeof(char*) * ssym->field_count);
                    sym->struct_field_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * ssym->field_count);
                    for (int fi = 0; fi < ssym->field_count; fi++) {
                        sym->struct_field_names[fi] = strdup(ssym->fields[fi].name);
                        sym->struct_field_types[fi] = type_new(ssym->fields[fi].type);
                        if (ssym->fields[fi].struct_name) {
                            sym->struct_field_types[fi]->struct_name = strdup(ssym->fields[fi].struct_name);
                        }
                    }
                    // 设置泛型类型参数信息
                    sym->struct_type_param_count = ssym->type_param_count;
                    if (ssym->type_param_count > 0 && ssym->type_param_names) {
                        sym->struct_type_params = (char**)malloc(sizeof(char*) * ssym->type_param_count);
                        for (int tpi = 0; tpi < ssym->type_param_count; tpi++) {
                            sym->struct_type_params[tpi] = strdup(ssym->type_param_names[tpi]);
                        }
                    }
                    // 注册 struct 定义到全局表
                    if (!ssym->is_cstruct && !struct_def_find(dep_name)) {
                        ObjStructDef* sdef = struct_def_new(dep_name, ssym->field_count, ssym->method_count);
                        if (sdef) {
                            sdef->type_param_count = ssym->type_param_count;
                            if (ssym->type_param_count > 0 && ssym->type_param_names) {
                                sdef->type_param_names = (char**)malloc(sizeof(char*) * ssym->type_param_count);
                                for (int tpi = 0; tpi < ssym->type_param_count; tpi++) {
                                    sdef->type_param_names[tpi] = strdup(ssym->type_param_names[tpi]);
                                }
                            }
                            for (int mi = 0; mi < ssym->method_count; mi++) {
                                const char* full = ssym->methods[mi].name;
                                const char* mn = full;
                                const char* sep = strstr(full, "::");
                                if (sep) mn = sep + 2;
                                sdef->methods[mi].name = strdup(mn);
                            }
                            // 设置 impl 信息
                            if (ssym->impl_count > 0) {
                                sdef->impl_count = ssym->impl_count;
                                sdef->impl_names = (char**)malloc(sizeof(char*) * ssym->impl_count);
                                for (int ii = 0; ii < ssym->impl_count; ii++) {
                                    sdef->impl_names[ii] = strdup(ssym->impl_names[ii]);
                                }
                            }
                            struct_def_register(sdef);
                        }
                    }
                    // 注册 struct 方法到 func_table
                    for (int mi = 0; mi < ssym->method_count; mi++) {
                        const char* full_method_name = ssym->methods[mi].name;
                        Ast* existing_func = func_table_find(&s->func_table, full_method_name);
                        if (!existing_func) {
                            Ast* placeholder = ast_new(AST_FUNC_DEF, 0);
                            if (placeholder) {
                                placeholder->u.func.name = strdup(full_method_name);
                                placeholder->u.func.pcnt = 1 + ssym->methods[mi].param_count;
                                placeholder->u.func.return_type = type_new(ssym->methods[mi].return_type);
                                if (ssym->methods[mi].return_struct_name) {
                                    placeholder->u.func.return_type->struct_name = strdup(ssym->methods[mi].return_struct_name);
                                }
                                func_table_add(&s->func_table, full_method_name, placeholder);
                            }
                        }
                    }
                }
                break;
            }
            // 尝试 enum
            ModuleEnumSymbol* esym = module_symbol_table_find_enum(module_info->sym_table, dep_name);
            if (esym) {
                Symbol* sym = scope_define(s->current, dep_name, SYM_TYPE);
                if (sym) {
                    sym->type = type_new(TYPE_ENUM);
                    sym->type->struct_name = strdup(dep_name);
                    sym->enum_value_count = esym->member_count;
                    sym->enum_value_names = (char**)malloc(sizeof(char*) * esym->member_count);
                    sym->enum_values = (int*)malloc(sizeof(int) * esym->member_count);
                    for (int mi = 0; mi < esym->member_count; mi++) {
                        sym->enum_value_names[mi] = strdup(esym->member_names[mi]);
                        sym->enum_values[mi] = esym->member_values[mi];
                    }
                }
                break;
            }
            // 尝试 face
            ModuleFaceSymbol* fsym = module_symbol_table_find_face(module_info->sym_table, dep_name);
            if (fsym) {
                Symbol* sym = scope_define(s->current, dep_name, SYM_TYPE);
                if (sym) {
                    sym->type = type_new(TYPE_FACE);
                    sym->type->struct_name = strdup(dep_name);
                }
                if (!face_def_find(dep_name)) {
                    ObjFaceDef* fdef = face_def_new(dep_name, fsym->method_count);
                    if (fdef) {
                        for (int mi = 0; mi < fsym->method_count; mi++) {
                            fdef->methods[mi].name = strdup(fsym->methods[mi].name);
                            fdef->methods[mi].return_type = type_new(fsym->methods[mi].return_type);
                            fdef->methods[mi].param_count = 0;
                            fdef->methods[mi].param_types = NULL;
                        }
                        face_def_register(fdef);
                    }
                }
                break;
            }
            // 尝试 alias（依赖也可能是另一个 alias）
            ModuleAliasSymbol* asym = module_symbol_table_find_alias(module_info->sym_table, dep_name);
            if (asym) {
                Symbol* sym = scope_define(s->current, dep_name, SYM_TYPE);
                if (sym && asym->type_info) {
                    sym->type = type_copy(asym->type_info);
                    // 递归导入 alias 的底层类型依赖
                    import_type_deps(s, module_info, asym->type_info);
                }
                break;
            }
            // clib 类型
            ModuleClibSymbol* csym = module_symbol_table_find_clib(module_info->sym_table, dep_name);
            if (csym) {
                Symbol* sym = scope_define(s->current, dep_name, SYM_TYPE);
                if (sym) {
                    sym->type = type_new(TYPE_CLIB);
                    sym->type->struct_name = strdup(dep_name);
                }
                break;
            }
            break;
        }
        default:
            break;
    }
}

// ============================================================================
// 访问者模式 - 单遍处理
// ============================================================================
void visit_list(Semantic* s, AstList* list);

void visit(Semantic* s, Ast* ast) {
    if (!ast) return;

    switch (ast->kind) {
        // 块处理
        #include "visitinc/visit_block.inc"

        // 函数定义
        #include "visitinc/visit_func_def.inc"

        // 变量和赋值
        #include "visitinc/visit_var.inc"

        // 控制流语句
        #include "visitinc/visit_control.inc"

        // 表达式
        #include "visitinc/visit_expr.inc"

        // 简单语句
        #include "visitinc/visit_stmt.inc"

        // 模块相关
        #include "visitinc/visit_module.inc"

        // 异常处理
        #include "visitinc/visit_exception.inc"

        // 类型检查
        #include "visitinc/visit_type_check.inc"

        // 类型定义
        #include "visitinc/visit_type_def.inc"

        // FFI 相关
        #include "visitinc/visit_ffi.inc"

        // 枚举和别名
        #include "visitinc/visit_enum.inc"

        // struct 初始化
        #include "visitinc/visit_struct_init.inc"

        // await
        #include "visitinc/visit_await.inc"

        // 字段访问
        #include "visitinc/visit_field_access.inc"

        default:
            break;
    }
}

// ============================================================================
// visit_list 函数
// ============================================================================
#include "visitinc/visit_list.inc"