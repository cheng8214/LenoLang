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
    // ============================================================================
    // 浮点有序比较特化（T10-④）：两侧静态类型都是 float（编译器已知）⇒ 直接双精度比较，
    // 不再走 `value_compare_stdlib()` 的跨 TU 函数调用与类型派发。
    // 语义与通用 OP_LT/LE/GT/GE **完全一致**（这是正确性的硬要求）：
    //   ① 任一操作数为 null ⇒ false（与通用版同一行代码）；
    //   ② 两边都是数值且不是 bigint（int/float）⇒ 按数值比较；
    //   ③ 其余（字符串 / bigint / 其它）⇒ **回退** value_compare_stdlib，
    //      连报错文案都用通用版那一份（ops_name 相同），保证行为不变。
    // ============================================================================
#define FLOAT_CMP_IMPL(dbl_cmp, cmp_cmp, ops_name)                                 \
    {                                                                              \
        int a = READ_A(), b = READ_B(), c = READ_C();                              \
        Value bv = R(b), cv = R(c);                                                \
        if (unlikely(val_is_null(bv) || val_is_null(cv))) {                        \
            R(a) = val_bool(0);                                                    \
            DISPATCH();                                                            \
        }                                                                          \
        if (likely(val_is_num(bv) && val_is_num(cv) &&                             \
                   !VM_IS_BIG(bv) && !VM_IS_BIG(cv))) {                            \
            double x = value_to_double(bv), y = value_to_double(cv);                \
            R(a) = val_bool(dbl_cmp);                                              \
            DISPATCH();                                                            \
        }                                                                          \
        int cmp = 0;                                                               \
        if (likely(value_compare_stdlib(bv, cv, &cmp))) {                          \
            R(a) = val_bool(cmp_cmp);                                              \
            DISPATCH();                                                            \
        }                                                                          \
        if (!throw_runtime_error_with_operands("比较运算: 操作数类型不可比较", ops_name, \
                                               bv, cv, GET_CURRENT_LINE())) return -1; \
        frame = &vm.frames[vm.frame_cnt - 1];                                      \
        DISPATCH();                                                                \
    }
    LABEL(OP_LT_F): FLOAT_CMP_IMPL(x < y, cmp < 0, "LT (<)")
    LABEL(OP_LE_F): FLOAT_CMP_IMPL(x <= y, cmp <= 0, "LE (<=)")
    LABEL(OP_GT_F): FLOAT_CMP_IMPL(x > y, cmp > 0, "GT (>)")
    LABEL(OP_GE_F): FLOAT_CMP_IMPL(x >= y, cmp >= 0, "GE (>=)")
#undef FLOAT_CMP_IMPL
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
    // 立即数比较（右操作数是 -128..127 的整数字面量，编进 C 字段的 8 位有符号）。
    // ============================================================================
    // 「比较 + 条件跳转」融合（T10-①）：`*_INT[_IMM] + JMP_IF_FALSE` 合成一条
    // ----------------------------------------------------------------------------
    // 省掉的是一条 OP_JMP_IF_FALSE 的**派发**（取指 + 行号写回 + 间接跳转）——
    // 条件判断在热循环里每轮都要付，fib 的递归路径因此 9 条 → 8 条。
    // 编码：A = 左操作数寄存器；B = 右操作数（寄存器号，或 C 的 0x80 位置 1 时是 int8 立即数）；
    //       C 只当标志用：bit7 = 立即数，bit6 = **极性**（0 = 比较为假则跳 / 1 = 比较为真则跳）；
    //       紧随的**第二个 4 字节字** = 跳转偏移（offset + 32768，与 iAsBx 同约定）。
    // 语义与 `OP_LT_INT[_IMM] + OP_JMP_IF_FALSE` **逐条等价**（极性为 0 时）：
    //   ① int 快路径只排除 null、不做类型检查（与 *_INT 系列同口径，stack 一致）；
    //   ② 比较为假 ⇒ 跳转（与 JMP_IF_FALSE 同向）。
    // 极性位（bit6）是后来加的，用来把 `while` 的**回边**也融合成一条：回边要的是
    // 「条件为真则往回跳」，有了它就不必再跟一条独立 `OP_JMP`（每轮省一次派发）。
    // bit6 在寄存器/立即数两种模式下原本都没人用 ⇒ 纯扩展，既有字节码语义不变。
    // ============================================================================
