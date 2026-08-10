// ============================================================================
// 变量操作类操作码 (OP_VARIABLES)
// ============================================================================

OPCODE(OP_GET_LOCAL) {
    frame = &vm.frames[vm.frame_cnt - 1];
    uint16_t slot = READ_SHORT();
    if (frame->locals != NULL) {
        if (likely(slot < frame->local_count)) {
            vm_stack_push(&vm, frame->locals[slot]);
        } else {
            runtime_error("局部变量索引越界", GET_CURRENT_LINE());
            return -1;
        }
    } else {
        vm_stack_push(&vm, vm.stack[frame->stack_base + slot]);
    }
    DISPATCH;
}

OPCODE(OP_SET_LOCAL) {
    frame = &vm.frames[vm.frame_cnt - 1];
    uint16_t slot = READ_SHORT();
    if (frame->locals != NULL) {
        if (likely(slot < frame->local_count)) {
            frame->locals[slot] = vm_stack_peek_fast(&vm, 0);
        } else {
            runtime_error("局部变量索引越界", GET_CURRENT_LINE());
            return -1;
        }
    } else {
        vm.stack[frame->stack_base + slot] = vm_stack_peek_fast(&vm, 0);
    }
    DISPATCH;
}

OPCODE(OP_SET_LOCAL_POP) {
    frame = &vm.frames[vm.frame_cnt - 1];
    uint16_t slot = READ_SHORT();
    if (frame->locals != NULL) {
        if (likely(slot < frame->local_count)) {
            frame->locals[slot] = vm_stack_pop_fast(&vm);
        } else {
            runtime_error("局部变量索引越界", GET_CURRENT_LINE());
            return -1;
        }
    } else {
        vm.stack[frame->stack_base + slot] = vm_stack_pop_fast(&vm);
    }
    DISPATCH;
}

OPCODE(OP_MOVE_LOCAL) {
    // 局部变量间直接复制：帧局部变量 src → dst，并压入栈（融合 OP_GET_LOCAL + OP_SET_LOCAL）
    frame = &vm.frames[vm.frame_cnt - 1];
    uint16_t src_slot = READ_SHORT();
    uint16_t dst_slot = READ_SHORT();
    Value value;
    if (frame->locals != NULL) {
        if (src_slot >= frame->local_count || dst_slot >= frame->local_count) {
            runtime_error("局部变量索引越界", GET_CURRENT_LINE());
            return -1;
        }
        value = frame->locals[src_slot];
        frame->locals[dst_slot] = value;
    } else {
        value = vm.stack[frame->stack_base + src_slot];
        vm.stack[frame->stack_base + dst_slot] = value;
    }
    vm_stack_push(&vm, value);
    DISPATCH;
}

OPCODE(OP_MOVE_LOCAL_POP) {
    // 局部变量间直接复制：src → dst，不压栈（a = b 语句用）
    frame = &vm.frames[vm.frame_cnt - 1];
    uint16_t src_slot = READ_SHORT();
    uint16_t dst_slot = READ_SHORT();
    if (frame->locals != NULL) {
        if (src_slot >= frame->local_count || dst_slot >= frame->local_count) {
            runtime_error("局部变量索引越界", GET_CURRENT_LINE());
            return -1;
        }
        frame->locals[dst_slot] = frame->locals[src_slot];
    } else {
        vm.stack[frame->stack_base + dst_slot] = vm.stack[frame->stack_base + src_slot];
    }
    // 不压栈，省掉 OP_POP
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
    frame = &vm.frames[vm.frame_cnt - 1];
    uint16_t slot = READ_SHORT();
    if (frame->closure == NULL) {
        runtime_error("没有闭包环境", GET_CURRENT_LINE());
        return -1;
    }
    vm_stack_push(&vm, *frame->closure->upvalues[slot]->location);
    DISPATCH;
}

OPCODE(OP_SET_UPVALUE) {
    frame = &vm.frames[vm.frame_cnt - 1];
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

#ifdef VM_USE_THREAD_LOCAL
    // 子线程：不使用内联缓存
    const char* name = ((ObjString*)val_as_obj(name_val))->chars;
    ObjNative* native = native_find_function(name);
    if (!native) {
        runtime_error("未找到 native 函数", GET_CURRENT_LINE());
        return -1;
    }
    vm_stack_push(&vm, val_obj((Object*)native));
#else
    // 主线程：使用内联缓存
    uint8_t* ip_before = frame->ip - 3;  // OP_GET_NATIVE = 3 字节 (opcode + 2)
    int cache_idx = ((uintptr_t)ip_before >> 2) & (IC_NATIVE_CACHE_SIZE - 1);
    InlineNativeCacheEntry* cache = &vm.ic_native_cache[cache_idx];
    uint32_t name_hash = ((ObjString*)val_as_obj(name_val))->hash;

    // 检查缓存是否命中
    if (cache->valid && cache->name_hash == name_hash) {
        vm.ic_hits++;
        vm_stack_push(&vm, val_obj((Object*)cache->fn));
        DISPATCH;
    }

    // 缓存未命中，查找 native 函数
    vm.ic_misses++;
    const char* name = ((ObjString*)val_as_obj(name_val))->chars;
    ObjNative* native = native_find_function(name);
    if (!native) {
        runtime_error("未找到 native 函数", GET_CURRENT_LINE());
        return -1;
    }

    // 更新缓存
    cache->valid = 1;
    cache->name_hash = name_hash;
    cache->fn = native;

    vm_stack_push(&vm, val_obj((Object*)native));
#endif
    DISPATCH;
}
