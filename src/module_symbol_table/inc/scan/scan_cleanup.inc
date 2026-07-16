    // 释放本地别名表
    for (int i = 0; i < local_alias_count; i++) {
        free(local_alias_names[i]);
        type_free(local_alias_types[i]);
    }
    #undef FIND_LOCAL_ALIAS

    // 释放 struct 名称数组
    for (int i = 0; i < struct_name_count; i++) {
        free(struct_names[i]);
    }
    // 释放 cstruct 名称数组
    for (int i = 0; i < cstruct_name_count; i++) {
        free(cstruct_names[i]);
    }
    // 释放 face 名称数组
    for (int i = 0; i < face_name_count; i++) {
        free(face_names[i]);
    }

    free(source);
    return 0;
}
