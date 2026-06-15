#include "include/lenolang.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

// 使用 base-2^32 表示，便于位运算
#define BASE 0x100000000ULL  // 2^32
#define BASE_BITS 32
#define BASE_MASK 0xFFFFFFFFULL

// ============================================================================
// BigInt 辅助函数
// ============================================================================

// 去除前导零
static inline void strip_zeros(ObjBigInt* bigint) {
    while (bigint->limb_count > 1 && bigint->limbs[bigint->limb_count - 1] == 0) {
        bigint->limb_count--;
    }
}

// 比较两个 bigint 的绝对值，返回 1(a>b), 0(a==b), -1(a<b)
static inline int compare_abs(const ObjBigInt* a, const ObjBigInt* b) {
    if (a->limb_count != b->limb_count) {
        return a->limb_count > b->limb_count ? 1 : -1;
    }
    for (int i = a->limb_count - 1; i >= 0; i--) {
        if (a->limbs[i] != b->limbs[i]) {
            return a->limbs[i] > b->limbs[i] ? 1 : -1;
        }
    }
    return 0;
}

// ============================================================================
// BigInt 对象操作
// ============================================================================

ObjBigInt* bigint_new(const uint32_t* limbs, int limb_count, int is_negative) {
    ObjBigInt* bigint = (ObjBigInt*)gc_alloc(sizeof(ObjBigInt), OBJ_BIGINT);
    if (!bigint) return NULL;

    size_t limbs_size = limb_count * sizeof(uint32_t);
    bigint->limbs = (uint32_t*)malloc(limbs_size);
    if (!bigint->limbs) {
        free(bigint);
        return NULL;
    }

    memcpy(bigint->limbs, limbs, limbs_size);
    bigint->limb_count = limb_count;
    bigint->is_negative = is_negative;

    strip_zeros(bigint);

    return bigint;
}

ObjBigInt* bigint_from_int64(int64_t value) {
    int is_negative = value < 0;
    uint64_t abs_value = is_negative ? (uint64_t)(-(value + 1)) + 1 : (uint64_t)value;

    uint32_t limbs[2];
    int count = 0;

    if (abs_value > 0) {
        limbs[count++] = (uint32_t)(abs_value & BASE_MASK);
        if (abs_value >> BASE_BITS) {
            limbs[count++] = (uint32_t)(abs_value >> BASE_BITS);
        }
    }

    if (count == 0) {
        limbs[0] = 0;
        count = 1;
    }

    return bigint_new(limbs, count, is_negative);
}

ObjBigInt* bigint_from_uint64(uint64_t value) {
    uint32_t limbs[2];
    int count = 0;

    if (value > 0) {
        limbs[count++] = (uint32_t)(value & BASE_MASK);
        if (value >> BASE_BITS) {
            limbs[count++] = (uint32_t)(value >> BASE_BITS);
        }
    }

    if (count == 0) {
        limbs[0] = 0;
        count = 1;
    }

    return bigint_new(limbs, count, 0);
}

Value val_bigint_from_uint64(uint64_t value) {
    ObjBigInt* bigint = bigint_from_uint64(value);
    if (!bigint) return val_null();
    return val_obj((Object*)bigint);
}

ObjBigInt* bigint_from_string(const char* str) {
    int is_negative = 0;
    if (*str == '-') {
        is_negative = 1;
        str++;
    }

    // 跳过前导零
    while (*str == '0') str++;
    if (*str == '\0') {
        uint32_t limb = 0;
        return bigint_new(&limb, 1, 0);
    }

    // 检查十六进制前缀 (0x 或 0X)
    int is_hex = 0;
    if ((str[0] == 'x' || str[0] == 'X') && str[1] != '\0') {
        is_hex = 1;
        str++;
    }

    // 估算需要的 limb 数量
    int len = strlen(str);
    int max_limbs;
    if (is_hex) {
        max_limbs = (len + 7) / 8 + 2;
    } else {
        max_limbs = (len * 4) / 9 + 2;
    }

    uint32_t* limbs = (uint32_t*)calloc(max_limbs, sizeof(uint32_t));
    if (!limbs) return NULL;

    int limb_count = 1;
    limbs[0] = 0;

    if (is_hex) {
        for (int i = 0; i < len; i++) {
            char c = str[i];
            int digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else continue;

            uint64_t carry = digit;
            for (int j = 0; j < limb_count; j++) {
                uint64_t val = ((uint64_t)limbs[j] << 4) + carry;
                limbs[j] = (uint32_t)(val & BASE_MASK);
                carry = val >> BASE_BITS;
            }
            if (carry > 0) {
                limbs[limb_count++] = (uint32_t)carry;
            }
        }
    } else {
        for (int i = 0; i < len; i++) {
            char c = str[i];
            if (c < '0' || c > '9') continue;

            int digit = c - '0';

            uint64_t carry = digit;
            for (int j = 0; j < limb_count; j++) {
                uint64_t val = (uint64_t)limbs[j] * 10 + carry;
                limbs[j] = (uint32_t)(val & BASE_MASK);
                carry = val >> BASE_BITS;
            }
            if (carry > 0) {
                limbs[limb_count++] = (uint32_t)carry;
            }
        }
    }

    ObjBigInt* result = bigint_new(limbs, limb_count, is_negative);
    free(limbs);
    return result;
}

