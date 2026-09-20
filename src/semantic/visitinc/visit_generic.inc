// 从实参类型推断泛型参数的绑定
static void infer_generic_from_type(TypeInfo* generic_type, TypeInfo* concrete_type,
                                     char** param_names, TypeInfo** inferred, int count) {
    if (!generic_type || !concrete_type) return;
    
    // 如果是泛型参数，记录绑定
    if (generic_type->kind == TYPE_GENERIC_PARAM && generic_type->type_param_name) {
        for (int i = 0; i < count; i++) {
            if (strcmp(generic_type->type_param_name, param_names[i]) == 0) {
                if (!inferred[i]) {
                    inferred[i] = type_copy(concrete_type);
                }
                return;
            }
        }
        return;
    }
    
    // 类型不匹配，无法推断子类型
    if (generic_type->kind != concrete_type->kind) return;
    
    // 递归进入子类型
    switch (generic_type->kind) {
        case TYPE_ARRAY:
            infer_generic_from_type(generic_type->element_type, concrete_type->element_type,
                                    param_names, inferred, count);
            break;
        case TYPE_DICT:
            infer_generic_from_type(generic_type->key_type, concrete_type->key_type,
                                    param_names, inferred, count);
            infer_generic_from_type(generic_type->value_type, concrete_type->value_type,
                                    param_names, inferred, count);
            break;
        case TYPE_FUNCTION:
            for (int i = 0; i < generic_type->param_count && i < concrete_type->param_count; i++) {
                infer_generic_from_type(generic_type->param_types[i], concrete_type->param_types[i],
                                        param_names, inferred, count);
            }
            infer_generic_from_type(generic_type->return_type, concrete_type->return_type,
                                    param_names, inferred, count);
            break;
        case TYPE_PTR_GENERIC:
            infer_generic_from_type(generic_type->element_type, concrete_type->element_type,
                                    param_names, inferred, count);
            break;
        default:
            break;
    }
}

// 从实参推断泛型参数绑定（入口函数，兼容不同结构）
void infer_generic_bindings(TypeInfo* param_type, TypeInfo* arg_type,
                            char** param_names, TypeInfo** inferred, int count) {
    infer_generic_from_type(param_type, arg_type, param_names, inferred, count);
}