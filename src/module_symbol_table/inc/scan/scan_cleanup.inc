    // 释放本地别名表（动态数组）
    for (int i = 0; i < local_alias_count; i++) {
        free(local_alias_names[i]);
        type_free(local_alias_types[i]);
    }
    free(local_alias_names);
    free(local_alias_types);
    #undef FIND_LOCAL_ALIAS

    // 释放各名称数组（动态数组；含数组本体）
    for (int i = 0; i < struct_name_count; i++) {
        free(struct_names[i]);
    }
    free(struct_names);
    for (int i = 0; i < cstruct_name_count; i++) {
        free(cstruct_names[i]);
    }
    free(cstruct_names);
    for (int i = 0; i < face_name_count; i++) {
        free(face_names[i]);
    }
    free(face_names);
    for (int i = 0; i < clib_name_count; i++) {
        free(clib_names[i]);
    }
    free(clib_names);
    for (int i = 0; i < enum_name_count; i++) {
        free(enum_names[i]);
    }
    free(enum_names);

    free(source);
    return 0;
}