int bigint_is_int(ObjBigInt* bigint) {
    if (bigint->limb_count > 1) return 0;

    int64_t val = bigint->limbs[0];
    if (bigint->is_negative) val = -val;

    return val >= INT48_MIN && val <= INT48_MAX;
}

int bigint_to_int(ObjBigInt* bigint) {
    return (int)bigint_to_int64(bigint);
}

int64_t bigint_to_int64(ObjBigInt* bigint) {
    // 检查溢出：如果超过 64 位，返回饱和值
    if (bigint->limb_count > 2) {
        // 超过 64 位，返回最大/最小 int64 值
        return bigint->is_negative ? INT64_MIN : INT64_MAX;
    }

    // 对于 2 个 limb 的情况，检查是否超过 int64 范围
    if (bigint->limb_count == 2) {
        uint64_t abs_val = ((uint64_t)bigint->limbs[1] << BASE_BITS) | bigint->limbs[0];
        // int64 最大值为 2^63-1 = 0x7FFFFFFFFFFFFFFF
        // int64 最小值为 -2^63 = 0x8000000000000000（绝对值）
        if (!bigint->is_negative && abs_val > 0x7FFFFFFFFFFFFFFFULL) {
            return INT64_MAX;
        }
        if (bigint->is_negative && abs_val > 0x8000000000000000ULL) {
            return INT64_MIN;
        }
    }

    uint64_t val = 0;
    for (int i = bigint->limb_count - 1; i >= 0; i--) {
        val = (val << BASE_BITS) | bigint->limbs[i];
    }

    if (bigint->is_negative) {
        // 对于负数，使用特殊处理避免溢出
        if (val == 0x8000000000000000ULL) {
            return INT64_MIN;
        }
        return -(int64_t)val;
    }
    return (int64_t)val;
}

Value val_bigint_from_int64(int64_t value) {
    ObjBigInt* bigint = bigint_from_int64(value);
    if (!bigint) return val_null();
    return val_obj((Object*)bigint);
}

Value val_bigint_from_string(const char* str) {
    ObjBigInt* bigint = bigint_from_string(str);
    if (!bigint) return val_null();
    return val_obj((Object*)bigint);
}

Value val_int_safe(int64_t value) {
    if (value >= INT48_MIN && value <= INT48_MAX) {
        return val_int(value);
    }
    return val_bigint_from_int64(value);
}

// ============================================================================
// BigInt 运算
// ============================================================================

Value bigint_add(ObjBigInt* a, ObjBigInt* b) {
    if (a->is_negative == b->is_negative) {
        // 同号相加
        int max_limbs = a->limb_count > b->limb_count ? a->limb_count : b->limb_count;
        uint32_t* result_limbs = (uint32_t*)calloc(max_limbs + 1, sizeof(uint32_t));
        if (!result_limbs) return val_null();

        uint64_t carry = 0;
        for (int i = 0; i < max_limbs; i++) {
            uint64_t sum = carry;
            if (i < a->limb_count) sum += a->limbs[i];
            if (i < b->limb_count) sum += b->limbs[i];
            result_limbs[i] = (uint32_t)(sum & BASE_MASK);
            carry = sum >> BASE_BITS;
        }

        int result_count = max_limbs;
        if (carry > 0) {
            result_limbs[result_count++] = (uint32_t)carry;
        }

        Value result = bigint_compact_to_int(val_bigint_from_limbs(result_limbs, result_count, a->is_negative));
        free(result_limbs);
        return result;
    } else {
        // 异号相加，转为减法
        int cmp = compare_abs(a, b);
        if (cmp == 0) {
            return val_int(0);
        }

        ObjBigInt* larger = cmp > 0 ? a : b;
        ObjBigInt* smaller = cmp > 0 ? b : a;
        int result_negative = larger->is_negative;

        uint32_t* result_limbs = (uint32_t*)calloc(larger->limb_count, sizeof(uint32_t));
        if (!result_limbs) return val_null();

        int64_t borrow = 0;
        for (int i = 0; i < larger->limb_count; i++) {
            int64_t diff = (int64_t)larger->limbs[i] - borrow;
            if (i < smaller->limb_count) diff -= smaller->limbs[i];

            if (diff < 0) {
                diff += BASE;
                borrow = 1;
            } else {
                borrow = 0;
            }
            result_limbs[i] = (uint32_t)diff;
        }

        Value result = bigint_compact_to_int(val_bigint_from_limbs(result_limbs, larger->limb_count, result_negative));
        free(result_limbs);
        return result;
    }
}

Value bigint_sub(ObjBigInt* a, ObjBigInt* b) {
    // a - b = a + (-b)
    ObjBigInt temp_b = *b;
    temp_b.is_negative = !b->is_negative;
    return bigint_add(a, &temp_b);
}

