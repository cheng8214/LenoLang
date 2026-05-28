#define PUSH_INT64_RESULT(result) do { \
    int64_t _r = (result); \
    if (_r < INT32_MIN || _r > INT32_MAX) { \
        vm_stack_push(&vm, val_bigint_from_int64(_r)); \
    } else { \
        vm_stack_push(&vm, val_int((int)_r)); \
    } \
} while(0)

// 快速版本：使用 vm_stack_push_fast，用于类型特化指令
#define PUSH_INT64_RESULT_FAST(result) do { \
    int64_t _r = (result); \
    if (_r < INT32_MIN || _r > INT32_MAX) { \
        vm_stack_push_fast(&vm, val_bigint_from_int64(_r)); \
    } else { \
        vm_stack_push_fast(&vm, val_int((int)_r)); \
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
        vm_stack_push(&vm, _result); \
    } else if (_sum < INT32_MIN || _sum > INT32_MAX) { \
        vm_stack_push(&vm, val_bigint_from_int64(_sum)); \
    } else { \
        vm_stack_push(&vm, val_int((int)_sum)); \
    } \
} while(0)

// 快速版本：使用 vm_stack_push_fast，用于类型特化指令
#define PUSH_SAFE_INT64_ADD_FAST(a, b) do { \
    int64_t _a = (a), _b = (b); \
    int64_t _sum = _a + _b; \
    if ((_b > 0 && _a > INT64_MAX - _b) || \
        (_b < 0 && _a < INT64_MIN - _b)) { \
        ObjBigInt* _ba = bigint_from_int64(_a); \
        ObjBigInt* _bb = bigint_from_int64(_b); \
        Value _result = bigint_add(_ba, _bb); \
        vm_stack_push_fast(&vm, _result); \
    } else if (_sum < INT32_MIN || _sum > INT32_MAX) { \
        vm_stack_push_fast(&vm, val_bigint_from_int64(_sum)); \
    } else { \
        vm_stack_push_fast(&vm, val_int((int)_sum)); \
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
        vm_stack_push(&vm, _result); \
    } else if (_diff < INT32_MIN || _diff > INT32_MAX) { \
        vm_stack_push(&vm, val_bigint_from_int64(_diff)); \
    } else { \
        vm_stack_push(&vm, val_int((int)_diff)); \
    } \
} while(0)

// 快速版本：使用 vm_stack_push_fast，用于类型特化指令
#define PUSH_SAFE_INT64_SUB_FAST(a, b) do { \
    int64_t _a = (a), _b = (b); \
    int64_t _diff = _a - _b; \
    if ((_b < 0 && _a > INT64_MAX + _b) || \
        (_b > 0 && _a < INT64_MIN + _b)) { \
        ObjBigInt* _ba = bigint_from_int64(_a); \
        ObjBigInt* _bb = bigint_from_int64(_b); \
        Value _result = bigint_sub(_ba, _bb); \
        vm_stack_push_fast(&vm, _result); \
    } else if (_diff < INT32_MIN || _diff > INT32_MAX) { \
        vm_stack_push_fast(&vm, val_bigint_from_int64(_diff)); \
    } else { \
        vm_stack_push_fast(&vm, val_int((int)_diff)); \
    } \
} while(0)

#define PUSH_SAFE_INT64_MUL(a, b) do { \
    int64_t _a = (a), _b = (b); \
    if (_a == 0 || _b == 0) { \
        vm_stack_push(&vm, val_int(0)); \
    } else { \
        int64_t _result = _a * _b; \
        if (_result / _a != _b) { \
            ObjBigInt* _ba = bigint_from_int64(_a); \
            ObjBigInt* _bb = bigint_from_int64(_b); \
            Value _rv = bigint_mul(_ba, _bb); \
            vm_stack_push(&vm, _rv); \
        } else { \
            PUSH_INT64_RESULT(_result); \
        } \
    } \
} while(0)

// 快速版本：使用 vm_stack_push_fast，用于类型特化指令
#define PUSH_SAFE_INT64_MUL_FAST(a, b) do { \
    int64_t _a = (a), _b = (b); \
    if (_a == 0 || _b == 0) { \
        vm_stack_push_fast(&vm, val_int(0)); \
    } else { \
        int64_t _result = _a * _b; \
        if (_result / _a != _b) { \
            ObjBigInt* _ba = bigint_from_int64(_a); \
            ObjBigInt* _bb = bigint_from_int64(_b); \
            Value _rv = bigint_mul(_ba, _bb); \
            vm_stack_push_fast(&vm, _rv); \
        } else { \
            PUSH_INT64_RESULT_FAST(_result); \
        } \
    } \
} while(0)
