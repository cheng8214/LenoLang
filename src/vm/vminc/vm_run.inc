// ============================================================================
// 寄存器式 VM 执行核心逻辑
// 定长 4 字节指令，寄存器文件 = CallFrame.locals
// ============================================================================

    CallFrame* frame = &vm.frames[vm.frame_cnt - 1];
    vm.current_line = 0;

    // 寄存器文件 = frame->locals
    #define R(idx) (frame->locals[(idx)])

    // 读取 4 字节指令的字段（大端）
    #define READ_OPCODE()      (frame->ip[0])
    #define READ_A()            (frame->ip[1])
    #define READ_B()            (frame->ip[2])
    #define READ_C()            (frame->ip[3])
    #define READ_Bx()           (((int)frame->ip[2] << 8) | (int)frame->ip[3])
    #define READ_sBx()          (READ_Bx() - 32768)
    #define READ_sJ()           REG_sJ(frame->ip[1], frame->ip[2], frame->ip[3])
    #define READ_Ax()           (((int)frame->ip[1] << 16) | ((int)frame->ip[2] << 8) | (int)frame->ip[3])
    #define GET_CURRENT_LINE()  (frame->chunk->lines[(int)(frame->ip - frame->chunk->code)])

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
        [OP_EXTRAARG] = &&LABEL(OP_EXTRAARG),
        [OP_EXTEND] = &&LABEL(OP_EXTEND),
    };

    #define DISPATCH() \
        do { \
            uint8_t op = READ_OPCODE(); \
            vm.current_line = GET_CURRENT_LINE(); \
            ADVANCE(); \
            goto *jump_table[op]; \
        } while (0)

    #else // switch 分发

    #define DISPATCH() \
        do { \
            vm.current_line = GET_CURRENT_LINE(); \
            ADVANCE(); \
            goto dispatch_loop; \
        } while (0)

    #endif

    // =========================================================================
    // 主循环
    // =========================================================================

    #if VM_USE_JUMPTABLE
        DISPATCH();
    #else
        for (;;) {
            dispatch_loop:
            {
                uint8_t op = READ_OPCODE();
                vm.current_line = GET_CURRENT_LINE();
                ADVANCE();
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
        R(a) = vm.globals[bx];
        DISPATCH();
    }
    LABEL(OP_SETGLOBAL): {
        int a = READ_A();
        int bx = READ_Bx();
        vm.globals[bx] = R(a);
        DISPATCH();
    }
    LABEL(OP_DEFGLOBAL): {
        int a = READ_A();
        int bx = READ_Bx();
        vm.globals[bx] = R(a);
        DISPATCH();
    }
    LABEL(OP_GETGLOBALFUNC): {
        int a = READ_A();
        int bx = READ_Bx();
        R(a) = val_obj((Object*)vm.global_funcs[bx]);
        DISPATCH();
    }
    LABEL(OP_DEFGLOBALFUNC): {
        int a = READ_A();
        int bx = READ_Bx();
        vm.global_funcs[bx] = R(a);  // global_funcs 是 Value 数组，直接存值
        DISPATCH();
    }

    // --- upvalue / 闭包 ---
    LABEL(OP_GETUPVAL): {
        int a = READ_A();
        int bx = READ_Bx();
        R(a) = *frame->closure->upvalues[bx]->location;
        DISPATCH();
    }
    LABEL(OP_SETUPVAL): {
        int a = READ_A();
        int bx = READ_Bx();
        *frame->closure->upvalues[bx]->location = R(a);
        DISPATCH();
    }
    LABEL(OP_CLOSE): {
        // 关闭 upvalue（简化）
        DISPATCH();
    }
    LABEL(OP_CLOSURE): {
        int a = READ_A();
        int bx = READ_Bx();
        ObjFunction* func = (ObjFunction*)val_as_obj(frame->chunk->constants[bx]);
        // 直接 gc_alloc 构造闭包（栈式基线 op_call.inc 同样做法）
        // TODO: 绑定 upvalue（参考栈式 OP_CLOSURE）
        ObjClosure* closure = (ObjClosure*)gc_alloc(sizeof(ObjClosure), OBJ_CLOSURE);
        if (!closure) { runtime_error("OP_CLOSURE: gc_alloc 失败", GET_CURRENT_LINE()); return -1; }
        closure->function = func;
        closure->upvalue_count = func->upvalue_count;
        for (int i = 0; i < func->upvalue_count; i++) closure->upvalues[i] = NULL;
        R(a) = val_obj((Object*)closure);
        DISPATCH();
    }

    // --- 算术运算（通用，简化版：int/float 两条路径） ---
    // TODO: BigInt / 字符串拼接 / null 报错（参考栈式 op_arith.inc / op_unary.inc 加回来）
    LABEL(OP_ADD): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        if (val_is_int(bv) && val_is_int(cv)) {
            R(a) = val_int(val_as_int(bv) + val_as_int(cv));
        } else if (val_is_float(bv) || val_is_float(cv)) {
            R(a) = val_float(val_as_num(bv) + val_as_num(cv));
        } else {
            runtime_error("OP_ADD: 暂未支持的类型组合", GET_CURRENT_LINE());
            return -1;
        }
        DISPATCH();
    }
    LABEL(OP_SUB): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        if (val_is_int(bv) && val_is_int(cv)) {
            R(a) = val_int(val_as_int(bv) - val_as_int(cv));
        } else if (val_is_float(bv) || val_is_float(cv)) {
            R(a) = val_float(val_as_num(bv) - val_as_num(cv));
        } else {
            runtime_error("OP_SUB: 暂未支持的类型组合", GET_CURRENT_LINE());
            return -1;
        }
        DISPATCH();
    }
    LABEL(OP_MUL): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        if (val_is_int(bv) && val_is_int(cv)) {
            R(a) = val_int(val_as_int(bv) * val_as_int(cv));
        } else if (val_is_float(bv) || val_is_float(cv)) {
            R(a) = val_float(val_as_num(bv) * val_as_num(cv));
        } else {
            runtime_error("OP_MUL: 暂未支持的类型组合", GET_CURRENT_LINE());
            return -1;
        }
        DISPATCH();
    }
    LABEL(OP_DIV): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        if (val_is_int(bv) && val_is_int(cv)) {
            int64_t dv = val_as_int(cv);
            if (dv == 0) { runtime_error("除零错误", GET_CURRENT_LINE()); return -1; }
            R(a) = val_int(val_as_int(bv) / dv);
        } else if (val_is_float(bv) || val_is_float(cv)) {
            R(a) = val_float(val_as_num(bv) / val_as_num(cv));
        } else {
            runtime_error("OP_DIV: 暂未支持的类型组合", GET_CURRENT_LINE());
            return -1;
        }
        DISPATCH();
    }
    LABEL(OP_MOD): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        if (val_is_int(bv) && val_is_int(cv)) {
            int64_t dv = val_as_int(cv);
            if (dv == 0) { runtime_error("取模除零错误", GET_CURRENT_LINE()); return -1; }
            R(a) = val_int(val_as_int(bv) % dv);
        } else {
            runtime_error("OP_MOD: 取模操作数必须是整数", GET_CURRENT_LINE());
            return -1;
        }
        DISPATCH();
    }
    LABEL(OP_NEG): {
        int a = READ_A(), b = READ_B();
        Value bv = R(b);
        if (val_is_int(bv)) {
            R(a) = val_int(-val_as_int(bv));
        } else if (val_is_float(bv)) {
            R(a) = val_float(-val_as_double(bv));
        } else {
            runtime_error("OP_NEG: 操作数必须是数字", GET_CURRENT_LINE());
            return -1;
        }
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

    // --- 比较运算（简化版：数值/同类型对象引用比较） ---
    // TODO: 字符串内容比较 / 数组逐元素 / BigInt / Ptr vs null（参考栈式 op_compare.inc）
    LABEL(OP_EQ): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        int eq = 0;
        if (val_is_int(bv) && val_is_int(cv)) {
            eq = (val_as_int(bv) == val_as_int(cv));
        } else if (val_is_float(bv) || val_is_float(cv)) {
            eq = (val_as_num(bv) == val_as_num(cv));
        } else if (val_is_null(bv) && val_is_null(cv)) {
            eq = 1;
        } else if (val_is_bool(bv) && val_is_bool(cv)) {
            eq = (bv == cv);
        } else if (val_is_obj(bv) && val_is_obj(cv)) {
            // 对象引用相等（字符串内容比较留 TODO）
            eq = (val_as_obj(bv) == val_as_obj(cv));
        }
        R(a) = val_bool(eq);
        DISPATCH();
    }
    LABEL(OP_LT): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        if (val_is_int(bv) && val_is_int(cv)) {
            R(a) = val_bool(val_as_int(bv) < val_as_int(cv));
        } else if (val_is_float(bv) || val_is_float(cv)) {
            R(a) = val_bool(val_as_num(bv) < val_as_num(cv));
        } else {
            runtime_error("OP_LT: 操作数必须是数字", GET_CURRENT_LINE());
            return -1;
        }
        DISPATCH();
    }
    LABEL(OP_GT): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        if (val_is_int(bv) && val_is_int(cv)) {
            R(a) = val_bool(val_as_int(bv) > val_as_int(cv));
        } else if (val_is_float(bv) || val_is_float(cv)) {
            R(a) = val_bool(val_as_num(bv) > val_as_num(cv));
        } else {
            runtime_error("OP_GT: 操作数必须是数字", GET_CURRENT_LINE());
            return -1;
        }
        DISPATCH();
    }
    LABEL(OP_LE): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        if (val_is_int(bv) && val_is_int(cv)) {
            R(a) = val_bool(val_as_int(bv) <= val_as_int(cv));
        } else if (val_is_float(bv) || val_is_float(cv)) {
            R(a) = val_bool(val_as_num(bv) <= val_as_num(cv));
        } else {
            runtime_error("OP_LE: 操作数必须是数字", GET_CURRENT_LINE());
            return -1;
        }
        DISPATCH();
    }
    LABEL(OP_GE): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        if (val_is_int(bv) && val_is_int(cv)) {
            R(a) = val_bool(val_as_int(bv) >= val_as_int(cv));
        } else if (val_is_float(bv) || val_is_float(cv)) {
            R(a) = val_bool(val_as_num(bv) >= val_as_num(cv));
        } else {
            runtime_error("OP_GE: 操作数必须是数字", GET_CURRENT_LINE());
            return -1;
        }
        DISPATCH();
    }
    LABEL(OP_NEQ): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        int eq = 0;
        if (val_is_int(bv) && val_is_int(cv)) {
            eq = (val_as_int(bv) == val_as_int(cv));
        } else if (val_is_float(bv) || val_is_float(cv)) {
            eq = (val_as_num(bv) == val_as_num(cv));
        } else if (val_is_null(bv) && val_is_null(cv)) {
            eq = 1;
        } else if (val_is_bool(bv) && val_is_bool(cv)) {
            eq = (bv == cv);
        } else if (val_is_obj(bv) && val_is_obj(cv)) {
            eq = (val_as_obj(bv) == val_as_obj(cv));
        }
        R(a) = val_bool(!eq);
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

    // --- 类型转换 ---
    LABEL(OP_CAST_FLOAT): {
        int a = READ_A(), b = READ_B();
        if (val_is_int(R(b))) R(a) = val_float((double)val_as_int(R(b)));
        else R(a) = R(b);
        DISPATCH();
    }
    LABEL(OP_CAST_INT): {
        int a = READ_A(), b = READ_B();
        if (val_is_float(R(b))) R(a) = val_int((int)val_as_double(R(b)));
        else R(a) = R(b);
        DISPATCH();
    }
    LABEL(OP_CAST_STRING): {
        int a = READ_A(), b = READ_B();
        // TODO: 值转字符串
        R(a) = R(b);
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

    // --- 自增自减 ---
    LABEL(OP_INC): {
        int a = READ_A(), b = READ_B();
        R(a) = val_int(val_as_int(R(b)) + 1);
        DISPATCH();
    }
    LABEL(OP_DEC): {
        int a = READ_A(), b = READ_B();
        R(a) = val_int(val_as_int(R(b)) - 1);
        DISPATCH();
    }

    // --- 调用 / 返回 ---
    LABEL(OP_CALL): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        int nargs = b - 1;
        int nresults = c - 1;
        // R[A] = callee, R[A+1..A+nargs] = args
        Value callee = R(a);
        if (val_is_obj(callee) && val_as_obj(callee)->type == OBJ_CLOSURE) {
            ObjClosure* closure = (ObjClosure*)val_as_obj(callee);
            // 调用 call()，参数在 R[A+1..]
            // call() 会设置新帧，参数拷入 callee->locals[0..arity-1]
            // TODO: 完整 call() 集成
            // 简化：直接调用
            call(closure, nargs, GET_CURRENT_LINE());
            // 结果在 frame->locals[0]（被调用方的 R0）
            // 回写：R[A] = 结果（如果 nresults > 0）
            if (nresults > 0) {
                // 被调用方返回值在新帧的 locals[0]
                // call() 返回后帧已弹出，结果在 vm.ret
                // TODO: 正确的结果回写
            }
        }
        DISPATCH();
    }
    LABEL(OP_CALL_NATIVE): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        int name_const = b;
        int nargs = c;
        // K[name_const] = 函数名
        // 实参在 R[A+1..A+nargs]
        // TODO: 完整 native 调用
        // 简化：结果 = null
        R(a) = val_null();
        DISPATCH();
    }
    LABEL(OP_RETURN): {
        int a = READ_A(), b = READ_B();
        int nresults = b - 1;
        // 返回值在 R[A..A+nresults-1]
        // 弹出当前帧，把返回值写入调用方的寄存器
        // TODO: 完整 return 逻辑（finally / upvalue 关闭）
        if (nresults > 0) {
            vm.last_return_value = R(a);
            vm.last_return_count = 1;
        } else {
            vm.last_return_count = 0;
        }
        // 弹帧
        vm.frame_cnt--;
        if (vm.frame_cnt == 0) {
            // 顶层返回
            goto vm_exit;
        }
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH();
    }
    LABEL(OP_RETURN_MULTI): {
        // TODO: 多值返回
        vm.frame_cnt--;
        if (vm.frame_cnt == 0) goto vm_exit;
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH();
    }
    LABEL(OP_TAIL_CALL): {
        // TODO: 尾调用
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
    LABEL(OP_NEWDICT): {
        int a = READ_A(), c = READ_C();
        ObjDict* dict = dict_new(c);
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
    LABEL(OP_INDEX): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value obj = R(b);
        Value idx = R(c);
        if (val_is_obj(obj)) {
            if (val_as_obj(obj)->type == OBJ_ARRAY) {
                R(a) = ((ObjArray*)val_as_obj(obj))->elements[(int)val_as_int(idx)];
            } else if (val_as_obj(obj)->type == OBJ_DICT) {
                R(a) = dict_get((ObjDict*)val_as_obj(obj), idx);
            } else {
                R(a) = val_null();
            }
        } else {
            R(a) = val_null();
        }
        DISPATCH();
    }
    LABEL(OP_INDEX_SET): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value obj = R(b);
        Value idx = R(c);
        if (val_is_obj(obj)) {
            if (val_as_obj(obj)->type == OBJ_ARRAY) {
                ((ObjArray*)val_as_obj(obj))->elements[(int)val_as_int(idx)] = R(a);
            } else if (val_as_obj(obj)->type == OBJ_DICT) {
                dict_set((ObjDict*)val_as_obj(obj), idx, R(a));
            }
        }
        DISPATCH();
    }
    LABEL(OP_SLICE): {
        // TODO: 切片
        DISPATCH();
    }

    // --- 迭代 ---
    LABEL(OP_ITER_GET): {
        // TODO: 迭代
        DISPATCH();
    }
    LABEL(OP_ITER_GET_VALUE): {
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
    LABEL(OP_TRY): {
        int bx = READ_Bx();
        frame->catch_ip = frame->chunk->code + (frame->ip - frame->chunk->code - 4 + bx);
        DISPATCH();
    }
    LABEL(OP_CATCH): {
        int a = READ_A();
        R(a) = vm.exception;
        vm.exception = val_null();
        frame->catch_ip = NULL;
        DISPATCH();
    }
    LABEL(OP_FINALLY): {
        int bx = READ_Bx();
        frame->finally_ip = frame->chunk->code + (frame->ip - frame->chunk->code - 4 + bx);
        DISPATCH();
    }
    LABEL(OP_END_TRY): {
        frame->catch_ip = NULL;
        frame->finally_ip = NULL;
        DISPATCH();
    }
    LABEL(OP_THROW): {
        int a = READ_A();
        vm.exception = R(a);
        // TODO: 异常展开
        goto vm_exit;
    }

    // --- 模块 ---
    LABEL(OP_LOAD_NATIVE_MODULE): DISPATCH();
    LABEL(OP_MODULE_CALL): DISPATCH();
    LABEL(OP_GET_MODULE_CONST): DISPATCH();
    LABEL(OP_GET_MODULE_VAR): DISPATCH();
    LABEL(OP_SET_MODULE_VAR): DISPATCH();
    LABEL(OP_GET_MODULE_FUNC): DISPATCH();
    LABEL(OP_DEFINE_MODULE_FUNC): DISPATCH();
    LABEL(OP_INIT_LENOMODULE): DISPATCH();

    // --- struct ---
    LABEL(OP_STRUCT_DEF): DISPATCH();
    LABEL(OP_STRUCT_INIT): DISPATCH();
    LABEL(OP_GET_FIELD): DISPATCH();
    LABEL(OP_SET_FIELD): DISPATCH();
    LABEL(OP_GET_FIELD_ADDR): DISPATCH();
    LABEL(OP_GET_METHOD): DISPATCH();
    LABEL(OP_SET_PTR_ELEM_TYPE): DISPATCH();
    LABEL(OP_SET_DECLARED_FACE): DISPATCH();

    // --- enum / face / cstruct ---
    LABEL(OP_ENUM_DEF): DISPATCH();
    LABEL(OP_FACE_DEF): DISPATCH();
    LABEL(OP_CSTRUCT_DEF): DISPATCH();
    LABEL(OP_GET_CSTRUCT_DEF): DISPATCH();

    // --- 类型检查 / 转换 ---
    LABEL(OP_TYPE_CHECK): DISPATCH();
    LABEL(OP_AS_CAST): DISPATCH();

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
