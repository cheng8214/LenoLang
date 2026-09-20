// int48 运算辅助宏（溢出时自动提升为 BigInt）

// int48 回绕运算辅助宏（仅用于需要显式回绕语义的场景）
#define PUSH_INT_WRAP(result) \
    vm_stack_push_fast(&vm, val_int((int64_t)(result)))

#define PUSH_INT_WRAP_FAST(result) \
    vm_stack_push_fast(&vm, val_int((int64_t)(result)))

// 安全提升宏：结果超出 int48 范围时自动提升为 BigInt
#define PUSH_INT64_RESULT(result) do { \
    int64_t _r = (result); \
    if (_r < INT48_MIN || _r > INT48_MAX) { \
        vm_stack_push_fast(&vm, val_bigint_from_int64(_r)); \
    } else { \
        vm_stack_push_fast(&vm, val_int(_r)); \
    } \
} while(0)

#define PUSH_INT64_RESULT_FAST(result) do { \
    int64_t _r = (result); \
    if (_r < INT48_MIN || _r > INT48_MAX) { \
        vm_stack_push_fast(&vm, val_bigint_from_int64(_r)); \
    } else { \
        vm_stack_push_fast(&vm, val_int(_r)); \
    } \
} while(0)

#define PUSH_SAFE_INT64_ADD(a, b) do { \
    int64_t _a = (a), _b = (b); \
    int64_t _sum = _a + _b; \
    if ((_b > 0 && _a > INT64_MAX - _b) || \
        (_b < 0 && _a < INT64_MIN - _b)) { \
        ObjBigInt* _ba = bigint_from_int64(_a); \
        ObjBigInt* _bb = bigint_from_int64(_b); \
        Value _result = bigint_add(_ba, _bb); \
        vm_stack_push_fast(&vm, _result); \
    } else if (_sum < INT48_MIN || _sum > INT48_MAX) { \
        vm_stack_push_fast(&vm, val_bigint_from_int64(_sum)); \
    } else { \
        vm_stack_push_fast(&vm, val_int(_sum)); \
    } \
} while(0)

#define PUSH_SAFE_INT64_ADD_FAST(a, b) do { \
    int64_t _a = (a), _b = (b); \
    int64_t _sum = _a + _b; \
    if ((_b > 0 && _a > INT64_MAX - _b) || \
        (_b < 0 && _a < INT64_MIN - _b)) { \
        ObjBigInt* _ba = bigint_from_int64(_a); \
        ObjBigInt* _bb = bigint_from_int64(_b); \
        Value _result = bigint_add(_ba, _bb); \
        vm_stack_push_fast(&vm, _result); \
    } else if (_sum < INT48_MIN || _sum > INT48_MAX) { \
        vm_stack_push_fast(&vm, val_bigint_from_int64(_sum)); \
    } else { \
        vm_stack_push_fast(&vm, val_int(_sum)); \
    } \
} while(0)

#define PUSH_SAFE_INT64_SUB(a, b) do { \
    int64_t _a = (a), _b = (b); \
    int64_t _diff = _a - _b; \
    if ((_b < 0 && _a > INT64_MAX + _b) || \
        (_b > 0 && _a < INT64_MIN + _b)) { \
        ObjBigInt* _ba = bigint_from_int64(_a); \
        ObjBigInt* _bb = bigint_from_int64(_b); \
        Value _result = bigint_sub(_ba, _bb); \
        vm_stack_push_fast(&vm, _result); \
    } else if (_diff < INT48_MIN || _diff > INT48_MAX) { \
        vm_stack_push_fast(&vm, val_bigint_from_int64(_diff)); \
    } else { \
        vm_stack_push_fast(&vm, val_int(_diff)); \
    } \
} while(0)

#define PUSH_SAFE_INT64_SUB_FAST(a, b) do { \
    int64_t _a = (a), _b = (b); \
    int64_t _diff = _a - _b; \
    if ((_b < 0 && _a > INT64_MAX + _b) || \
        (_b > 0 && _a < INT64_MIN + _b)) { \
        ObjBigInt* _ba = bigint_from_int64(_a); \
        ObjBigInt* _bb = bigint_from_int64(_b); \
        Value _result = bigint_sub(_ba, _bb); \
        vm_stack_push_fast(&vm, _result); \
    } else if (_diff < INT48_MIN || _diff > INT48_MAX) { \
        vm_stack_push_fast(&vm, val_bigint_from_int64(_diff)); \
    } else { \
        vm_stack_push_fast(&vm, val_int(_diff)); \
    } \
} while(0)

/* ---- §8.123：乘法溢出检测**不能**用 `_result / _a != _b` ✗ ----
 * 旧写法先把 `_a * _b` 算进 int64 ✗ —— 一旦真的溢出就是**有符号溢出（UB）** ✗，
 * 而编译器有权按"不会溢出"假设把那条判等**优化掉** ⇒ 直接走下面 `PUSH_INT64_RESULT(-2^63)`
 * ⇒ 得到**负** bigint ✗（实测 `0x80000000 * 4294967296`：字面量按无符号解析成**正** 2^63 ✓，
 * 乘法却给出 **负** 2^63 ✗ ⇒ 同一位模式两种语义，bug3 ✓，并进一步喂给 bigint_shr 引发 bug1 ✓）。
 * 修法：用 __builtin_mul_overflow（不执行溢出乘法、不依赖 UB ✓）；
 * 无该内建时用**纯整数比较**的等价判定 ✓（也不做溢出乘法 ✓）。 */
#if defined(__GNUC__) || defined(__clang__)
  #define LENO_MUL_OVERFLOW(_a, _b, _out) (__builtin_mul_overflow((_a), (_b), (_out)) != 0)
#else
  #define LENO_MUL_OVERFLOW(_a, _b, _out) \
      ( ((_a) > 0) ? ( ((_b) > 0) ? ((_a) > INT64_MAX / (_b)) \
                                  : ((_b) < INT64_MIN / (_a)) ) \
                   : ( ((_b) > 0) ? ((_a) < INT64_MIN / (_b)) \
                                  : ((_a) != 0 && (_b) < INT64_MAX / (_a)) ) )
#endif

#define PUSH_SAFE_INT64_MUL(a, b) do { \
    int64_t _a = (a), _b = (b); \
    int64_t _result = 0; \
    if (_a == 0 || _b == 0) { \
        vm_stack_push_fast(&vm, val_int(0)); \
    } else if (LENO_MUL_OVERFLOW(_a, _b, &_result)) { \
        ObjBigInt* _ba = bigint_from_int64(_a); \
        ObjBigInt* _bb = bigint_from_int64(_b); \
        Value _rv = bigint_mul(_ba, _bb); \
        vm_stack_push_fast(&vm, _rv); \
    } else { \
        PUSH_INT64_RESULT(_result); \
    } \
} while(0)

#define PUSH_SAFE_INT64_MUL_FAST(a, b) do { \
    int64_t _a = (a), _b = (b); \
    int64_t _result = 0; \
    if (_a == 0 || _b == 0) { \
        vm_stack_push_fast(&vm, val_int(0)); \
    } else if (LENO_MUL_OVERFLOW(_a, _b, &_result)) { \
        ObjBigInt* _ba = bigint_from_int64(_a); \
        ObjBigInt* _bb = bigint_from_int64(_b); \
        Value _rv = bigint_mul(_ba, _bb); \
        vm_stack_push_fast(&vm, _rv); \
    } else { \
        PUSH_INT64_RESULT_FAST(_result); \
    } \
} while(0)