#define CMPJMP_IMPL(int_cmp)                                                       \
    {                                                                              \
        int a = READ_A(), b = READ_B(), c = READ_C();                              \
        int want_imm = (c & 0x80) != 0;                                            \
        int want_true = (c & 0x40) != 0;   /* bit6：比较为真则跳（while 回边用）*/  \
        int bx = ((int)frame->ip[0] << 8) | (int)frame->ip[1];                     \
        ADVANCE();                          /* 跳过携带偏移的第二个字 */           \
        Value bv = R(a);                                                           \
        int truthy;                                                                \
        if (want_imm) {                                                            \
            truthy = !val_is_null(bv) && (VM_IO(bv) int_cmp (int64_t)(int8_t)b);   \
        } else {                                                                   \
            Value cv = R(b);                                                       \
            truthy = !val_is_null(bv) && !val_is_null(cv) &&                       \
                     (VM_IO(bv) int_cmp VM_IO(cv));                                \
        }                                                                          \
        if (truthy == want_true) frame->ip += (bx - 32768);                        \
        DISPATCH();                                                                \
    }
    LABEL(OP_CMPJMP_LT): CMPJMP_IMPL(<)
    LABEL(OP_CMPJMP_LE): CMPJMP_IMPL(<=)
    LABEL(OP_CMPJMP_GT): CMPJMP_IMPL(>)
    LABEL(OP_CMPJMP_GE): CMPJMP_IMPL(>=)
#undef CMPJMP_IMPL

    // 语义与 OP_*_INT 完全一致：左值为 null ⇒ 结果 false（立即数本身不可能为 null）。
    // 动机：`if n <= 1 {` 原先要 MOV + LOADI + LE_INT + JMP_IF_FALSE 四条，
    // 有立即数比较后右值那条 LOADI 直接省掉。
    LABEL(OP_LT_INT_IMM): {
        int a = READ_A(), b = READ_B();
        int imm = (int)(int8_t)READ_C();
        Value bv = R(b);
        R(a) = val_bool(!val_is_null(bv) && VM_IO(bv) < (int64_t)imm);
        DISPATCH();
    }
    LABEL(OP_GT_INT_IMM): {
        int a = READ_A(), b = READ_B();
        int imm = (int)(int8_t)READ_C();
        Value bv = R(b);
        R(a) = val_bool(!val_is_null(bv) && VM_IO(bv) > (int64_t)imm);
        DISPATCH();
    }
    LABEL(OP_LE_INT_IMM): {
        int a = READ_A(), b = READ_B();
        int imm = (int)(int8_t)READ_C();
        Value bv = R(b);
        R(a) = val_bool(!val_is_null(bv) && VM_IO(bv) <= (int64_t)imm);
        DISPATCH();
    }
    LABEL(OP_GE_INT_IMM): {
        int a = READ_A(), b = READ_B();
        int imm = (int)(int8_t)READ_C();
        Value bv = R(b);
        R(a) = val_bool(!val_is_null(bv) && VM_IO(bv) >= (int64_t)imm);
        DISPATCH();
    }

    // --- 位运算 ---
    // 位运算：任一侧是 bigint（或移位/结果越界）⇒ 走 bigint 路径（无限精度补码语义）
    //
    // ★ T14：下面每个 op 的**函数体抽成宏**，寄存器版与常量化版（`*_K`）各实例化一次 ——
    //   "语义逐字同体"由编译期强制成立（唯一差异是右操作数取 `R[C]` 还是 `constants[C]`）。
    //   仓库有专门的《单一事实来源与重复实现收敛》待办 ⇒ 新指令不再抄一份 handler。
    //   常量版解决的是：`inp & 7` / `inp >> 8` 原先必须 `LOADI r,7 + OP_BITAND`（两条，
    //   而且**每轮重装同一个常量**）；常量编进 C 字段后省掉那条 LOADI。
    //   实测 `examples/性能测试/bench_bitwise_loop.leno`：每轮 10 条 → 6 条。
    //   右侧操作数取法：寄存器版 `R(READ_C())`；常量版 `constants[READ_CONST_IDX(READ_C())]`
    //   （后者在 C 为 0 时顺手跳过紧随的 EXTRAARG，见 01_prologue.inc 的 READ_CONST_IDX）。
    //
    // ★ 免装箱快路径（原 OP_BITAND 里的注释，现在整族共用；与栈式 op_bitwise.inc 的 §8.133 同口径）：
    //   一侧 bigint、另一侧 int48 ⇒ 走 `bigint_*_i64`（内部用栈上 view，**不为 int 操作数装箱**）。
    //   为什么不能直接对 int64 做位运算：bigint 的 int64 表示可能是"负号扩展"的
    //   位模式（如 2^64-1 ⇒ -1），而语言语义是**无限精度补码** ⇒ 结果会算错
    //   （实测 test_bigint_arith 立刻挂）。库函数自己按符号/幅值处理 ⇒ 语义精确。
    //   实测重灾区：SHA-256 的 right_rotate = `(ux >> c) | (ux << (32-c))`，
    //   左移结果超 int48 ⇒ 每次旋转都要为 OR 的 int 操作数装箱（每调用数百次）。
