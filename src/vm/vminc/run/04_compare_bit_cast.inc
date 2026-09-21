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
        // null 参与的有序比较一律为 false（`notFound.id >= 0` 不能因 null 变成 true 或报错）
        if (unlikely(val_is_null(bv) || val_is_null(cv))) { R(a) = val_bool(0); DISPATCH(); }
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
        if (unlikely(val_is_null(bv) || val_is_null(cv))) { R(a) = val_bool(0); DISPATCH(); }
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
        if (unlikely(val_is_null(bv) || val_is_null(cv))) { R(a) = val_bool(0); DISPATCH(); }
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
        if (unlikely(val_is_null(bv) || val_is_null(cv))) { R(a) = val_bool(0); DISPATCH(); }
        if (likely(value_compare_stdlib(bv, cv, &cmp))) {
            R(a) = val_bool(cmp >= 0);
            DISPATCH();
        }
        if (!throw_runtime_error_with_operands("比较运算: 操作数类型不可比较", "GE (>=)", bv, cv, GET_CURRENT_LINE())) return -1;
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH();
    }

    // --- int 比较（操作数可能是 bigint ⇒ 同样要解引用）---
    // ★ null 参与的有序比较一律 false（与通用 LT/GT/LE/GE 一致）：
    //   int 专用路径若不判 null，`val_as_int(null) == 0` 会让 `notFound.id >= 0`
    //   变成 true ✗（test_struct_edge 明确断言必须 false）
    LABEL(OP_LT_INT): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        R(a) = val_bool(!val_is_null(bv) && !val_is_null(cv) && VM_IO(bv) < VM_IO(cv));
        DISPATCH();
    }
    LABEL(OP_GT_INT): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        R(a) = val_bool(!val_is_null(bv) && !val_is_null(cv) && VM_IO(bv) > VM_IO(cv));
        DISPATCH();
    }
    LABEL(OP_LE_INT): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        R(a) = val_bool(!val_is_null(bv) && !val_is_null(cv) && VM_IO(bv) <= VM_IO(cv));
        DISPATCH();
    }
    LABEL(OP_GE_INT): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        R(a) = val_bool(!val_is_null(bv) && !val_is_null(cv) && VM_IO(bv) >= VM_IO(cv));
        DISPATCH();
    }
    LABEL(OP_EQ_INT): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        R(a) = val_bool(VM_IO(R(b)) == VM_IO(R(c)));
        DISPATCH();
    }

    // --- 位运算 ---
    // 位运算：任一侧是 bigint（或移位/结果越界）⇒ 走 bigint 路径（无限精度补码语义）
    LABEL(OP_BITAND): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        if (VM_IS_BIG(bv) || VM_IS_BIG(cv)) {
            R(a) = bigint_compact_to_int(bigint_and(promote_to_bigint(bv), promote_to_bigint(cv)));
        } else {
            R(a) = VM_INT_RESULT(val_as_int(bv) & val_as_int(cv));
        }
        DISPATCH();
    }
    LABEL(OP_BITOR): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        if (VM_IS_BIG(bv) || VM_IS_BIG(cv)) {
            R(a) = bigint_compact_to_int(bigint_or(promote_to_bigint(bv), promote_to_bigint(cv)));
        } else {
            R(a) = VM_INT_RESULT(val_as_int(bv) | val_as_int(cv));
        }
        DISPATCH();
    }
    LABEL(OP_BITXOR): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        if (VM_IS_BIG(bv) || VM_IS_BIG(cv)) {
            R(a) = bigint_compact_to_int(bigint_xor(promote_to_bigint(bv), promote_to_bigint(cv)));
        } else {
            R(a) = VM_INT_RESULT(val_as_int(bv) ^ val_as_int(cv));
        }
        DISPATCH();
    }
    LABEL(OP_BITNOT): {
        int a = READ_A(), b = READ_B();
        Value bv = R(b);
        if (VM_IS_BIG(bv)) {
            R(a) = bigint_compact_to_int(bigint_not(val_as_bigint(bv)));
        } else {
            R(a) = VM_INT_RESULT(~val_as_int(bv));
        }
        DISPATCH();
    }
    LABEL(OP_SHL): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        int64_t sh = VM_IO(cv);
        // bigint 操作数、移位量 >= 32（int48 结果必然越界）、或结果超 int48 ⇒ bigint 路径
        if (VM_IS_BIG(bv) || VM_IS_BIG(cv) || sh < 0 || sh >= 32) {
            R(a) = bigint_compact_to_int(bigint_shl(promote_to_bigint(bv), (int)(sh < 0 ? 0 : sh)));
            DISPATCH();
        }
        R(a) = VM_INT_RESULT(val_as_int(bv) << sh);
        DISPATCH();
    }
    LABEL(OP_SHR): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        int64_t sh = VM_IO(cv);
        if (VM_IS_BIG(bv) || VM_IS_BIG(cv) || sh < 0 || sh > 47) {
            R(a) = bigint_compact_to_int(bigint_shr(promote_to_bigint(bv), (int)(sh < 0 ? 0 : sh)));
            DISPATCH();
        }
        R(a) = val_int(val_as_int(bv) >> sh);
        DISPATCH();
    }
    LABEL(OP_USHR): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value vb = R(b), vc = R(c);
        // LenoC 的 int 是 48 位有符号：逻辑右移先截断为 48 位无符号再右移。
        // 之前用 `(unsigned int)`（32 位）截断 ⇒ `-1 >>> 8` 得 16777215 而非 1099511627775 ✗
        if (VM_IS_BIG(vb) || VM_IS_BIG(vc)) {
            int64_t sh = VM_IO(vc);
            R(a) = bigint_compact_to_int(bigint_ushr(promote_to_bigint(vb), (int)(sh < 0 ? 0 : sh)));
        } else if (likely(val_is_int(vb) && val_is_int(vc))) {
            uint64_t uva = (uint64_t)val_as_int(vb) & PAYLOAD_MASK;
            int64_t sh = val_as_int(vc);
            R(a) = val_int_safe((int64_t)(uva >> sh));
        } else {
            R(a) = val_int(0);
        }
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