Value bigint_mul(ObjBigInt* a, ObjBigInt* b) {
    int result_is_negative = a->is_negative != b->is_negative;
    int result_count = a->limb_count + b->limb_count;

    uint64_t* temp = (uint64_t*)calloc(result_count, sizeof(uint64_t));
    if (!temp) return val_null();

    for (int i = 0; i < a->limb_count; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < b->limb_count || carry; j++) {
            uint64_t cur = temp[i + j] + (uint64_t)a->limbs[i] * (j < b->limb_count ? b->limbs[j] : 0) + carry;
            temp[i + j] = cur & BASE_MASK;
            carry = cur >> BASE_BITS;
        }
    }

    uint32_t* result_limbs = (uint32_t*)malloc(result_count * sizeof(uint32_t));
    if (!result_limbs) {
        free(temp);
        return val_null();
    }

    for (int i = 0; i < result_count; i++) {
        result_limbs[i] = (uint32_t)temp[i];
    }

    free(temp);

    // 去除前导零
    while (result_count > 1 && result_limbs[result_count - 1] == 0) {
        result_count--;
    }

    Value result = bigint_compact_to_int(val_bigint_from_limbs(result_limbs, result_count, result_is_negative));
    free(result_limbs);
    return result;
}

Value bigint_neg(ObjBigInt* a) {
    if (a->limb_count == 1 && a->limbs[0] == 0) {
        return val_int(0);
    }
    return bigint_compact_to_int(val_bigint_from_limbs(a->limbs, a->limb_count, !a->is_negative));
}

// 辅助函数：复制 ObjBigInt（浅拷贝 limbs 引用）
static inline void bigint_copy_shallow(ObjBigInt* dst, const ObjBigInt* src) {
    dst->header.type = src->header.type;
    dst->limbs = src->limbs;
    dst->limb_count = src->limb_count;
    dst->is_negative = src->is_negative;
}


// 辅助函数：limb 数组除以 uint32_t，返回商，余数保存在 remainder
static uint32_t limbs_div_uint32(const uint32_t* a, int a_count, uint32_t divisor, uint32_t* quotient, uint32_t* remainder) {
    uint64_t rem = 0;
    for (int i = a_count - 1; i >= 0; i--) {
        rem = (rem << BASE_BITS) | a[i];
        if (quotient) {
            quotient[i] = (uint32_t)(rem / divisor);
        }
        rem = rem % divisor;
    }
    if (remainder) {
        *remainder = (uint32_t)rem;
    }
    return (uint32_t)rem;
}

// 辅助函数：复制 ObjBigInt
static ObjBigInt* bigint_copy(const ObjBigInt* src) {
    return bigint_new(src->limbs, src->limb_count, src->is_negative);
}

// 长除法实现：计算 a % b，返回余数
// 使用可靠的二进制长除法算法
static Value bigint_mod_internal(ObjBigInt* a, ObjBigInt* b) {
    // 如果 |a| < |b|，余数就是 a
    if (compare_abs(a, b) < 0) {
        return val_obj((Object*)bigint_new(a->limbs, a->limb_count, a->is_negative));
    }

    // 如果 b 是 1 或 -1，余数为 0
    if (b->limb_count == 1 && b->limbs[0] == 1) {
        return val_int(0);
    }

    // 对于小除数（单个 limb），使用快速路径
    if (b->limb_count == 1) {
        uint32_t divisor = b->limbs[0];
        uint32_t remainder;
        limbs_div_uint32(a->limbs, a->limb_count, divisor, NULL, &remainder);
        
        int is_zero = (remainder == 0);
        int result_negative = a->is_negative && !is_zero;
        
        uint32_t rem_arr[1] = {remainder};
        return val_bigint_from_limbs(rem_arr, 1, result_negative);
    }

    // 多位数除法：使用移位减法
    // 计算需要多少位来左移 b
    int shift_bits = (a->limb_count - b->limb_count) * BASE_BITS;
    
    // 计算 b 最高位的有效位数
    uint32_t b_high = b->limbs[b->limb_count - 1];
    int b_high_bits = BASE_BITS;
    while (b_high_bits > 0 && ((b_high >> (b_high_bits - 1)) & 1) == 0) {
        b_high_bits--;
    }
    
    // 计算 a 最高位的有效位数
    uint32_t a_high = a->limbs[a->limb_count - 1];
    int a_high_bits = BASE_BITS;
    while (a_high_bits > 0 && ((a_high >> (a_high_bits - 1)) & 1) == 0) {
        a_high_bits--;
    }
    
    shift_bits += (a_high_bits - b_high_bits);
    if (shift_bits < 0) shift_bits = 0;

    // 复制余数
    ObjBigInt* rem = bigint_copy(a);
    if (!rem) return val_null();
    
    // 从高位到低位逐步减
    for (int s = shift_bits; s >= 0; s--) {
        // 将 b 左移 s 位
        Value shifted = bigint_shl(b, s);
        if (!val_is_bigint(shifted)) {
            return val_null();
        }
        ObjBigInt* shifted_b = val_as_bigint(shifted);
        
        // 当余数 >= shifted_b 时，减去 shifted_b
        while (compare_abs(rem, shifted_b) >= 0) {
            Value new_rem = bigint_sub(rem, shifted_b);
            if (!val_is_bigint(new_rem)) {
                return val_null();
            }
            rem = val_as_bigint(new_rem);
            // 需要复制因为 bigint_sub 可能返回新对象
            rem = bigint_copy(rem);
        }
    }

    // 余数的符号与被除数相同（如果余数不为 0）
    int is_zero = (rem->limb_count == 1 && rem->limbs[0] == 0);
    int result_negative = a->is_negative && !is_zero;
    rem->is_negative = result_negative;

    Value result = val_obj((Object*)rem);
    return result;
}