#define VM_BIT_CV_REG()   R(READ_C())
#define VM_BIT_CV_K()     (frame->chunk->constants[READ_CONST_IDX(READ_C())])

// ★ T11：**null 不能参与运算**（栈式 op_bitwise.inc 一直如此，寄存器式漏了 ⇒ 静默算 0）。
//   守卫写成「两侧都是 int48」（原来是「非 bigint」）—— 两者都是**两次 tag 比较**，
//   快路径代价不变；null / float / string 被挤进慢路径 ⇒ null 报错、其余类型也**报错**
//   （与栈式同一句"操作数必须是整数"，此前这里会把 float 位当 int 算成垃圾）。
//   文案逐字取自栈式 op_bitwise.inc，勿另起炉灶。
//
//   ⚠⚠ **守卫必须写成否定式**（`if (unlikely(!...)) { 慢路径 }`，快路径自然落空）：
//     实测把快路径放进 `if (likely(...)) { 算; DISPATCH(); }` 会让位运算基准
//     掉 **+11%**（2273ms → 2522ms，4 轮无重叠）—— 每条指令多一次 taken 跳转。
//     算术特化家族一开始就是这个形态，所以那边是中性 ✓。
//   ⚠ 冷路径也要**尽量小**：位运算指令是全 VM 最短的（快路径就一条 AND/OR/XOR），
//     实测每多一份内联报错代码，位运算基准就掉一档（2273 → 2540ms）。
//     ⇒ 两个错误（null / 非整数）**合并成一个报错点**，只在真正非法时才拼文案。
#define BITAND_BODY(CV)                                                                \
    {                                                                                  \
        int a = READ_A(), b = READ_B();                                                \
        Value bv = R(b), cv = (CV);                                                    \
        if (unlikely(!val_is_int(bv) || !val_is_int(cv))) {                            \
            if (VM_IS_BIG(bv) || VM_IS_BIG(cv)) {                                      \
                if (VM_IS_BIG(bv) && !VM_IS_BIG(cv)) {                                 \
                    R(a) = bigint_compact_to_int(bigint_and_i64(val_as_bigint(bv), VM_IO(cv))); \
                } else if (!VM_IS_BIG(bv) && VM_IS_BIG(cv)) {                          \
                    R(a) = bigint_compact_to_int(bigint_and_i64(val_as_bigint(cv), VM_IO(bv))); \
                } else {                                                               \
                    R(a) = bigint_compact_to_int(bigint_and(promote_to_bigint(bv), promote_to_bigint(cv))); \
                }                                                                      \
                DISPATCH();                                                            \
            }                                                                          \
            if (!throw_runtime_error_with_operands(                                    \
                    (val_is_null(bv) || val_is_null(cv)) ? "位与运算: null 不能参与运算" \
                                                         : "位与运算: 操作数必须是整数",  \
                    "BITAND (&)", bv, cv, GET_CURRENT_LINE())) return -1;              \
            frame = &vm.frames[vm.frame_cnt - 1];                                      \
            DISPATCH();                                                                \
        }                                                                              \
        R(a) = val_int(val_as_int(bv) & val_as_int(cv));                               \
        DISPATCH();                                                                    \
    }
    LABEL(OP_BITAND):   BITAND_BODY(VM_BIT_CV_REG())
    LABEL(OP_BITAND_K): BITAND_BODY(VM_BIT_CV_K())

