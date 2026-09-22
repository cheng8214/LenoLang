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
        [OP_ADD_INT_IMM] = &&LABEL(OP_ADD_INT_IMM),
        [OP_SUB_INT_IMM] = &&LABEL(OP_SUB_INT_IMM),
        [OP_LT_INT_IMM] = &&LABEL(OP_LT_INT_IMM),
        [OP_GT_INT_IMM] = &&LABEL(OP_GT_INT_IMM),
        [OP_LE_INT_IMM] = &&LABEL(OP_LE_INT_IMM),
        [OP_GE_INT_IMM] = &&LABEL(OP_GE_INT_IMM),
        [OP_CALL_GLOBAL_FUNC] = &&LABEL(OP_CALL_GLOBAL_FUNC),
        [OP_CMPJMP_LT] = &&LABEL(OP_CMPJMP_LT),
        [OP_CMPJMP_LE] = &&LABEL(OP_CMPJMP_LE),
        [OP_CMPJMP_GT] = &&LABEL(OP_CMPJMP_GT),
        [OP_CMPJMP_GE] = &&LABEL(OP_CMPJMP_GE),
        [OP_MUL_INT_IMM] = &&LABEL(OP_MUL_INT_IMM),
        [OP_LT_F] = &&LABEL(OP_LT_F),
        [OP_LE_F] = &&LABEL(OP_LE_F),
        [OP_GT_F] = &&LABEL(OP_GT_F),
        [OP_GE_F] = &&LABEL(OP_GE_F),
        [OP_CALL_GLOBAL_FUNC_TYPED] = &&LABEL(OP_CALL_GLOBAL_FUNC_TYPED),
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
        [OP_INDEX_ARRAY_INT] = &&LABEL(OP_INDEX_ARRAY_INT),
        [OP_INDEX_ARRAY_FLOAT] = &&LABEL(OP_INDEX_ARRAY_FLOAT),
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
        [OP_GET_FIELD_FAST] = &&LABEL(OP_GET_FIELD_FAST),
        [OP_ACC_FIELDS] = &&LABEL(OP_ACC_FIELDS),
        [OP_INVOKE_METHOD_TYPED] = &&LABEL(OP_INVOKE_METHOD_TYPED),
    };

    // ★ 行号**不再每条指令写回**：`vm.current_line` 在本仓库**没有 VM 内读者** ——
    //   原生函数取当前行走 native_get_current_line()（native.c:1504），它自己从
    //   frame->ip / chunk->lines **重算**，只在"拿不到帧"时才回退到该字段。
    //   原先每条指令都要 2 次依赖加载（chunk → lines）+ 1 次写；路线图 ⑤-c 的消融值是 7–13%。
    //   （字段仍保留：宿主/回调等"帧不可用"的场景由它兜底，初值 0 见本文件开头。）
    #define DISPATCH() \
        do { \
            uint8_t op = frame->ip[0]; \
            ADVANCE(); \
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
        if (vm.current_coroutine && vm.frame_cnt <= vm.current_coroutine->saved_frame_cnt) { \
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
                // 行号不再每条指令写回（理由见上面 DISPATCH 宏处的注释）
                switch (op) {
    #endif

    // --- NOP ---