Value bigint_mod(ObjBigInt* a, ObjBigInt* b) {
    if (bigint_is_zero(b)) {
        return val_null();
    }

    // 如果 |a| < |b|，余数就是 a
    if (compare_abs(a, b) < 0) {
        return val_obj((Object*)bigint_new(a->limbs, a->limb_count, a->is_negative));
    }

    // 检查是否可以用 int64 计算（数值必须在 int64 范围内）
    int can_use_int64 = 1;
    if (a->limb_count > 2 || b->limb_count > 2) {
        can_use_int64 = 0;
    } else if (a->limb_count == 2) {
        uint64_t abs_a = ((uint64_t)a->limbs[1] << BASE_BITS) | a->limbs[0];
        if (!a->is_negative && abs_a > 0x7FFFFFFFFFFFFFFFULL) can_use_int64 = 0;
        if (a->is_negative && abs_a > 0x8000000000000000ULL) can_use_int64 = 0;
    }
    
    if (can_use_int64) {
        int64_t a_val = bigint_to_int64(a);
        int64_t b_val = bigint_to_int64(b);
        int64_t result = a_val % b_val;

        // 调整余数符号（Python/JavaScript 风格：结果与除数同号）
        if (result != 0 && (result < 0) != (b_val < 0)) {
            result += b_val;
        }

        return val_int_safe(result);
    }

    // 对于超大数，使用长除法
    return bigint_mod_internal(a, b);
}

// 长除法实现：计算 a / b，返回商
static Value bigint_div_internal(ObjBigInt* a, ObjBigInt* b) {
    // 处理除零
    if (bigint_is_zero(b)) {
        return val_null();
    }
    
    // 如果 |a| < |b|，商为 0
    if (compare_abs(a, b) < 0) {
        return val_int(0);
    }
    
    // 如果 b 是 1 或 -1，商就是 a（符号可能不同）
    if (b->limb_count == 1 && b->limbs[0] == 1) {
        int result_negative = a->is_negative != b->is_negative;
        return val_obj((Object*)bigint_new(a->limbs, a->limb_count, result_negative));
    }
    
    // 对于小除数（单个 limb），使用快速路径
    if (b->limb_count == 1) {
        uint32_t divisor = b->limbs[0];
        uint32_t* quotient_limbs = (uint32_t*)malloc(a->limb_count * sizeof(uint32_t));
        if (!quotient_limbs) return val_null();
        
        uint32_t remainder;
        limbs_div_uint32(a->limbs, a->limb_count, divisor, quotient_limbs, &remainder);
        
        // 计算商的有效位数
        int quotient_count = a->limb_count;
        while (quotient_count > 1 && quotient_limbs[quotient_count - 1] == 0) {
            quotient_count--;
        }
        
        int result_negative = a->is_negative != b->is_negative;
        
        // 如果商可以用 int 表示，返回 int
        if (quotient_count == 1) {
            int64_t result_val = quotient_limbs[0];
            if (result_negative) result_val = -result_val;
            free(quotient_limbs);
            return val_int_safe(result_val);
        }
        
        Value result = val_bigint_from_limbs(quotient_limbs, quotient_count, result_negative);
        free(quotient_limbs);
        return result;
    }
    
    // 多位数除法：使用移位减法（长除法）
    int shift_bits = (a->limb_count - b->limb_count) * BASE_BITS;
    
    uint32_t b_high = b->limbs[b->limb_count - 1];
    int b_high_bits = BASE_BITS;
    while (b_high_bits > 0 && ((b_high >> (b_high_bits - 1)) & 1) == 0) {
        b_high_bits--;
    }
    
    uint32_t a_high = a->limbs[a->limb_count - 1];
    int a_high_bits = BASE_BITS;
    while (a_high_bits > 0 && ((a_high >> (a_high_bits - 1)) & 1) == 0) {
        a_high_bits--;
    }
    
    shift_bits += (a_high_bits - b_high_bits);
    if (shift_bits < 0) shift_bits = 0;
    
    // 初始化余数为被除数
    ObjBigInt* rem = bigint_copy(a);
    if (!rem) return val_null();
    
    // 商初始化为 0
    uint32_t* quotient_limbs = (uint32_t*)calloc(a->limb_count + 1, sizeof(uint32_t));
    if (!quotient_limbs) {
        free(rem);
        return val_null();
    }
    int quotient_count = 1;
    
    // 从高位到低位逐步减
    for (int s = shift_bits; s >= 0; s--) {
        Value shifted = bigint_shl(b, s);
        if (!val_is_bigint(shifted)) {
            free(quotient_limbs);
            return val_null();
        }
        ObjBigInt* shifted_b = val_as_bigint(shifted);
        
        // 当余数 >= shifted_b 时，减去 shifted_b，商的对应位加 1
        while (compare_abs(rem, shifted_b) >= 0) {
            Value new_rem = bigint_sub(rem, shifted_b);
            if (!val_is_bigint(new_rem)) {
                free(quotient_limbs);
                return val_null();
            }
            rem = val_as_bigint(new_rem);
            rem = bigint_copy(rem);
            
            // 商的对应位加 1
            int limb_idx = s / BASE_BITS;
            int bit_idx = s % BASE_BITS;
            quotient_limbs[limb_idx] |= (1u << bit_idx);
            if (limb_idx >= quotient_count) {
                quotient_count = limb_idx + 1;
            }
        }
    }
    
    // 计算商的有效位数
    while (quotient_count > 1 && quotient_limbs[quotient_count - 1] == 0) {
        quotient_count--;
    }
    
    int result_negative = a->is_negative != b->is_negative;
    
    // 如果商可以用 int 表示，返回 int
    if (quotient_count == 1 && quotient_limbs[0] <= INT32_MAX) {
        int64_t result_val = quotient_limbs[0];
        if (result_negative) result_val = -result_val;
        free(quotient_limbs);
        return val_int_safe(result_val);
    }
    
    Value result = val_bigint_from_limbs(quotient_limbs, quotient_count, result_negative);
    free(quotient_limbs);
    return result;
}

