// 从模块文件读取源代码（需要外部提供或复制相关代码）
extern char* read_module_file(const char* file_path, const char* current_file);

// 递归传导 alias 底层类型依赖到当前模块符号表
// 当 use 导入 alias（如 EventHandler = func(Event):bool）时，
// alias 底层类型引用的其他类型（如 Event）也需要传导到当前模块的符号表
static void import_alias_type_deps_to_sym_table(
    ModuleSymbolTable* table, ModuleSymbolTable* dep_table, TypeInfo* type_info,
    char** struct_names, int* struct_name_count,
    char** cstruct_names, int* cstruct_name_count,
    char** enum_names, int* enum_name_count,
    char** face_names, int* face_name_count,
    char** local_alias_names, TypeInfo** local_alias_types, int* local_alias_count)
{
    if (!type_info || !dep_table) return;

    // 递归处理子类型
    switch (type_info->kind) {
        case TYPE_FUNCTION:
            if (type_info->param_types) {
                for (int i = 0; i < type_info->param_count; i++) {
                    import_alias_type_deps_to_sym_table(table, dep_table, type_info->param_types[i],
                        struct_names, struct_name_count,
                        cstruct_names, cstruct_name_count,
                        enum_names, enum_name_count,
                        face_names, face_name_count,
                        local_alias_names, local_alias_types, local_alias_count);
                }
            }
            if (type_info->return_type) {
                import_alias_type_deps_to_sym_table(table, dep_table, type_info->return_type,
                    struct_names, struct_name_count,
                    cstruct_names, cstruct_name_count,
                    enum_names, enum_name_count,
                    face_names, face_name_count,
                    local_alias_names, local_alias_types, local_alias_count);
            }
            break;
        case TYPE_ARRAY:
            if (type_info->element_type) {
                import_alias_type_deps_to_sym_table(table, dep_table, type_info->element_type,
                    struct_names, struct_name_count,
                    cstruct_names, cstruct_name_count,
                    enum_names, enum_name_count,
                    face_names, face_name_count,
                    local_alias_names, local_alias_types, local_alias_count);
            }
            break;
        case TYPE_DICT:
            if (type_info->key_type) {
                import_alias_type_deps_to_sym_table(table, dep_table, type_info->key_type,
                    struct_names, struct_name_count,
                    cstruct_names, cstruct_name_count,
                    enum_names, enum_name_count,
                    face_names, face_name_count,
                    local_alias_names, local_alias_types, local_alias_count);
            }
            if (type_info->value_type) {
                import_alias_type_deps_to_sym_table(table, dep_table, type_info->value_type,
                    struct_names, struct_name_count,
                    cstruct_names, cstruct_name_count,
                    enum_names, enum_name_count,
                    face_names, face_name_count,
                    local_alias_names, local_alias_types, local_alias_count);
            }
            break;
        case TYPE_PTR_GENERIC:
            if (type_info->element_type) {
                import_alias_type_deps_to_sym_table(table, dep_table, type_info->element_type,
                    struct_names, struct_name_count,
                    cstruct_names, cstruct_name_count,
                    enum_names, enum_name_count,
                    face_names, face_name_count,
                    local_alias_names, local_alias_types, local_alias_count);
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
                // 添加到本地名称列表
                if (ssym->is_cstruct) {
                    if (*cstruct_name_count < MOD_MAX_TYPES) {
                        cstruct_names[*cstruct_name_count] = strdup(dep_name);
                        (*cstruct_name_count)++;
                    }
                } else {
                    if (*struct_name_count < MOD_MAX_TYPES) {
                        struct_names[*struct_name_count] = strdup(dep_name);
                        (*struct_name_count)++;
                    }
                }
                break;
            }
            // 尝试传导 enum
            ModuleEnumSymbol* esym = module_symbol_table_find_enum(dep_table, dep_name);
            if (esym) {
                module_symbol_table_add_enum(table, dep_name,
                    esym->member_count, esym->member_names, esym->member_values);
                if (*enum_name_count < MOD_MAX_ENUMS) {
                    enum_names[*enum_name_count] = strdup(dep_name);
                    (*enum_name_count)++;
                }
                break;
            }
            // 尝试传导 face
            ModuleFaceSymbol* fsym = module_symbol_table_find_face(dep_table, dep_name);
            if (fsym) {
                module_symbol_table_add_face(table, dep_name,
                    fsym->method_count, fsym->methods, fsym->type_param_count);
                if (*face_name_count < MOD_MAX_TYPES) {
                    face_names[*face_name_count] = strdup(dep_name);
                    (*face_name_count)++;
                }
                break;
            }
            // 尝试传导 alias（依赖可能是另一个 alias）
            ModuleAliasSymbol* asym = module_symbol_table_find_alias(dep_table, dep_name);
            if (asym && !module_symbol_table_find_alias(table, dep_name)) {
                module_symbol_table_add_alias(table, dep_name,
                    asym->type_info ? type_copy(asym->type_info) : NULL);
                if (*local_alias_count < MOD_MAX_ALIASES) {
                    local_alias_names[*local_alias_count] = strdup(dep_name);
                    local_alias_types[*local_alias_count] = asym->type_info ? type_copy(asym->type_info) : NULL;
                    (*local_alias_count)++;
                }
                // 递归传导 alias 的底层类型依赖
                if (asym->type_info) {
                    import_alias_type_deps_to_sym_table(table, dep_table, asym->type_info,
                        struct_names, struct_name_count,
                        cstruct_names, cstruct_name_count,
                        enum_names, enum_name_count,
                        face_names, face_name_count,
                        local_alias_names, local_alias_types, local_alias_count);
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