#define BITOR_BODY(CV)                                                                 \
    {                                                                                  \
        int a = READ_A(), b = READ_B();                                                \
        Value bv = R(b), cv = (CV);                                                    \
        if (unlikely(!val_is_int(bv) || !val_is_int(cv))) {   /* 见 BITAND_BODY 说明 */  \
            if (VM_IS_BIG(bv) || VM_IS_BIG(cv)) {                                      \
                if (VM_IS_BIG(bv) && !VM_IS_BIG(cv)) {                                 \
                    R(a) = bigint_compact_to_int(bigint_or_i64(val_as_bigint(bv), VM_IO(cv))); \
                } else if (!VM_IS_BIG(bv) && VM_IS_BIG(cv)) {                          \
                    R(a) = bigint_compact_to_int(bigint_or_i64(val_as_bigint(cv), VM_IO(bv))); \
                } else {                                                               \
                    R(a) = bigint_compact_to_int(bigint_or(promote_to_bigint(bv), promote_to_bigint(cv))); \
                }                                                                      \
                DISPATCH();                                                            \
            }                                                                          \
            if (!throw_runtime_error_with_operands(                                    \
                    (val_is_null(bv) || val_is_null(cv)) ? "位或运算: null 不能参与运算" \
                                                         : "位或运算: 操作数必须是整数",  \
                    "BITOR (|)", bv, cv, GET_CURRENT_LINE())) return -1;               \
            frame = &vm.frames[vm.frame_cnt - 1];                                      \
            DISPATCH();                                                                \
        }                                                                              \
        R(a) = val_int(val_as_int(bv) | val_as_int(cv));                               \
        DISPATCH();                                                                    \
    }
    LABEL(OP_BITOR):   BITOR_BODY(VM_BIT_CV_REG())
    LABEL(OP_BITOR_K): BITOR_BODY(VM_BIT_CV_K())

