    LABEL(OP_TYPE_CHECK): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        TypeKind expected = (TypeKind)b;
        TypeKind elem = (TypeKind)c;
        Value name_val = val_null();
        if (expected == TYPE_STRUCT || expected == TYPE_FACE || expected == TYPE_ENUM) {
            int nidx = ((int)frame->ip[0] << 8) | (int)frame->ip[1];
            frame->ip += 2;
            if (nidx >= 0 && nidx < frame->chunk->const_cnt) {
                name_val = frame->chunk->constants[nidx];
            }
        }
        R(a) = val_bool(type_check_value(R(a), expected, elem, name_val));
        DISPATCH();
    }
    // OP_AS_CAST A B C: R[A] = R[A] as 类型B[元素C]（不匹配 → null）
    LABEL(OP_AS_CAST): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        TypeKind expected = (TypeKind)b;
        TypeKind elem = (TypeKind)c;
        Value name_val = val_null();
        if (expected == TYPE_STRUCT || expected == TYPE_FACE || expected == TYPE_CSTRUCT) {
            int nidx = ((int)frame->ip[0] << 8) | (int)frame->ip[1];
            frame->ip += 2;
            if (nidx >= 0 && nidx < frame->chunk->const_cnt) {
                name_val = frame->chunk->constants[nidx];
            }
        }
        R(a) = vm_as_cast(R(a), expected, elem, name_val);
        DISPATCH();
    }
    // OP_TYPE_CHECK_DICT A B C: R[A] = (R[A] is Dict[B, C])（B/C = TypeKind，TYPE_ANY = 该侧不校验）
    //   与 OP_TYPE_CHECK 的区别只有一处：**逐键值**校验（口径见 vm.c 的 dict_matches_kv）。
    //   为什么单开 opcode：K/V 两个 TypeKind 塞不进 4 字节头的 B/C（B 已被顶层类型占用）。
    LABEL(OP_TYPE_CHECK_DICT): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        R(a) = val_bool(dict_type_check(R(a), (TypeKind)b, (TypeKind)c));
        DISPATCH();
    }
    // OP_AS_CAST_DICT A B C: R[A] = R[A] as Dict[B, C]（不匹配 → null）
    LABEL(OP_AS_CAST_DICT): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        R(a) = dict_as_cast(R(a), (TypeKind)b, (TypeKind)c);
        DISPATCH();
    }

    // --- 协程 ---
    // OP_ASYNC_CALL A B: 调用 async 函数 —— R[A] = 闭包，R[A+1..A+B] = 实参，
    //   结果写回 R[A]（本次调用的 Future）。协程加入事件循环就绪队列。
    LABEL(OP_ASYNC_CALL): {
        int a = READ_A(), b = READ_B();
        int arg_count = b;
        int line = GET_CURRENT_LINE();
        Value callee = R(a);

        // 被调对象：闭包，或**绑定方法**（`t.wait()` 这种 struct 的 async 方法 ——
        // OP_GET_METHOD 产出的是"方法闭包 + receiver"的绑定对象）。
        // ⚠ 只认 OBJ_CLOSURE 时，async 方法调用会直接报「async 调用只能调用 async 函数」
        //   （examples/async await/struct 方法 + async.leno）。
        ObjClosure* closure = NULL;
        int need_self = 0;
        Value self_val = val_null();

        if (val_is_obj(callee) && val_as_obj(callee)->type == OBJ_CLOSURE) {
            closure = (ObjClosure*)val_as_obj(callee);
        } else if (val_is_obj(callee) && val_as_obj(callee)->type == OBJ_FUNCTION) {
            // ★ 裸函数对象：跨模块调用（`m.work()`）从模块 exports 里取到的就是它
            //   （模块导出的是 ObjFunction 本身，不是闭包）—— 与 OP_CALL 的 OBJ_FUNCTION
            //   分支同口径：现场包一层闭包再建协程。不认它就会报
            //   「async 调用只能调用 async 函数」（C2：`await m.work()` 走的就是这条路）。
            ObjFunction* raw = (ObjFunction*)val_as_obj(callee);
            ObjClosure* wrap = (ObjClosure*)gc_alloc(sizeof(ObjClosure), OBJ_CLOSURE);
            if (unlikely(!wrap)) { runtime_error("内存分配失败", line); return -1; }
            wrap->function = raw;
            wrap->upvalue_count = 0;
            wrap->type_param_count = 0;
            wrap->type_param_args = NULL;
            for (int i = 0; i < MAX_UPVALUES; i++) wrap->upvalues[i] = NULL;
            closure = wrap;
        } else if (val_is_obj(callee) && val_as_obj(callee)->type == OBJ_BOUND_METHOD) {
            ObjBoundMethod* bound = (ObjBoundMethod*)val_as_obj(callee);
            closure = bound->closure;      // native 绑定方法没有闭包 ⇒ 下面统一报错
            if (closure) {
                // receiver 是否已作为首实参传入（判据与 OP_CALL 的绑定方法分支一致：
                // 首实参值等于 receiver ⇒ 调用方已经带了 self；再用形参个数佐证，
                // 避免 `a.m(a)` 这类自引用被误判）。
                int self_in_args = 0;
                int expect_arity = closure->function ? closure->function->arity : 0;
                if (arg_count > 0) {
                    Value a0 = R(a + 1);
                    if (val_is_obj(a0) && val_is_obj(bound->receiver)) {
                        self_in_args = (val_as_obj(a0) == val_as_obj(bound->receiver));
                    } else if (!val_is_obj(a0) && !val_is_obj(bound->receiver)) {
                        self_in_args = (a0 == bound->receiver);
                    }
                    if (self_in_args && expect_arity > 0 && arg_count != expect_arity &&
                        arg_count + 1 == expect_arity) {
                        self_in_args = 0;
                    }
                }
                need_self = !self_in_args;
                self_val = bound->receiver;
            }
        }
        if (unlikely(!closure)) {
            if (!throw_runtime_error("async 调用只能调用 async 函数", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        // 建协程 + 拷实参 + 写回 Future —— 与 OP_CALL 的 async 兜底共用同一实现
        if (unlikely(vm_start_async_call(&vm, closure, frame->locals, a, arg_count, a,
                                         self_val, need_self, line) != 0)) {
            if (!vm.has_exception) return -1;
        }
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH();
    }
    // OP_AWAIT A B: R[A] = await R[B]（B 是 Future）
    //   已完成 → 直接取值；未完成 → 保存本协程的所有帧并挂起，
    //   恢复时由 vm_run_coroutine 把结果写回顶层帧的 R[await_dst_reg]
    LABEL(OP_AWAIT): {
        int a = READ_A(), b = READ_B();
        int line = GET_CURRENT_LINE();
        Value future_val = R(b);

        if (unlikely(!val_is_obj(future_val) || val_as_obj(future_val)->type != OBJ_FUTURE)) {
            if (!throw_runtime_error("await 只能等待 Future 对象", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        ObjFuture* future = (ObjFuture*)val_as_obj(future_val);

        if (future->completed) {
            if (!val_is_null(future->error)) {
                // Future 失败：按异常抛出（交给统一的异常处理路径）
                if (!throw_runtime_error_with_operand("await: 异步任务失败", "AWAIT", future->error, line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            R(a) = future->result;
            DISPATCH();
        }

        ObjCoroutine* current = vm.current_coroutine;
        if (unlikely(!current)) {
            if (!throw_runtime_error("await 只能在 async 函数中使用", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }

        current->state = COROUTINE_SUSPENDED;
        current->saved_sp = vm.sp;
        current->await_dst_reg = a;

        // 保存本协程的所有帧（[co_start, frame_cnt)）：所有权转给 saved_frames，
        // vm.frames 里的帧被 frame_cnt 重置"放弃"
        // ★ co_start 就是"协程首帧索引"（saved_frame_cnt 的语义，见 vm.c 里的设置）——
        //   不能再 -1：恢复后协程可能有多个帧（挂起时在嵌套调用里），
        //   减一会漏存除顶层以外的帧，下次再挂起时那些帧被别的协程覆盖 ⇒ 数据错乱。
        int co_start = current->saved_frame_cnt;
        if (co_start < 0) co_start = 0;
        int num_frames = vm.frame_cnt - co_start;
        if (current->saved_frames) {
            for (int i = 0; i < current->saved_frame_count; i++) {
                CallFrame* sf = &current->saved_frames[i];
                if (sf->locals_is_dynamic && sf->locals) free(sf->locals);
            }
            free(current->saved_frames);
            current->saved_frames = NULL;
        }
        if (num_frames > 0) {
            current->saved_frames = (CallFrame*)malloc((size_t)num_frames * sizeof(CallFrame));
            if (unlikely(!current->saved_frames)) { runtime_error("协程帧保存失败", line); return -1; }
            for (int i = 0; i < num_frames; i++) {
                CallFrame* src = &vm.frames[co_start + i];
                CallFrame* dst = &current->saved_frames[i];
                memcpy(dst, src, sizeof(CallFrame));
                if (!src->locals_is_dynamic && src->local_count > 0) {
                    dst->locals = dst->inline_locals;
                }
            }
            current->saved_frame_count = num_frames;
        }

        current->waiting_for = future;
        future->waiter = current;
        gc_write_barrier((Object*)current, future_val);
        gc_write_barrier((Object*)future, val_obj((Object*)current));
        gc_remember_object((Object*)current);

        // 返回 0 让控制权交回事件循环（帧保留在 saved_frames，恢复时再装回）
        return 0;
    }

    // --- FFI ---
    // OP_CLIB_CALL A B C: C = 用户实参个数
    //   R[A] = clib 库对象，R[A+1] = 函数名，R[A+2..A+1+C] = 实参
    //   紧随指令的变长操作数：ret_type_kind(1) + arg_types[C](1 each)
    //   结果写回 R[A]（类型按 ret_type_kind 自动展开：str8 → string 等）
    LABEL(OP_CLIB_CALL): {
        int a = READ_A();
        int user_arg_count = READ_C();
        int line = GET_CURRENT_LINE();

        if (unlikely(user_arg_count < 0 || user_arg_count > FFI_MAX_ARGS)) {
            if (!throw_runtime_error("clib 调用参数过多", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        int ret_type_kind = (int)frame->ip[0];
        int arg_types[FFI_MAX_ARGS];
        for (int i = 0; i < user_arg_count; i++) {
            arg_types[i] = (int)frame->ip[1 + i];
        }
        frame->ip += 1 + user_arg_count;

        int argc = 2 + user_arg_count;
        // 寄存器区 R[A..A+argc-1] 在 frame->locals 中连续，可直接当 FFI 的 Value* 用
        Value result = ffi_clib_call(argc, &R(a), ret_type_kind, arg_types);

        // ★ ffi_clib_call 内部可能回调进 VM（grow_frames 会 realloc vm.frames），
        //   frame 必须重新取，否则 DISPATCH 读到悬空指针
        frame = &vm.frames[vm.frame_cnt - 1];

        if (unlikely(vm.has_exception)) {
            char* msg = NULL;
            if (val_is_obj(vm.exception) && val_as_obj(vm.exception)->type == OBJ_STRING) {
                msg = strdup(((ObjString*)val_as_obj(vm.exception))->chars);
            } else {
                msg = value_to_string(vm.exception);
            }
            error_clear();
            if (!throw_runtime_error(msg, line)) { free(msg); return -1; }
            free(msg);
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }

        R(a) = result;
        DISPATCH();
    }
    // OP_CFUNC_CALLBACK A: R[A] = FFI 回调（由 cfunc 声明式签名创建）
    //   紧随指令的变长操作数：ret_type(1) param_count(1) param_types[param_count](1 each)
    //   R[A] 传入的是 Leno 侧函数值（闭包 / 函数对象）
    LABEL(OP_CFUNC_CALLBACK): {
        int a = READ_A();
        int ret_type = (int)frame->ip[0];
        int param_count = (int)frame->ip[1];
        if (unlikely(param_count < 0 || param_count > 12)) {
            if (!throw_runtime_error("cfunc 回调参数过多", GET_CURRENT_LINE())) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        uint8_t param_types[12];
        for (int i = 0; i < param_count; i++) param_types[i] = frame->ip[2 + i];
        frame->ip += 2 + param_count;

        Value result = ffi_callback_create_with_sig(R(a), ret_type, param_count, param_types);

        // ffi_callback_create_with_sig 可能回调进 VM（frames 会被 realloc）
        frame = &vm.frames[vm.frame_cnt - 1];
        if (unlikely(vm.has_exception)) {
            char* msg = NULL;
            if (val_is_obj(vm.exception) && val_as_obj(vm.exception)->type == OBJ_STRING) {
                msg = strdup(((ObjString*)val_as_obj(vm.exception))->chars);
            } else {
                msg = value_to_string(vm.exception);
            }
            int line = GET_CURRENT_LINE();
            error_clear();
            if (!throw_runtime_error(msg, line)) { free(msg); return -1; }
            free(msg);
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        R(a) = result;
        DISPATCH();
    }
    LABEL(OP_U8_TO_F64): DISPATCH();

    // --- 泛型 / 析构 ---
    LABEL(OP_PUSH_TYPE_ARGS): DISPATCH();
    // OP_DTOR_LOCAL A: 对 R[A] 持有的 struct 实例调用其析构函数（无析构则为空操作）
    LABEL(OP_DTOR_LOCAL): {
        int a = READ_A();
        Value val = R(a);
        if (val_is_obj(val) && val_as_obj(val)->type == OBJ_STRUCT) {
            ObjStruct* so = (ObjStruct*)val_as_obj(val);
            ObjStructDef* def = so->def;
            if (def && def->has_dtor && def->dtor_index >= 0 &&
                def->dtor_index < def->method_count) {
                StructMethodInfo* mi = &def->methods[def->dtor_index];
                ObjClosure* dcl = mi->closure;
                if (!dcl && mi->func) {
                    dcl = (ObjClosure*)gc_alloc(sizeof(ObjClosure), OBJ_CLOSURE);
                    if (unlikely(!dcl)) {
                        runtime_error("内存不足，无法创建析构函数闭包", GET_CURRENT_LINE());
                        return -1;
                    }
                    dcl->function = mi->func;
                    dcl->upvalue_count = mi->func->upvalue_count;
                    dcl->type_param_count = 0;
                    dcl->type_param_args = NULL;
                    for (int u = 0; u < mi->func->upvalue_count; u++) dcl->upvalues[u] = NULL;
                    mi->closure = dcl;
                }
                if (dcl) {
                    // 析构调用：self 直接给 receiver，无实参、结果丢弃（ret_want=0）
                    if (call_reg(dcl, 1, val, a + 1, 0, a, 0, GET_CURRENT_LINE())) {
                        frame = &vm.frames[vm.frame_cnt - 1];
                        DISPATCH();
                    }
                    if (!vm.has_exception) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
            }
        }
        DISPATCH();
    }

    // --- switch ---