Value bigint_div(ObjBigInt* a, ObjBigInt* b) {
    if (bigint_is_zero(b)) {
        return val_null();
    }
    
    // 如果 |a| < |b|，商为 0
    if (compare_abs(a, b) < 0) {
        return val_int(0);
    }
    
    // 检查是否可以用 int64 计算（数值必须在 int64 范围内）
    int can_use_int64 = 1;
    if (a->limb_count > 2 || b->limb_count > 2) {
        can_use_int64 = 0;
    } else if (a->limb_count == 2) {
        uint64_t abs_a = ((uint64_t)a->limbs[1] << BASE_BITS) | a->limbs[0];
        if (!a->is_negative && abs_a > 0x7FFFFFFFFFFFFFFFULL) can_use_int64 = 0;
        if (a->is_negative && abs_a > 0x8000000000000000ULL) can_use_int64 = 0;
    }
    
    if (can_use_int64) {
        int64_t a_val = bigint_to_int64(a);
        int64_t b_val = bigint_to_int64(b);
        int64_t result = a_val / b_val;
        return val_int_safe(result);
    }
    
    // 对于超大数，使用长除法
    return bigint_div_internal(a, b);
}

int bigint_compare(ObjBigInt* a, ObjBigInt* b) {
    if (a->is_negative != b->is_negative) {
        return a->is_negative ? -1 : 1;
    }

    int cmp = compare_abs(a, b);
    return a->is_negative ? -cmp : cmp;
}

inline int bigint_is_zero(ObjBigInt* bigint) {
    return bigint->limb_count == 1 && bigint->limbs[0] == 0;
}

char* bigint_to_string(ObjBigInt* bigint) {
    if (bigint->limb_count == 1 && bigint->limbs[0] == 0) {
        char* str = (char*)malloc(2);
        if (str) {
            str[0] = '0';
            str[1] = '\0';
        }
        return str;
    }

    // 复制 limbs 用于计算（从高位到低位）
    uint32_t* temp = (uint32_t*)malloc(bigint->limb_count * sizeof(uint32_t));
    if (!temp) return NULL;
    memcpy(temp, bigint->limbs, bigint->limb_count * sizeof(uint32_t));
    int temp_count = bigint->limb_count;

    // 估算最大十进制位数：每个 32-bit limb 最多 10 位十进制数
    // 额外加 2 用于符号和结尾符
    int max_digits = bigint->limb_count * 10 + 2;
    char* digits = (char*)malloc(max_digits);
    if (!digits) {
        free(temp);
        return NULL;
    }

    // 从低位开始生成十进制数字
    int digit_count = 0;

    while (temp_count > 0 && !(temp_count == 1 && temp[0] == 0)) {
        uint64_t remainder = 0;

        // 除以 10
        for (int i = temp_count - 1; i >= 0; i--) {
            uint64_t val = (remainder << BASE_BITS) | temp[i];
            temp[i] = (uint32_t)(val / 10);
            remainder = val % 10;
        }

        digits[digit_count++] = '0' + (char)remainder;

        // 移除前导零
        while (temp_count > 0 && temp[temp_count - 1] == 0) {
            temp_count--;
        }
    }

    free(temp);

    // 分配最终字符串（包含符号和结尾符）
    int str_len = digit_count + (bigint->is_negative ? 1 : 0) + 1;
    char* str = (char*)malloc(str_len);
    if (!str) {
        free(digits);
        return NULL;
    }

    // 反转数字并生成字符串
    int pos = 0;
    if (bigint->is_negative) {
        str[pos++] = '-';
    }

    for (int i = digit_count - 1; i >= 0; i--) {
        str[pos++] = digits[i];
    }
    str[pos] = '\0';

    free(digits);
    return str;
}