#define BITXOR_BODY(CV)                                                                \
    {                                                                                  \
        int a = READ_A(), b = READ_B();                                                \
        Value bv = R(b), cv = (CV);                                                    \
        if (unlikely(!val_is_int(bv) || !val_is_int(cv))) {   /* 见 BITAND_BODY 说明 */  \
            if (VM_IS_BIG(bv) || VM_IS_BIG(cv)) {                                      \
                if (VM_IS_BIG(bv) && !VM_IS_BIG(cv)) {                                 \
                    R(a) = bigint_compact_to_int(bigint_xor_i64(val_as_bigint(bv), VM_IO(cv))); \
                } else if (!VM_IS_BIG(bv) && VM_IS_BIG(cv)) {                          \
                    R(a) = bigint_compact_to_int(bigint_xor_i64(val_as_bigint(cv), VM_IO(bv))); \
                } else {                                                               \
                    R(a) = bigint_compact_to_int(bigint_xor(promote_to_bigint(bv), promote_to_bigint(cv))); \
                }                                                                      \
                DISPATCH();                                                            \
            }                                                                          \
            if (!throw_runtime_error_with_operands(                                    \
                    (val_is_null(bv) || val_is_null(cv)) ? "位异或运算: null 不能参与运算" \
                                                         : "位异或运算: 操作数必须是整数", \
                    "BITXOR (^)", bv, cv, GET_CURRENT_LINE())) return -1;              \
            frame = &vm.frames[vm.frame_cnt - 1];                                      \
            DISPATCH();                                                                \
        }                                                                              \
        R(a) = val_int(val_as_int(bv) ^ val_as_int(cv));                               \
        DISPATCH();                                                                    \
    }
    LABEL(OP_BITXOR):   BITXOR_BODY(VM_BIT_CV_REG())
    LABEL(OP_BITXOR_K): BITXOR_BODY(VM_BIT_CV_K())

    LABEL(OP_BITNOT): {
        int a = READ_A(), b = READ_B();
        Value bv = R(b);
        // 守卫形态见 BITAND_BODY 的 ⚠⚠（否定式：快路径必须能落空）
        if (unlikely(!val_is_int(bv))) {
            if (VM_IS_BIG(bv)) {
                R(a) = bigint_compact_to_int(bigint_not(val_as_bigint(bv)));
                DISPATCH();
            }
            // null 与其它非法类型：与栈式 op_bitwise.inc 的 BITNOT 同一句
            if (!throw_runtime_error_with_operand("位非运算: 操作数必须是整数", "BITNOT (~)", bv, GET_CURRENT_LINE())) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        R(a) = val_int(~val_as_int(bv));
        DISPATCH();
    }
#define SHL_BODY(CV)                                                                   \
    {                                                                                  \
        int a = READ_A(), b = READ_B();                                                \
        Value bv = R(b), cv = (CV);                                                    \
        int64_t shi = val_as_int(cv);   /* 先取（非 int48 时无意义，下面守卫会挡住）*/   \
        if (unlikely(!val_is_int(bv) || !val_is_int(cv) || shi < 0 || shi >= 32)) {     \
            if (val_is_null(bv) || val_is_null(cv)) {                                  \
                if (!throw_runtime_error_with_operands("左移运算: null 不能参与运算",    \
                        "SHL (<<)", bv, cv, GET_CURRENT_LINE())) return -1;            \
                frame = &vm.frames[vm.frame_cnt - 1];                                  \
                DISPATCH();                                                            \
            }                                                                          \
            /* bigint 操作数、或移位量 >= 32（int48 结果必然越界）⇒ bigint 路径（与改动前同口径）*/ \
            {                                                                          \
                int64_t sh = VM_IO(cv);                                                \
                R(a) = bigint_compact_to_int(bigint_shl(promote_to_bigint(bv), (int)(sh < 0 ? 0 : sh))); \
            }                                                                          \
            DISPATCH();                                                                \
        }                                                                              \
        R(a) = val_int(val_as_int(bv) << shi);                                         \
        DISPATCH();                                                                    \
    }
    LABEL(OP_SHL):   SHL_BODY(VM_BIT_CV_REG())
    LABEL(OP_SHL_K): SHL_BODY(VM_BIT_CV_K())

#define SHR_BODY(CV)                                                                   \
    {                                                                                  \
        int a = READ_A(), b = READ_B();                                                \
        Value bv = R(b), cv = (CV);                                                    \
        int64_t shi = val_as_int(cv);   /* 见 SHL_BODY */                               \
        if (unlikely(!val_is_int(bv) || !val_is_int(cv) || shi < 0 || shi > 47)) {      \
            if (val_is_null(bv) || val_is_null(cv)) {                                  \
                if (!throw_runtime_error_with_operands("右移运算: null 不能参与运算",    \
                        "SHR (>>)", bv, cv, GET_CURRENT_LINE())) return -1;            \
                frame = &vm.frames[vm.frame_cnt - 1];                                  \
                DISPATCH();                                                            \
            }                                                                          \
            {                                                                          \
                int64_t sh = VM_IO(cv);                                                \
                R(a) = bigint_compact_to_int(bigint_shr(promote_to_bigint(bv), (int)(sh < 0 ? 0 : sh))); \
            }                                                                          \
            DISPATCH();                                                                \
        }                                                                              \
        R(a) = val_int(val_as_int(bv) >> shi);                                         \
        DISPATCH();                                                                    \
    }
    LABEL(OP_SHR):   SHR_BODY(VM_BIT_CV_REG())
    LABEL(OP_SHR_K): SHR_BODY(VM_BIT_CV_K())

#define USHR_BODY(CV)                                                                  \
    {                                                                                  \
        int a = READ_A(), b = READ_B();                                                \
        Value vb = R(b), vc = (CV);                                                    \
        /* LenoC 的 int 是 48 位有符号：逻辑右移先截断为 48 位无符号再右移。 */          \
        /* 之前用 `(unsigned int)`（32 位）截断 ⇒ `-1 >>> 8` 得 16777215 而非 1099511627775 ✗ */ \
        if (unlikely(!val_is_int(vb) || !val_is_int(vc))) {   /* 见 BITAND_BODY */      \
            if (val_is_null(vb) || val_is_null(vc)) {                                  \
                if (!throw_runtime_error_with_operands("逻辑右移运算: null 不能参与运算", \
                        "USHR (>>>)", vb, vc, GET_CURRENT_LINE())) return -1;          \
                frame = &vm.frames[vm.frame_cnt - 1];                                  \
                DISPATCH();                                                            \
            }                                                                          \
            if (VM_IS_BIG(vb) || VM_IS_BIG(vc)) {                                      \
                int64_t sh = VM_IO(vc);                                                \
                R(a) = bigint_compact_to_int(bigint_ushr(promote_to_bigint(vb), (int)(sh < 0 ? 0 : sh))); \
                DISPATCH();                                                            \
            }                                                                          \
            /* 其它非法类型：与栈式同句报错（改动前这里**静默返回 0** ✗）*/              \
            if (!throw_runtime_error_with_operands("逻辑右移运算: 操作数必须是整数",     \
                    "USHR (>>>)", vb, vc, GET_CURRENT_LINE())) return -1;              \
            frame = &vm.frames[vm.frame_cnt - 1];                                      \
            DISPATCH();                                                                \
        }                                                                              \
        {                                                                              \
            uint64_t uva = (uint64_t)val_as_int(vb) & PAYLOAD_MASK;                     \
            int64_t sh = val_as_int(vc);                                                \
            R(a) = val_int_safe((int64_t)(uva >> sh));                                  \
        }                                                                              \
        DISPATCH();                                                                    \
    }
    LABEL(OP_USHR):   USHR_BODY(VM_BIT_CV_REG())
    LABEL(OP_USHR_K): USHR_BODY(VM_BIT_CV_K())

#undef BITAND_BODY
#undef BITOR_BODY
#undef BITXOR_BODY
#undef SHL_BODY
#undef SHR_BODY
#undef USHR_BODY
#undef VM_BIT_CV_REG
#undef VM_BIT_CV_K

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
