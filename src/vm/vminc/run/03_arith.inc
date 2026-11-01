    LABEL(OP_ADD): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        int line = GET_CURRENT_LINE();

        if (likely(val_is_int(bv) && val_is_int(cv))) {
            // ★ 2026-10-03：与 OP_FOR_LOOP 同一套微调（那里有完整说明与实测）
            //   ① 符号扩展无分支 ② 范围判定折成一次无符号比较
            int64_t x = VAL_INT48_PAYLOAD(bv), y = VAL_INT48_PAYLOAD(cv), s = 0;
            if (unlikely(__builtin_add_overflow(x, y, &s))) {
                R(a) = bigint_add(bigint_from_int64(x), bigint_from_int64(y));
            } else if (unlikely(!INT48_IN_RANGE(s))) {
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

    // T33：字符串自追加（`s = s + e`）—— 允许**原地修改** R[A] 指向的串，
    //   把这类累加的 O(n²) 摊平成 O(n)（CPython 靠 refcount==1 的 in-place realloc 特例
    //   做到同一件事；Leno 的 GC 没有引用计数，所以改由**编译期**证明"该槽位无别名"）。
    //   运行时这两道闸保证"即使编译期判断失误也只是变慢，不会写坏别人的串"：
    //     · 只有 `capacity > len + 1` 的串才原地追加 —— 该容量只由 str_append_copy 预留，
    //       字面量 / 内化共享串 / 外部传入的串的 capacity 恒为 len+1 ⇒ 一律走复制路径；
    //     · 左操作数不是字符串时直接回退 string_add（含"值→字符串"的通用转换）。
    LABEL(OP_STR_APPEND): {
        int a = READ_A(), b = READ_B();
        Value av = R(a);
        Value bv = R(b);
        if (likely(val_is_obj(av) && val_as_obj(av)->type == OBJ_STRING)) {
            ObjString* as = (ObjString*)val_as_obj(av);
            if (as->capacity > as->len + 1) {
                ObjString* r = str_append_inplace(as, bv);   // 独占 ⇒ 原地追加
                R(a) = r ? val_obj((Object*)r) : string_add(av, bv);
            } else {
                ObjString* r = str_append_copy(as, bv);      // 首次/分叉 ⇒ 复制并预留容量
                R(a) = r ? val_obj((Object*)r) : string_add(av, bv);
            }
        } else {
            R(a) = string_add(av, bv);
        }
        DISPATCH();
    }

    LABEL(OP_SUB): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        int line = GET_CURRENT_LINE();

        if (likely(val_is_int(bv) && val_is_int(cv))) {
            // ★ 2026-10-03：同 OP_ADD（无分支符号扩展 + 一次无符号范围判定）
            int64_t x = VAL_INT48_PAYLOAD(bv), y = VAL_INT48_PAYLOAD(cv), s = 0;
            if (unlikely(__builtin_sub_overflow(x, y, &s))) {
                R(a) = bigint_sub(bigint_from_int64(x), bigint_from_int64(y));
            } else if (unlikely(!INT48_IN_RANGE(s))) {
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
            // ★ 2026-10-03：同 OP_ADD（无分支符号扩展 + 一次无符号范围判定）
            int64_t x = VAL_INT48_PAYLOAD(bv), y = VAL_INT48_PAYLOAD(cv), s = 0;
            if (x == 0 || y == 0) {
                R(a) = val_int(0);
            } else if (unlikely(LENO_MUL_OVERFLOW(x, y, &s))) {
                R(a) = bigint_mul(bigint_from_int64(x), bigint_from_int64(y));
            } else if (unlikely(!INT48_IN_RANGE(s))) {
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
            // ★ 用 val_int_safe：int48 除法**只有一个**越界组合 ——
            //   INT48_MIN / -1 == +2^47 == INT48_MAX + 1，越出 int48 上界。
            //   原先直接 val_int(...) 会把它**静默回绕**成 -140737488355328（负号都没了），
            //   而本语言对 int48 越界的一贯约定是**提升 bigint**（见 OP_NEG / OP_SHL /
            //   VM_INT_RESULT）⇒ 这里必须同口径。其余任何 (bv, dv) 组合结果都在
            //   [-2^47, 2^47-1] 内，走不到提升分支，热路径代价只是一次范围比较。
            R(a) = val_int_safe(val_as_int(bv) / dv);
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

        // ★ 2026-10-24：**float 取模**
        //   原来只有**编译期常量折叠**支持 float（`7.5 % 3` 直接给出 1.5），
        //   一旦操作数是运行期变量就报「取模操作数必须是整数」⇒ 同一运算符两套行为，
        //   字面量能算、变量崩（第四轮报告 B3，最难排查的那种不一致）。
        //   现在统一走 fmod：
        //     · 方向与整数一致 —— `-7 % 3 = -1`（C 式向零截断）⇒ `fmod(-7.5, 3) = -1.5` ✓
        //       （不用 Python 的"向负无穷"，否则又变成两套口径）
        //     · 除零仍然**明确报错**，不给 NaN（NaN 静默流传比报错糟得多）
        if ((val_is_int(bv) || val_is_float(bv)) && (val_is_int(cv) || val_is_float(cv)) &&
            (val_is_float(bv) || val_is_float(cv))) {
            double dvf = val_as_num(cv);
            if (unlikely(dvf == 0.0)) {
                if (!throw_runtime_error_with_operands("取模除零错误：除数为 0", "MOD (%)", bv, cv, line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            R(a) = val_float(fmod(val_as_num(bv), dvf));
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
            // ★ 2026-10-03：同 OP_ADD（无分支符号扩展 + 一次无符号范围判定；
            //   `-x` 在 INT64_MIN 的极端情形由上面那条 unlikely 分支先兜住 ⇒ 此处 -x 不会 UB ✓）
            int64_t x = VAL_INT48_PAYLOAD(bv);
            if (unlikely(x == INT64_MIN)) {
                R(a) = bigint_sub(bigint_from_int64(0), bigint_from_int64(x));
            } else if (unlikely(!INT48_IN_RANGE(-x))) {
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
    // ★ 2026-10-03：同 OP_ADD —— `VM_IO` 用无分支的 `VAL_INT48_PAYLOAD`，
    //   `VM_INT_RESULT` 用一次无符号范围判定（语义不变 ✓）
    #define VM_IO(_v) (val_is_bigint(_v) ? vm_bigint_as_i64(_v) : VAL_INT48_PAYLOAD(_v))
    #define VM_INT_RESULT(_x) \
        (INT48_IN_RANGE(_x) ? val_int((int64_t)(_x)) \
         : bigint_compact_to_int(val_obj((Object*)bigint_from_int64((int64_t)(_x)))))
    #define VM_IS_BIG(_v) (unlikely(val_is_bigint(_v)))
    // ★ T11：**null 不能参与运算** —— 特化指令此前把 null 读成 0（静默错值）。
    //   守卫从「非 bigint」改成「**两侧都是 int48**」：两者都是**两次 tag 比较**，
    //   快路径代价不变（与 VM_IS_BIG 的判断合并成同一个分支）；但 null / 其它类型会被
    //   挤进这个慢分支 ⇒ null 报错、bigint 走原路、其余类型保持改动前行为。
    //   此前静默算错的形态（实测都返回 1，而通用路径会报「null 不能参与运算」）：
    //     `int? a = null; a + 1`（编译期只给警告，运行期本该报错）、
    //     `struct S{int x}; new S().x + 1`（值类型字段未赋值 ⇒ 运行期 null）、
    //     `s.x += 1`（复合赋值走同一条特化指令）。
    LABEL(OP_ADD_INT): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        if (unlikely(!val_is_int(bv) || !val_is_int(cv))) {
            if (val_is_null(bv) || val_is_null(cv)) {
                if (!throw_runtime_error_with_operands("加法运算: null 不能参与运算", "ADD (+)", bv, cv, GET_CURRENT_LINE())) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            if (VM_IS_BIG(bv) || VM_IS_BIG(cv)) {
                R(a) = bigint_compact_to_int(bigint_add(promote_to_bigint(bv), promote_to_bigint(cv)));
                DISPATCH();
            }
            // 其余类型：保持改动前行为（落到下面的 int 算术）
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
        if (unlikely(!val_is_int(bv) || !val_is_int(cv))) {   // 守卫理由见 OP_ADD_INT
            if (val_is_null(bv) || val_is_null(cv)) {
                if (!throw_runtime_error_with_operands("减法: null 不能参与运算", "SUB (-)", bv, cv, GET_CURRENT_LINE())) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            if (VM_IS_BIG(bv) || VM_IS_BIG(cv)) {
                R(a) = bigint_compact_to_int(bigint_sub(promote_to_bigint(bv), promote_to_bigint(cv)));
                DISPATCH();
            }
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
        if (unlikely(!val_is_int(bv))) {                      // 守卫理由见 OP_ADD_INT
            if (val_is_null(bv)) {
                if (!throw_runtime_error_with_operand("加法运算: null 不能参与运算", "ADD (+)", bv, GET_CURRENT_LINE())) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            if (VM_IS_BIG(bv)) {
                R(a) = bigint_compact_to_int(
                    bigint_add(promote_to_bigint(bv), bigint_from_int64((int64_t)imm)));
                DISPATCH();
            }
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
        if (unlikely(!val_is_int(bv))) {                      // 守卫理由见 OP_ADD_INT
            if (val_is_null(bv)) {
                if (!throw_runtime_error_with_operand("减法: null 不能参与运算", "SUB (-)", bv, GET_CURRENT_LINE())) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            if (VM_IS_BIG(bv)) {
                R(a) = bigint_compact_to_int(
                    bigint_sub(promote_to_bigint(bv), bigint_from_int64((int64_t)imm)));
                DISPATCH();
            }
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
        if (unlikely(!val_is_int(bv) || !val_is_int(cv))) {   // 守卫理由见 OP_ADD_INT
            if (val_is_null(bv) || val_is_null(cv)) {
                if (!throw_runtime_error_with_operands("乘法: null 不能参与运算", "MUL (*)", bv, cv, GET_CURRENT_LINE())) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            if (VM_IS_BIG(bv) || VM_IS_BIG(cv)) {
                R(a) = bigint_compact_to_int(bigint_mul(promote_to_bigint(bv), promote_to_bigint(cv)));
                DISPATCH();
            }
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
        if (unlikely(!val_is_int(bv))) {                      // 守卫理由见 OP_ADD_INT
            if (val_is_null(bv)) {
                if (!throw_runtime_error_with_operand("乘法: null 不能参与运算", "MUL (*)", bv, GET_CURRENT_LINE())) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            if (VM_IS_BIG(bv)) {
                R(a) = bigint_compact_to_int(
                    bigint_mul(promote_to_bigint(bv), bigint_from_int64((int64_t)imm)));
                DISPATCH();
            }
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
        if (unlikely(!val_is_int(bv))) {                      // 守卫理由见 OP_ADD_INT
            if (VM_IS_BIG(bv)) {
                R(a) = bigint_compact_to_int(bigint_neg(val_as_bigint(bv)));
                DISPATCH();
            }
            // null（与其它非法类型）：与通用 OP_NEG 同口径报错 —— 通用一元取负本来就这一句
            if (val_is_null(bv)) {
                if (!throw_runtime_error_with_operand("取负 (-): 操作数必须是数字", "NEG (-)", bv, GET_CURRENT_LINE())) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
        }
        R(a) = VM_INT_RESULT(-val_as_int(bv));
        DISPATCH();
    }

    // --- float 特化 ---
    // ★ T11：null 检查一律放在**兜底路径**里 —— 快路径（两侧确实 float）零额外代价。
    //   文案与对应的通用指令逐字一致（见文件开头的通用 OP_ADD/SUB/...）。
    LABEL(OP_ADD_F): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value bv = R(b), cv = R(c);
        // 快路径：两侧确实 float（codegen 静态判 float 触发）
        if (likely(val_is_float(bv) && val_is_float(cv))) {
            R(a) = val_float(val_as_double(bv) + val_as_double(cv));
            DISPATCH();
        }
        if (unlikely(val_is_null(bv) || val_is_null(cv))) {
            if (!throw_runtime_error_with_operands("加法运算: null 不能参与运算", "ADD (+)", bv, cv, GET_CURRENT_LINE())) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
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
        if (unlikely(val_is_null(bv) || val_is_null(cv))) {   // 说明见 OP_ADD_F
            if (!throw_runtime_error_with_operands("减法: null 不能参与运算", "SUB (-)", bv, cv, GET_CURRENT_LINE())) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
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
        if (unlikely(val_is_null(bv) || val_is_null(cv))) {   // 说明见 OP_ADD_F
            if (!throw_runtime_error_with_operands("乘法: null 不能参与运算", "MUL (*)", bv, cv, GET_CURRENT_LINE())) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
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
            if (unlikely(val_is_null(bv) || val_is_null(cv))) {   // 说明见 OP_ADD_F
                if (!throw_runtime_error_with_operands("除法: null 不能参与运算", "DIV (/)", bv, cv, line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
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
        if (unlikely(val_is_null(bv))) {                      // 说明见 OP_ADD_F
            if (!throw_runtime_error_with_operand("取负 (-): 操作数必须是数字", "NEG (-)", bv, GET_CURRENT_LINE())) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        R(a) = val_float(-val_as_num_ex(bv));
        DISPATCH();
    }

    // ★ 2026-11-01：`maths.*` 纯数值函数的**编译期内联**（见 leno_vm.h 的 OP_MATH_FAST）。
    //   语义与 maths 模块里的同名函数逐条对齐（连返回类型都一致：roundi/clampi/mini/maxi/mod 给 int，
    //   其余给 float），只是省掉了「取模块对象 → 装方法名 → OP_INDEX 查表 → native 调用」四步。
    //   实测（tools/bench_maths_native.leno）：native clamp 比内联手写贵 2.1ns，本条指令约 1~2ns。
    //   ⚠ 参数只求值一次（编译期已把实参放进 R[B..]），不存在"条件表达式重复求值"的坑 ✓
    LABEL(OP_MATH_FAST): {
        int a = READ_A(), b = READ_B(), c = READ_C();

        // 取数口径与 maths 的 get_number 一致：int 直接取，其余按 double
        #define MATHF_D(reg) (val_is_int(R(reg)) ? (double)val_as_int(R(reg)) : val_as_num_ex(R(reg)))
        #define MATHF_I(reg) (val_is_int(R(reg)) ? val_as_int(R(reg)) : (int64_t)llround(val_as_num_ex(R(reg))))
        double x = MATHF_D(b);
        // 2/3 参的才读 y、z（1 参的省掉两次取值）
        int nargs = (c == MATHF_CLAMP || c == MATHF_LERP || c == MATHF_CLAMPI) ? 3
                  : (c == MATHF_MIN || c == MATHF_MAX || c == MATHF_MINI || c == MATHF_MAXI
                     || c == MATHF_MOD) ? 2 : 1;
        double y = (nargs >= 2) ? MATHF_D(b + 1) : 0.0;
        double z = (nargs >= 3) ? MATHF_D(b + 2) : 0.0;
        switch (c) {
            case MATHF_ABS:
                R(a) = val_float(fabs(x));
                break;
            case MATHF_CLAMP:
                R(a) = val_float(x < y ? y : (x > z ? z : x));
                break;
            case MATHF_MIN:
                R(a) = val_float(x < y ? x : y);
                break;
            case MATHF_MAX:
                R(a) = val_float(x > y ? x : y);
                break;
            case MATHF_LERP:
                R(a) = val_float(x + (y - x) * z);
                break;
            case MATHF_SMOOTHSTEP: {
                // 与 maths.smoothstep 同口径：先夹 0..1 再算 3t²-2t³（死区直接返回）
                if (x <= 0.0) R(a) = val_float(0.0);
                else if (x >= 1.0) R(a) = val_float(1.0);
                else R(a) = val_float(x * x * (3.0 - 2.0 * x));
            } break;
            case MATHF_SATURATE:
                R(a) = val_float(x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x));
                break;
            case MATHF_FRACT:
                R(a) = val_float(x - floor(x));
                break;
            case MATHF_ROUNDI:
                R(a) = val_int((int64_t)llround(x));
                break;
            case MATHF_CLAMPI: {
                int64_t v = MATHF_I(b), lo = MATHF_I(b + 1), hi = MATHF_I(b + 2);
                if (hi < lo) hi = lo;      // 边界写反 ⇒ 收成 lo（与 maths.clampi 同一口径）
                R(a) = val_int(v < lo ? lo : (v > hi ? hi : v));
            } break;
            case MATHF_MINI: {
                int64_t u = MATHF_I(b), v = MATHF_I(b + 1);
                R(a) = val_int(u < v ? u : v);
            } break;
            case MATHF_MAXI: {
                int64_t u = MATHF_I(b), v = MATHF_I(b + 1);
                R(a) = val_int(u > v ? u : v);
            } break;
            case MATHF_MOD: {
                int64_t u = MATHF_I(b), v = MATHF_I(b + 1);
                if (unlikely(v == 0)) {
                    if (!throw_runtime_error("mod() 除数不能为 0", GET_CURRENT_LINE())) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                int64_t r = u % v;
                if (r != 0 && ((r < 0) != (v < 0))) r += v;    // 正取模：结余符号跟除数走
                R(a) = val_int(r);
            } break;
            default:
                R(a) = val_null();
                break;
        }
        #undef MATHF_D
        #undef MATHF_I
        DISPATCH();
    }

    // --- 比较运算（完整语义见 vm.c 的 value_eq_stdlib / value_compare_stdlib）---
