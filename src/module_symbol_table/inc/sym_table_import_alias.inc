// 从模块文件读取源代码（需要外部提供或复制相关代码）
extern char* read_module_file(const char* file_path, const char* current_file);

// 递归传导 alias 底层类型依赖到当前模块符号表
// 当 use 导入 alias（如 EventHandler = func(Event):bool）时，
// alias 底层类型引用的其他类型（如 Event）也需要传导到当前模块的符号表
// （名称数组为动态数组：传入数组指针、计数与容量，追加时按需扩容，无固定上限）
static void import_alias_type_deps_to_sym_table(
    ModuleSymbolTable* table, ModuleSymbolTable* dep_table, TypeInfo* type_info,
    char*** struct_names, int* struct_name_count, int* struct_name_cap,
    char*** cstruct_names, int* cstruct_name_count, int* cstruct_name_cap,
    char*** enum_names, int* enum_name_count, int* enum_name_cap,
    char*** face_names, int* face_name_count, int* face_name_cap,
    char*** local_alias_names, TypeInfo*** local_alias_types, int* local_alias_count, int* local_alias_cap)
{
    if (!type_info || !dep_table) return;

    // 递归处理子类型
    switch (type_info->kind) {
        case TYPE_FUNCTION:
            if (type_info->param_types) {
                for (int i = 0; i < type_info->param_count; i++) {
                    import_alias_type_deps_to_sym_table(table, dep_table, type_info->param_types[i],
                        struct_names, struct_name_count, struct_name_cap,
                        cstruct_names, cstruct_name_count, cstruct_name_cap,
                        enum_names, enum_name_count, enum_name_cap,
                        face_names, face_name_count, face_name_cap,
                        local_alias_names, local_alias_types, local_alias_count, local_alias_cap);
                }
            }
            if (type_info->return_type) {
                import_alias_type_deps_to_sym_table(table, dep_table, type_info->return_type,
                    struct_names, struct_name_count, struct_name_cap,
                    cstruct_names, cstruct_name_count, cstruct_name_cap,
                    enum_names, enum_name_count, enum_name_cap,
                    face_names, face_name_count, face_name_cap,
                    local_alias_names, local_alias_types, local_alias_count, local_alias_cap);
            }
            break;
        case TYPE_ARRAY:
            if (type_info->element_type) {
                import_alias_type_deps_to_sym_table(table, dep_table, type_info->element_type,
                    struct_names, struct_name_count, struct_name_cap,
                    cstruct_names, cstruct_name_count, cstruct_name_cap,
                    enum_names, enum_name_count, enum_name_cap,
                    face_names, face_name_count, face_name_cap,
                    local_alias_names, local_alias_types, local_alias_count, local_alias_cap);
            }
            break;
        case TYPE_DICT:
            if (type_info->key_type) {
                import_alias_type_deps_to_sym_table(table, dep_table, type_info->key_type,
                    struct_names, struct_name_count, struct_name_cap,
                    cstruct_names, cstruct_name_count, cstruct_name_cap,
                    enum_names, enum_name_count, enum_name_cap,
                    face_names, face_name_count, face_name_cap,
                    local_alias_names, local_alias_types, local_alias_count, local_alias_cap);
            }
            if (type_info->value_type) {
                import_alias_type_deps_to_sym_table(table, dep_table, type_info->value_type,
                    struct_names, struct_name_count, struct_name_cap,
                    cstruct_names, cstruct_name_count, cstruct_name_cap,
                    enum_names, enum_name_count, enum_name_cap,
                    face_names, face_name_count, face_name_cap,
                    local_alias_names, local_alias_types, local_alias_count, local_alias_cap);
            }
            break;
        case TYPE_PTR_GENERIC:
            if (type_info->element_type) {
                import_alias_type_deps_to_sym_table(table, dep_table, type_info->element_type,
                    struct_names, struct_name_count, struct_name_cap,
                    cstruct_names, cstruct_name_count, cstruct_name_cap,
                    enum_names, enum_name_count, enum_name_cap,
                    face_names, face_name_count, face_name_cap,
                    local_alias_names, local_alias_types, local_alias_count, local_alias_cap);
            }
            break;
        case TYPE_STRUCT:
        case TYPE_CSTRUCT:
        case TYPE_FACE:
        case TYPE_ENUM: {
            if (!type_info->struct_name) break;
            const char* dep_name = type_info->struct_name;

            // 如果当前模块符号表已有该类型，无需重复传导
            if (module_symbol_table_find_struct(table, dep_name)) break;
            if (module_symbol_table_find_enum(table, dep_name)) break;
            if (module_symbol_table_find_face(table, dep_name)) break;
            if (module_symbol_table_find_alias(table, dep_name)) break;
            if (module_symbol_table_find_clib(table, dep_name)) break;

            // 尝试从依赖模块传导 struct/cstruct
            ModuleStructSymbol* ssym = module_symbol_table_find_struct(dep_table, dep_name);
            if (ssym) {
                module_symbol_table_add_struct(table, dep_name,
                    ssym->field_count, ssym->fields, ssym->method_count, ssym->methods,
                    ssym->is_cstruct, ssym->type_param_count, ssym->type_param_names);
                // 传导关联常量（module_symbol_table_add_struct 不含常量参数，需手动复制）
                if (ssym->const_count > 0) {
                    ModuleStructSymbol* added = module_symbol_table_find_struct(table, dep_name);
                    if (added && added->const_count == 0) {
                        added->const_count = ssym->const_count;
                        added->const_names = (char**)malloc(sizeof(char*) * ssym->const_count);
                        added->const_value_strs = (char**)calloc(ssym->const_count, sizeof(char*));
                        for (int ci = 0; ci < ssym->const_count; ci++) {
                            added->const_names[ci] = strdup(ssym->const_names[ci]);
                            if (ssym->const_value_strs && ssym->const_value_strs[ci]) {
                                added->const_value_strs[ci] = strdup(ssym->const_value_strs[ci]);
                            }
                        }
                    }
                }
                // 添加到本地名称列表（动态追加，无固定上限）
                if (ssym->is_cstruct) {
                    mod_names_push_z(cstruct_names, cstruct_name_count, cstruct_name_cap, dep_name);
                } else {
                    mod_names_push_z(struct_names, struct_name_count, struct_name_cap, dep_name);
                }
                break;
            }
            // 尝试传导 enum
            ModuleEnumSymbol* esym = module_symbol_table_find_enum(dep_table, dep_name);
            if (esym) {
                module_symbol_table_add_enum(table, dep_name,
                    esym->member_count, esym->member_names, esym->member_values);
                mod_names_push_z(enum_names, enum_name_count, enum_name_cap, dep_name);
                break;
            }
            // 尝试传导 face
            ModuleFaceSymbol* fsym = module_symbol_table_find_face(dep_table, dep_name);
            if (fsym) {
                module_symbol_table_add_face(table, dep_name,
                    fsym->method_count, fsym->methods, fsym->type_param_count);
                mod_names_push_z(face_names, face_name_count, face_name_cap, dep_name);
                break;
            }
            // 尝试传导 alias（依赖可能是另一个 alias）
            ModuleAliasSymbol* asym = module_symbol_table_find_alias(dep_table, dep_name);
            if (asym && !module_symbol_table_find_alias(table, dep_name)) {
                module_symbol_table_add_alias(table, dep_name,
                    asym->type_info ? type_copy(asym->type_info) : NULL);
                mod_alias_push(local_alias_names, local_alias_types, local_alias_count, local_alias_cap,
                    dep_name, asym->type_info ? type_copy(asym->type_info) : NULL);
                // 递归传导 alias 的底层类型依赖
                if (asym->type_info) {
                    import_alias_type_deps_to_sym_table(table, dep_table, asym->type_info,
                        struct_names, struct_name_count, struct_name_cap,
                        cstruct_names, cstruct_name_count, cstruct_name_cap,
                        enum_names, enum_name_count, enum_name_cap,
                        face_names, face_name_count, face_name_cap,
                        local_alias_names, local_alias_types, local_alias_count, local_alias_cap);
                }
                break;
            }
            // 尝试传导 clib
            ModuleClibSymbol* csym = module_symbol_table_find_clib(dep_table, dep_name);
            if (csym) {
                module_symbol_table_add_clib(table, dep_name,
                    csym->func_count, csym->funcs);
                break;
            }
            break;
        }
        default:
            break;
    }
}

// 扫描模块文件，提取所有符号信息
// 两遍扫描：第一遍收集所有 struct 名称，第二遍解析所有符号
// max_depth: 递归深度限制，防止循环 use 依赖导致栈溢出
#define MODULE_SCAN_MAX_DEPTH 32
