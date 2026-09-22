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
    // ★ 三条必须一致（此前都错）：
    //   ① bigint 值的**静态类型也是 int** ⇒ 任一侧是 bigint 时必须走 bigint 运算
    //      （val_as_int(bigint) 取到的是对象指针位模式 ⇒ 静默算错 ✗）；
    //   ② 结果超出 int48 / int64 溢出 ⇒ **提升为 bigint**（48 位截断会算错 ✗）；
    //   ③ 位运算/移位是**无限精度补码**语义（与栈式 op_bitwise.inc 一致）：
    //      `(1<<63) >> 63 == 1`、`0xffffffffffffffff` 是 2^64-1 而不是 -1。
    #define VM_IO(_v) (val_is_bigint(_v) ? vm_bigint_as_i64(_v) : val_as_int(_v))
    #define VM_INT_RESULT(_x) \
        (((_x) >= INT48_MIN && (_x) <= INT48_MAX) ? val_int((int64_t)(_x)) \
         : bigint_compact_to_int(val_obj((Object*)bigint_from_int64((int64_t)(_x)))))
    #define VM_IS_BIG(_v) (unlikely(val_is_bigint(_v)))
    LABEL(OP_ADD_INT): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        if (VM_IS_BIG(bv) || VM_IS_BIG(cv)) {
            R(a) = bigint_compact_to_int(bigint_add(promote_to_bigint(bv), promote_to_bigint(cv)));
            DISPATCH();
        }
        int64_t x = val_as_int(bv), y = val_as_int(cv), s = 0;
        if (likely(!__builtin_add_overflow(x, y, &s))) {
            R(a) = VM_INT_RESULT(s);
        } else {
            R(a) = bigint_add(bigint_from_int64(x), bigint_from_int64(y));
        }
        DISPATCH();
    }
    LABEL(OP_SUB_INT): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        if (VM_IS_BIG(bv) || VM_IS_BIG(cv)) {
            R(a) = bigint_compact_to_int(bigint_sub(promote_to_bigint(bv), promote_to_bigint(cv)));
            DISPATCH();
        }
        int64_t x = val_as_int(bv), y = val_as_int(cv), s = 0;
        if (likely(!__builtin_sub_overflow(x, y, &s))) {
            R(a) = VM_INT_RESULT(s);
        } else {
            R(a) = bigint_sub(bigint_from_int64(x), bigint_from_int64(y));
        }
        DISPATCH();
    }
    // 立即数版加减：右操作数是 -128..127 的小整数字面量（编进指令的 C 字段，8 位有符号）。
    // 语义与 *_INT 完全一致（含 int48 提升 bigint），只是省掉了「LOADI + *_INT」两条指令。
    // 动机：examples/性能测试/经典斐波那契数列测试_int.leno 里 `n - 2` / `n - 1` 各占 3 条指令，
    // 而栈式的 SUB_INT_IMM 只要 1 条 ⇒ 递归基准上指令数差一大截。
    LABEL(OP_ADD_INT_IMM): {
        int a = READ_A(), b = READ_B();
        int imm = (int)(int8_t)READ_C();
        Value bv = R(b);
        if (VM_IS_BIG(bv)) {
            R(a) = bigint_compact_to_int(
                bigint_add(promote_to_bigint(bv), bigint_from_int64((int64_t)imm)));
            DISPATCH();
        }
        int64_t x = val_as_int(bv), s = 0;
        if (likely(!__builtin_add_overflow(x, (int64_t)imm, &s))) {
            R(a) = VM_INT_RESULT(s);
        } else {
            R(a) = bigint_add(bigint_from_int64(x), bigint_from_int64((int64_t)imm));
        }
        DISPATCH();
    }
    LABEL(OP_SUB_INT_IMM): {
        int a = READ_A(), b = READ_B();
        int imm = (int)(int8_t)READ_C();
        Value bv = R(b);
        if (VM_IS_BIG(bv)) {
            R(a) = bigint_compact_to_int(
                bigint_sub(promote_to_bigint(bv), bigint_from_int64((int64_t)imm)));
            DISPATCH();
        }
        int64_t x = val_as_int(bv), s = 0;
        if (likely(!__builtin_sub_overflow(x, (int64_t)imm, &s))) {
            R(a) = VM_INT_RESULT(s);
        } else {
            R(a) = bigint_sub(bigint_from_int64(x), bigint_from_int64((int64_t)imm));
        }
        DISPATCH();
    }
    LABEL(OP_MUL_INT): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        if (VM_IS_BIG(bv) || VM_IS_BIG(cv)) {
            R(a) = bigint_compact_to_int(bigint_mul(promote_to_bigint(bv), promote_to_bigint(cv)));
            DISPATCH();
        }
        int64_t x = val_as_int(bv), y = val_as_int(cv), s = 0;
        if (likely(!__builtin_mul_overflow(x, y, &s))) {
            R(a) = VM_INT_RESULT(s);
        } else {
            R(a) = bigint_mul(bigint_from_int64(x), bigint_from_int64(y));
        }
        DISPATCH();
    }
    // 乘法立即数版（T10-③）：与 OP_MUL_INT 完全同语义（含 bigint 与溢出提升），
    // 只是右值是 int8 立即数 —— 省掉把字面量装进寄存器的那条 LOADI。
    LABEL(OP_MUL_INT_IMM): {
        int a = READ_A(), b = READ_B();
        int imm = (int)(int8_t)READ_C();
        Value bv = R(b);
        if (VM_IS_BIG(bv)) {
            R(a) = bigint_compact_to_int(
                bigint_mul(promote_to_bigint(bv), bigint_from_int64((int64_t)imm)));
            DISPATCH();
        }
        int64_t x = val_as_int(bv), s = 0;
        if (likely(!__builtin_mul_overflow(x, (int64_t)imm, &s))) {
            R(a) = VM_INT_RESULT(s);
        } else {
            R(a) = bigint_mul(bigint_from_int64(x), bigint_from_int64((int64_t)imm));
        }
        DISPATCH();
    }
    LABEL(OP_NEG_INT): {
        int a = READ_A(), b = READ_B();
        Value bv = R(b);
        if (VM_IS_BIG(bv)) {
            R(a) = bigint_compact_to_int(bigint_neg(val_as_bigint(bv)));
            DISPATCH();
        }
        R(a) = VM_INT_RESULT(-val_as_int(bv));
        DISPATCH();
    }

    // --- float 特化 ---
    LABEL(OP_ADD_F): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        // 快路径：两侧确实 float（codegen 静态判 float 触发）
        if (likely(val_is_float(bv) && val_is_float(cv))) {
            R(a) = val_float(val_as_double(bv) + val_as_double(cv));
            DISPATCH();
        }
        // 兜底：静态判 float、运行期却是 int（如 float 字段被赋 int、dict.get 默认 int）
        // ⇒ 用 val_as_num_ex 提升后相加，避免把 int 位当 double 读成次正规数/NaN。
        // 快路径之外仅此一段，数值型全覆盖（string/bigint 静态不判 float，到不了这）。
        R(a) = val_float(val_as_num_ex(bv) + val_as_num_ex(cv));
        DISPATCH();
    }
    LABEL(OP_SUB_F): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        if (likely(val_is_float(bv) && val_is_float(cv))) {
            R(a) = val_float(val_as_double(bv) - val_as_double(cv));
            DISPATCH();
        }
        // 兜底：静态 float、运行期 int（float 字段/ dict.get 默认 int）→ 提升后相减
        R(a) = val_float(val_as_num_ex(bv) - val_as_num_ex(cv));
        DISPATCH();
    }
    LABEL(OP_MUL_F): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        if (likely(val_is_float(bv) && val_is_float(cv))) {
            R(a) = val_float(val_as_double(bv) * val_as_double(cv));
            DISPATCH();
        }
        R(a) = val_float(val_as_num_ex(bv) * val_as_num_ex(cv));
        DISPATCH();
    }
    LABEL(OP_DIV_F): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        int line = GET_CURRENT_LINE();
        // 快路径：两侧确实 float
        if (likely(val_is_float(bv) && val_is_float(cv))) {
            double dv = val_as_double(cv);
            // 保留除零语义（C 的 /0.0 给 inf，而语言要求抛「除零错误」，见通用 OP_DIV）
            if (unlikely(dv == 0.0)) {
                if (!throw_runtime_error_with_operands("除零错误：除数为 0", "DIV (/)", bv, cv, line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            R(a) = val_float(val_as_double(bv) / dv);
            DISPATCH();
        }
        // 兜底：一侧 int → 提升；除数 0 同样抛错
        {
            double dv = val_as_num_ex(cv);
            if (unlikely(dv == 0.0)) {
                if (!throw_runtime_error_with_operands("除零错误：除数为 0", "DIV (/)", bv, cv, line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            R(a) = val_float(val_as_num_ex(bv) / dv);
            DISPATCH();
        }
    }
    LABEL(OP_NEG_F): {
        int a = READ_A(), b = READ_B();
        Value bv = R(b);
        if (likely(val_is_float(bv))) {
            R(a) = val_float(-val_as_double(bv));
            DISPATCH();
        }
        R(a) = val_float(-val_as_num_ex(bv));
        DISPATCH();
    }

    // --- 比较运算（完整语义见 vm.c 的 value_eq_stdlib / value_compare_stdlib）---
