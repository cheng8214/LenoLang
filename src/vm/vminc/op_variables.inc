// ============================================================================
// 变量操作类操作码 (OP_VARIABLES)
// ============================================================================

OPCODE(OP_GET_LOCAL) {
    uint16_t slot = READ_SHORT();
    if (frame->locals != NULL) {
        if (slot < frame->local_count) {
            vm_stack_push_fast(&vm, frame->locals[slot]);
        } else {
            runtime_error("局部变量索引越界", GET_CURRENT_LINE());
            return -1;
        }
    } else {
        vm_stack_push_fast(&vm, vm.stack[frame->stack_base + slot]);
    }
    DISPATCH;
}

OPCODE(OP_SET_LOCAL) {
    uint16_t slot = READ_SHORT();
    if (frame->locals != NULL) {
        if (slot < frame->local_count) {
            frame->locals[slot] = vm_stack_peek(&vm, 0);
        } else {
            runtime_error("局部变量索引越界", GET_CURRENT_LINE());
            return -1;
        }
    } else {
        vm.stack[frame->stack_base + slot] = vm_stack_peek(&vm, 0);
    }
    DISPATCH;
}

OPCODE(OP_GET_GLOBAL) {
    uint16_t slot = READ_SHORT();
    if (unlikely(slot >= vm.global_capacity)) {
        if (!ensure_global_capacity(slot)) {
            runtime_error("全局变量数组扩容失败", GET_CURRENT_LINE());
            return -1;
        }
    }
    vm_stack_push(&vm, vm.globals[slot]);
    DISPATCH;
}

OPCODE(OP_SET_GLOBAL) {
    uint16_t slot = READ_SHORT();
    if (unlikely(slot >= vm.global_capacity)) {
        if (!ensure_global_capacity(slot)) {
            runtime_error("全局变量数组扩容失败", GET_CURRENT_LINE());
            return -1;
        }
    }
    vm.globals[slot] = vm_stack_peek(&vm, 0);
    // 更新 global_count 以跟踪已使用的全局变量数量
    if (slot >= vm.global_count) {
        vm.global_count = slot + 1;
    }
    DISPATCH;
}

OPCODE(OP_GET_UPVALUE) {
    uint16_t slot = READ_SHORT();
    if (frame->closure == NULL) {
        runtime_error("没有闭包环境", GET_CURRENT_LINE());
        return -1;
    }
    vm_stack_push(&vm, *frame->closure->upvalues[slot]->location);
    DISPATCH;
}

OPCODE(OP_SET_UPVALUE) {
    uint16_t slot = READ_SHORT();
    if (frame->closure == NULL) {
        runtime_error("没有闭包环境", GET_CURRENT_LINE());
        return -1;
    }
    *frame->closure->upvalues[slot]->location = vm_stack_peek(&vm, 0);
    DISPATCH;
}

OPCODE(OP_CLOSE_UPVALUE) {
    frame = &vm.frames[vm.frame_cnt - 1];
    if (frame->locals != NULL && frame->local_count > 0 && frame->has_captures) {
        close_upvalues(frame->locals, frame->local_count, frame->has_captures);
    }
    vm_stack_pop(&vm);
    DISPATCH;
}

OPCODE(OP_DEFINE_GLOBAL) {
    frame = &vm.frames[vm.frame_cnt - 1];
    uint16_t slot = READ_SHORT();
    if (unlikely(slot >= vm.global_capacity)) {
        if (!ensure_global_capacity(slot)) {
            runtime_error("全局变量数组扩容失败", GET_CURRENT_LINE());
            return -1;
        }
    }
    vm.globals[slot] = vm_stack_pop(&vm);
    if (slot >= vm.global_count) {
        vm.global_count = slot + 1;
    }
    DISPATCH;
}

OPCODE(OP_GET_GLOBAL_FUNC) {
    uint16_t slot = READ_SHORT();
    if (slot >= vm.global_func_capacity) {
        runtime_error("全局函数索引越界", GET_CURRENT_LINE());
        return -1;
    }
    vm_stack_push(&vm, vm.global_funcs[slot]);
    DISPATCH;
}

OPCODE(OP_DEFINE_GLOBAL_FUNC) {
    frame = &vm.frames[vm.frame_cnt - 1];
    uint16_t slot = READ_SHORT();
    if (unlikely(slot >= vm.global_func_capacity)) {
        if (!ensure_global_func_capacity(slot)) {
            runtime_error("全局函数数组扩容失败", GET_CURRENT_LINE());
            return -1;
        }
    }
    vm.global_funcs[slot] = vm_stack_pop(&vm);
    if (slot >= vm.global_func_count) {
        vm.global_func_count = slot + 1;
    }
    DISPATCH;
}

OPCODE(OP_GET_NATIVE) {
    frame = &vm.frames[vm.frame_cnt - 1];
    uint16_t name_idx = READ_SHORT();
    Value name_val = frame->chunk->constants[name_idx];
    if (!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING) {
        runtime_error("native 函数名必须是字符串", GET_CURRENT_LINE());
        return -1;
    }
    const char* name = ((ObjString*)val_as_obj(name_val))->chars;
    ObjNative* native = native_find_function(name);
    if (!native) {
        runtime_error("未找到 native 函数", GET_CURRENT_LINE());
        return -1;
    }
    vm_stack_push(&vm, val_obj((Object*)native));
    DISPATCH;
}