Value val_bigint_from_limbs(const uint32_t* limbs, int limb_count, int is_negative) {
    // 如果值在 int48 范围内，直接返回 int（避免不必要的大数分配和类型不兼容问题）
    if (limb_count == 0) {
        return val_int(0);
    }
    if (limb_count <= 2) {
        // 对于 1-2 个 limb，检查是否在 int48 范围内
        uint64_t abs_val = limbs[0];
        if (limb_count == 2) {
            abs_val |= ((uint64_t)limbs[1] << 32);
        }
        if (!is_negative && abs_val <= (uint64_t)INT48_MAX) {
            return val_int((int64_t)abs_val);
        }
        // INT48_MIN 绝对值是 2^47 = 0x800000000000
        if (is_negative && abs_val <= 0x800000000000ULL) {
            return val_int(-(int64_t)abs_val);
        }
    }
    ObjBigInt* bigint = bigint_new(limbs, limb_count, is_negative);
    if (!bigint) return val_null();
    return val_obj((Object*)bigint);
}

// ============================================================================
// BigInt 位运算（基于 base-2^32，直接操作 limb）
// ============================================================================

// 辅助函数：获取第 i 个 limb 的值，考虑符号扩展
static inline uint32_t get_limb_with_sign_ext(const ObjBigInt* bigint, int i) {
    if (i < bigint->limb_count) {
        return bigint->limbs[i];
    }
    // 符号扩展：正数补0，负数补1（BASE_MASK）
    return bigint->is_negative ? BASE_MASK : 0;
}

// 辅助函数：将负数转换为补码表示
// 注意：正数返回原值，负数返回补码（取反加1）
static inline uint32_t to_twos_complement(uint32_t val, int is_negative) {
    if (!is_negative) return val;
    return ~val + 1;  // 负数转补码
}

// 辅助函数：将补码转换回原码
static inline uint32_t from_twos_complement(uint32_t val, int is_negative) {
    if (!is_negative) return val;
    return ~val + 1;  // 补码转回原码
}

// 辅助函数：将多 limb 负数转换为补码表示（在临时数组中）
// 返回值：1 表示成功，0 表示失败
static int bigint_to_twos_complement_array(const ObjBigInt* bigint, uint32_t* out, int count) {
    if (!bigint->is_negative) {
        // 正数：直接复制
        for (int i = 0; i < count; i++) {
            out[i] = get_limb_with_sign_ext(bigint, i);
        }
        return 1;
    }

    // 负数：取反加 1
    uint64_t carry = 1;
    for (int i = 0; i < count; i++) {
        uint32_t val = get_limb_with_sign_ext(bigint, i);
        uint64_t sum = (uint64_t)(~val) + carry;
        out[i] = (uint32_t)(sum & BASE_MASK);
        carry = sum >> BASE_BITS;
    }
    return 1;
}

// 辅助函数：将补码数组转换回原码（假设输入是补码形式）
static int bigint_from_twos_complement_array(uint32_t* arr, int count, int is_negative) {
    if (!is_negative) return 1;  // 正数无需转换

    // 负数补码转原码：减 1 后取反，或再次取反加 1
    uint64_t carry = 1;
    for (int i = 0; i < count; i++) {
        uint64_t val = (uint64_t)(~arr[i]) + carry;
        arr[i] = (uint32_t)(val & BASE_MASK);
        carry = val >> BASE_BITS;
    }
    return 1;
}

Value bigint_and(ObjBigInt* a, ObjBigInt* b) {
    int max_limbs = (a->limb_count > b->limb_count) ? a->limb_count : b->limb_count;

    uint32_t* a_complement = (uint32_t*)calloc(max_limbs, sizeof(uint32_t));
    uint32_t* b_complement = (uint32_t*)calloc(max_limbs, sizeof(uint32_t));
    uint32_t* result_limbs = (uint32_t*)calloc(max_limbs, sizeof(uint32_t));

    if (!a_complement || !b_complement || !result_limbs) {
        free(a_complement);
        free(b_complement);
        free(result_limbs);
        return val_null();
    }

    // 转换为补码表示（正确处理多 limb 借位）
    bigint_to_twos_complement_array(a, a_complement, max_limbs);
    bigint_to_twos_complement_array(b, b_complement, max_limbs);

    // 执行位与操作
    for (int i = 0; i < max_limbs; i++) {
        result_limbs[i] = a_complement[i] & b_complement[i];
    }

    // 确定结果符号（看最高位）
    int result_negative = (result_limbs[max_limbs - 1] >> (BASE_BITS - 1)) & 1;

    // 如果结果是负数，从补码转回原码
    if (result_negative) {
        bigint_from_twos_complement_array(result_limbs, max_limbs, 1);
    }

    // 去除前导零
    int result_count = max_limbs;
    while (result_count > 1 && result_limbs[result_count - 1] == 0) {
        result_count--;
    }

    ObjBigInt* result = bigint_new(result_limbs, result_count, result_negative);
    free(a_complement);
    free(b_complement);
    free(result_limbs);

    if (!result) return val_null();
    return bigint_compact_to_int(val_obj((Object*)result));
}

