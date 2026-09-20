// ============================================================================
// 寄存器式 VM 执行核心逻辑
// 定长 4 字节指令，寄存器文件 = CallFrame.locals
// ============================================================================

    CallFrame* frame = &vm.frames[vm.frame_cnt - 1];
    vm.current_line = 0;

    // 寄存器文件 = frame->locals
    #define R(idx) (frame->locals[(idx)])

    // 读取 4 字节指令的字段（大端）
    // ⚠ DISPATCH 取指后已把 ip 前进 4 字节（ip 指向下一条指令），
    //   因此 LABEL 内取本指令操作数一律用 ip[-4..-1]。
    //   与 Lua 的 GETARG_A(*pc) 同一约定（pc 指向下一条指令）。
    #define READ_OPCODE()      (frame->ip[0])
    #define READ_A()            (frame->ip[-3])
    #define READ_B()            (frame->ip[-2])
    #define READ_C()            (frame->ip[-1])
    #define READ_Bx()           (((int)frame->ip[-2] << 8) | (int)frame->ip[-1])
    #define READ_sBx()          (READ_Bx() - 32768)
    #define READ_sJ()           REG_sJ(frame->ip[-3], frame->ip[-2], frame->ip[-1])
    #define READ_Ax()           (((int)frame->ip[-3] << 16) | ((int)frame->ip[-2] << 8) | (int)frame->ip[-1])
    #define GET_CURRENT_LINE()  (frame->chunk->lines[(int)(frame->ip - frame->chunk->code) - 4])

    // 读取"可扩展常量索引"：字段为 0 时表示紧随一条 EXTRAARG（24 位索引），
    // 读完后跳过它（编译器在常量索引为 0 或 > 255 时这么发）。
    #define READ_CONST_IDX(_cidx) \
        ((_cidx) != 0 ? (_cidx) : read_ext_const(&frame->ip))

    #define ADVANCE()          (frame->ip += 4)

    // =========================================================================
    // 跳转表定义（仅 GCC/Clang）
    // =========================================================================
    #if VM_USE_JUMPTABLE

    #define LABEL(op) LABEL_##op

    const void* jump_table[] = {
        [OP_NOP] = &&LABEL(OP_NOP),
        [OP_LOADK] = &&LABEL(OP_LOADK),
        [OP_LOADI] = &&LABEL(OP_LOADI),
        [OP_LOADF] = &&LABEL(OP_LOADF),
        [OP_LOADNIL] = &&LABEL(OP_LOADNIL),
        [OP_LOADTRUE] = &&LABEL(OP_LOADTRUE),
        [OP_LOADFALSE] = &&LABEL(OP_LOADFALSE),
        [OP_MOV] = &&LABEL(OP_MOV),
        [OP_GETGLOBAL] = &&LABEL(OP_GETGLOBAL),
        [OP_SETGLOBAL] = &&LABEL(OP_SETGLOBAL),
        [OP_DEFGLOBAL] = &&LABEL(OP_DEFGLOBAL),
        [OP_GETGLOBALFUNC] = &&LABEL(OP_GETGLOBALFUNC),
        [OP_DEFGLOBALFUNC] = &&LABEL(OP_DEFGLOBALFUNC),
        [OP_GETUPVAL] = &&LABEL(OP_GETUPVAL),
        [OP_SETUPVAL] = &&LABEL(OP_SETUPVAL),
        [OP_CLOSE] = &&LABEL(OP_CLOSE),
        [OP_CLOSURE] = &&LABEL(OP_CLOSURE),
        [OP_ADD] = &&LABEL(OP_ADD),
        [OP_SUB] = &&LABEL(OP_SUB),
        [OP_MUL] = &&LABEL(OP_MUL),
        [OP_DIV] = &&LABEL(OP_DIV),
        [OP_MOD] = &&LABEL(OP_MOD),
        [OP_NEG] = &&LABEL(OP_NEG),
        [OP_NOT] = &&LABEL(OP_NOT),
        [OP_ADD_INT] = &&LABEL(OP_ADD_INT),
        [OP_SUB_INT] = &&LABEL(OP_SUB_INT),
        [OP_MUL_INT] = &&LABEL(OP_MUL_INT),
        [OP_NEG_INT] = &&LABEL(OP_NEG_INT),
        [OP_ADD_F] = &&LABEL(OP_ADD_F),
        [OP_SUB_F] = &&LABEL(OP_SUB_F),
        [OP_MUL_F] = &&LABEL(OP_MUL_F),
        [OP_DIV_F] = &&LABEL(OP_DIV_F),
        [OP_NEG_F] = &&LABEL(OP_NEG_F),
        [OP_EQ] = &&LABEL(OP_EQ),
        [OP_LT] = &&LABEL(OP_LT),
        [OP_GT] = &&LABEL(OP_GT),
        [OP_LE] = &&LABEL(OP_LE),
        [OP_GE] = &&LABEL(OP_GE),
        [OP_NEQ] = &&LABEL(OP_NEQ),
        [OP_LT_INT] = &&LABEL(OP_LT_INT),
        [OP_GT_INT] = &&LABEL(OP_GT_INT),
        [OP_LE_INT] = &&LABEL(OP_LE_INT),
        [OP_GE_INT] = &&LABEL(OP_GE_INT),
        [OP_EQ_INT] = &&LABEL(OP_EQ_INT),
        [OP_BITAND] = &&LABEL(OP_BITAND),
        [OP_BITOR] = &&LABEL(OP_BITOR),
        [OP_BITXOR] = &&LABEL(OP_BITXOR),
        [OP_BITNOT] = &&LABEL(OP_BITNOT),
        [OP_SHL] = &&LABEL(OP_SHL),
        [OP_SHR] = &&LABEL(OP_SHR),
        [OP_USHR] = &&LABEL(OP_USHR),
        [OP_CAST_FLOAT] = &&LABEL(OP_CAST_FLOAT),
        [OP_CAST_INT] = &&LABEL(OP_CAST_INT),
        [OP_CAST_STRING] = &&LABEL(OP_CAST_STRING),
        [OP_IS_NULL] = &&LABEL(OP_IS_NULL),
        [OP_STRCAT] = &&LABEL(OP_STRCAT),
        [OP_JMP] = &&LABEL(OP_JMP),
        [OP_JMP_IF_FALSE] = &&LABEL(OP_JMP_IF_FALSE),
        [OP_JMP_IF_TRUE] = &&LABEL(OP_JMP_IF_TRUE),
        [OP_TEST] = &&LABEL(OP_TEST),
        [OP_INC] = &&LABEL(OP_INC),
        [OP_DEC] = &&LABEL(OP_DEC),
        [OP_CALL] = &&LABEL(OP_CALL),
        [OP_CALL_NATIVE] = &&LABEL(OP_CALL_NATIVE),
        [OP_RETURN] = &&LABEL(OP_RETURN),
        [OP_RETURN_MULTI] = &&LABEL(OP_RETURN_MULTI),
        [OP_TAIL_CALL] = &&LABEL(OP_TAIL_CALL),
        [OP_NEWARRAY] = &&LABEL(OP_NEWARRAY),
        [OP_ARRAY_GET] = &&LABEL(OP_ARRAY_GET),
        [OP_ARRAY_SET] = &&LABEL(OP_ARRAY_SET),
        [OP_ARRAY_APPEND] = &&LABEL(OP_ARRAY_APPEND),
        [OP_LEN] = &&LABEL(OP_LEN),
        [OP_NEWDICT] = &&LABEL(OP_NEWDICT),
        [OP_DICT_GET] = &&LABEL(OP_DICT_GET),
        [OP_DICT_SET] = &&LABEL(OP_DICT_SET),
        [OP_DICT_GET_KEY] = &&LABEL(OP_DICT_GET_KEY),
        [OP_INDEX] = &&LABEL(OP_INDEX),
        [OP_INDEX_SET] = &&LABEL(OP_INDEX_SET),
        [OP_SLICE] = &&LABEL(OP_SLICE),
        [OP_ITER_GET] = &&LABEL(OP_ITER_GET),
        [OP_ITER_GET_VALUE] = &&LABEL(OP_ITER_GET_VALUE),
        [OP_RANGE] = &&LABEL(OP_RANGE),
        [OP_IN] = &&LABEL(OP_IN),
        [OP_TRY] = &&LABEL(OP_TRY),
        [OP_CATCH] = &&LABEL(OP_CATCH),
        [OP_FINALLY] = &&LABEL(OP_FINALLY),
        [OP_END_TRY] = &&LABEL(OP_END_TRY),
        [OP_THROW] = &&LABEL(OP_THROW),
        [OP_LOAD_NATIVE_MODULE] = &&LABEL(OP_LOAD_NATIVE_MODULE),
        [OP_MODULE_CALL] = &&LABEL(OP_MODULE_CALL),
        [OP_GET_MODULE_CONST] = &&LABEL(OP_GET_MODULE_CONST),
        [OP_GET_MODULE_VAR] = &&LABEL(OP_GET_MODULE_VAR),
        [OP_SET_MODULE_VAR] = &&LABEL(OP_SET_MODULE_VAR),
        [OP_GET_MODULE_FUNC] = &&LABEL(OP_GET_MODULE_FUNC),
        [OP_DEFINE_MODULE_FUNC] = &&LABEL(OP_DEFINE_MODULE_FUNC),
        [OP_INIT_LENOMODULE] = &&LABEL(OP_INIT_LENOMODULE),
        [OP_STRUCT_DEF] = &&LABEL(OP_STRUCT_DEF),
        [OP_STRUCT_INIT] = &&LABEL(OP_STRUCT_INIT),
        [OP_GET_FIELD] = &&LABEL(OP_GET_FIELD),
        [OP_SET_FIELD] = &&LABEL(OP_SET_FIELD),
        [OP_GET_FIELD_ADDR] = &&LABEL(OP_GET_FIELD_ADDR),
        [OP_GET_METHOD] = &&LABEL(OP_GET_METHOD),
        [OP_SET_PTR_ELEM_TYPE] = &&LABEL(OP_SET_PTR_ELEM_TYPE),
        [OP_SET_DECLARED_FACE] = &&LABEL(OP_SET_DECLARED_FACE),
        [OP_ENUM_DEF] = &&LABEL(OP_ENUM_DEF),
        [OP_FACE_DEF] = &&LABEL(OP_FACE_DEF),
        [OP_CSTRUCT_DEF] = &&LABEL(OP_CSTRUCT_DEF),
        [OP_GET_CSTRUCT_DEF] = &&LABEL(OP_GET_CSTRUCT_DEF),
        [OP_TYPE_CHECK] = &&LABEL(OP_TYPE_CHECK),
        [OP_AS_CAST] = &&LABEL(OP_AS_CAST),
        [OP_AWAIT] = &&LABEL(OP_AWAIT),
        [OP_ASYNC_CALL] = &&LABEL(OP_ASYNC_CALL),
        [OP_CLIB_CALL] = &&LABEL(OP_CLIB_CALL),
        [OP_CFUNC_CALLBACK] = &&LABEL(OP_CFUNC_CALLBACK),
        [OP_U8_TO_F64] = &&LABEL(OP_U8_TO_F64),
        [OP_PUSH_TYPE_ARGS] = &&LABEL(OP_PUSH_TYPE_ARGS),
        [OP_DTOR_LOCAL] = &&LABEL(OP_DTOR_LOCAL),
        [OP_SWITCH_LOOKUP] = &&LABEL(OP_SWITCH_LOOKUP),
        [OP_FOR_PREP] = &&LABEL(OP_FOR_PREP),
        [OP_FOR_LOOP] = &&LABEL(OP_FOR_LOOP),
        [OP_EXTRAARG] = &&LABEL(OP_EXTRAARG),
        [OP_EXTEND] = &&LABEL(OP_EXTEND),
    };

    #define DISPATCH() \
        do { \
            uint8_t op = frame->ip[0]; \
            ADVANCE(); \
            vm.current_line = GET_CURRENT_LINE(); \
            goto *jump_table[op]; \
        } while (0)

    #else // switch 分发

    #define DISPATCH() \
        do { \
            ADVANCE(); \
            goto dispatch_loop; \
        } while (0)

    #endif

    // =========================================================================
    // 帧返回收尾（寄存器式）
    //   关闭 upvalue → 弹出帧 → 结果写回调用方寄存器（ret_frame/ret_reg）
    //   或宿主栈（ret_frame < 0）；宿主停止条件 / 协程顶层返回在此判定。
    //   末尾会重设 frame（或 goto vm_exit）。
    // =========================================================================
    #define REG_FINISH_RETURN(_result) do { \
        Value _res = (_result); \
        int _ret_frame = frame->ret_frame; \
        int _ret_reg = frame->ret_reg; \
        int _ret_want = frame->ret_want; \
        int _stack_base = frame->stack_base; \
        Value* _locals = frame->locals; \
        int _lc = frame->local_count; \
        int _dyn = frame->locals_is_dynamic; \
        if (_locals != NULL && _lc > 0 && vm.open_upvalues != NULL) { \
            close_upvalues(_locals, _lc, 1); \
        } \
        vm.frame_cnt--; \
        if (vm.stop_frame_cnt > 0 && vm.frame_cnt <= vm.stop_frame_cnt) { \
            vm.sp = _stack_base; \
            vm_stack_push(&vm, _res); \
            vm.last_return_value = _res; \
            vm.last_return_count = 1; \
            vm.last_return_values[0] = _res; \
            vm.stop_frame_cnt = 0; \
            if (_locals != NULL && _dyn) free(_locals); \
            return 0; \
        } \
        if (vm.current_coroutine && vm.frame_cnt < vm.current_coroutine->saved_frame_cnt) { \
            vm.current_coroutine->result = _res; \
            gc_write_barrier((Object*)vm.current_coroutine, _res); \
            if (_locals != NULL && _dyn) free(_locals); \
            return 0; \
        } \
        vm.last_return_value = _res; \
        vm.last_return_count = 1; \
        vm.last_return_values[0] = _res; \
        if (_ret_frame >= 0 && _ret_frame < vm.frame_cnt) { \
            if (_ret_want > 0) { \
                Value* _t = &vm.frames[_ret_frame].locals[_ret_reg]; \
                _t[0] = _res; \
                for (int _i = 1; _i < _ret_want; _i++) _t[_i] = val_null(); \
            } \
        } else if (_ret_frame < 0 && vm.frame_cnt > 0) { \
            vm.sp = _stack_base; \
            vm_stack_push(&vm, _res); \
        } \
        if (_locals != NULL && _dyn) free(_locals); \
        if (vm.frame_cnt == 0) goto vm_exit; \
        frame = &vm.frames[vm.frame_cnt - 1]; \
        vm.sp = frame->stack_base; \
    } while (0)

    // =========================================================================
    // 主循环
    // =========================================================================

    #if VM_USE_JUMPTABLE
        DISPATCH();
    #else
        for (;;) {
            dispatch_loop:
            {
                uint8_t op = frame->ip[0];
                ADVANCE();
                vm.current_line = GET_CURRENT_LINE();
                switch (op) {
    #endif

    // --- NOP ---
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
    LABEL(OP_ADD): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        int line = GET_CURRENT_LINE();

        if (likely(val_is_int(bv) && val_is_int(cv))) {
            int64_t x = val_as_int(bv), y = val_as_int(cv), s = 0;
            if (unlikely(__builtin_add_overflow(x, y, &s))) {
                R(a) = bigint_add(bigint_from_int64(x), bigint_from_int64(y));
            } else if (unlikely(s < INT48_MIN || s > INT48_MAX)) {
                R(a) = val_obj((Object*)bigint_from_int64(s));
            } else {
                R(a) = val_int(s);
            }
            DISPATCH();
        }

        // 任一侧是字符串 → 拼接
        if ((val_is_obj(bv) && val_as_obj(bv)->type == OBJ_STRING) ||
            (val_is_obj(cv) && val_as_obj(cv)->type == OBJ_STRING)) {
            R(a) = string_add(bv, cv);
            DISPATCH();
        }

        if (val_is_float(bv) || val_is_float(cv)) {
            R(a) = val_float(val_as_num_ex(bv) + val_as_num_ex(cv));
            DISPATCH();
        }

        if (unlikely(val_is_bigint(bv) || val_is_bigint(cv))) {
            if (val_is_bigint(bv) && val_is_int(cv)) {
                R(a) = bigint_add_i64(val_as_bigint(bv), val_as_int(cv));
                DISPATCH();
            }
            if (val_is_bigint(cv) && val_is_int(bv)) {
                R(a) = bigint_add_i64(val_as_bigint(cv), val_as_int(bv));
                DISPATCH();
            }
            R(a) = bigint_add(promote_to_bigint(bv), promote_to_bigint(cv));
            DISPATCH();
        }

        if (val_is_null(bv) || val_is_null(cv)) {
            if (!throw_runtime_error_with_operands("加法运算: null 不能参与运算", "ADD (+)", bv, cv, line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }

        // 其他类型：转字符串拼接
        R(a) = string_add(bv, cv);
        DISPATCH();
    }
    LABEL(OP_SUB): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        int line = GET_CURRENT_LINE();

        if (likely(val_is_int(bv) && val_is_int(cv))) {
            int64_t x = val_as_int(bv), y = val_as_int(cv), s = 0;
            if (unlikely(__builtin_sub_overflow(x, y, &s))) {
                R(a) = bigint_sub(bigint_from_int64(x), bigint_from_int64(y));
            } else if (unlikely(s < INT48_MIN || s > INT48_MAX)) {
                R(a) = val_obj((Object*)bigint_from_int64(s));
            } else {
                R(a) = val_int(s);
            }
            DISPATCH();
        }
        if (val_is_float(bv) || val_is_float(cv)) {
            R(a) = val_float(val_as_num_ex(bv) - val_as_num_ex(cv));
            DISPATCH();
        }
        if (val_is_null(bv) || val_is_null(cv)) {
            if (!throw_runtime_error_with_operands("减法: null 不能参与运算", "SUB (-)", bv, cv, line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        if (unlikely(val_is_bigint(bv) || val_is_bigint(cv))) {
            R(a) = bigint_sub(promote_to_bigint(bv), promote_to_bigint(cv));
            DISPATCH();
        }
        if (!throw_vm_type_error("操作数必须是数字", "SUB (-)", bv, cv, line)) return -1;
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH();
    }
    LABEL(OP_MUL): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        int line = GET_CURRENT_LINE();

        if (likely(val_is_int(bv) && val_is_int(cv))) {
            int64_t x = val_as_int(bv), y = val_as_int(cv), s = 0;
            if (x == 0 || y == 0) {
                R(a) = val_int(0);
            } else if (unlikely(LENO_MUL_OVERFLOW(x, y, &s))) {
                R(a) = bigint_mul(bigint_from_int64(x), bigint_from_int64(y));
            } else if (unlikely(s < INT48_MIN || s > INT48_MAX)) {
                R(a) = val_obj((Object*)bigint_from_int64(s));
            } else {
                R(a) = val_int(s);
            }
            DISPATCH();
        }
        if (val_is_float(bv) || val_is_float(cv)) {
            R(a) = val_float(val_as_num_ex(bv) * val_as_num_ex(cv));
            DISPATCH();
        }
        if (val_is_null(bv) || val_is_null(cv)) {
            if (!throw_runtime_error_with_operands("乘法: null 不能参与运算", "MUL (*)", bv, cv, line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        if (unlikely(val_is_bigint(bv) || val_is_bigint(cv))) {
            R(a) = bigint_mul(promote_to_bigint(bv), promote_to_bigint(cv));
            DISPATCH();
        }
        if (!throw_vm_type_error("操作数必须是数字", "MUL (*)", bv, cv, line)) return -1;
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH();
    }
    LABEL(OP_DIV): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        int line = GET_CURRENT_LINE();

        if (likely(val_is_int(bv) && val_is_int(cv))) {
            int64_t dv = val_as_int(cv);
            if (unlikely(dv == 0)) {
                if (!throw_runtime_error_with_operands("除零错误：除数为 0", "DIV (/)", bv, cv, line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            R(a) = val_int(val_as_int(bv) / dv);
            DISPATCH();
        }
        if (val_is_float(bv) || val_is_float(cv)) {
            double dv = val_as_num_ex(cv);
            if (unlikely(dv == 0)) {
                if (!throw_runtime_error_with_operands("除零错误：除数为 0", "DIV (/)", bv, cv, line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            R(a) = val_float(val_as_num_ex(bv) / dv);
            DISPATCH();
        }
        if (unlikely(val_is_bigint(bv) || val_is_bigint(cv))) {
            if (val_is_bigint(cv)) {
                ObjBigInt* bb = val_as_bigint(cv);
                if (bb->limb_count == 1 && bb->limbs[0] == 0) {
                    if (!throw_runtime_error_with_operands("除零错误：除数为 0", "DIV (/)", bv, cv, line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
            } else if (val_as_num(cv) == 0) {
                if (!throw_runtime_error_with_operands("除零错误：除数为 0", "DIV (/)", bv, cv, line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            R(a) = bigint_div(promote_to_bigint(bv), promote_to_bigint(cv));
            DISPATCH();
        }
        if (val_is_null(bv) || val_is_null(cv)) {
            if (!throw_runtime_error_with_operands("除法: null 不能参与运算", "DIV (/)", bv, cv, line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        if (!throw_vm_type_error("操作数必须是数字", "DIV (/)", bv, cv, line)) return -1;
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH();
    }
    LABEL(OP_MOD): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        int line = GET_CURRENT_LINE();

        if (likely(val_is_int(bv) && val_is_int(cv))) {
            int64_t dv = val_as_int(cv);
            if (unlikely(dv == 0)) {
                if (!throw_runtime_error_with_operands("取模除零错误：除数为 0", "MOD (%)", bv, cv, line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            R(a) = val_int(val_as_int(bv) % dv);
            DISPATCH();
        }
        if (val_is_null(bv) || val_is_null(cv)) {
            if (!throw_runtime_error_with_operands("取模: null 不能参与运算", "MOD (%)", bv, cv, line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        if (unlikely(val_is_bigint(bv) || val_is_bigint(cv))) {
            ObjBigInt* bb = promote_to_bigint(cv);
            if (bb->limb_count == 1 && bb->limbs[0] == 0) {
                if (!throw_runtime_error_with_operands("取模除零错误：除数为 0", "MOD (%)", bv, cv, line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            R(a) = bigint_mod(promote_to_bigint(bv), bb);
            DISPATCH();
        }
        if (!throw_vm_type_error("取模操作数必须是整数", "MOD (%)", bv, cv, line)) return -1;
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH();
    }
    LABEL(OP_NEG): {
        int a = READ_A(), b = READ_B();
        Value bv = R(b);
        int line = GET_CURRENT_LINE();

        if (likely(val_is_int(bv))) {
            int64_t x = val_as_int(bv);
            if (unlikely(x == INT64_MIN)) {
                R(a) = bigint_sub(bigint_from_int64(0), bigint_from_int64(x));
            } else if (unlikely(-x < INT48_MIN || -x > INT48_MAX)) {
                R(a) = val_obj((Object*)bigint_from_int64(-x));
            } else {
                R(a) = val_int(-x);
            }
            DISPATCH();
        }
        if (val_is_float(bv)) {
            R(a) = val_float(-val_as_double(bv));
            DISPATCH();
        }
        if (unlikely(val_is_bigint(bv))) {
            R(a) = bigint_neg(val_as_bigint(bv));
            DISPATCH();
        }
        if (!throw_runtime_error_with_operand("取负 (-): 操作数必须是数字", "NEG (-)", bv, line)) return -1;
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH();
    }
    LABEL(OP_NOT): {
        int a = READ_A(), b = READ_B();
        R(a) = val_bool(!val_is_truthy(R(b)));
        DISPATCH();
    }

    // --- int 特化 ---
    LABEL(OP_ADD_INT): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        R(a) = val_int(val_as_int(R(b)) + val_as_int(R(c)));
        DISPATCH();
    }
    LABEL(OP_SUB_INT): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        R(a) = val_int(val_as_int(R(b)) - val_as_int(R(c)));
        DISPATCH();
    }
    LABEL(OP_MUL_INT): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        R(a) = val_int(val_as_int(R(b)) * val_as_int(R(c)));
        DISPATCH();
    }
    LABEL(OP_NEG_INT): {
        int a = READ_A(), b = READ_B();
        R(a) = val_int(-val_as_int(R(b)));
        DISPATCH();
    }

    // --- float 特化 ---
    LABEL(OP_ADD_F): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        R(a) = val_float(val_as_double(R(b)) + val_as_double(R(c)));
        DISPATCH();
    }
    LABEL(OP_SUB_F): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        R(a) = val_float(val_as_double(R(b)) - val_as_double(R(c)));
        DISPATCH();
    }
    LABEL(OP_MUL_F): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        R(a) = val_float(val_as_double(R(b)) * val_as_double(R(c)));
        DISPATCH();
    }
    LABEL(OP_DIV_F): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        R(a) = val_float(val_as_double(R(b)) / val_as_double(R(c)));
        DISPATCH();
    }
    LABEL(OP_NEG_F): {
        int a = READ_A(), b = READ_B();
        R(a) = val_float(-val_as_double(R(b)));
        DISPATCH();
    }

    // --- 比较运算（完整语义见 vm.c 的 value_eq_stdlib / value_compare_stdlib）---
    LABEL(OP_EQ): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        R(a) = val_bool(value_eq_stdlib(R(b), R(c)));
        DISPATCH();
    }
    LABEL(OP_NEQ): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        R(a) = val_bool(!value_eq_stdlib(R(b), R(c)));
        DISPATCH();
    }
    LABEL(OP_LT): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        int cmp = 0;
        if (likely(value_compare_stdlib(bv, cv, &cmp))) {
            R(a) = val_bool(cmp < 0);
            DISPATCH();
        }
        if (!throw_runtime_error_with_operands("比较运算: 操作数类型不可比较", "LT (<)", bv, cv, GET_CURRENT_LINE())) return -1;
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH();
    }
    LABEL(OP_GT): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        int cmp = 0;
        if (likely(value_compare_stdlib(bv, cv, &cmp))) {
            R(a) = val_bool(cmp > 0);
            DISPATCH();
        }
        if (!throw_runtime_error_with_operands("比较运算: 操作数类型不可比较", "GT (>)", bv, cv, GET_CURRENT_LINE())) return -1;
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH();
    }
    LABEL(OP_LE): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        int cmp = 0;
        if (likely(value_compare_stdlib(bv, cv, &cmp))) {
            R(a) = val_bool(cmp <= 0);
            DISPATCH();
        }
        if (!throw_runtime_error_with_operands("比较运算: 操作数类型不可比较", "LE (<=)", bv, cv, GET_CURRENT_LINE())) return -1;
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH();
    }
    LABEL(OP_GE): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        int cmp = 0;
        if (likely(value_compare_stdlib(bv, cv, &cmp))) {
            R(a) = val_bool(cmp >= 0);
            DISPATCH();
        }
        if (!throw_runtime_error_with_operands("比较运算: 操作数类型不可比较", "GE (>=)", bv, cv, GET_CURRENT_LINE())) return -1;
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH();
    }

    // --- int 比较 ---
    LABEL(OP_LT_INT): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        R(a) = val_bool(val_as_int(R(b)) < val_as_int(R(c)));
        DISPATCH();
    }
    LABEL(OP_GT_INT): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        R(a) = val_bool(val_as_int(R(b)) > val_as_int(R(c)));
        DISPATCH();
    }
    LABEL(OP_LE_INT): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        R(a) = val_bool(val_as_int(R(b)) <= val_as_int(R(c)));
        DISPATCH();
    }
    LABEL(OP_GE_INT): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        R(a) = val_bool(val_as_int(R(b)) >= val_as_int(R(c)));
        DISPATCH();
    }
    LABEL(OP_EQ_INT): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        R(a) = val_bool(val_as_int(R(b)) == val_as_int(R(c)));
        DISPATCH();
    }

    // --- 位运算 ---
    LABEL(OP_BITAND): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        R(a) = val_int(val_as_int(R(b)) & val_as_int(R(c)));
        DISPATCH();
    }
    LABEL(OP_BITOR): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        R(a) = val_int(val_as_int(R(b)) | val_as_int(R(c)));
        DISPATCH();
    }
    LABEL(OP_BITXOR): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        R(a) = val_int(val_as_int(R(b)) ^ val_as_int(R(c)));
        DISPATCH();
    }
    LABEL(OP_BITNOT): {
        int a = READ_A(), b = READ_B();
        R(a) = val_int(~val_as_int(R(b)));
        DISPATCH();
    }
    LABEL(OP_SHL): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        R(a) = val_int(val_as_int(R(b)) << val_as_int(R(c)));
        DISPATCH();
    }
    LABEL(OP_SHR): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        R(a) = val_int(val_as_int(R(b)) >> val_as_int(R(c)));
        DISPATCH();
    }
    LABEL(OP_USHR): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        R(a) = val_int((int)((unsigned int)val_as_int(R(b)) >> val_as_int(R(c))));
        DISPATCH();
    }

    // --- 类型转换（语义同栈式 op_unary.inc）---
    LABEL(OP_CAST_FLOAT): {
        int a = READ_A(), b = READ_B();
        Value v = R(b);
        if (val_is_int(v) || val_is_float(v)) {
            R(a) = val_float(val_as_num(v));
            DISPATCH();
        }
        if (val_is_bool(v)) {
            R(a) = val_float(val_as_bool(v) ? 1.0 : 0.0);
            DISPATCH();
        }
        if (val_is_bigint(v)) {
            R(a) = val_float(bigint_to_double(val_as_bigint(v)));
            DISPATCH();
        }
        if (val_is_null(v)) {
            R(a) = v;
            DISPATCH();
        }
        if (!throw_runtime_error_with_operand("类型转换 float: 操作数必须是数字", "CAST_FLOAT", v, GET_CURRENT_LINE())) return -1;
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH();
    }
    LABEL(OP_CAST_INT): {
        int a = READ_A(), b = READ_B();
        Value v = R(b);
        if (val_is_bigint(v)) {
            ObjBigInt* bi = val_as_bigint(v);
            int64_t iv = bigint_to_int64(bi);
            if (iv >= INT48_MIN && iv <= INT48_MAX) R(a) = val_int(iv);
            else R(a) = v;   // 超出 int48 → 保持 bigint
            DISPATCH();
        }
        if (val_is_num(v)) {
            R(a) = val_int((int64_t)val_as_num(v));
            DISPATCH();
        }
        if (val_is_bool(v)) {
            R(a) = val_int(val_as_bool(v) ? 1 : 0);
            DISPATCH();
        }
        if (val_is_null(v)) {
            R(a) = v;
            DISPATCH();
        }
        if (!throw_runtime_error_with_operand("类型转换 int: 操作数必须是数字", "CAST_INT", v, GET_CURRENT_LINE())) return -1;
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH();
    }
    LABEL(OP_CAST_STRING): {
        int a = READ_A(), b = READ_B();
        Value v = R(b);
        if (val_is_obj(v) && val_as_obj(v)->type == OBJ_STRING) {
            R(a) = v;
            DISPATCH();
        }
        if (val_is_null(v)) {
            R(a) = v;
            DISPATCH();
        }
        {
            const char* s = val_to_string(v);
            R(a) = val_obj((Object*)str_copy(s, (int)strlen(s)));
        }
        DISPATCH();
    }
    LABEL(OP_IS_NULL): {
        int a = READ_A(), b = READ_B();
        R(a) = val_bool(val_is_null(R(b)));
        DISPATCH();
    }

    // --- 字符串拼接 ---
    LABEL(OP_STRCAT): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        R(a) = string_add(R(b), R(c));
        DISPATCH();
    }

    // --- 跳转 ---
    LABEL(OP_JMP): {
        int sj = READ_sJ();
        frame->ip += sj;  // ip 已经 ADVANCE 了 4 字节，sj 是相对于下一条指令的偏移
        DISPATCH();
    }
    LABEL(OP_JMP_IF_FALSE): {
        int a = READ_A();
        int sbx = READ_sBx();
        if (!val_is_truthy(R(a))) {
            frame->ip += sbx;
        }
        DISPATCH();
    }
    LABEL(OP_JMP_IF_TRUE): {
        int a = READ_A();
        int sbx = READ_sBx();
        if (val_is_truthy(R(a))) {
            frame->ip += sbx;
        }
        DISPATCH();
    }
    LABEL(OP_TEST): {
        int b = READ_B(), c = READ_C();
        if (c == 0) { // not
            if (!val_is_truthy(R(b))) frame->ip += 4;  // 跳过下一条
        }
        DISPATCH();
    }

    // --- 自增自减（保持操作数类型；int 溢出/超 int48 自动提升 bigint）---
    LABEL(OP_INC): {
        int a = READ_A(), b = READ_B();
        int err = 0;
        Value r = do_inc_dec(R(b), 1, &err);
        if (unlikely(err)) {
            if (!throw_runtime_error_with_operand("自增 (++): 操作数必须是数字", "INC (++)", R(b), GET_CURRENT_LINE())) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        R(a) = r;
        DISPATCH();
    }
    LABEL(OP_DEC): {
        int a = READ_A(), b = READ_B();
        int err = 0;
        Value r = do_inc_dec(R(b), -1, &err);
        if (unlikely(err)) {
            if (!throw_runtime_error_with_operand("自减 (--): 操作数必须是数字", "DEC (--)", R(b), GET_CURRENT_LINE())) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        R(a) = r;
        DISPATCH();
    }

    // --- 调用 / 返回 ---
    // OP_CALL A B C: R[A] = callee, R[A+1 .. A+B-1] = 实参, B = nargs+1, C = nresults+1
    LABEL(OP_CALL): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        int nargs = b - 1;
        int nresults = c - 1;
        if (nresults < 0) nresults = 1;
        Value callee = R(a);
        int line = GET_CURRENT_LINE();

        if (likely(val_is_obj(callee))) {
            ObjType ct = val_as_obj(callee)->type;

            // --- 脚本闭包：压入新帧，解释器继续在新帧执行 ---
            if (ct == OBJ_CLOSURE) {
                if (likely(call_reg((ObjClosure*)val_as_obj(callee), 0, val_null(),
                                    a + 1, nargs, a, nresults, line))) {
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                if (!vm.has_exception) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }

            // --- 裸函数对象：现场包装成闭包后调用 ---
            if (ct == OBJ_FUNCTION) {
                ObjFunction* f = (ObjFunction*)val_as_obj(callee);
                ObjClosure* cl = (ObjClosure*)gc_alloc(sizeof(ObjClosure), OBJ_CLOSURE);
                if (unlikely(!cl)) { runtime_error("内存分配失败", line); return -1; }
                cl->function = f;
                cl->upvalue_count = 0;
                cl->type_param_count = 0;
                cl->type_param_args = NULL;
                for (int i = 0; i < MAX_UPVALUES; i++) cl->upvalues[i] = NULL;
                if (call_reg(cl, 0, val_null(), a + 1, nargs, a, nresults, line)) {
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                if (!vm.has_exception) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }

            // --- 原生函数：同步调用，结果直接写 R[A] ---
            if (ct == OBJ_NATIVE) {
                ObjNative* native = (ObjNative*)val_as_obj(callee);
                int base = vm.sp;
                for (int i = 0; i < nargs; i++) vm_stack_push(&vm, R(a + 1 + i));
                Value result = native->function(nargs, vm.stack + base);
                vm.sp = base;
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
                if (nresults > 0) R(a) = result;
                DISPATCH();
            }

            // --- 绑定方法 ---
            // ⚠ receiver 去重：语义分析对 struct 方法调用会把 receiver 作为第 0 个
            //   实参放进 args（栈式对应 OP_INVOKE_METHOD_TYPED），而内置类型方法不带。
            //   若实参首项就是 receiver，则不重复插入，否则隐式补上。
            if (ct == OBJ_BOUND_METHOD) {
                ObjBoundMethod* bound = (ObjBoundMethod*)val_as_obj(callee);
                int self_in_args = 0;
                if (nargs > 0) {
                    Value a0 = R(a + 1);
                    if (val_is_obj(a0) && val_is_obj(bound->receiver)) {
                        self_in_args = (val_as_obj(a0) == val_as_obj(bound->receiver));
                    } else if (!val_is_obj(a0) && !val_is_obj(bound->receiver)) {
                        self_in_args = (a0 == bound->receiver);
                    }
                }
                if (bound->closure) {
                    if (call_reg(bound->closure, self_in_args ? 0 : 1, bound->receiver,
                                 a + 1, nargs, a, nresults, line)) {
                        frame = &vm.frames[vm.frame_cnt - 1];
                        DISPATCH();
                    }
                    if (!vm.has_exception) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                ObjNative* native = bound->method;
                if (native == NULL) {
                    if (!throw_runtime_error("绑定方法无效", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                int base = vm.sp;
                int pass_nargs;
                if (self_in_args) {
                    for (int i = 0; i < nargs; i++) vm_stack_push(&vm, R(a + 1 + i));
                    pass_nargs = nargs;
                } else {
                    vm_stack_push(&vm, bound->receiver);
                    for (int i = 0; i < nargs; i++) vm_stack_push(&vm, R(a + 1 + i));
                    pass_nargs = nargs + 1;
                }
                Value result = native->function(pass_nargs, vm.stack + base);
                vm.sp = base;
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
                if (nresults > 0) R(a) = result;
                DISPATCH();
            }

            // --- 结构体定义：按位置参数构造实例 ---
            if (ct == OBJ_STRUCT_DEF) {
                ObjStructDef* def = (ObjStructDef*)val_as_obj(callee);
                ObjStruct* obj = struct_instance_new(def);
                if (unlikely(!obj)) { runtime_error("内存不足，无法创建结构体实例", line); return -1; }
                for (int i = 0; i < nargs && i < def->field_count; i++) {
                    Value fv = R(a + 1 + i);
                    TypeKind et = def->fields[i].type;
                    if (et == TYPE_FLOAT && val_is_int(fv)) {
                        fv = val_float((double)val_as_int(fv));
                    } else if (et == TYPE_FLOAT && val_is_bigint(fv)) {
                        fv = val_float(bigint_to_double(val_as_bigint(fv)));
                    }
                    struct_set_field(obj, i, fv);
                }
                if (nresults > 0) R(a) = val_obj((Object*)obj);
                DISPATCH();
            }
        }

        // 不可调用
        {
            const char* msg = val_is_null(callee) ? "函数未定义" : "只能调用函数（不是对象类型）";
            if (!throw_runtime_error(msg, line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
    }

    // OP_CALL_NATIVE A B C: R[A] = native(K[B])(R[A+1 .. A+C])
    LABEL(OP_CALL_NATIVE): {
        int a = READ_A(), b = READ_B();
        int nargs = READ_C();
        int line = GET_CURRENT_LINE();
        int cidx = READ_CONST_IDX(b);
        if (unlikely(cidx < 0 || cidx >= frame->chunk->const_cnt)) {
            if (!throw_runtime_error("native 函数名常量索引越界", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        Value name_val = frame->chunk->constants[cidx];
        if (unlikely(!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING)) {
            if (!throw_runtime_error("native 函数名必须是字符串", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }

        uint8_t* ip_before = frame->ip - 4;
        int cache_idx = (int)(((uintptr_t)ip_before >> 2) & (IC_NATIVE_CACHE_SIZE - 1));
        InlineNativeCacheEntry* cache = &vm.ic_native_cache[cache_idx];
        uint32_t name_hash = ((ObjString*)val_as_obj(name_val))->hash;

        ObjNative* native;
        if (likely(cache->valid && cache->name_hash == name_hash)) {
            vm.ic_hits++;
            native = cache->fn;
        } else {
            vm.ic_misses++;
            native = native_find_function(((ObjString*)val_as_obj(name_val))->chars);
            if (unlikely(!native)) {
                if (!throw_runtime_error("未找到 native 函数", line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            cache->valid = 1;
            cache->name_hash = name_hash;
            cache->fn = native;
        }

        int base = vm.sp;
        for (int i = 0; i < nargs; i++) vm_stack_push(&vm, R(a + 1 + i));
        Value result = native->function(nargs, vm.stack + base);
        vm.sp = base;

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

    // OP_RETURN A B: 返回 R[A .. A+B-2]（B-1 个值）
    LABEL(OP_RETURN): {
        int a = READ_A(), b = READ_B();
        int nresults = b - 1;

        // 返回是天然 GC 安全点（与栈式一致：每 256 次 return 检查延迟 GC）
        if (unlikely(vm.gc_force_request)) {
            vm.gc_force_request = 0;
            gc_force_collect();
        } else if (unlikely(++vm.gc_return_counter >= 256)) {
            vm.gc_return_counter = 0;
            if (gc.deferred_gc && gc.enabled && !gc.running) {
                gc_try_collect_deferred();
            }
        }

        Value result = (nresults >= 1) ? R(a) : val_null();
        // 构造函数：返回 self（R0）
        if (frame->closure && frame->closure->function && frame->closure->function->is_ctor) {
            result = frame->locals[0];
        }

        // 有 finally 且尚未执行：先执行 finally，返回值暂存
        if (FRAME_TRY_VALID(frame) && frame->finally_ip != NULL && !frame->in_finally) {
            frame->try_return_value = result;
            frame->has_try_return = 1;
            frame->ip = frame->finally_ip;
            DISPATCH();
        }

        int ret_frame = frame->ret_frame;
        int ret_reg = frame->ret_reg;
        int ret_want = frame->ret_want;
        int stack_base = frame->stack_base;
        Value* locals = frame->locals;
        int local_count = frame->local_count;
        int locals_is_dynamic = frame->locals_is_dynamic;

        // 关闭指向本帧局部变量的所有 upvalue
        if (locals != NULL && local_count > 0 && vm.open_upvalues != NULL) {
            close_upvalues(locals, local_count, 1);
        }

        vm.frame_cnt--;

        // 宿主/回调停止条件（vm_call_value 路径）
        if (vm.stop_frame_cnt > 0 && vm.frame_cnt <= vm.stop_frame_cnt) {
            vm.sp = stack_base;
            vm_stack_push(&vm, result);
            vm.last_return_value = result;
            vm.last_return_count = 1;
            vm.last_return_values[0] = result;
            vm.stop_frame_cnt = 0;
            if (locals != NULL && locals_is_dynamic) free(locals);
            return 0;
        }

        // 协程顶层函数返回
        if (vm.current_coroutine) {
            if (vm.frame_cnt < vm.current_coroutine->saved_frame_cnt) {
                vm.current_coroutine->result = result;
                gc_write_barrier((Object*)vm.current_coroutine, result);
                if (locals != NULL && locals_is_dynamic) free(locals);
                return 0;
            }
        }

        vm.last_return_value = result;
        vm.last_return_count = 1;
        vm.last_return_values[0] = result;

        // 结果写回调用方
        if (ret_frame >= 0 && ret_frame < vm.frame_cnt) {
            if (ret_want > 0) {
                Value* target = &vm.frames[ret_frame].locals[ret_reg];
                target[0] = result;
                for (int i = 1; i < ret_want; i++) target[i] = val_null();
            }
        } else if (ret_frame < 0 && vm.frame_cnt > 0) {
            // 宿主调用（栈式接口）：结果压回栈
            vm.sp = stack_base;
            vm_stack_push(&vm, result);
        }

        if (locals != NULL && locals_is_dynamic) free(locals);

        if (vm.frame_cnt == 0) {
            goto vm_exit;
        }

        frame = &vm.frames[vm.frame_cnt - 1];
        vm.sp = frame->stack_base;
        DISPATCH();
    }

    // OP_RETURN_MULTI A B: 返回 R[A .. A+B-1]，个数由 OP_RETURN_MULTI 的 B 寄存器给出
    //   编码：A = 结果起始寄存器，B = 存放个数的寄存器
    LABEL(OP_RETURN_MULTI): {
        int a = READ_A(), b = READ_B();
        int count = 0;
        if (val_is_int(R(b))) count = (int)val_as_int(R(b));
        if (count < 0) count = 0;
        if (count > VM_MAX_RETURNS) count = VM_MAX_RETURNS;

        if (unlikely(vm.gc_force_request)) {
            vm.gc_force_request = 0;
            gc_force_collect();
        } else if (unlikely(++vm.gc_return_counter >= 256)) {
            vm.gc_return_counter = 0;
            if (gc.deferred_gc && gc.enabled && !gc.running) {
                gc_try_collect_deferred();
            }
        }

        Value results[VM_MAX_RETURNS];
        for (int i = 0; i < count; i++) results[i] = R(a + i);
        if (count == 0) results[0] = val_null();

        if (FRAME_TRY_VALID(frame) && frame->finally_ip != NULL && !frame->in_finally) {
            frame->try_return_value = results[0];
            frame->has_try_return = 1;
            frame->ip = frame->finally_ip;
            DISPATCH();
        }

        int ret_frame = frame->ret_frame;
        int ret_reg = frame->ret_reg;
        int ret_want = frame->ret_want;
        int stack_base = frame->stack_base;
        Value* locals = frame->locals;
        int local_count = frame->local_count;
        int locals_is_dynamic = frame->locals_is_dynamic;

        if (locals != NULL && local_count > 0 && vm.open_upvalues != NULL) {
            close_upvalues(locals, local_count, 1);
        }

        vm.frame_cnt--;

        if (vm.stop_frame_cnt > 0 && vm.frame_cnt <= vm.stop_frame_cnt) {
            vm.sp = stack_base;
            for (int i = 0; i < count; i++) vm_stack_push(&vm, results[i]);
            vm.last_return_value = results[0];
            vm.last_return_count = count;
            for (int i = 0; i < count && i < VM_MAX_RETURNS; i++) vm.last_return_values[i] = results[i];
            vm.stop_frame_cnt = 0;
            if (locals != NULL && locals_is_dynamic) free(locals);
            return 0;
        }

        if (vm.current_coroutine) {
            if (vm.frame_cnt < vm.current_coroutine->saved_frame_cnt) {
                vm.current_coroutine->result = results[0];
                gc_write_barrier((Object*)vm.current_coroutine, results[0]);
                if (locals != NULL && locals_is_dynamic) free(locals);
                return 0;
            }
        }

        vm.last_return_value = results[0];
        vm.last_return_count = count;
        for (int i = 0; i < count && i < VM_MAX_RETURNS; i++) vm.last_return_values[i] = results[i];

        if (ret_frame >= 0 && ret_frame < vm.frame_cnt) {
            if (ret_want > 0) {
                Value* target = &vm.frames[ret_frame].locals[ret_reg];
                for (int i = 0; i < ret_want; i++) {
                    target[i] = (i < count) ? results[i] : val_null();
                }
            }
        } else if (ret_frame < 0 && vm.frame_cnt > 0) {
            vm.sp = stack_base;
            for (int i = 0; i < count; i++) vm_stack_push(&vm, results[i]);
        }

        if (locals != NULL && locals_is_dynamic) free(locals);

        if (vm.frame_cnt == 0) {
            goto vm_exit;
        }

        frame = &vm.frames[vm.frame_cnt - 1];
        vm.sp = frame->stack_base;
        DISPATCH();
    }

    // OP_TAIL_CALL: 暂按普通调用执行（codegen 目前生成 CALL + RETURN）；
    // 真正的帧复用尾调用优化见 docs 的后续阶段。
    LABEL(OP_TAIL_CALL): {
        int a = READ_A(), b = READ_B();
        int nargs = b - 1;
        Value callee = R(a);
        int line = GET_CURRENT_LINE();
        if (likely(val_is_obj(callee) && val_as_obj(callee)->type == OBJ_CLOSURE)) {
            if (call_reg((ObjClosure*)val_as_obj(callee), 0, val_null(),
                         a + 1, nargs, a, 1, line)) {
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            if (!vm.has_exception) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        if (!throw_runtime_error("尾调用目标必须是函数", line)) return -1;
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH();
    }

    // --- 数组 ---
    LABEL(OP_NEWARRAY): {
        int a = READ_A(), c = READ_C();
        ObjArray* arr = arr_new(c);
        if (!arr) { runtime_error("OP_NEWARRAY: arr_new 失败", GET_CURRENT_LINE()); return -1; }
        for (int i = 0; i < c; i++) {
            arr->elements[i] = R(a + 1 + i);
        }
        arr->count = c;
        R(a) = val_obj((Object*)arr);
        DISPATCH();
    }
    LABEL(OP_ARRAY_GET): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        ObjArray* arr = (ObjArray*)val_as_obj(R(b));
        int idx = (int)val_as_int(R(c));
        R(a) = arr->elements[idx];
        DISPATCH();
    }
    LABEL(OP_ARRAY_SET): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        ObjArray* arr = (ObjArray*)val_as_obj(R(b));
        int idx = (int)val_as_int(R(c));
        arr->elements[idx] = R(a);
        DISPATCH();
    }
    LABEL(OP_ARRAY_APPEND): {
        int a = READ_A(), b = READ_B();
        ObjArray* arr = (ObjArray*)val_as_obj(R(b));
        if (arr->count >= arr->capacity) { if (!arr_grow(arr)) { runtime_error("OP_ARRAY_APPEND: arr_grow 失败", GET_CURRENT_LINE()); return -1; } }
        arr->elements[arr->count++] = R(a);
        R(a) = val_int(arr->count);
        DISPATCH();
    }
    LABEL(OP_LEN): {
        int a = READ_A(), b = READ_B();
        Value v = R(b);
        if (val_is_obj(v)) {
            if (val_as_obj(v)->type == OBJ_ARRAY) R(a) = val_int(((ObjArray*)val_as_obj(v))->count);
            else if (val_as_obj(v)->type == OBJ_STRING) R(a) = val_int(((ObjString*)val_as_obj(v))->len);
            else if (val_as_obj(v)->type == OBJ_DICT) R(a) = val_int(((ObjDict*)val_as_obj(v))->count);
            else R(a) = val_int(0);
        } else {
            R(a) = val_int(0);
        }
        DISPATCH();
    }

    // --- 字典 ---
    // OP_NEWDICT A B C: 用 R[A+1..A+2C] 交替的 (键, 值) 建字典
    LABEL(OP_NEWDICT): {
        int a = READ_A(), c = READ_C();
        ObjDict* dict = dict_new(c > 0 ? c : 8);
        if (unlikely(!dict)) {
            runtime_error("OP_NEWDICT: 内存分配失败", GET_CURRENT_LINE());
            return -1;
        }
        for (int i = 0; i < c; i++) {
            dict_set(dict, R(a + 1 + i * 2), R(a + 2 + i * 2));
        }
        R(a) = val_obj((Object*)dict);
        DISPATCH();
    }
    LABEL(OP_DICT_GET): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        ObjDict* dict = (ObjDict*)val_as_obj(R(b));
        R(a) = dict_get(dict, R(c));
        DISPATCH();
    }
    LABEL(OP_DICT_SET): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        ObjDict* dict = (ObjDict*)val_as_obj(R(b));
        dict_set(dict, R(c), R(a));
        DISPATCH();
    }
    LABEL(OP_DICT_GET_KEY): {
        // TODO: 字典键迭代
        DISPATCH();
    }

    // --- 通用索引 ---
    // OP_INDEX A B C: R[A] = R[B][R[C]]（数组 / 字符串 / 字典）
    LABEL(OP_INDEX): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value obj = R(b);
        Value idx = R(c);
        int line = GET_CURRENT_LINE();

        if (likely(val_is_obj(obj))) {
            ObjType ot = val_as_obj(obj)->type;
            if (ot == OBJ_ARRAY) {
                ObjArray* arr = (ObjArray*)val_as_obj(obj);
                if (unlikely(!val_is_num(idx))) {
                    if (!throw_runtime_error("数组索引必须是数字", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                int i2 = (int)value_to_double(idx);
                if (unlikely((uint64_t)i2 >= (uint64_t)arr->count)) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "数组索引越界: 索引 %d, 长度 %d", i2, arr->count);
                    if (!throw_runtime_error(msg, line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                R(a) = arr->elements[i2];
                DISPATCH();
            }
            if (ot == OBJ_STRING) {
                ObjString* s = (ObjString*)val_as_obj(obj);
                if (unlikely(!val_is_num(idx))) {
                    if (!throw_runtime_error("字符串索引必须是数字", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                int i2 = (int)value_to_double(idx);
                if (unlikely(i2 < 0 || i2 >= s->char_len)) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "字符串索引越界: 索引 %d, 长度 %d", i2, s->char_len);
                    if (!throw_runtime_error(msg, line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                int off = utf8_char_offset(s->chars, s->len, i2);
                int cb = utf8_char_byte_len(s->chars, s->len, off);
                R(a) = val_obj((Object*)str_new(&s->chars[off], cb));
                DISPATCH();
            }
            if (ot == OBJ_DICT) {
                R(a) = dict_get((ObjDict*)val_as_obj(obj), idx);
                DISPATCH();
            }
            // .leno 模块：成员在 exports 字典里
            if (ot == OBJ_MODULE) {
                R(a) = dict_get(((ObjModule*)val_as_obj(obj))->exports, idx);
                DISPATCH();
            }
            // struct 实例：按**字段名**索引。
            //   语义分析把 `s.field` 生成成 obj["field"]（通用索引），因此字段读
            //   最终走到这里；缺了这个分支就报「下标访问: 对象不支持索引」。
            if (ot == OBJ_STRUCT) {
                if (unlikely(!val_is_string(idx))) {
                    if (!throw_runtime_error("struct 字段名必须是字符串", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                ObjStruct* so = (ObjStruct*)val_as_obj(obj);
                int fi = struct_get_field_index(so->def, ((ObjString*)val_as_obj(idx))->chars);
                if (unlikely(fi < 0)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "struct 不存在字段 '%s'",
                             ((ObjString*)val_as_obj(idx))->chars);
                    if (!throw_runtime_error(msg, line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                R(a) = struct_get_field(so, fi);
                DISPATCH();
            }
        }

        if (!throw_runtime_error("下标访问: 对象不支持索引", line)) return -1;
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH();
    }
    // OP_INDEX_SET A B C: R[B][R[C]] = R[A]
    LABEL(OP_INDEX_SET): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value obj = R(b);
        Value idx = R(c);
        int line = GET_CURRENT_LINE();

        if (likely(val_is_obj(obj))) {
            ObjType ot = val_as_obj(obj)->type;
            if (ot == OBJ_ARRAY) {
                ObjArray* arr = (ObjArray*)val_as_obj(obj);
                int i2 = val_is_num(idx) ? (int)value_to_double(idx) : -1;
                if (unlikely(i2 < 0 || i2 >= arr->count)) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "数组索引越界: 索引 %d, 长度 %d", i2, arr->count);
                    if (!throw_runtime_error(msg, line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                arr->elements[i2] = R(a);
                gc_write_barrier((Object*)arr, R(a));
                DISPATCH();
            }
            if (ot == OBJ_DICT) {
                dict_set((ObjDict*)val_as_obj(obj), idx, R(a));
                DISPATCH();
            }
            // struct 实例：按字段名赋值（`s.field = v` 的通用索引形态）
            if (ot == OBJ_STRUCT) {
                if (unlikely(!val_is_string(idx))) {
                    if (!throw_runtime_error("struct 字段名必须是字符串", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                ObjStruct* so = (ObjStruct*)val_as_obj(obj);
                int fi = struct_get_field_index(so->def, ((ObjString*)val_as_obj(idx))->chars);
                if (unlikely(fi < 0)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "struct 不存在字段 '%s'",
                             ((ObjString*)val_as_obj(idx))->chars);
                    if (!throw_runtime_error(msg, line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                Value nv = R(a);
                TypeKind et = so->def->fields[fi].type;
                if (et == TYPE_FLOAT && val_is_int(nv)) {
                    nv = val_float((double)val_as_int(nv));
                } else if (et == TYPE_FLOAT && val_is_bigint(nv)) {
                    nv = val_float(bigint_to_double(val_as_bigint(nv)));
                }
                struct_set_field(so, fi, nv);
                gc_write_barrier((Object*)so, nv);
                DISPATCH();
            }
        }

        if (!throw_runtime_error("下标赋值: 对象不支持索引", line)) return -1;
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH();
    }
    // OP_SLICE A B C: R[A] = R[B][R[A+1] : R[A+2]]（结束索引**包含**；null 表示省略）
    LABEL(OP_SLICE): {
        int a = READ_A(), b = READ_B();
        Value obj_val = R(b);
        Value start_val = R(a + 1);
        Value end_val = R(a + 2);
        int line = GET_CURRENT_LINE();

        if (likely(val_is_obj(obj_val) && val_as_obj(obj_val)->type == OBJ_ARRAY)) {
            ObjArray* arr = (ObjArray*)val_as_obj(obj_val);
            int arr_len = arr->count;
            int start_idx = 0, end_idx = arr_len - 1;

            if (!val_is_null(start_val)) {
                if (unlikely(!val_is_num(start_val))) {
                    if (!throw_runtime_error("切片起始索引必须是数字", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                start_idx = (int)value_to_double(start_val);
                if (start_idx < 0) start_idx = 0;
                if (start_idx > arr_len) start_idx = arr_len;
            }
            if (!val_is_null(end_val)) {
                if (unlikely(!val_is_num(end_val))) {
                    if (!throw_runtime_error("切片结束索引必须是数字", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                end_idx = (int)value_to_double(end_val);
                if (end_idx < 0) end_idx = 0;
                if (end_idx >= arr_len) end_idx = arr_len - 1;
            }

            int slice_len = end_idx - start_idx + 1;
            if (slice_len < 0) slice_len = 0;
            ObjArray* result = arr_new(slice_len > 0 ? slice_len : 1);
            if (unlikely(!result)) {
                runtime_error("切片数组内存分配失败", line);
                return -1;
            }
            for (int i = 0; i < slice_len; i++) {
                arr_write(result, i, arr->elements[start_idx + i]);
            }
            R(a) = val_obj((Object*)result);
            DISPATCH();
        }

        if (val_is_obj(obj_val) && val_as_obj(obj_val)->type == OBJ_STRING) {
            ObjString* s = (ObjString*)val_as_obj(obj_val);
            int arr_len = s->char_len;
            int start_idx = 0, end_idx = arr_len - 1;

            if (!val_is_null(start_val)) {
                if (unlikely(!val_is_num(start_val))) {
                    if (!throw_runtime_error("切片起始索引必须是数字", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                start_idx = (int)value_to_double(start_val);
                if (start_idx < 0) start_idx = 0;
                if (start_idx > arr_len) start_idx = arr_len;
            }
            if (!val_is_null(end_val)) {
                if (unlikely(!val_is_num(end_val))) {
                    if (!throw_runtime_error("切片结束索引必须是数字", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                end_idx = (int)value_to_double(end_val);
                if (end_idx < 0) end_idx = 0;
                if (end_idx >= arr_len) end_idx = arr_len - 1;
            }

            int nchars = end_idx - start_idx + 1;
            if (nchars < 0) nchars = 0;
            if (nchars == 0) {
                R(a) = val_obj((Object*)str_new("", 0));
                DISPATCH();
            }
            int byte_start = utf8_char_offset(s->chars, s->len, start_idx);
            int byte_end = (start_idx + nchars >= s->char_len)
                               ? s->len
                               : utf8_char_offset(s->chars, s->len, start_idx + nchars);
            R(a) = val_obj((Object*)str_new(&s->chars[byte_start], byte_end - byte_start));
            DISPATCH();
        }

        if (!throw_runtime_error("切片: 对象必须是数组或字符串", line)) return -1;
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH();
    }

    // --- 迭代 ---
    // OP_ITER_GET A B C: R[A] = iterate(R[B], R[C])
    //   数组 → 元素；字符串 → 第 N 个字符（按码点）；字典/struct → 第 N 个键/字段名；
    //   数字 → 序号本身（for n to i 的遍历语义）
    LABEL(OP_ITER_GET): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value obj_val = R(b);
        Value idx_val = R(c);
        int line = GET_CURRENT_LINE();

        if (unlikely(!val_is_num(idx_val))) {
            if (!throw_runtime_error("ITER_GET 索引必须是数字", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        int index = (int)value_to_double(idx_val);

        if (val_is_num(obj_val)) {
            R(a) = val_int(index);
            DISPATCH();
        }
        if (unlikely(!val_is_obj(obj_val))) {
            if (!throw_runtime_error("ITER_GET 需要对象或数字类型", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }

        switch (val_as_obj(obj_val)->type) {
            case OBJ_ARRAY: {
                ObjArray* arr = (ObjArray*)val_as_obj(obj_val);
                if (likely((uint64_t)index < (uint64_t)arr->count)) {
                    R(a) = arr->elements[index];
                    DISPATCH();
                }
                if (!throw_runtime_error("数组迭代索引越界", line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            case OBJ_STRING: {
                ObjString* str = (ObjString*)val_as_obj(obj_val);
                if (unlikely(index < 0 || index >= str->char_len)) {
                    if (!throw_runtime_error("字符串迭代索引越界", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                int byte_offset = utf8_char_offset(str->chars, str->len, index);
                int char_bytes = utf8_char_byte_len(str->chars, str->len, byte_offset);
                ObjString* result = str_new(&str->chars[byte_offset], char_bytes);
                R(a) = val_obj((Object*)result);
                DISPATCH();
            }
            case OBJ_DICT: {
                ObjDict* dict = (ObjDict*)val_as_obj(obj_val);
                if (unlikely(index < 0 || index >= dict->order_count)) {
                    if (!throw_runtime_error("字典键迭代索引越界", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                R(a) = dict_get_key_by_index(dict, index);
                DISPATCH();
            }
            case OBJ_ENUM_DEF: {
                ObjEnumDef* edef = (ObjEnumDef*)val_as_obj(obj_val);
                if (unlikely(index < 0 || index >= edef->member_count)) {
                    if (!throw_runtime_error("枚举迭代索引越界", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                R(a) = val_int_safe(edef->members[index].value);
                DISPATCH();
            }
            case OBJ_STRUCT: {
                ObjStruct* so = (ObjStruct*)val_as_obj(obj_val);
                if (unlikely(index < 0 || index >= so->def->field_count)) {
                    if (!throw_runtime_error("结构体字段迭代索引越界", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                R(a) = val_obj((Object*)str_copy(so->def->fields[index].name,
                                                 (int)strlen(so->def->fields[index].name)));
                DISPATCH();
            }
            default:
                if (!throw_runtime_error("ITER_GET 不支持的类型", line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
        }
    }
    // OP_ITER_GET_VALUE A B C: R[A] = 第 R[C] 个键对应的值（字典 / struct 字段值）
    LABEL(OP_ITER_GET_VALUE): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value obj_val = R(b);
        Value idx_val = R(c);
        int line = GET_CURRENT_LINE();

        if (unlikely(!val_is_num(idx_val))) {
            if (!throw_runtime_error("ITER_GET_VALUE 索引必须是数字", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        int index = (int)value_to_double(idx_val);

        if (unlikely(!val_is_obj(obj_val))) {
            if (!throw_runtime_error("ITER_GET_VALUE 需要对象类型", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }

        if (val_as_obj(obj_val)->type == OBJ_DICT) {
            ObjDict* dict = (ObjDict*)val_as_obj(obj_val);
            if (unlikely(index < 0 || index >= dict->order_count)) {
                if (!throw_runtime_error("字典值迭代索引越界", line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            R(a) = dict_get_value_by_index(dict, index);
            DISPATCH();
        }
        if (val_as_obj(obj_val)->type == OBJ_STRUCT) {
            ObjStruct* so = (ObjStruct*)val_as_obj(obj_val);
            if (unlikely(index < 0 || index >= so->def->field_count)) {
                if (!throw_runtime_error("结构体字段值迭代索引越界", line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            R(a) = struct_get_field(so, index);
            DISPATCH();
        }
        if (!throw_runtime_error("ITER_GET_VALUE 只能用于字典或结构体", line)) return -1;
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH();
    }

    // --- 范围 ---
    LABEL(OP_RANGE): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        // TODO: Range 对象
        R(a) = val_null();
        DISPATCH();
    }

    // --- in ---
    LABEL(OP_IN): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        // TODO: in 操作
        R(a) = val_bool(0);
        DISPATCH();
    }

    // --- 异常处理 ---
    // OP_TRY Bx: Bx = catch 块相对本指令起始的字节偏移
    LABEL(OP_TRY): {
        int bx = READ_Bx();
        uint8_t* try_start = frame->ip - 4;
        frame->prev_catch_ip = frame->catch_ip;
        frame->prev_finally_ip = frame->finally_ip;
        frame->catch_ip = try_start + bx;
        frame->finally_ip = NULL;
        DISPATCH();
    }
    // OP_CATCH A: R[A] = 当前异常；清除 catch 状态（寄存器式不再走栈传异常）
    LABEL(OP_CATCH): {
        int a = READ_A();
        R(a) = vm.exception;
        vm.exception = val_null();
        vm.has_exception = 0;
        frame->catch_ip = NULL;
        DISPATCH();
    }
    // OP_FINALLY Bx: Bx = finally 块相对本指令起始的字节偏移
    LABEL(OP_FINALLY): {
        int bx = READ_Bx();
        uint8_t* fin_start = frame->ip - 4;
        if (!vm.pending_exception) {
            vm.exception = val_null();
        }
        frame->finally_ip = fin_start + bx;
        frame->in_finally = 1;
        DISPATCH();
    }
    LABEL(OP_END_TRY): {
        frame->catch_ip = frame->prev_catch_ip;
        frame->finally_ip = frame->prev_finally_ip;
        frame->prev_catch_ip = NULL;
        frame->prev_finally_ip = NULL;
        frame->in_finally = 0;

        // 需要在 finally 之后继续传播的异常
        if (vm.pending_exception) {
            vm.pending_exception = 0;
            if (frame->catch_ip) {
                frame->ip = frame->catch_ip;
                frame->catch_ip = NULL;
                vm.has_exception = 0;
                DISPATCH();
            } else if (frame->finally_ip) {
                vm.pending_exception = 1;
                frame->ip = frame->finally_ip;
                vm.has_exception = 0;
                DISPATCH();
            } else {
                // 当前帧已无处理器：继续向上抛
                if (!throw_runtime_error("未捕获的异常", vm.exception_line ? vm.exception_line : GET_CURRENT_LINE())) {
                    return -1;
                }
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
        }

        // 离开 try/catch/finally 范围：异常状态不再需要保留。
        // 若不清理，has_exception 会一直挂着，后续 native 调用（OP_CALL/OP_CALL_NATIVE
        // 里 "if (vm.has_exception) 转成 throw_runtime_error"）会把**早已处理过的旧异常**
        // 重新抛出（表现为 `} catch { }` 之后 add() 报前面那次越界的错）。
        vm.has_exception = 0;
        vm.exception = val_null();
        vm.exception_line = 0;

        // try 块中的 return 在此收尾（finally 已执行完）
        if (frame->has_try_return) {
            frame->has_try_return = 0;
            Value saved = frame->try_return_value;
            REG_FINISH_RETURN(saved);
            DISPATCH();
        }
        DISPATCH();
    }
    // OP_THROW A: 抛出 R[A]（null 表示重新抛出当前异常）
    LABEL(OP_THROW): {
        int a = READ_A();
        int line = GET_CURRENT_LINE();
        Value exception = R(a);
        if (val_is_null(exception)) exception = vm.exception;
        if (val_is_string(exception)) exception = wrap_string_exception(exception, line);
        if (vm.pending_exception) vm.pending_exception = 0;
        vm.exception = exception;
        vm.has_exception = 1;
        vm.exception_line = line;

        // 在 finally 中 throw：清掉本帧 finally 防止死循环
        if (FRAME_TRY_VALID(frame) && frame->in_finally && frame->finally_ip) {
            frame->finally_ip = NULL;
        }

        int handled = 0;
        for (int i = vm.frame_cnt - 1; i >= 0; i--) {
            CallFrame* tf = &vm.frames[i];
            if (tf->closure && tf->closure->function && !tf->closure->function->has_try) continue;
            uint8_t* tcatch = tf->catch_ip ? tf->catch_ip : tf->prev_catch_ip;
            uint8_t* tfin = tf->finally_ip ? tf->finally_ip : tf->prev_finally_ip;
            if (!tcatch && !tfin) continue;

            for (int j = vm.frame_cnt - 1; j > i; j--) {
                CallFrame* uw = &vm.frames[j];
                if (uw->locals && uw->locals_is_dynamic) {
                    free(uw->locals);
                    uw->locals = NULL;
                }
            }
            vm.frame_cnt = i + 1;
            vm.sp = tf->stack_base;

            if (tf->finally_ip && !tf->in_finally && !tf->catch_ip) {
                vm.pending_exception = 1;
                tf->ip = tf->finally_ip;
                vm.has_exception = 0;
            } else if (tcatch) {
                tf->ip = tcatch;
                vm.has_exception = 0;
                if (tf->catch_ip) tf->catch_ip = NULL;
                else tf->prev_catch_ip = NULL;
            } else {
                vm.pending_exception = 1;
                tf->ip = tfin;
                vm.has_exception = 0;
            }
            handled = 1;
            break;
        }

        if (!handled) {
            print_unhandled_exception(exception, line);
            char* msg = NULL;
            if (val_is_obj(exception) && val_as_obj(exception)->type == OBJ_DICT) {
                ObjString* msg_key = str_copy("msg", 3);
                Value mv = dict_get((ObjDict*)val_as_obj(exception), val_obj((Object*)msg_key));
                if (val_is_string(mv)) msg = strdup(((ObjString*)val_as_obj(mv))->chars);
            }
            if (!msg) msg = value_to_string(exception);
            runtime_error(msg, line);
            free(msg);
            return -1;
        }
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH();
    }

    // --- 模块 ---
    // OP_LOAD_NATIVE_MODULE A Bx: 运行时加载原生模块，R[A] = null（占位）
    LABEL(OP_LOAD_NATIVE_MODULE): {
        int a = READ_A();
        int bx = READ_Bx();
        int line = GET_CURRENT_LINE();
        if (unlikely(bx < 0 || bx >= frame->chunk->const_cnt)) {
            if (!throw_runtime_error("模块名常量索引越界", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        Value mv = frame->chunk->constants[bx];
        if (unlikely(!val_is_obj(mv) || val_as_obj(mv)->type != OBJ_STRING)) {
            if (!throw_runtime_error("模块名必须是字符串", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        const char* module_name = ((ObjString*)val_as_obj(mv))->chars;
        if (native_init_module(module_name) != 0) {
            char nmsg[256];
            snprintf(nmsg, sizeof(nmsg), "未知的原生模块: %s", module_name);
            if (!throw_runtime_error(nmsg, line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        R(a) = val_null();
        DISPATCH();
    }
    // OP_MODULE_CALL A B C: R[A] = 模块方法(R[A+1 .. A+C])
    //   B = 常量表中 "模块名\0方法名" 组合串的索引（0/>255 时紧随 EXTRAARG）
    LABEL(OP_MODULE_CALL): {
        int a = READ_A(), b = READ_B();
        int nargs = READ_C();
        int line = GET_CURRENT_LINE();
        int cidx = READ_CONST_IDX(b);
        if (unlikely(cidx < 0 || cidx >= frame->chunk->const_cnt)) {
            if (!throw_runtime_error("模块方法名常量索引越界", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        Value nv = frame->chunk->constants[cidx];
        if (unlikely(!val_is_obj(nv) || val_as_obj(nv)->type != OBJ_STRING)) {
            if (!throw_runtime_error("模块方法名必须是字符串", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        ObjString* combo = (ObjString*)val_as_obj(nv);
        const char* module_name = combo->chars;
        int mlen = (int)strlen(module_name);
        if (unlikely(mlen + 1 > combo->len)) {
            if (!throw_runtime_error("模块方法名格式错误", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        const char* method_name = combo->chars + mlen + 1;

        ModuleMethodMeta* meta = native_find_module_method(module_name, method_name);
        if (unlikely(!meta)) {
            char msg[512];
            snprintf(msg, sizeof(msg), "未找到模块方法: %s.%s", module_name, method_name);
            if (!throw_runtime_error(msg, line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }

        int base = vm.sp;
        for (int i = 0; i < nargs; i++) vm_stack_push(&vm, R(a + 1 + i));
        Value result = meta->function(nargs, vm.stack + base);
        vm.sp = base;

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
    LABEL(OP_GET_MODULE_CONST): DISPATCH();
    // 模块变量 / 函数：以**当前帧所属模块**为上下文，索引到 module->globals[]
    // OP_GET_MODULE_VAR A Bx: R[A] = 模块全局[Bx]
    LABEL(OP_GET_MODULE_VAR): {
        int a = READ_A();
        int bx = READ_Bx();
        int line = GET_CURRENT_LINE();
        ObjModule* module = (ObjModule*)frame->module;
        if (unlikely(!module)) {
            if (!throw_runtime_error("模块变量访问: 不在模块上下文中", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        if (unlikely(bx < 0 || bx >= module->global_count)) {
            // 尚未写入过的槽位：视为 null（与全局变量未初始化一致）
            R(a) = val_null();
            DISPATCH();
        }
        R(a) = module->globals[bx];
        DISPATCH();
    }
    // OP_SET_MODULE_VAR A Bx: 模块全局[Bx] = R[A]（必要时扩容，见 DEFINE_MODULE_FUNC）
    LABEL(OP_SET_MODULE_VAR): {
        int a = READ_A();
        int bx = READ_Bx();
        int line = GET_CURRENT_LINE();
        ObjModule* module = (ObjModule*)frame->module;
        if (unlikely(!module)) {
            if (!throw_runtime_error("模块变量设置: 不在模块上下文中", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        if (bx >= module->global_capacity) {
            int new_capacity = module->global_capacity == 0 ? 8 : module->global_capacity * 2;
            while (new_capacity <= bx) new_capacity *= 2;
            Value* new_globals = (Value*)realloc(module->globals, sizeof(Value) * (size_t)new_capacity);
            if (unlikely(!new_globals)) {
                runtime_error("模块变量设置: 内存分配失败", line);
                return -1;
            }
            for (int i = module->global_capacity; i < new_capacity; i++) {
                new_globals[i] = val_null();
            }
            module->globals = new_globals;
            module->global_capacity = new_capacity;
        }
        module->globals[bx] = R(a);
        if (bx >= module->global_count) module->global_count = bx + 1;
        gc_write_barrier((Object*)module, R(a));
        DISPATCH();
    }
    // OP_GET_MODULE_FUNC A Bx: 同 GET_MODULE_VAR（模块函数与变量共用 globals 槽）
    LABEL(OP_GET_MODULE_FUNC): {
        int a = READ_A();
        int bx = READ_Bx();
        int line = GET_CURRENT_LINE();
        ObjModule* module = (ObjModule*)frame->module;
        if (unlikely(!module)) {
            if (!throw_runtime_error("模块函数访问: 不在模块上下文中", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        if (unlikely(bx < 0 || bx >= module->global_count)) {
            if (!throw_runtime_error("模块函数索引越界", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        R(a) = module->globals[bx];
        DISPATCH();
    }
    // OP_DEFINE_MODULE_FUNC A Bx: 模块全局[Bx] = R[A]（必要时扩容）
    LABEL(OP_DEFINE_MODULE_FUNC): {
        int a = READ_A();
        int bx = READ_Bx();
        int line = GET_CURRENT_LINE();
        ObjModule* module = (ObjModule*)frame->module;
        if (unlikely(!module)) {
            if (!throw_runtime_error("模块函数定义: 不在模块上下文中", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        if (bx >= module->global_capacity) {
            int new_capacity = module->global_capacity == 0 ? 8 : module->global_capacity * 2;
            while (new_capacity <= bx) new_capacity *= 2;
            Value* new_globals = (Value*)realloc(module->globals, sizeof(Value) * (size_t)new_capacity);
            if (unlikely(!new_globals)) {
                runtime_error("模块函数定义: 内存分配失败", line);
                return -1;
            }
            for (int i = module->global_capacity; i < new_capacity; i++) {
                new_globals[i] = val_null();
            }
            module->globals = new_globals;
            module->global_capacity = new_capacity;
        }
        module->globals[bx] = R(a);
        if (bx >= module->global_count) module->global_count = bx + 1;
        gc_write_barrier((Object*)module, R(a));
        DISPATCH();
    }
    // OP_INIT_LENOMODULE A: 若 R[A] 是未初始化的 .leno 模块，执行其 init_chunk
    //   执行完毕 R[A] 仍是模块对象（供别名变量持有）
    LABEL(OP_INIT_LENOMODULE): {
        int a = READ_A();
        int line = GET_CURRENT_LINE();
        Value module_val = R(a);

        if (unlikely(!val_is_obj(module_val) || val_as_obj(module_val)->type != OBJ_MODULE)) {
            if (!throw_runtime_error("模块初始化: 目标不是模块对象", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }

        ObjModule* module = (ObjModule*)val_as_obj(module_val);

        if (!module->initialized) {
            module->initialized = 1;

            // 模块依赖的原生模块
            if (module->native_imports && module->native_import_count > 0) {
                for (int i = 0; i < module->native_import_count; i++) {
                    if (native_init_module(module->native_imports[i]) != 0) {
                        char nmsg[256];
                        snprintf(nmsg, sizeof(nmsg), "未知的原生模块: %s", module->native_imports[i]);
                        if (!throw_runtime_error(nmsg, line)) return -1;
                        frame = &vm.frames[vm.frame_cnt - 1];
                        DISPATCH();
                    }
                }
            }

            if (module->init_chunk) {
                int saved_sp = vm.sp;
                ModuleFrame* mod_frame = module_frame_new(module);
                module_frame_enter(mod_frame);
                int result = vm_run_chunk(module->init_chunk);
                module_frame_exit();
                vm.sp = saved_sp;
                // 递归执行后 vm.frames 可能已 realloc，frame 必须重新取
                frame = &vm.frames[vm.frame_cnt - 1];
                if (result < 0) return -1;
            }

            // 导出映射：把模块内全局变量/类型补进 exports
            if (module->export_mappings && module->export_mapping_count > 0) {
                for (int i = 0; i < module->export_mapping_count; i++) {
                    ExportGlobalMapping* mapping = &module->export_mappings[i];
                    ObjString* key = str_copy(mapping->name, (int)strlen(mapping->name));
                    Value current_val = dict_get(module->exports, val_obj((Object*)key));
                    if (val_is_null(current_val)) {
                        if (mapping->global_index >= 0 && mapping->global_index < module->global_count) {
                            Value actual_val = module->globals[mapping->global_index];
                            if (!val_is_null(actual_val)) {
                                dict_set(module->exports, val_obj((Object*)key), actual_val);
                            }
                        } else if (mapping->global_index == -2) {
                            ObjStructDef* def = struct_def_find(mapping->name);
                            if (def) {
                                dict_set(module->exports, val_obj((Object*)key), val_obj((Object*)def));
                            }
                        }
                    }
                }
            }

            // use 再导出的类型
            if (module->use_reexport_names && module->use_reexport_count > 0) {
                for (int i = 0; i < module->use_reexport_count; i++) {
                    const char* name = module->use_reexport_names[i];
                    TypeKind kind = (TypeKind)module->use_reexport_kinds[i];
                    ObjString* key = str_copy(name, (int)strlen(name));
                    Value current_val = dict_get(module->exports, val_obj((Object*)key));
                    if (val_is_null(current_val)) {
                        if (kind == TYPE_ENUM) {
                            ObjEnumDef* def = enum_def_find(name);
                            if (def) dict_set(module->exports, val_obj((Object*)key), val_obj((Object*)def));
                        } else if (kind == TYPE_STRUCT) {
                            ObjStructDef* def = struct_def_find(name);
                            if (def) dict_set(module->exports, val_obj((Object*)key), val_obj((Object*)def));
                        }
                    }
                }
            }
        }

        R(a) = module_val;
        DISPATCH();
    }

    // --- struct ---
    // OP_STRUCT_DEF A Bx: Bx = 结构体名常量索引；其余元数据紧随指令（布局与栈式一致）：
    //   field_count(1) method_count(1)
    //   impl_count(1) + impl_name_idx(2)×n
    //   type_param_count(1) + param_name_idx(2)×n
    //   每个字段: name_idx(2) type(1) nullable(1)
    //             [type==STRUCT: has_name(1) [name_idx(2)]]
    //             [type==PTR_GENERIC: elem_type(1)]
    //             has_default(1) [default_idx(2)]
    //   每个方法: method_name_idx(2) func_idx(2)
    //   ctor_dtor_flags(1) [ctor_index(1)] [dtor_index(1)]
    //   const_count(1) + (cname_idx(2) cval_idx(2))×n
    LABEL(OP_STRUCT_DEF): {
        int line = GET_CURRENT_LINE();
        int name_idx = READ_Bx();
        if (unlikely(name_idx < 0 || name_idx >= frame->chunk->const_cnt)) {
            if (!throw_runtime_error("结构体名常量索引越界", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        Value name_val = frame->chunk->constants[name_idx];
        if (unlikely(!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING)) {
            if (!throw_runtime_error("结构体名称必须是字符串", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        ObjString* sname = (ObjString*)val_as_obj(name_val);

        uint8_t* p = frame->ip;
        uint8_t field_count = *p++;
        uint8_t method_count = *p++;

        ObjStructDef* def = struct_def_new(sname->chars, field_count, method_count);
        if (unlikely(!def)) {
            runtime_error("结构体定义内存分配失败", line);
            return -1;
        }
        if (!def->owner) def->owner = module_frame_current_module();

        uint8_t impl_count = *p++;
        if (impl_count > 0) {
            def->impl_names = (char**)calloc(impl_count, sizeof(char*));
            def->impl_count = impl_count;
            for (int i = 0; i < impl_count; i++) {
                int idx = ((int)p[0] << 8) | (int)p[1];
                p += 2;
                Value v = frame->chunk->constants[idx];
                if (val_is_obj(v) && val_as_obj(v)->type == OBJ_STRING) {
                    def->impl_names[i] = strdup(((ObjString*)val_as_obj(v))->chars);
                }
            }
        }

        uint8_t type_param_count = *p++;
        def->type_param_count = type_param_count;
        if (type_param_count > 0) {
            def->type_param_names = (char**)malloc(sizeof(char*) * type_param_count);
            def->type_param_constraints = (char**)calloc(type_param_count, sizeof(char*));
            for (int i = 0; i < type_param_count; i++) {
                int idx = ((int)p[0] << 8) | (int)p[1];
                p += 2;
                Value v = frame->chunk->constants[idx];
                if (val_is_obj(v) && val_as_obj(v)->type == OBJ_STRING) {
                    def->type_param_names[i] = strdup(((ObjString*)val_as_obj(v))->chars);
                } else {
                    def->type_param_names[i] = strdup("T");
                }
            }
        }

        for (int i = 0; i < field_count; i++) {
            int field_name_idx = ((int)p[0] << 8) | (int)p[1];
            p += 2;
            uint8_t field_type = *p++;
            uint8_t is_nullable = *p++;

            char* struct_type_name = NULL;
            if (field_type == TYPE_STRUCT) {
                uint8_t has_struct_type_name = *p++;
                if (has_struct_type_name) {
                    int tn_idx = ((int)p[0] << 8) | (int)p[1];
                    p += 2;
                    Value tv = frame->chunk->constants[tn_idx];
                    if (val_is_obj(tv) && val_as_obj(tv)->type == OBJ_STRING) {
                        struct_type_name = ((ObjString*)val_as_obj(tv))->chars;
                    }
                }
            }

            TypeKind element_type = TYPE_PTR;
            if (field_type == TYPE_PTR_GENERIC) {
                element_type = (TypeKind)(*p++);
            }

            uint8_t has_default = *p++;
            Value field_name_val = frame->chunk->constants[field_name_idx];
            if (unlikely(!val_is_obj(field_name_val) || val_as_obj(field_name_val)->type != OBJ_STRING)) {
                if (!throw_runtime_error("结构体字段名必须是字符串", line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            ObjString* field_name = (ObjString*)val_as_obj(field_name_val);

            Value default_value = val_null();
            if (has_default) {
                int d_idx = ((int)p[0] << 8) | (int)p[1];
                p += 2;
                default_value = frame->chunk->constants[d_idx];
            }

            struct_def_set_field(def, i, field_name->chars, (TypeKind)field_type,
                                 struct_type_name, default_value, has_default, element_type, is_nullable);
        }

        for (int i = 0; i < method_count; i++) {
            int mn_idx = ((int)p[0] << 8) | (int)p[1];
            p += 2;
            Value mn_val = frame->chunk->constants[mn_idx];
            if (unlikely(!val_is_obj(mn_val) || val_as_obj(mn_val)->type != OBJ_STRING)) {
                if (!throw_runtime_error("结构体方法名必须是字符串", line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            def->methods[i].name = strdup(((ObjString*)val_as_obj(mn_val))->chars);

            int fn_idx = ((int)p[0] << 8) | (int)p[1];
            p += 2;
            Value fn_val = frame->chunk->constants[fn_idx];
            if (unlikely(!val_is_obj(fn_val) || val_as_obj(fn_val)->type != OBJ_FUNCTION)) {
                if (!throw_runtime_error("结构体方法必须是函数对象", line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            def->methods[i].func = (ObjFunction*)val_as_obj(fn_val);
            ObjFunction* mfunc = def->methods[i].func;
            if (mfunc->upvalue_count == 0) {
                ObjClosure* cl = (ObjClosure*)gc_alloc(sizeof(ObjClosure), OBJ_CLOSURE);
                if (unlikely(!cl)) {
                    runtime_error("结构体方法闭包内存分配失败", line);
                    return -1;
                }
                cl->function = mfunc;
                cl->upvalue_count = 0;
                cl->type_param_count = 0;
                cl->type_param_args = NULL;
                def->methods[i].closure = cl;
            } else {
                // 需要 upvalue 的方法：运行时动态创建闭包
                def->methods[i].closure = NULL;
            }
        }

        uint8_t ctor_dtor_flags = *p++;
        def->has_ctor = (ctor_dtor_flags & 1) != 0;
        def->has_dtor = (ctor_dtor_flags & 2) != 0;
        if (def->has_ctor) def->ctor_index = *p++;
        if (def->has_dtor) def->dtor_index = *p++;

        uint8_t const_count = *p++;
        def->const_count = const_count;
        if (const_count > 0) {
            def->const_names = (char**)malloc(sizeof(char*) * const_count);
            def->const_values = (Value*)calloc(const_count, sizeof(Value));
            for (int i = 0; i < const_count; i++) {
                int cn_idx = ((int)p[0] << 8) | (int)p[1];
                p += 2;
                Value cn_val = frame->chunk->constants[cn_idx];
                if (val_is_obj(cn_val) && val_as_obj(cn_val)->type == OBJ_STRING) {
                    def->const_names[i] = strdup(((ObjString*)val_as_obj(cn_val))->chars);
                } else {
                    def->const_names[i] = strdup("");
                }
                int cv_idx = ((int)p[0] << 8) | (int)p[1];
                p += 2;
                def->const_values[i] = frame->chunk->constants[cv_idx];
            }
        }

        frame->ip = p;
        struct_def_register(def);
        DISPATCH();
    }
    // OP_FACE_DEF A Bx: 定义接口（与栈式同一数据布局）
    LABEL(OP_FACE_DEF): {
        int line = GET_CURRENT_LINE();
        int name_idx = READ_Bx();
        if (unlikely(name_idx < 0 || name_idx >= frame->chunk->const_cnt)) {
            if (!throw_runtime_error("接口名常量索引越界", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        Value name_val = frame->chunk->constants[name_idx];
        if (unlikely(!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING)) {
            if (!throw_runtime_error("接口名称必须是字符串", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        ObjString* fname = (ObjString*)val_as_obj(name_val);

        uint8_t* p = frame->ip;
        uint8_t method_count = *p++;
        uint8_t type_param_count = *p++;

        ObjFaceDef* def = face_def_new(fname->chars, method_count);
        if (unlikely(!def)) {
            runtime_error("接口定义内存分配失败", line);
            return -1;
        }
        if (!def->owner) def->owner = module_frame_current_module();

        if (type_param_count > 0) {
            char** tp_names = (char**)malloc(sizeof(char*) * type_param_count);
            for (int tp = 0; tp < type_param_count; tp++) {
                int idx = ((int)p[0] << 8) | (int)p[1];
                p += 2;
                Value v = frame->chunk->constants[idx];
                if (val_is_obj(v) && val_as_obj(v)->type == OBJ_STRING) {
                    tp_names[tp] = strdup(((ObjString*)val_as_obj(v))->chars);
                } else {
                    tp_names[tp] = strdup("T");
                }
            }
            face_def_set_type_params(def, type_param_count, tp_names, NULL);
            for (int tp = 0; tp < type_param_count; tp++) free(tp_names[tp]);
            free(tp_names);
        }

        for (int i = 0; i < method_count; i++) {
            int mn_idx = ((int)p[0] << 8) | (int)p[1];
            p += 2;
            uint8_t param_count = *p++;
            uint8_t return_type_kind = *p++;
            Value mn_val = frame->chunk->constants[mn_idx];
            if (unlikely(!val_is_obj(mn_val) || val_as_obj(mn_val)->type != OBJ_STRING)) {
                if (!throw_runtime_error("接口方法名必须是字符串", line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            def->methods[i].name = strdup(((ObjString*)val_as_obj(mn_val))->chars);
            def->methods[i].param_count = param_count;
            def->methods[i].return_type = (return_type_kind != TYPE_INFER)
                                              ? type_new((TypeKind)return_type_kind) : NULL;
            def->methods[i].param_types = NULL;
            if (param_count > 0) {
                def->methods[i].param_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * param_count);
                for (int j = 0; j < param_count; j++) {
                    uint8_t ptk = *p++;
                    def->methods[i].param_types[j] = (ptk != TYPE_INFER) ? type_new((TypeKind)ptk) : NULL;
                }
            }
        }

        frame->ip = p;
        face_def_register(def);
        DISPATCH();
    }
    // OP_ENUM_DEF A Bx: R[A] = enum 定义对象；数据区 member_count(1) + (名idx(2) 值idx(2))×n
    LABEL(OP_ENUM_DEF): {
        int a = READ_A();
        int line = GET_CURRENT_LINE();
        int name_idx = READ_Bx();
        if (unlikely(name_idx < 0 || name_idx >= frame->chunk->const_cnt)) {
            if (!throw_runtime_error("枚举名常量索引越界", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        Value name_val = frame->chunk->constants[name_idx];
        if (unlikely(!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING)) {
            if (!throw_runtime_error("enum 名称必须是字符串", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        ObjString* ename = (ObjString*)val_as_obj(name_val);

        uint8_t* p = frame->ip;
        uint8_t member_count = *p++;
        ObjEnumDef* def = enum_def_new(ename->chars, member_count);
        if (unlikely(!def)) {
            runtime_error("枚举定义内存分配失败", line);
            return -1;
        }
        if (!def->owner) def->owner = module_frame_current_module();

        for (int i = 0; i < member_count; i++) {
            int mn_idx = ((int)p[0] << 8) | (int)p[1];
            p += 2;
            int mv_idx = ((int)p[0] << 8) | (int)p[1];
            p += 2;
            Value mn_val = frame->chunk->constants[mn_idx];
            if (unlikely(!val_is_obj(mn_val) || val_as_obj(mn_val)->type != OBJ_STRING)) {
                if (!throw_runtime_error("enum 成员名必须是字符串", line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            Value mv_val = frame->chunk->constants[mv_idx];
            int64_t member_value;
            if (val_is_obj(mv_val) && val_as_obj(mv_val)->type == OBJ_BIGINT) {
                member_value = bigint_to_int64((ObjBigInt*)val_as_obj(mv_val));
            } else {
                member_value = (int64_t)val_as_num(mv_val);
            }
            enum_def_set_member(def, i, ((ObjString*)val_as_obj(mn_val))->chars, member_value);
        }

        frame->ip = p;
        enum_def_register(def);
        R(a) = val_obj((Object*)def);
        DISPATCH();
    }
    // OP_STRUCT_INIT A B C: R[A] = new Struct(...)（B = 实参个数）
    //   数据区：name_idx(2) generic_count(1) [generic_name_idx(2)]×n
    //           [field_name_idx(2)]×arg_count
    //   构造实参在 R[A+1 .. A+B]
    LABEL(OP_STRUCT_INIT): {
        int a = READ_A();
        int arg_count = READ_B();
        int line = GET_CURRENT_LINE();

        uint8_t* p = frame->ip;
        int name_idx = ((int)p[0] << 8) | (int)p[1];
        p += 2;
        int generic_count = *p++;

        char** generic_args = NULL;
        if (generic_count > 0) {
            generic_args = (char**)malloc(sizeof(char*) * generic_count);
            for (int gi = 0; gi < generic_count; gi++) {
                int idx = ((int)p[0] << 8) | (int)p[1];
                p += 2;
                Value v = frame->chunk->constants[idx];
                if (val_is_obj(v) && val_as_obj(v)->type == OBJ_STRING) {
                    generic_args[gi] = strdup(((ObjString*)val_as_obj(v))->chars);
                } else {
                    generic_args[gi] = strdup("unknown");
                }
            }
        }

        Value name_val = frame->chunk->constants[name_idx];
        if (unlikely(!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING)) {
            if (!throw_runtime_error("结构体名称必须是字符串", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        ObjString* sname = (ObjString*)val_as_obj(name_val);

        // 三级查找：模块内精查 → 带前缀 → 裸名（与栈式一致）
        const char* bare = sname->chars;
        const char* dotp = strchr(bare, '.');
        if (dotp) bare = dotp + 1;
        ObjModule* owner = module_frame_current_module();
        ObjStructDef* def = NULL;
        if (owner) def = struct_def_find_in_module(owner, bare);
        if (!def) def = struct_def_find_qualified(sname->chars);
        if (unlikely(!def)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "未定义的结构体 '%s'", sname->chars);
            if (generic_args) {
                for (int gi = 0; gi < generic_count; gi++) free(generic_args[gi]);
                free(generic_args);
            }
            if (!throw_runtime_error(msg, line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }

        ObjStruct* obj = struct_instance_new(def);
        if (unlikely(!obj)) {
            runtime_error("结构体实例内存分配失败", line);
            return -1;
        }
        if (generic_count > 0) {
            obj->generic_type_args = generic_args;
            obj->generic_type_arg_count = generic_count;
        }

        // 按字段名逐个赋值（编译期给名字，运行时查索引；fn_idx == 0 表示按位置）
        for (int i = 0; i < arg_count; i++) {
            int fn_idx = ((int)p[0] << 8) | (int)p[1];
            p += 2;
            int fidx;
            if (fn_idx == 0) {
                fidx = i;
            } else {
                Value fn_val = frame->chunk->constants[fn_idx];
                if (unlikely(!val_is_obj(fn_val) || val_as_obj(fn_val)->type != OBJ_STRING)) {
                    if (!throw_runtime_error("结构体字段名必须是字符串", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                fidx = struct_get_field_index(def, ((ObjString*)val_as_obj(fn_val))->chars);
            }
            if (unlikely(fidx < 0 || fidx >= def->field_count)) {
                if (!throw_runtime_error("结构体字段不存在", line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            Value v = R(a + 1 + i);
            TypeKind et = def->fields[fidx].type;
            if (et == TYPE_FLOAT && val_is_int(v)) {
                v = val_float((double)val_as_int(v));
            } else if (et == TYPE_FLOAT && val_is_bigint(v)) {
                v = val_float(bigint_to_double(val_as_bigint(v)));
            }
            struct_set_field(obj, fidx, v);
        }
        frame->ip = p;

        // 结果先落到寄存器（GC 根），构造器可能在其内部触发分配
        R(a) = val_obj((Object*)obj);

        // 调用构造函数（如果有）：self = 实例，实参紧随其后
        if (def->has_ctor && def->ctor_index >= 0 && def->ctor_index < def->method_count) {
            StructMethodInfo* mi = &def->methods[def->ctor_index];
            ObjClosure* cl = mi->closure;
            if (!cl && mi->func) {
                cl = (ObjClosure*)gc_alloc(sizeof(ObjClosure), OBJ_CLOSURE);
                if (cl) {
                    cl->function = mi->func;
                    cl->upvalue_count = mi->func->upvalue_count;
                    cl->type_param_count = 0;
                    cl->type_param_args = NULL;
                    for (int ui = 0; ui < mi->func->upvalue_count; ui++) cl->upvalues[ui] = NULL;
                }
            }
            if (cl) {
                if (call_reg(cl, 1, R(a), a + 1, arg_count, a, 1, line)) {
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                if (!vm.has_exception) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
        }
        DISPATCH();
    }
    // OP_GET_FIELD A B C: R[A] = R[B].field(C)（C = 字段索引）
    LABEL(OP_GET_FIELD): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value obj = R(b);
        int line = GET_CURRENT_LINE();
        if (unlikely(!val_is_obj(obj) || val_as_obj(obj)->type != OBJ_STRUCT)) {
            if (!throw_runtime_error("字段访问需要结构体对象", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        ObjStruct* so = (ObjStruct*)val_as_obj(obj);
        if (unlikely(c < 0 || c >= so->def->field_count)) {
            if (!throw_runtime_error("字段索引越界", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        R(a) = struct_get_field(so, c);
        DISPATCH();
    }
    // OP_SET_FIELD A B C: R[B].field(C) = R[A]
    LABEL(OP_SET_FIELD): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value obj = R(b);
        int line = GET_CURRENT_LINE();
        if (unlikely(!val_is_obj(obj) || val_as_obj(obj)->type != OBJ_STRUCT)) {
            if (!throw_runtime_error("字段赋值需要结构体对象", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        ObjStruct* so = (ObjStruct*)val_as_obj(obj);
        if (unlikely(c < 0 || c >= so->def->field_count)) {
            if (!throw_runtime_error("字段索引越界", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        struct_set_field(so, c, R(a));
        DISPATCH();
    }
    // OP_GET_METHOD A B C: R[A] = R[B].方法名(K[C]) —— 内置类型 → 绑定原生方法；
    //   struct → 绑定脚本方法闭包；dict → 退化为按键取属性。
    LABEL(OP_GET_METHOD): {
        int a = READ_A(), b = READ_B();
        int cidx = READ_CONST_IDX(READ_C());
        int line = GET_CURRENT_LINE();
        if (unlikely(cidx < 0 || cidx >= frame->chunk->const_cnt)) {
            if (!throw_runtime_error("方法名常量索引越界", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        Value name_val = frame->chunk->constants[cidx];
        if (unlikely(!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING)) {
            if (!throw_runtime_error("方法名必须是字符串", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        ObjString* name = (ObjString*)val_as_obj(name_val);
        Value receiver = R(b);
        ObjType rt = get_receiver_type(receiver);

        if (ic_type_supported(rt)) {
            ObjNative* method = find_method_by_type(rt, name->chars);
            if (method) {
                ObjBoundMethod* bound = bound_method_new(receiver, method);
                if (unlikely(!bound)) { runtime_error("内存分配失败", line); return -1; }
                R(a) = val_obj((Object*)bound);
                DISPATCH();
            }
        }

        if (rt == OBJ_STRUCT) {
            ObjStruct* so = (ObjStruct*)val_as_obj(receiver);
            ObjStructDef* def = so->def;
            ObjClosure* pre = NULL;
            ObjFunction* fn = NULL;
            if (def && struct_method_lookup(def, name, &pre, &fn)) {
                ObjClosure* cl = pre;
                if (!cl) {
                    cl = (ObjClosure*)gc_alloc(sizeof(ObjClosure), OBJ_CLOSURE);
                    if (unlikely(!cl)) { runtime_error("内存分配失败", line); return -1; }
                    cl->function = fn;
                    cl->upvalue_count = fn->upvalue_count;
                    cl->type_param_count = 0;
                    cl->type_param_args = NULL;
                    for (int ui = 0; ui < fn->upvalue_count; ui++) cl->upvalues[ui] = NULL;
                }
                ObjBoundMethod* bound = bound_closure_method_new(receiver, cl);
                if (unlikely(!bound)) { runtime_error("内存分配失败", line); return -1; }
                R(a) = val_obj((Object*)bound);
                DISPATCH();
            }
        }

        if (rt == OBJ_DICT) {
            R(a) = dict_get((ObjDict*)val_as_obj(receiver), name_val);
            DISPATCH();
        }

        // cstruct 定义对象：malloc / size / free 等方法来自 C 侧方法表
        if (rt == OBJ_CSTRUCT_DEF) {
            extern ObjNative* cstruct_find_method(const char* name);
            ObjNative* native_method = cstruct_find_method(name->chars);
            if (unlikely(!native_method)) {
                char msg[256];
                snprintf(msg, sizeof(msg), "cstruct 不存在方法 '%s'", name->chars);
                if (!throw_runtime_error(msg, line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            R(a) = val_obj((Object*)native_method);
            DISPATCH();
        }

        // .leno 模块：成员（变量 / 函数 / 类型）都在 exports 字典里
        if (rt == OBJ_MODULE) {
            Value member = dict_get(((ObjModule*)val_as_obj(receiver))->exports, name_val);
            if (unlikely(val_is_null(member))) {
                char msg[256];
                snprintf(msg, sizeof(msg), "模块中不存在成员 '%s'", name->chars);
                if (!throw_runtime_error(msg, line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            R(a) = member;
            DISPATCH();
        }

        {
            char msg[256];
            snprintf(msg, sizeof(msg), "类型 %s 上不存在方法 '%s'", obj_type_to_name(rt), name->chars);
            if (!throw_runtime_error(msg, line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
    }
    LABEL(OP_GET_FIELD_ADDR): DISPATCH();
    LABEL(OP_SET_PTR_ELEM_TYPE): DISPATCH();
    LABEL(OP_SET_DECLARED_FACE): DISPATCH();

    // --- cstruct ---
    LABEL(OP_CSTRUCT_DEF): DISPATCH();
    LABEL(OP_GET_CSTRUCT_DEF): DISPATCH();

    // --- 类型检查 / 转换 ---
    // OP_TYPE_CHECK A B C: R[A] = (R[A] is 类型B[元素C])
    //   需要名字的类型（struct/face/enum）在指令后紧跟 2 字节名字常量索引
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

    // --- 协程 ---
    LABEL(OP_AWAIT): DISPATCH();
    LABEL(OP_ASYNC_CALL): DISPATCH();

    // --- FFI ---
    LABEL(OP_CLIB_CALL): DISPATCH();
    LABEL(OP_CFUNC_CALLBACK): DISPATCH();
    LABEL(OP_U8_TO_F64): DISPATCH();

    // --- 泛型 / 析构 ---
    LABEL(OP_PUSH_TYPE_ARGS): DISPATCH();
    LABEL(OP_DTOR_LOCAL): DISPATCH();

    // --- switch ---
    LABEL(OP_SWITCH_LOOKUP): DISPATCH();

    // --- for 数值循环（8 字节定长网格；与栈式 op_for_loop.inc 语义一致）---
    // OP_FOR_PREP: op + A(start_slot) + B(end_slot) + C(step_slot)
    //              + [var_slot:u8][inclusive:u8][sBx:u16]           （共 8 字节）
    //   局部变量槽位号即寄存器号（每个 slot 独立，不要求连续）
    LABEL(OP_FOR_PREP): {
        int start_slot = READ_A();
        int end_slot = READ_B();
        int step_slot = READ_C();
        int var_slot = frame->ip[0];
        int inclusive = frame->ip[1];
        int sbx = (((int)frame->ip[2] << 8) | (int)frame->ip[3]) - 32768;
        frame->ip += 4;

        if (unlikely(start_slot >= frame->local_count || end_slot >= frame->local_count ||
                     step_slot >= frame->local_count || var_slot >= frame->local_count ||
                     start_slot < 0 || end_slot < 0 || step_slot < 0 || var_slot < 0)) {
            if (!throw_runtime_error("for 循环槽位越界", GET_CURRENT_LINE())) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }

        Value start_val = frame->locals[start_slot];
        Value end_val = frame->locals[end_slot];
        Value step_val = frame->locals[step_slot];
        frame->locals[var_slot] = start_val;

        double start_num = value_to_double(start_val);
        double end_num = value_to_double(end_val);
        double step_num = value_to_double(step_val);

        int should_run = 0;
        if (step_num > 0) {
            should_run = inclusive ? (start_num <= end_num) : (start_num < end_num);
        } else if (step_num < 0) {
            should_run = inclusive ? (start_num >= end_num) : (start_num > end_num);
        }
        // step == 0 时不执行

        if (!should_run) {
            frame->ip += sbx;
        }
        DISPATCH();
    }
    // OP_FOR_LOOP: op + A(var_slot) + B(step_slot) + C(end_slot)
    //              + [inclusive:u8][pad:u8][sBx:u16]                （共 8 字节）
    LABEL(OP_FOR_LOOP): {
        int var_slot = READ_A();
        int step_slot = READ_B();
        int end_slot = READ_C();
        int inclusive = frame->ip[0];
        int sbx = (((int)frame->ip[2] << 8) | (int)frame->ip[3]) - 32768;
        frame->ip += 4;

        if (unlikely(var_slot >= frame->local_count || step_slot >= frame->local_count ||
                     end_slot >= frame->local_count ||
                     var_slot < 0 || step_slot < 0 || end_slot < 0)) {
            if (!throw_runtime_error("for 循环槽位越界", GET_CURRENT_LINE())) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }

        /* 回边是天然 GC 安全点 */
        if (unlikely(vm.gc_force_request)) {
            vm.gc_force_request = 0;
            gc_force_collect();
        }

        Value loop_var = frame->locals[var_slot];
        Value step_val = frame->locals[step_slot];
        Value end_val = frame->locals[end_slot];

        if (likely(val_is_int(loop_var) && val_is_int(step_val) && val_is_int(end_val))) {
            int64_t loop_num = val_as_int(loop_var) + val_as_int(step_val);
            int64_t step_num = val_as_int(step_val);
            int64_t end_num = val_as_int(end_val);
            frame->locals[var_slot] = (loop_num >= INT48_MIN && loop_num <= INT48_MAX)
                                          ? val_int(loop_num)
                                          : val_obj((Object*)bigint_from_int64(loop_num));
            int cont = 0;
            if (step_num > 0) cont = inclusive ? (loop_num <= end_num) : (loop_num < end_num);
            else if (step_num < 0) cont = inclusive ? (loop_num >= end_num) : (loop_num > end_num);
            if (cont) frame->ip += sbx;
        } else if (unlikely(val_is_bigint(loop_var) || val_is_bigint(step_val) || val_is_bigint(end_val))) {
            ObjBigInt* lb = get_bigint_operand(loop_var);
            ObjBigInt* sb = get_bigint_operand(step_val);
            ObjBigInt* eb = get_bigint_operand(end_val);
            Value nv = bigint_add(lb, sb);
            frame->locals[var_slot] = nv;
            int cmp = bigint_compare(get_bigint_operand(nv), eb);
            int step_cmp = bigint_compare(sb, bigint_from_int64(0));
            int cont = 0;
            if (step_cmp > 0) cont = inclusive ? (cmp <= 0) : (cmp < 0);
            else if (step_cmp < 0) cont = inclusive ? (cmp >= 0) : (cmp > 0);
            if (cont) frame->ip += sbx;
        } else {
            double loop_num = value_to_double(loop_var) + value_to_double(step_val);
            double step_num = value_to_double(step_val);
            double end_num = value_to_double(end_val);
            frame->locals[var_slot] = val_num(loop_num);
            int cont = 0;
            if (step_num > 0) cont = inclusive ? (loop_num <= end_num) : (loop_num < end_num);
            else if (step_num < 0) cont = inclusive ? (loop_num >= end_num) : (loop_num > end_num);
            if (cont) frame->ip += sbx;
        }
        DISPATCH();
    }

    // --- 扩展指令 ---
    LABEL(OP_EXTRAARG): DISPATCH();
    LABEL(OP_EXTEND): DISPATCH();

    #if !VM_USE_JUMPTABLE
                default:
                    // 未知 opcode
                    goto vm_exit;
            } // switch
        } // for
    #endif

vm_exit:
    #undef R
    #undef READ_OPCODE
    #undef READ_A
    #undef READ_B
    #undef READ_C
    #undef READ_Bx
    #undef READ_sBx
    #undef READ_sJ
    #undef READ_Ax
    #undef GET_CURRENT_LINE
    #undef ADVANCE
    #undef DISPATCH
    #undef LABEL
    #undef REG_FINISH_RETURN
    #undef READ_CONST_IDX
    return 0;
