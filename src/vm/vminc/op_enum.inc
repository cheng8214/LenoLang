// ============================================================================
// enum 相关操作码
// ============================================================================

OPCODE(OP_ENUM_DEF) {
    frame = &vm.frames[vm.frame_cnt - 1];
    // 定义 enum
    // 操作数：2字节名称常量索引 + 1字节成员数量
    // 每个成员：2字节成员名常量索引 + 2字节成员值常量索引
    uint16_t name_idx = READ_SHORT();
    uint8_t member_count = READ_BYTE();

    Value name_val = frame->chunk->constants[name_idx];
    if (!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING) {
        runtime_error("enum 名称必须是字符串", GET_CURRENT_LINE());
        return -1;
    }
    ObjString* name = (ObjString*)val_as_obj(name_val);

    // 创建 enum 定义
    ObjEnumDef* def = enum_def_new(name->chars, member_count);

    // 读取成员信息
    for (int i = 0; i < member_count; i++) {
        uint16_t member_name_idx = READ_SHORT();
        uint16_t member_val_idx = READ_SHORT();

        Value member_name_val = frame->chunk->constants[member_name_idx];
        if (!val_is_obj(member_name_val) || val_as_obj(member_name_val)->type != OBJ_STRING) {
            runtime_error("enum 成员名必须是字符串", GET_CURRENT_LINE());
            return -1;
        }
        ObjString* member_name = (ObjString*)val_as_obj(member_name_val);

        Value member_val = frame->chunk->constants[member_val_idx];
        int64_t member_value;
        if (val_is_obj(member_val) && val_as_obj(member_val)->type == OBJ_BIGINT) {
            member_value = bigint_to_int64((ObjBigInt*)val_as_obj(member_val));
        } else {
            member_value = (int64_t)val_as_num(member_val);
        }

        enum_def_set_member(def, i, member_name->chars, member_value);
    }

    // 注册 enum 定义
    enum_def_register(def);

    // 将 enum 定义对象压入栈，供后续存储到变量使用
    vm_stack_push(&vm, val_obj((Object*)def));

    DISPATCH;
}
