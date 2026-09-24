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
// 修复方式：realloc 后对 open_upvalues 做整块范围重映射——凡 location 落在
// 旧帧块地址范围内的一律按新旧块地址差平移。无数量上限、覆盖全部帧；
// 地址用 uintptr_t 整数比较（旧块虽已释放，纯数值比较安全，也避免编译器
// use-after-free 告警）；动态分配的 locals 数组不在旧帧块内，天然跳过。
//
// 历史 bug：旧实现用 fixes[256] 固定数组记录待修复项，超过 256 个静默丢弃。
// 顶层代码 local_count 记账修正后，主帧改用 inline_locals，GUI 程序顶层
// 变量被大量回调闭包捕获（upvalue 永久 open），轻易冲破 256 上限，
// 导致 upvalue 悬空、闭包读到垃圾值（file_manager 对话框只剩关闭按钮）。
//
// 返回 1=成功, 0=失败（内存不足）
static int vm_grow_frames(VM* vm_ptr) {
    int new_capacity = vm_grow_capacity(vm_ptr->frame_capacity);
    int old_capacity = vm_ptr->frame_capacity;   // 扩容前容量（初始化新槽时要用）

    // 硬上限：最后一次扩容**钳制**到 VM_MAX_FRAMES（倍增会越过上限，直接拒绝会让
    // 可用帧数停在半路，永远触发不了调用方的"调用栈溢出"报错）；到顶后拒绝
    if (new_capacity > VM_MAX_FRAMES) {
        if (old_capacity >= VM_MAX_FRAMES) return 0;
        new_capacity = VM_MAX_FRAMES;
    }

    // === Phase 1: realloc 前记录旧帧块地址范围（仅作整数比较，之后不解引用旧指针） ===
    uintptr_t old_base = (uintptr_t)vm_ptr->frames;
    uintptr_t old_end = old_base + (uintptr_t)vm_ptr->frame_capacity * sizeof(CallFrame);

    // === Phase 2: 执行 realloc ===
    CallFrame* new_frames = (CallFrame*)realloc(vm_ptr->frames, new_capacity * sizeof(CallFrame));
    if (!new_frames) return 0;

    vm_ptr->frames = new_frames;
    vm_ptr->frame_capacity = new_capacity;

    // === Phase 3: 重定位 locals 指针 ===
    // 非动态分配（locals_is_dynamic == 0）的帧，其 locals 一定指向**旧帧块内部**：
    //   · 自己的 inline_locals（它是 CallFrame 的成员，就在帧块里）；
    //   · 或零拷贝原地帧 —— 指向**调用方帧**的 inline_locals（调用方实参区那一段）。
    //   两者都是「旧块地址 + 同一偏移 = 新块地址」，故统一按 delta 平移即可，
    //   不必区分（原先无条件写成自己的 inline_locals，会把零拷贝原地帧的指针改错）。
    // 动态分配（malloc）的 locals 不在旧块范围内，下面的范围判断天然跳过。
    uintptr_t new_base = (uintptr_t)new_frames;
    uintptr_t delta = new_base - old_base;
    for (int i = 0; i < vm_ptr->frame_cnt; i++) {
        CallFrame* f = &new_frames[i];
        if (f->locals_is_dynamic || f->locals == NULL) continue;
        uintptr_t loc = (uintptr_t)f->locals;
        if (loc >= old_base && loc < old_end) {
            f->locals = (Value*)(loc + delta);
        }
    }

    // === Phase 3.5: 初始化**新增帧槽**的整个 inline_locals 为 null ===
    // 为什么必须做：零拷贝原地帧的建帧判定用的是 locals_capacity（物理容量，见
    // CallFrame.locals_capacity），允许被调方写到调用方高水位**之上**、直到容量上限。
    // 那片区域若从未被写过，GC 按 val_is_obj 判读随机字节就会把垃圾当对象指针 → 崩。
    // 每个帧槽只需初始化这一次：此后帧槽里永远只存合法 Value（写入的都是 Value）。
    for (int i = old_capacity; i < new_capacity; i++) {
        for (int j = 0; j < INLINE_LOCALS_MAX; j++) {
            new_frames[i].inline_locals[j] = val_null();
        }
        new_frames[i].locals_capacity = INLINE_LOCALS_MAX;
    }

    // === Phase 4: 范围重映射所有指向旧帧块的 open upvalue locations ===
    if (delta != 0) {
        for (Upvalue* uv = vm_ptr->open_upvalues; uv != NULL; uv = uv->next) {
            uintptr_t loc = (uintptr_t)uv->location;
            if (loc >= old_base && loc < old_end) {
                uv->location = (Value*)(loc + delta);
            }
        }
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