Value bigint_or(ObjBigInt* a, ObjBigInt* b) {
    int max_limbs = (a->limb_count > b->limb_count) ? a->limb_count : b->limb_count;

    uint32_t* a_complement = (uint32_t*)calloc(max_limbs, sizeof(uint32_t));
    uint32_t* b_complement = (uint32_t*)calloc(max_limbs, sizeof(uint32_t));
    uint32_t* result_limbs = (uint32_t*)calloc(max_limbs, sizeof(uint32_t));

    if (!a_complement || !b_complement || !result_limbs) {
        free(a_complement);
        free(b_complement);
        free(result_limbs);
        return val_null();
    }

    // 转换为补码表示（正确处理多 limb 借位）
    bigint_to_twos_complement_array(a, a_complement, max_limbs);
    bigint_to_twos_complement_array(b, b_complement, max_limbs);

    // 执行位或操作
    for (int i = 0; i < max_limbs; i++) {
        result_limbs[i] = a_complement[i] | b_complement[i];
    }

    // 确定结果符号（看最高位）
    int result_negative = (result_limbs[max_limbs - 1] >> (BASE_BITS - 1)) & 1;

    // 如果结果是负数，从补码转回原码
    if (result_negative) {
        bigint_from_twos_complement_array(result_limbs, max_limbs, 1);
    }

    // 去除前导零
    int result_count = max_limbs;
    while (result_count > 1 && result_limbs[result_count - 1] == 0) {
        result_count--;
    }

    ObjBigInt* result = bigint_new(result_limbs, result_count, result_negative);
    free(a_complement);
    free(b_complement);
    free(result_limbs);

    if (!result) return val_null();
    return bigint_compact_to_int(val_obj((Object*)result));
}

Value bigint_xor(ObjBigInt* a, ObjBigInt* b) {
    int max_limbs = (a->limb_count > b->limb_count) ? a->limb_count : b->limb_count;

    uint32_t* a_complement = (uint32_t*)calloc(max_limbs, sizeof(uint32_t));
    uint32_t* b_complement = (uint32_t*)calloc(max_limbs, sizeof(uint32_t));
    uint32_t* result_limbs = (uint32_t*)calloc(max_limbs, sizeof(uint32_t));

    if (!a_complement || !b_complement || !result_limbs) {
        free(a_complement);
        free(b_complement);
        free(result_limbs);
        return val_null();
    }

    // 转换为补码表示（正确处理多 limb 借位）
    bigint_to_twos_complement_array(a, a_complement, max_limbs);
    bigint_to_twos_complement_array(b, b_complement, max_limbs);

    // 执行位异或操作
    for (int i = 0; i < max_limbs; i++) {
        result_limbs[i] = a_complement[i] ^ b_complement[i];
    }

    // 确定结果符号（看最高位）
    int result_negative = (result_limbs[max_limbs - 1] >> (BASE_BITS - 1)) & 1;

    // 如果结果是负数，从补码转回原码
    if (result_negative) {
        bigint_from_twos_complement_array(result_limbs, max_limbs, 1);
    }

    // 去除前导零
    int result_count = max_limbs;
    while (result_count > 1 && result_limbs[result_count - 1] == 0) {
        result_count--;
    }

    ObjBigInt* result = bigint_new(result_limbs, result_count, result_negative);
    free(a_complement);
    free(b_complement);
    free(result_limbs);

    if (!result) return val_null();
    return bigint_compact_to_int(val_obj((Object*)result));
}

Value bigint_not(ObjBigInt* a) {
    // ~a = -a - 1
    Value negated = bigint_neg(a);

    if (val_is_bigint(negated)) {
        ObjBigInt* neg = val_as_bigint(negated);
        ObjBigInt* one = bigint_from_int64(1);
        Value result = bigint_sub(neg, one);
        return result;
    }

    if (val_is_int(negated)) {
        return val_int(val_as_int(negated) - 1);
    }

    return val_null();
}

Value bigint_shl(ObjBigInt* a, int shift) {
    if (shift < 0) {
        return bigint_shr(a, -shift);
    }
    if (shift == 0) {
        return val_obj((Object*)bigint_new(a->limbs, a->limb_count, a->is_negative));
    }

    int limb_shift = shift / BASE_BITS;
    int bit_shift = shift % BASE_BITS;

    // 计算结果需要的 limb 数量
    int result_limbs = a->limb_count + limb_shift;
    if (bit_shift > 0) {
        uint32_t high_bits = a->limbs[a->limb_count - 1] >> (BASE_BITS - bit_shift);
        if (high_bits != 0) {
            result_limbs++;
        }
    }

    uint32_t* result_arr = (uint32_t*)calloc(result_limbs, sizeof(uint32_t));
    if (!result_arr) return val_null();

    // 左移
    for (int i = a->limb_count - 1; i >= 0; i--) {
        uint64_t val = ((uint64_t)a->limbs[i] << bit_shift);
        result_arr[i + limb_shift] |= (uint32_t)(val & BASE_MASK);
        if (bit_shift > 0 && i + limb_shift + 1 < result_limbs) {
            result_arr[i + limb_shift + 1] |= (uint32_t)(val >> BASE_BITS);
        }
    }

    ObjBigInt* result = bigint_new(result_arr, result_limbs, a->is_negative);
    free(result_arr);

    if (!result) return val_null();
    return bigint_compact_to_int(val_obj((Object*)result));
}

