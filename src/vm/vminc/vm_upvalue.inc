// ============================================================================
// 闭包 Upvalue 管理 - 从 vm_upvalue.c 迁移
// ============================================================================

// 创建一个新的 upvalue（不加入 open_upvalues 链表）
static inline Upvalue* new_upvalue(Value value) {
    Upvalue* upvalue = (Upvalue*)gc_alloc(sizeof(Upvalue), OBJ_UPVALUE);
    if (upvalue == NULL) return NULL;
    upvalue->location = &upvalue->closed;
    upvalue->closed = value;
    upvalue->next = NULL;
    return upvalue;
}

// 捕获 upvalue
static inline Upvalue* capture_upvalue(Value* local) {
    Upvalue* prev_upvalue = NULL;
    Upvalue* upvalue = vm.open_upvalues;
    
    // 查找是否已经有upvalue指向这个local
    while (upvalue != NULL && upvalue->location > local) {
        prev_upvalue = upvalue;
        upvalue = upvalue->next;
    }
    
    if (upvalue != NULL && upvalue->location == local) {
        return upvalue;
    }
    
    Upvalue* created = (Upvalue*)gc_alloc(sizeof(Upvalue), OBJ_UPVALUE);
    if (!created) return NULL;
    
    // 关键：location直接指向locals数组中的位置
    // 由于locals是独立的数组，不会被native调用覆盖，可以安全引用
    created->location = local;
    created->closed = val_null();  // 初始时closed未使用
    created->next = upvalue;
    
    if (prev_upvalue == NULL) {
        vm.open_upvalues = created;
    } else {
        prev_upvalue->next = created;
    }
    
    return created;
}

// 关闭 upvalues
// 注意：open_upvalues 链表按 location 地址降序排列，但不能假设
// 所有指向当前帧的 upvalue 都在链表头部。不同帧的 locals 可能由
// malloc/realloc 分配在不同地址，导致地址交错。必须遍历整个链表。
static inline void close_upvalues(Value* locals, int local_count, int has_captures) {
    // 优化：如果没有捕获 upvalue，直接跳过
    if (!has_captures || vm.open_upvalues == NULL) {
        return;
    }
    
    Value* locals_end = locals + local_count;
    
    // 使用二级指针遍历整个链表，只关闭在 [locals, locals_end) 范围内的
    Upvalue** prev = &vm.open_upvalues;
    while (*prev != NULL) {
        Upvalue* upvalue = *prev;
        if (upvalue->location >= locals && upvalue->location < locals_end) {
            // 关闭该 upvalue：复制值并从 open 链表中移除
            upvalue->closed = *upvalue->location;
            gc_write_barrier((Object*)upvalue, upvalue->closed);
            upvalue->location = &upvalue->closed;
            *prev = upvalue->next;
        } else {
            // 跳过，保持该 upvalue 在链表中
            prev = &upvalue->next;
        }
    }
}
