// ============================================================================
// 模块调用类操作码 (OP_MODULE_CALL)
// ============================================================================

OPCODE(OP_MODULE_CALL) {
    frame = &vm.frames[vm.frame_cnt - 1];
    int module_idx = READ_SHORT();
    int method_idx = READ_SHORT();
    int arg_count = READ_SHORT();

    // 获取模块名和方法名
    Value module_val = frame->chunk->constants[module_idx];
    Value method_val = frame->chunk->constants[method_idx];

    if (!val_is_obj(module_val) || val_as_obj(module_val)->type != OBJ_STRING ||
        !val_is_obj(method_val) || val_as_obj(method_val)->type != OBJ_STRING) {
        runtime_error("模块方法名必须是字符串", GET_CURRENT_LINE());
        return -1;
    }

    const char* module_name = ((ObjString*)val_as_obj(module_val))->chars;
    const char* method_name = ((ObjString*)val_as_obj(method_val))->chars;

    // 查找模块方法
#ifdef VM_USE_THREAD_LOCAL
    // 子线程：不使用内联缓存
    ModuleMethodMeta* meta = native_find_module_method(module_name, method_name);
#else
    // 主线程：使用内联缓存
    uint8_t* ip_before = frame->ip - 7;  // OP_MODULE_CALL = 7 字节 (opcode + 2+2+2)
    int cache_idx = ((uintptr_t)ip_before >> 2) & (IC_MODULE_CACHE_SIZE - 1);
    InlineModuleCallCacheEntry* cache = &vm.ic_module_cache[cache_idx];
    uint32_t combined_hash = ((ObjString*)val_as_obj(module_val))->hash
                           ^ (((ObjString*)val_as_obj(method_val))->hash << 1);

    ModuleMethodMeta* meta;
    if (cache->valid && cache->combined_hash == combined_hash) {
        // 缓存命中
        vm.ic_hits++;
        meta = (ModuleMethodMeta*)cache->meta;
    } else {
        // 缓存未命中
        vm.ic_misses++;
        meta = native_find_module_method(module_name, method_name);
        if (meta) {
            cache->valid = 1;
            cache->combined_hash = combined_hash;
            cache->meta = (void*)meta;
        }
    }
#endif

    if (!meta) {
        char msg[BUFFER_MEDIUM];
        snprintf(msg, sizeof(msg), "未找到模块方法: %s.%s", module_name, method_name);
        runtime_error(msg, GET_CURRENT_LINE());
        return -1;
    }

    // 调用模块方法
    // 参数在栈上，从 vm.sp - arg_count 开始
    Value result = meta->function(arg_count, vm.stack + vm.sp - arg_count);

    // 检查是否有异常
    if (vm.has_exception) {
        // 异常已在 native_throw_error 中设置
        // 使用 throw_runtime_error 正确处理异常以支持 try-catch
        char* msg = NULL;
        if (val_is_obj(vm.exception) && val_as_obj(vm.exception)->type == OBJ_STRING) {
            msg = strdup(((ObjString*)val_as_obj(vm.exception))->chars);
        } else {
            msg = value_to_string(vm.exception);
        }
        int line = GET_CURRENT_LINE();
        // 清除 native_throw_error 添加的错误，让 throw_runtime_error 重新报告
        error_clear();
        throw_runtime_error(msg, line);
        free(msg);
        // throw_runtime_error 已经处理了异常跳转或报告错误
        // 如果异常被捕获，has_exception 会被清除，frame->ip 会被设置为 catch 块
        // 如果未被捕获，has_exception 仍为 1，VM 的 DISPATCH 会检查并处理
        // 更新 frame 变量（因为 throw_runtime_error 可能修改了 vm.frame_cnt）
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    // 清理栈上的参数
    vm.sp -= arg_count;

    // 压入返回值
    vm_stack_push(&vm, result);
    DISPATCH;
}
