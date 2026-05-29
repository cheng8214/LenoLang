// ============================================================================
// VM 执行核心逻辑
// 这个文件包含 vm_run 的主体逻辑，直接使用全局 vm 变量
// ============================================================================

    CallFrame* frame = &vm.frames[vm.frame_cnt - 1];
    vm.current_line = 0;

    #define READ_BYTE() (*frame->ip++)
    #define READ_SHORT() (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
    #define READ_INT32() (frame->ip += 4, (int32_t)((frame->ip[-4] << 24) | (frame->ip[-3] << 16) | (frame->ip[-2] << 8) | frame->ip[-1]))
    #define READ_CONSTANT() (frame->chunk->constants[READ_SHORT()])
    #if VM_USE_JUMPTABLE
        // 跳转表模式：实时计算行号
        #define GET_CURRENT_LINE() (frame->chunk->lines[(int)(frame->ip - frame->chunk->code)])
    #else
        // Switch 模式：使用缓存的行号
        #define GET_CURRENT_LINE() (vm.current_line)
    #endif

    // 调试模式：开始执行跟踪
    if (debugMode) {
        printf("===== 执行跟踪 =====\n");
    }

    // =========================================================================
    // 跳转表定义（仅 GCC/Clang）
    // =========================================================================
    #if VM_USE_JUMPTABLE

    // 定义标签宏：将 OP_XXX 转换为 LABEL_OP_XXX 标签名
    #define LABEL(op) LABEL_##op

    // 跳转表数组：索引是操作码值，元素是对应代码的标签地址
    // 注意：不能加 static，否则多线程时会共享同一个跳转表导致竞争
    const void* jump_table[] = {
        [OP_CONST] = &&LABEL(OP_CONST),
        [OP_NULL] = &&LABEL(OP_NULL),
        [OP_TRUE] = &&LABEL(OP_TRUE),
        [OP_FALSE] = &&LABEL(OP_FALSE),
        [OP_ZERO] = &&LABEL(OP_ZERO),
        [OP_ONE] = &&LABEL(OP_ONE),
        [OP_POP] = &&LABEL(OP_POP),
        [OP_DUP] = &&LABEL(OP_DUP),
        [OP_GET_LOCAL] = &&LABEL(OP_GET_LOCAL),
        [OP_SET_LOCAL] = &&LABEL(OP_SET_LOCAL),
        [OP_SET_LOCAL_POP] = &&LABEL(OP_SET_LOCAL_POP),
        [OP_GET_GLOBAL] = &&LABEL(OP_GET_GLOBAL),
        [OP_SET_GLOBAL] = &&LABEL(OP_SET_GLOBAL),
        [OP_GET_UPVALUE] = &&LABEL(OP_GET_UPVALUE),
        [OP_SET_UPVALUE] = &&LABEL(OP_SET_UPVALUE),
        [OP_CLOSE_UPVALUE] = &&LABEL(OP_CLOSE_UPVALUE),
        [OP_DEFINE_GLOBAL] = &&LABEL(OP_DEFINE_GLOBAL),
        [OP_GET_GLOBAL_FUNC] = &&LABEL(OP_GET_GLOBAL_FUNC),
        [OP_DEFINE_GLOBAL_FUNC] = &&LABEL(OP_DEFINE_GLOBAL_FUNC),
        [OP_GET_NATIVE] = &&LABEL(OP_GET_NATIVE),
        [OP_ADD] = &&LABEL(OP_ADD),
        [OP_SUB] = &&LABEL(OP_SUB),
        [OP_MUL] = &&LABEL(OP_MUL),
        [OP_DIV] = &&LABEL(OP_DIV),
        [OP_MOD] = &&LABEL(OP_MOD),
        [OP_BITAND] = &&LABEL(OP_BITAND),
        [OP_BITOR] = &&LABEL(OP_BITOR),
        [OP_BITXOR] = &&LABEL(OP_BITXOR),
        [OP_BITNOT] = &&LABEL(OP_BITNOT),
        [OP_SHL] = &&LABEL(OP_SHL),
        [OP_SHR] = &&LABEL(OP_SHR),
        [OP_NEG] = &&LABEL(OP_NEG),
        [OP_NOT] = &&LABEL(OP_NOT),
        [OP_CAST_FLOAT] = &&LABEL(OP_CAST_FLOAT),
        [OP_CAST_INT] = &&LABEL(OP_CAST_INT),
        [OP_CAST_STRING] = &&LABEL(OP_CAST_STRING),
        [OP_INC] = &&LABEL(OP_INC),
        [OP_DEC] = &&LABEL(OP_DEC),
        [OP_INC_LOCAL] = &&LABEL(OP_INC_LOCAL),
        [OP_DEC_LOCAL] = &&LABEL(OP_DEC_LOCAL),
        [OP_PRE_INC_LOCAL] = &&LABEL(OP_PRE_INC_LOCAL),
        [OP_PRE_DEC_LOCAL] = &&LABEL(OP_PRE_DEC_LOCAL),
        [OP_EQ] = &&LABEL(OP_EQ),
        [OP_NEQ] = &&LABEL(OP_NEQ),
        [OP_LT] = &&LABEL(OP_LT),
        [OP_GT] = &&LABEL(OP_GT),
        [OP_LE] = &&LABEL(OP_LE),
        [OP_GE] = &&LABEL(OP_GE),
        [OP_IN] = &&LABEL(OP_IN),
        [OP_RANGE] = &&LABEL(OP_RANGE),
        [OP_JUMP] = &&LABEL(OP_JUMP),
        [OP_JUMP_IF_FALSE] = &&LABEL(OP_JUMP_IF_FALSE),
        [OP_JUMP_IF_TRUE] = &&LABEL(OP_JUMP_IF_TRUE),
        [OP_LOOP] = &&LABEL(OP_LOOP),
        [OP_CALL] = &&LABEL(OP_CALL),
        [OP_TAIL_CALL] = &&LABEL(OP_TAIL_CALL),
        [OP_CLOSURE] = &&LABEL(OP_CLOSURE),
        [OP_RETURN] = &&LABEL(OP_RETURN),
        [OP_ARRAY] = &&LABEL(OP_ARRAY),
        [OP_ARRAY_GET] = &&LABEL(OP_ARRAY_GET),
        [OP_ARRAY_SET] = &&LABEL(OP_ARRAY_SET),
        [OP_ARRAY_APPEND] = &&LABEL(OP_ARRAY_APPEND),
        [OP_DICT] = &&LABEL(OP_DICT),
        [OP_DICT_GET] = &&LABEL(OP_DICT_GET),
        [OP_DICT_SET] = &&LABEL(OP_DICT_SET),
        [OP_DICT_GET_KEY] = &&LABEL(OP_DICT_GET_KEY),
        [OP_LOAD_NATIVE_MODULE] = &&LABEL(OP_LOAD_NATIVE_MODULE),
        [OP_MODULE_CALL] = &&LABEL(OP_MODULE_CALL),
        [OP_STRING_ADD] = &&LABEL(OP_STRING_ADD),
        [OP_INDEX] = &&LABEL(OP_INDEX),
        [OP_INDEX_SET] = &&LABEL(OP_INDEX_SET),
        [OP_SLICE] = &&LABEL(OP_SLICE),
        [OP_LENGTH] = &&LABEL(OP_LENGTH),
        [OP_ITER_GET] = &&LABEL(OP_ITER_GET),
        [OP_ITER_GET_VALUE] = &&LABEL(OP_ITER_GET_VALUE),
        [OP_TRY] = &&LABEL(OP_TRY),
        [OP_CATCH] = &&LABEL(OP_CATCH),
        [OP_FINALLY] = &&LABEL(OP_FINALLY),
        [OP_END_TRY] = &&LABEL(OP_END_TRY),
        [OP_THROW] = &&LABEL(OP_THROW),
        [OP_GET_MODULE_VAR] = &&LABEL(OP_GET_MODULE_VAR),
        [OP_SET_MODULE_VAR] = &&LABEL(OP_SET_MODULE_VAR),
        [OP_GET_MODULE_FUNC] = &&LABEL(OP_GET_MODULE_FUNC),
        [OP_DEFINE_MODULE_FUNC] = &&LABEL(OP_DEFINE_MODULE_FUNC),
        [OP_GET_PROPERTY] = &&LABEL(OP_GET_PROPERTY),
        [OP_TYPE_CHECK] = &&LABEL(OP_TYPE_CHECK),
        [OP_AS_CAST] = &&LABEL(OP_AS_CAST),
        [OP_FOR_PREP] = &&LABEL(OP_FOR_PREP),
        [OP_FOR_LOOP] = &&LABEL(OP_FOR_LOOP),
        // 类型特化指令（跳转表条目）
        [OP_ADD_INT] = &&LABEL(OP_ADD_INT),
        [OP_SUB_INT] = &&LABEL(OP_SUB_INT),
        [OP_MUL_INT] = &&LABEL(OP_MUL_INT),
        [OP_DIV_INT] = &&LABEL(OP_DIV_INT),
        [OP_MOD_INT] = &&LABEL(OP_MOD_INT),
        [OP_ADD_FLOAT] = &&LABEL(OP_ADD_FLOAT),
        [OP_SUB_FLOAT] = &&LABEL(OP_SUB_FLOAT),
        [OP_MUL_FLOAT] = &&LABEL(OP_MUL_FLOAT),
        [OP_DIV_FLOAT] = &&LABEL(OP_DIV_FLOAT),
        [OP_NEG_INT] = &&LABEL(OP_NEG_INT),
        [OP_NEG_FLOAT] = &&LABEL(OP_NEG_FLOAT),
        [OP_EQ_INT] = &&LABEL(OP_EQ_INT),
        [OP_LT_INT] = &&LABEL(OP_LT_INT),
        [OP_GT_INT] = &&LABEL(OP_GT_INT),
        [OP_LE_INT] = &&LABEL(OP_LE_INT),
        [OP_GE_INT] = &&LABEL(OP_GE_INT),
        [OP_EQ_FLOAT] = &&LABEL(OP_EQ_FLOAT),
        [OP_LT_FLOAT] = &&LABEL(OP_LT_FLOAT),
        [OP_GT_FLOAT] = &&LABEL(OP_GT_FLOAT),
        [OP_LE_FLOAT] = &&LABEL(OP_LE_FLOAT),
        [OP_GE_FLOAT] = &&LABEL(OP_GE_FLOAT),
        // 立即数操作码（跳转表条目）
        [OP_ADD_INT_IMM] = &&LABEL(OP_ADD_INT_IMM),
        [OP_SUB_INT_IMM] = &&LABEL(OP_SUB_INT_IMM),
        [OP_MUL_INT_IMM] = &&LABEL(OP_MUL_INT_IMM),
        [OP_LT_INT_IMM] = &&LABEL(OP_LT_INT_IMM),
        [OP_GT_INT_IMM] = &&LABEL(OP_GT_INT_IMM),
        [OP_LE_INT_IMM] = &&LABEL(OP_LE_INT_IMM),
        [OP_GE_INT_IMM] = &&LABEL(OP_GE_INT_IMM),
        [OP_EQ_INT_IMM] = &&LABEL(OP_EQ_INT_IMM),
        // struct 相关操作码
        [OP_STRUCT_DEF] = &&LABEL(OP_STRUCT_DEF),
        [OP_STRUCT_INIT] = &&LABEL(OP_STRUCT_INIT),
        [OP_GET_FIELD] = &&LABEL(OP_GET_FIELD),
        [OP_SET_FIELD] = &&LABEL(OP_SET_FIELD),
        [OP_GET_METHOD] = &&LABEL(OP_GET_METHOD),
        // enum 相关操作码
        [OP_ENUM_DEF] = &&LABEL(OP_ENUM_DEF),
        [OP_FACE_DEF] = &&LABEL(OP_FACE_DEF),
        // cstruct 相关操作码
        [OP_CSTRUCT_DEF] = &&LABEL(OP_CSTRUCT_DEF),
        [OP_GET_CSTRUCT_DEF] = &&LABEL(OP_GET_CSTRUCT_DEF),
        // 协程相关操作码
        [OP_AWAIT] = &&LABEL(OP_AWAIT),
        [OP_ASYNC_CALL] = &&LABEL(OP_ASYNC_CALL),
        [OP_INIT_LENOMODULE] = &&LABEL(OP_INIT_LENOMODULE),
    };

    // 宏定义：取指令并分发
    #define FETCH_DISPATCH() do { \
        if (frame->ip >= frame->chunk->code + frame->chunk->len) { \
            return 0; \
        } \
        if (unlikely(vm.has_exception)) { \
            if (!frame->catch_ip && !frame->finally_ip) { \
                return -1; \
            } \
        } \
        uint8_t op = READ_BYTE(); \
        goto *jump_table[op]; \
    } while(0)

    #define CHECK_EXCEPTION() do { \
        if (vm.has_exception && !frame->catch_ip && !frame->finally_ip) { \
            return -1; \
        } \
    } while(0)

    #endif  // VM_USE_JUMPTABLE

    // =========================================================================
    // 主执行循环
    // =========================================================================
    #if VM_USE_JUMPTABLE

    // 跳转表模式：初始分发
    FETCH_DISPATCH();

    #else

    // Switch 模式：传统循环
    while (frame->ip < frame->chunk->code + frame->chunk->len) {
        frame = &vm.frames[vm.frame_cnt - 1];

        if (vm.has_exception && !frame->catch_ip && !frame->finally_ip) {
            return -1;
        }

        int offset = (int)(frame->ip - frame->chunk->code);
        vm.current_line = frame->chunk->lines[offset];

        if (debugMode) {
            debugPrintStack(vm.stack, vm.sp);
            disassembleInstruction(frame->chunk, offset);
            printf("\n");
        }

        uint8_t instruction;
        switch (instruction = READ_BYTE()) {
    #endif

    // =========================================================================
    // 操作码实现（两种模式共享）
    // =========================================================================
    #if VM_USE_JUMPTABLE
    #define OPCODE(op) LABEL(op):
    #define DISPATCH FETCH_DISPATCH()
    #else
    #define OPCODE(op) case op:
    #define DISPATCH break
    #endif

    #include "vm_helpers.inc"

            // 常量加载类操作码
            #include "op_constants.inc"

            // 栈操作类操作码
            #include "op_stack.inc"

            // 变量操作类操作码
            #include "op_variables.inc"

            // 通用算术运算类操作码
            #include "op_arith.inc"

            // 位运算类操作码
            #include "op_bitwise.inc"

            // 一元运算和类型转换类操作码
            #include "op_unary.inc"

            // 类型特化操作码
            #include "op_type_specialized.inc"

            // 自增自减操作码
            #include "op_inc_dec.inc"

            // 通用比较运算类操作码
            #include "op_compare.inc"

            // 集合操作类操作码
            #include "op_collection.inc"

            // 跳转控制类操作码
            #include "op_jump.inc"

            // 函数调用类操作码
            #include "op_call.inc"

            // 数组操作类操作码
            #include "op_array.inc"

            // 字典操作类操作码
            #include "op_dict.inc"

            // 索引和切片操作类操作码
            #include "op_index_slice.inc"

            // 原生模块加载操作码
            #include "op_load_native_module.inc"

            // 模块调用类操作码
            #include "op_module_call.inc"

            // 属性获取类操作码
            #include "op_property.inc"

            // 类型检查类操作码
            #include "op_type_check.inc"
            #include "op_as_cast.inc"

            // struct 相关操作码
            #include "op_struct.inc"

            // enum 相关操作码
            #include "op_enum.inc"
            #include "op_cstruct.inc"

            // For 循环操作类操作码
            #include "op_for_loop.inc"

            // 字符串操作类操作码
            #include "op_string.inc"

            // 工具操作类操作码
            #include "op_utils.inc"

            // 迭代器操作类操作码
            #include "op_iter.inc"

            // 异常处理类操作码
            #include "op_exception.inc"

            // 模块变量/函数操作类操作码
            #include "op_module_var.inc"

            // 协程相关操作码
            #include "op_coroutine.inc"

            // .leno 模块初始化操作码
            #include "op_init_lenomodule.inc"
    #if VM_USE_JUMPTABLE
    #else
        default:
            runtime_error("未知操作码", GET_CURRENT_LINE());
            return -1;
        }
    }
    #endif

    #undef READ_BYTE
    #undef READ_SHORT
    #undef READ_INT32
    #undef READ_CONSTANT
    #undef GET_CURRENT_LINE

    return 0;
