    LABEL(OP_NOP): DISPATCH();

    // --- 装载 / 移动 ---
    LABEL(OP_LOADK): {
        int a = READ_A();
        int bx = READ_Bx();
        R(a) = frame->chunk->constants[bx];
        DISPATCH();
    }
    LABEL(OP_LOADI): {
        int a = READ_A();
        int sbx = READ_sBx();
        R(a) = val_int(sbx);
        DISPATCH();
    }
    LABEL(OP_LOADF): {
        int a = READ_A();
        int sbx = READ_sBx();
        R(a) = val_float((double)sbx);
        DISPATCH();
    }
    LABEL(OP_LOADNIL): {
        R(READ_A()) = val_null();
        DISPATCH();
    }
    LABEL(OP_LOADTRUE): {
        R(READ_A()) = val_bool(1);
        DISPATCH();
    }
    LABEL(OP_LOADFALSE): {
        R(READ_A()) = val_bool(0);
        DISPATCH();
    }
    LABEL(OP_MOV): {
        int a = READ_A();
        int b = READ_B();
        R(a) = R(b);
        DISPATCH();
    }

    // --- 全局变量 ---
    LABEL(OP_GETGLOBAL): {
        int a = READ_A();
        int bx = READ_Bx();
        if (unlikely(bx >= vm.global_capacity)) {
            if (!ensure_global_capacity(bx)) {
                runtime_error("全局变量数组扩容失败", GET_CURRENT_LINE());
                return -1;
            }
        }
        R(a) = vm.globals[bx];
        DISPATCH();
    }
    LABEL(OP_SETGLOBAL): {
        int a = READ_A();
        int bx = READ_Bx();
        if (unlikely(bx >= vm.global_capacity)) {
            if (!ensure_global_capacity(bx)) {
                runtime_error("全局变量数组扩容失败", GET_CURRENT_LINE());
                return -1;
            }
        }
        vm.globals[bx] = R(a);
        if (bx >= vm.global_count) vm.global_count = bx + 1;
        DISPATCH();
    }
    LABEL(OP_DEFGLOBAL): {
        int a = READ_A();
        int bx = READ_Bx();
        if (unlikely(bx >= vm.global_capacity)) {
            if (!ensure_global_capacity(bx)) {
                runtime_error("全局变量数组扩容失败", GET_CURRENT_LINE());
                return -1;
            }
        }
        vm.globals[bx] = R(a);
        if (bx >= vm.global_count) vm.global_count = bx + 1;
        DISPATCH();
    }
    LABEL(OP_GETGLOBALFUNC): {
        int a = READ_A();
        int bx = READ_Bx();
        if (unlikely(bx >= vm.global_func_capacity)) {
            if (!ensure_global_func_capacity(bx)) {
                runtime_error("全局函数数组扩容失败", GET_CURRENT_LINE());
                return -1;
            }
        }
        if (unlikely(bx >= vm.global_func_count)) {
            runtime_error("全局函数索引越界", GET_CURRENT_LINE());
            return -1;
        }
        R(a) = vm.global_funcs[bx];
        DISPATCH();
    }
    LABEL(OP_DEFGLOBALFUNC): {
        int a = READ_A();
        int bx = READ_Bx();
        if (unlikely(bx >= vm.global_func_capacity)) {
            if (!ensure_global_func_capacity(bx)) {
                runtime_error("全局函数数组扩容失败", GET_CURRENT_LINE());
                return -1;
            }
        }
        vm.global_funcs[bx] = R(a);
        if (bx >= vm.global_func_count) vm.global_func_count = bx + 1;
        DISPATCH();
    }

    // --- upvalue / 闭包 ---
    LABEL(OP_GETUPVAL): {
        int a = READ_A();
        int bx = READ_Bx();
        if (unlikely(frame->closure == NULL || bx >= frame->closure->upvalue_count)) {
            if (!throw_runtime_error("闭包 upvalue 索引越界", GET_CURRENT_LINE())) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        R(a) = *frame->closure->upvalues[bx]->location;
        DISPATCH();
    }
    LABEL(OP_SETUPVAL): {
        int a = READ_A();
        int bx = READ_Bx();
        if (unlikely(frame->closure == NULL || bx >= frame->closure->upvalue_count)) {
            if (!throw_runtime_error("闭包 upvalue 索引越界", GET_CURRENT_LINE())) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        *frame->closure->upvalues[bx]->location = R(a);
        DISPATCH();
    }
    LABEL(OP_CLOSE): {
        // 关闭 R[A .. A+Bx-1] 范围内的 upvalue（作用域退出 / 循环迭代边界）
        int a = READ_A();
        int count = READ_Bx();
        if (count > 0 && vm.open_upvalues != NULL) {
            close_upvalues(&frame->locals[a], count, 1);
        }
        DISPATCH();
    }
    LABEL(OP_CLOSURE): {
        int a = READ_A();
        int bx = READ_Bx();
        Value fv = frame->chunk->constants[bx];
        if (unlikely(!val_is_obj(fv) || val_as_obj(fv)->type != OBJ_FUNCTION)) {
            if (!throw_runtime_error("OP_CLOSURE: 常量不是函数对象", GET_CURRENT_LINE())) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        ObjFunction* function = (ObjFunction*)val_as_obj(fv);
        ObjClosure* closure = (ObjClosure*)gc_alloc(sizeof(ObjClosure), OBJ_CLOSURE);
        if (unlikely(!closure)) { runtime_error("OP_CLOSURE: 内存分配失败", GET_CURRENT_LINE()); return -1; }

        closure->function = function;
        closure->upvalue_count = function->upvalue_count;
        closure->type_param_count = 0;
        closure->type_param_args = NULL;

        // 紧随 OP_CLOSURE 的捕获描述（非指令数据，每条 4 字节保持网格对齐）：
        //   [is_local:u8][index:u8][is_value_capture:u8][pad:u8] × upvalue_count
        for (int i = 0; i < function->upvalue_count; i++) {
            uint8_t is_local = frame->ip[0];
            uint8_t index = frame->ip[1];
            uint8_t is_value_capture = frame->ip[2];
            frame->ip += 4;
            if (is_local) {
                if (unlikely(index >= frame->local_count)) {
                    if (!throw_runtime_error("闭包捕获变量索引越界", GET_CURRENT_LINE())) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                if (is_value_capture) {
                    Upvalue* uv = new_upvalue(val_null());
                    if (unlikely(!uv)) { runtime_error("OP_CLOSURE: 内存分配失败", GET_CURRENT_LINE()); return -1; }
                    uv->closed = frame->locals[index];
                    gc_write_barrier((Object*)uv, frame->locals[index]);
                    uv->location = &uv->closed;
                    closure->upvalues[i] = uv;
                } else {
                    Upvalue* uv = capture_upvalue(&frame->locals[index]);
                    if (unlikely(!uv)) { runtime_error("OP_CLOSURE: 内存分配失败", GET_CURRENT_LINE()); return -1; }
                    closure->upvalues[i] = uv;
                    // ★ 本帧从此挂着一个**指向自己窗口**的 open upvalue ⇒ 返回时必须 close。
                    //   这是 CallFrame.has_captures 的正确语义（"本帧有被捕获的局部量"），
                    //   而不是"本闭包捕获了父函数的 upvalue"—— 后者会让返回路径漏关 upvalue
                    //   （test_closure / test_closure_upvalue_rw / test_function_closure 当场失败；
                    //   栈式那边同样的坑见 D:\CLeno\Leno\src\vm\vminc\op_call.inc:671 的注释）。
                    //   只有**非值捕获**（is_value_capture == 0）才在 locals 里留指针，
                    //   值捕获当场就拷进 uv->closed，不需要这个标记。
                    frame->has_captures = 1;
                }
            } else {
                // 从当前闭包中继承（多层嵌套捕获）
                if (unlikely(frame->closure == NULL || index >= frame->closure->upvalue_count)) {
                    if (!throw_runtime_error("闭包捕获 upvalue 索引越界", GET_CURRENT_LINE())) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                closure->upvalues[i] = frame->closure->upvalues[index];
            }
        }

        R(a) = val_obj((Object*)closure);
        DISPATCH();
    }

    // --- 算术运算（与栈式 op_arith.inc 语义一致：int48 / 溢出提升 bigint / 字符串拼接 / null 报错） ---
