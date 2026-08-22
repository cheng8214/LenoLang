// ============================================================================
// 字节码块操作 - 从 vm_chunk.c 迁移
// ============================================================================

// 动态扩容辅助函数（已内联）
static inline int grow_capacity(int old_capacity) {
    return old_capacity < 8 ? 8 : old_capacity * 2;
}

void chunk_init(Chunk* chunk) {
    chunk->code = NULL;
    chunk->len = 0;
    chunk->code_capacity = 0;
    chunk->constants = NULL;
    chunk->const_cnt = 0;
    chunk->const_capacity = 0;
    chunk->lines = NULL;
    chunk->filename = NULL;
    chunk->local_count = 0;
}

void chunk_free(Chunk* chunk) {
    free(chunk->code);
    free(chunk->constants);
    free(chunk->lines);
    free(chunk->filename);
    chunk->code = NULL;
    chunk->constants = NULL;
    chunk->lines = NULL;
    chunk->filename = NULL;
    chunk->len = 0;
    chunk->code_capacity = 0;
    chunk->const_cnt = 0;
    chunk->const_capacity = 0;
}

int chunk_add_const(Chunk* chunk, Value value) {
    // 需要扩容
    if (chunk->const_cnt >= chunk->const_capacity) {
        int new_capacity = grow_capacity(chunk->const_capacity);
        Value* new_constants = (Value*)realloc(chunk->constants, new_capacity * sizeof(Value));
        if (!new_constants) {
            error_add_at(ERR_RUNTIME, 0, 0, "常量表内存分配失败");
            return 0;
        }
        chunk->constants = new_constants;
        chunk->const_capacity = new_capacity;
    }
    chunk->constants[chunk->const_cnt] = value;
    return chunk->const_cnt++;
}

void chunk_write(Chunk* chunk, uint8_t byte, int line) {
    // 需要扩容
    if (chunk->len >= chunk->code_capacity) {
        int new_capacity = grow_capacity(chunk->code_capacity);
        
        // 使用 malloc 而不是 realloc，确保原子性分配
        // 避免第一个 realloc 成功后第二个失败导致的内存泄漏
        uint8_t* new_code = (uint8_t*)malloc(new_capacity * sizeof(uint8_t));
        int* new_lines = (int*)malloc(new_capacity * sizeof(int));
        
        if (!new_code || !new_lines) {
            free(new_code);
            free(new_lines);
            error_add_at(ERR_RUNTIME, line, 0, "代码块内存分配失败");
            return;
        }
        
        // 复制旧数据
        memcpy(new_code, chunk->code, chunk->len * sizeof(uint8_t));
        memcpy(new_lines, chunk->lines, chunk->len * sizeof(int));
        
        // 释放旧内存
        free(chunk->code);
        free(chunk->lines);
        
        chunk->code = new_code;
        chunk->lines = new_lines;
        chunk->code_capacity = new_capacity;
    }
    chunk->code[chunk->len] = byte;
    chunk->lines[chunk->len] = line;
    chunk->len++;
}

// ============================================================================
// 全局变量和函数容量管理
// ============================================================================

// 确保全局变量数组有足够的容量
int ensure_global_capacity(int slot) {
    if (slot >= vm.global_capacity) {
        int new_capacity = vm.global_capacity;
        while (new_capacity <= slot) {
            new_capacity = grow_capacity(new_capacity);
        }
        Value* new_globals = (Value*)realloc(vm.globals, new_capacity * sizeof(Value));
        if (!new_globals) {
            return 0;
        }
        // 初始化新分配的内存
        for (int i = vm.global_capacity; i < new_capacity; i++) {
            new_globals[i] = val_null();
        }
        vm.globals = new_globals;
        vm.global_capacity = new_capacity;
    }
    return 1;
}

// 确保全局函数数组有足够的容量
int ensure_global_func_capacity(int slot) {
    if (slot >= vm.global_func_capacity) {
        int new_capacity = vm.global_func_capacity;
        while (new_capacity <= slot) {
            new_capacity = grow_capacity(new_capacity);
        }
        Value* new_funcs = (Value*)realloc(vm.global_funcs, new_capacity * sizeof(Value));
        if (!new_funcs) {
            return 0;
        }
        vm.global_funcs = new_funcs;
        vm.global_func_capacity = new_capacity;
    }
    return 1;
}
