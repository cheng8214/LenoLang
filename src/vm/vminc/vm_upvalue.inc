// ============================================================================
// 闭包 Upvalue 管理 - 从 vm_upvalue.c 迁移
// ============================================================================

// vm.frames 扩容并修复所有引用（inline locals 指针 + open upvalue locations）
//
// 关键问题：当 vm.frames 被 realloc 后，使用内联 locals（local_count <= INLINE_LOCALS_MAX）
// 的帧的 inline_locals 数组地址会改变。代码已经更新了 frame->locals 指针，
// 但 open upvalues 的 location 指针仍指向旧的（已释放的）inline_locals 地址。
// 这导致闭包通过 upvalue 访问捕获变量时读取已释放内存，引发偶发性崩溃。
//
// 本函数在 realloc 前记录需要修复的 upvalue，realloc 后更新其 location 指针。
//
// 返回 1=成功, 0=失败（内存不足）
static int vm_grow_frames(VM* vm_ptr) {
    int new_capacity = vm_grow_capacity(vm_ptr->frame_capacity);

    // === Phase 1: realloc 前记录指向 inline locals 的 open upvalues ===
    struct { Upvalue* uv; int frame_idx; int offset; } fixes[256];
    int fix_count = 0;

    for (int i = 0; i < vm_ptr->frame_cnt && fix_count < 256; i++) {
        CallFrame* f = &vm_ptr->frames[i];
        if (!f->locals_is_dynamic && f->local_count > 0) {
            Value* start = f->inline_locals;
            Value* end = start + f->local_count;
            Upvalue* uv = vm_ptr->open_upvalues;
            while (uv && fix_count < 256) {
                if (uv->location >= start && uv->location < end) {
                    fixes[fix_count].uv = uv;
                    fixes[fix_count].frame_idx = i;
                    fixes[fix_count].offset = (int)(uv->location - start);
                    fix_count++;
                }
                uv = uv->next;
            }
        }
    }

    // === Phase 2: 执行 realloc ===
    CallFrame* new_frames = (CallFrame*)realloc(vm_ptr->frames, new_capacity * sizeof(CallFrame));
    if (!new_frames) return 0;

    vm_ptr->frames = new_frames;
    vm_ptr->frame_capacity = new_capacity;

    // === Phase 3: 更新 inline locals 指针 ===
    for (int i = 0; i < vm_ptr->frame_cnt; i++) {
        CallFrame* f = &new_frames[i];
        if (!f->locals_is_dynamic && f->local_count > 0) {
            f->locals = f->inline_locals;
        }
    }

    // === Phase 4: 修复 open upvalue locations ===
    for (int i = 0; i < fix_count; i++) {
        CallFrame* f = &new_frames[fixes[i].frame_idx];
        fixes[i].uv->location = &f->inline_locals[fixes[i].offset];
    }

    return 1;
}

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