Value bigint_shr(ObjBigInt* a, int shift) {
    if (shift < 0) {
        return bigint_shl(a, -shift);
    }
    if (shift == 0) {
        return val_obj((Object*)bigint_new(a->limbs, a->limb_count, a->is_negative));
    }

    int limb_shift = shift / BASE_BITS;
    int bit_shift = shift % BASE_BITS;

    // 计算结果需要的 limb 数量
    int result_limbs = a->limb_count - limb_shift;

    // 对于负数算术右移，需要特殊处理
    // 负数右移时，高位补 1，相当于向负无穷取整
    if (a->is_negative) {
        // 算术右移：负数右移结果仍为负数，高位补 1
        // 实现方式：~((~a) >> shift)  对于负数
        // 或者使用公式：-( (-a + (1<<shift) - 1) >> shift )

        // 对于大数负数，我们需要正确处理符号扩展
        if (result_limbs <= 0) {
            // 全部移出，负数算术右移结果为 -1（全 1）
            return val_int(-1);
        }

        uint32_t* result_arr = (uint32_t*)calloc(result_limbs, sizeof(uint32_t));
        if (!result_arr) return val_null();

        // 右移（逻辑右移）
        for (int i = limb_shift; i < a->limb_count; i++) {
            uint64_t val = ((uint64_t)a->limbs[i] >> bit_shift);
            result_arr[i - limb_shift] |= (uint32_t)val;
            if (bit_shift > 0 && i > limb_shift) {
                result_arr[i - limb_shift - 1] |= (uint32_t)(a->limbs[i] << (BASE_BITS - bit_shift));
            }
        }

        // 对于负数，需要向上取整（向负无穷）
        // 如果移出的位中有 1，结果需要加 1（因为 C 语言是向零取整）
        int need_round_up = 0;
        if (bit_shift > 0) {
            // 检查移出的低位中是否有 1
            uint32_t low_mask = (1U << bit_shift) - 1;
            if ((a->limbs[limb_shift] & low_mask) != 0) {
                need_round_up = 1;
            }
        }
        // 检查被完全移出的 limb 中是否有 1
        for (int i = 0; i < limb_shift && !need_round_up; i++) {
            if (a->limbs[i] != 0) {
                need_round_up = 1;
                break;
            }
        }

        ObjBigInt* result = bigint_new(result_arr, result_limbs, 0);
        free(result_arr);

        if (!result) return val_null();

        // 对结果加 1（取反加 1 的方式实现负数）
        // 实际上我们需要的是 ~result + 1 的负数形式
        // 更简单的方法：result = -(result + 1) 如果 need_round_up
        if (need_round_up) {
            // 转换为正数加 1 后再转负
            Value one = val_bigint_from_int64(1);
            Value added = bigint_add(result, val_as_bigint(one));
            return bigint_neg(val_as_bigint(added));
        }

        return bigint_neg(result);
    }

    // 正数右移（逻辑右移）
    if (result_limbs <= 0) {
        return val_int(0);
    }

    uint32_t* result_arr = (uint32_t*)calloc(result_limbs, sizeof(uint32_t));
    if (!result_arr) return val_null();

    // 右移
    for (int i = limb_shift; i < a->limb_count; i++) {
        uint64_t val = ((uint64_t)a->limbs[i] >> bit_shift);
        result_arr[i - limb_shift] |= (uint32_t)val;
        if (bit_shift > 0 && i > limb_shift) {
            result_arr[i - limb_shift - 1] |= (uint32_t)(a->limbs[i] << (BASE_BITS - bit_shift));
        }
    }

    ObjBigInt* result = bigint_new(result_arr, result_limbs, 0);
    free(result_arr);

    if (!result) return val_null();
    return val_obj((Object*)result);
}

// BigInt 逻辑右移（无符号右移，高位补 0）
Value bigint_ushr(ObjBigInt* a, int shift) {
    if (shift < 0) {
        return bigint_shl(a, -shift);
    }
    if (shift == 0) {
        return val_obj((Object*)bigint_new(a->limbs, a->limb_count, a->is_negative));
    }

    int limb_shift = shift / BASE_BITS;
    int bit_shift = shift % BASE_BITS;

    // 计算结果需要的 limb 数量
    int result_limbs = a->limb_count - limb_shift;

    // 逻辑右移：无论正负，高位都补 0
    if (result_limbs <= 0) {
        return val_int(0);
    }

    uint32_t* result_arr = (uint32_t*)calloc(result_limbs, sizeof(uint32_t));
    if (!result_arr) return val_null();

    // 右移（逻辑右移，高位补 0）
    for (int i = limb_shift; i < a->limb_count; i++) {
        uint64_t val = ((uint64_t)a->limbs[i] >> bit_shift);
        result_arr[i - limb_shift] |= (uint32_t)val;
        if (bit_shift > 0 && i > limb_shift) {
            result_arr[i - limb_shift - 1] |= (uint32_t)(a->limbs[i] << (BASE_BITS - bit_shift));
        }
    }

    ObjBigInt* result = bigint_new(result_arr, result_limbs, 0);
    free(result_arr);

    if (!result) return val_null();
    return val_obj((Object*)result);
}
