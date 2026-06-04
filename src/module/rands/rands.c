#include "include/native.h"
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ============================================================================
// PCG32 伪随机数生成器（高质量，支持 seed）
// PCG: Permuted Congruential Generator
// 比 rand() 质量高得多，周期长达 2^64，支持自定义种子
// ============================================================================

typedef struct {
    uint64_t state;
    uint64_t inc;
} pcg32_random_t;

static pcg32_random_t pcg32_global = { 0, 0 };
static int pcg32_initialized = 0;

// PCG32 初始化
static void pcg32_srandom_r(pcg32_random_t* rng, uint64_t initstate, uint64_t initseq) {
    rng->state = 0U;
    rng->inc = (initseq << 1u) | 1u;
    pcg32_random_t* p = rng;  // 避免未使用警告
    (void)p;
    // 运行一次以初始化状态
    (void)((*rng).state * 6364136223846793005ULL + rng->inc);
    rng->state += initstate;
    (void)((*rng).state * 6364136223846793005ULL + rng->inc);
}

// PCG32 生成随机数
static uint32_t pcg32_random_r(pcg32_random_t* rng) {
    uint64_t oldstate = rng->state;
    rng->state = oldstate * 6364136223846793005ULL + rng->inc;
    uint32_t xorshifted = (uint32_t)(((oldstate >> 18u) ^ oldstate) >> 27u);
    uint32_t rot = (uint32_t)(oldstate >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}

// PCG32 生成指定范围内的随机数（无模偏差）
static uint32_t pcg32_boundedrand_r(pcg32_random_t* rng, uint32_t bound) {
    // 使用拒绝采样避免模偏差
    uint32_t threshold = (-bound) % bound;
    while (1) {
        uint32_t r = pcg32_random_r(rng);
        if (r >= threshold) {
            return r % bound;
        }
    }
}

// 初始化全局 PCG32
static void init_pcg32(void) {
    if (!pcg32_initialized) {
        pcg32_srandom_r(&pcg32_global, (uint64_t)time(NULL), (uint64_t)clock());
        pcg32_initialized = 1;
    }
}

// 设置全局种子
static void set_pcg32_seed(uint64_t seed) {
    pcg32_srandom_r(&pcg32_global, seed, 1);  // 使用固定的序列号
    pcg32_initialized = 1;
}

// 生成随机 uint32_t
static uint32_t random_uint32(void) {
    init_pcg32();
    return pcg32_random_r(&pcg32_global);
}

// 生成随机 double [0, 1)（53 位精度）
static double random_double(void) {
    // 组合两个 32 位随机数生成 53 位精度
    uint64_t val = ((uint64_t)random_uint32() << 32) | random_uint32();
    return (double)(val >> (64 - 53)) / (double)(1ULL << 53);
}

// 生成指定范围内的随机整数 [0, bound)，无模偏差
static uint32_t random_bounded(uint32_t bound) {
    if (bound <= 1) return 0;
    init_pcg32();
    return pcg32_boundedrand_r(&pcg32_global, bound);
}

// ============================================================================
// Fisher-Yates 洗牌算法
// ============================================================================

static void fisher_yates_shuffle(Value* arr, int count) {
    for (int i = count - 1; i > 0; i--) {
        int j = (int)random_bounded((uint32_t)(i + 1));
        Value temp = arr[i];
        arr[i] = arr[j];
        arr[j] = temp;
    }
}

// ============================================================================
// 核心方法实现
// ============================================================================

// rands.om() - 生成0到1之间的随机小数
static Value rands_om(int argc, Value* args) {
    (void)argc;
    (void)args;
    return val_float(random_double());
}

// rands.ints(min, max) - 生成指定范围内的随机整数 [min, max]
// 当范围超过 INT_MAX 时，自动返回 BigInt
static Value rands_ints(int argc, Value* args) {
    (void)argc;
    
    // 提取 int64 值以检查范围
    int64_t min_val = val_is_bigint(args[0]) ? bigint_to_int64(val_as_bigint(args[0])) : (int64_t)val_as_num(args[0]);
    int64_t max_val = val_is_bigint(args[1]) ? bigint_to_int64(val_as_bigint(args[1])) : (int64_t)val_as_num(args[1]);
    
    if (min_val > max_val) {
        int64_t temp = min_val;
        min_val = max_val;
        max_val = temp;
    }
    
    int64_t range = max_val - min_val + 1;
    
    // 检查范围是否需要 BigInt
    if (range > INT_MAX || min_val < INT_MIN || max_val > INT_MAX) {
        // 确保 PCG32 已初始化
        init_pcg32();
        
        // 需要 BigInt — 使用 gc_push_root 保护所有中间对象，
        // 因为 bigint_from_int64/bigint_new/bigint_mod/bigint_add 内部都会调用 gc_alloc，
        // 如果 malloc 失败会触发 gc_major_collect()，未 root 的对象会被回收。
        Value min_big_val = val_obj((Object*)bigint_from_int64(min_val));
        gc_push_root(&min_big_val);
        
        Value range_big_val = val_obj((Object*)bigint_from_int64(range));
        gc_push_root(&range_big_val);
        
        // 生成 BigInt 随机数：min + random[0, range)
        // 简化：使用多次随机生成足够位数的 BigInt
        int bits_needed = 0;
        int64_t temp_range = range - 1;
        while (temp_range > 0) {
            bits_needed += 32;
            temp_range >>= 32;
        }
        
        int limbs_needed = (bits_needed + 31) / 32;
        if (limbs_needed < 1) limbs_needed = 1;
        
        // 生成随机 limbs
        uint32_t* rand_limbs = (uint32_t*)malloc(limbs_needed * sizeof(uint32_t));
        if (!rand_limbs) {
            gc_pop_root();
            gc_pop_root();
            return val_int(0);
        }
        
        for (int i = 0; i < limbs_needed; i++) {
            rand_limbs[i] = pcg32_random_r(&pcg32_global);
        }
        
        Value rand_big_val = val_obj((Object*)bigint_new(rand_limbs, limbs_needed, 0));
        gc_push_root(&rand_big_val);
        free(rand_limbs);
        
        // rand_big % range_big
        Value mod_result_val = bigint_mod(val_as_bigint(rand_big_val), val_as_bigint(range_big_val));
        gc_push_root(&mod_result_val);
        
        // min + mod_result
        Value result = bigint_add(val_as_bigint(min_big_val), val_as_bigint(mod_result_val));
        
        gc_pop_root(); // mod_result_val
        gc_pop_root(); // rand_big_val
        gc_pop_root(); // range_big_val
        gc_pop_root(); // min_big_val
        
        return result;
    }
    
    // 普通 int 范围
    int min = (int)min_val;
    int max = (int)max_val;
    
    uint32_t int_range = (uint32_t)(max - min + 1);
    return val_int(min + (int)random_bounded(int_range));
}

// rands.floats(min, max) - 生成指定范围内的随机小数 [min, max)
static Value rands_floats(int argc, Value* args) {
    (void)argc;
    
    double min = val_as_num_ex(args[0]);
    double max = val_as_num_ex(args[1]);
    
    return val_float(min + random_double() * (max - min));
}

// rands.bools(probability?) - 生成随机布尔值
static Value rands_bools(int argc, Value* args) {
    double probability = 0.5;
    if (argc >= 1) {
        probability = val_as_num_ex(args[0]);
        if (probability < 0.0 || probability > 1.0) {
            native_throw_error("bools() 概率参数必须在 0.0-1.0 范围内");
            return val_null();
        }
    }
    
    return val_bool(random_double() < probability);
}

// rands.choice(arr) - 从数组中随机选择一个元素
static Value rands_choice(int argc, Value* args) {
    (void)argc;
    
    ObjArray* arr = (ObjArray*)val_as_obj(args[0]);
    if (arr->count == 0) {
        native_throw_error("choice() 不能从空数组中选择");
        return val_null();
    }
    
    int index = (int)random_bounded((uint32_t)arr->count);
    return arr->elements[index];
}

// rands.sample(arr, count) - 从数组中随机采样多个不重复元素
static Value rands_sample(int argc, Value* args) {
    (void)argc;
    
    ObjArray* source = (ObjArray*)val_as_obj(args[0]);
    int count = val_is_bigint(args[1]) ? (int)bigint_to_int64(val_as_bigint(args[1])) : (int)val_as_num(args[1]);
    
    if (count < 0) {
        native_throw_error("sample() 采样数量不能为负数");
        return val_null();
    }
    
    if (count > source->count) {
        count = source->count;
    }
    
    if (count == 0) {
        return val_obj((Object*)arr_new(0));
    }
    
    // 创建副本
    Value* temp = (Value*)malloc(sizeof(Value) * source->count);
    if (!temp) {
        native_throw_error("sample() 内存分配失败");
        return val_null();
    }
    memcpy(temp, source->elements, sizeof(Value) * source->count);
    
    // 打乱前 count 个元素（从尾部开始）
    for (int i = source->count - 1; i > 0 && i >= source->count - count; i--) {
        int j = (int)random_bounded((uint32_t)(i + 1));
        Value swap = temp[i];
        temp[i] = temp[j];
        temp[j] = swap;
    }
    
    // 创建结果数组
    ObjArray* result = (ObjArray*)gc_alloc(sizeof(ObjArray), OBJ_ARRAY);
    if (!result) {
        free(temp);
        return val_null();
    }
    result->count = count;
    result->capacity = count;
    result->elements = (Value*)malloc(sizeof(Value) * count);
    if (!result->elements) {
        free(temp);
        native_throw_error("sample() 结果数组内存分配失败");
        return val_null();
    }
    
    int startIdx = source->count - count;
    memcpy(result->elements, &temp[startIdx], sizeof(Value) * count);
    free(temp);
    
    gc_track_memory((Object*)result, 0, sizeof(Value) * count);
    
    return val_obj((Object*)result);
}

// rands.shuffle(arr) - 随机打乱数组（返回新数组）
static Value rands_shuffle(int argc, Value* args) {
    (void)argc;
    
    ObjArray* source = (ObjArray*)val_as_obj(args[0]);

    // 创建新数组
    ObjArray* result = (ObjArray*)gc_alloc(sizeof(ObjArray), OBJ_ARRAY);
    if (!result) return val_null();
    
    result->count = source->count;
    result->capacity = source->count;
    result->elements = NULL;
    
    if (source->count > 0) {
        result->elements = (Value*)malloc(sizeof(Value) * source->count);
        if (!result->elements) {
            native_throw_error("shuffle() 内存分配失败");
            return val_null();
        }
        memcpy(result->elements, source->elements, sizeof(Value) * source->count);
        
        // Fisher-Yates 洗牌
        fisher_yates_shuffle(result->elements, result->count);
        
        gc_track_memory((Object*)result, 0, sizeof(Value) * source->count);
    }
    
    return val_obj((Object*)result);
}

// rands.str(len, chars?) - 生成随机字符串
static Value rands_str(int argc, Value* args) {
    int len = val_is_bigint(args[0]) ? (int)bigint_to_int64(val_as_bigint(args[0])) : (int)val_as_num(args[0]);
    if (len < 0) {
        native_throw_error("str() 长度不能为负数");
        return val_null();
    }
    
    const char* chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    int charsLen = 62;
    
    if (argc >= 2 && val_is_obj(args[1]) && val_as_obj(args[1])->type == OBJ_STRING) {
        ObjString* charsStr = (ObjString*)val_as_obj(args[1]);
        if (charsStr->len == 0) {
            native_throw_error("rands.str() 字符集不能为空");
            return val_null();
        }
        chars = charsStr->chars;
        charsLen = charsStr->len;
    }
    
    char* buffer = (char*)malloc(len + 1);
    if (!buffer) {
        native_throw_error("str() 内存分配失败");
        return val_null();
    }
    
    for (int i = 0; i < len; i++) {
        buffer[i] = chars[random_bounded((uint32_t)charsLen)];
    }
    buffer[len] = '\0';
    
    ObjString* result = str_copy(buffer, len);
    free(buffer);
    
    return val_obj((Object*)result);
}

// rands.int_array(min, max) - 生成指定范围内所有整数的随机排列
static Value rands_int_array(int argc, Value* args) {
    (void)argc;
    
    int min = val_is_bigint(args[0]) ? (int)bigint_to_int64(val_as_bigint(args[0])) : (int)val_as_num(args[0]);
    int max = val_is_bigint(args[1]) ? (int)bigint_to_int64(val_as_bigint(args[1])) : (int)val_as_num(args[1]);
    
    if (min > max) {
        int temp = min;
        min = max;
        max = temp;
    }
    
    int rangeSize = max - min + 1;
    
    // 限制最大范围，防止内存爆炸
    const int MAX_INT_ARRAY_SIZE = 10000000; // 1000 万
    if (rangeSize > MAX_INT_ARRAY_SIZE) {
        native_throw_error("int_array() 范围太大，最多支持 1000 万个元素");
        return val_null();
    }
    
    // 创建结果数组
    ObjArray* result = (ObjArray*)gc_alloc(sizeof(ObjArray), OBJ_ARRAY);
    if (!result) return val_null();
    
    result->count = rangeSize;
    result->capacity = rangeSize;
    result->elements = NULL;
    
    if (rangeSize > 0) {
        result->elements = (Value*)malloc(sizeof(Value) * rangeSize);
        if (!result->elements) {
            native_throw_error("int_array() 内存分配失败");
            return val_null();
        }
        
        // 填充数组
        for (int i = 0; i < rangeSize; i++) {
            result->elements[i] = val_int(min + i);
            gc_write_barrier((Object*)result, result->elements[i]);
        }
        
        // Fisher-Yates 洗牌
        fisher_yates_shuffle(result->elements, rangeSize);

        gc_track_memory((Object*)result, 0, sizeof(Value) * rangeSize);
    }
    
    return val_obj((Object*)result);
}

// rands.seed(seed) - 设置随机数种子
static Value rands_seed(int argc, Value* args) {
    (void)argc;
    unsigned int seed = val_is_bigint(args[0]) ? (unsigned int)bigint_to_int64(val_as_bigint(args[0])) : (unsigned int)val_as_num(args[0]);
    set_pcg32_seed(seed);
    return val_null();
}

// rands.array(n, min, max) - 生成 n 个随机整数数组（可重复）
static Value rands_array(int argc, Value* args) {
    (void)argc;
    
    int n = val_is_bigint(args[0]) ? (int)bigint_to_int64(val_as_bigint(args[0])) : (int)val_as_num(args[0]);
    int min = val_is_bigint(args[1]) ? (int)bigint_to_int64(val_as_bigint(args[1])) : (int)val_as_num(args[1]);
    int max = val_is_bigint(args[2]) ? (int)bigint_to_int64(val_as_bigint(args[2])) : (int)val_as_num(args[2]);
    
    if (n < 0) {
        native_throw_error("array() 数量不能为负数");
        return val_null();
    }
    if (n == 0) {
        return val_obj((Object*)arr_new(0));
    }
    
    if (min > max) {
        int temp = min;
        min = max;
        max = temp;
    }
    
    uint32_t range = (uint32_t)(max - min + 1);
    
    ObjArray* result = (ObjArray*)gc_alloc(sizeof(ObjArray), OBJ_ARRAY);
    if (!result) return val_null();
    
    result->count = n;
    result->capacity = n;
    result->elements = (Value*)malloc(sizeof(Value) * n);
    if (!result->elements) {
        native_throw_error("array() 内存分配失败");
        return val_null();
    }
    
    for (int i = 0; i < n; i++) {
        result->elements[i] = val_int(min + (int)random_bounded(range));
        gc_write_barrier((Object*)result, result->elements[i]);
    }
    
    gc_track_memory((Object*)result, 0, sizeof(Value) * n);
    
    return val_obj((Object*)result);
}

// rands.gauss(mean, std) - 生成正态分布随机数（Box-Muller 算法）
static Value rands_gauss(int argc, Value* args) {
    (void)argc;
    
    double mean = val_as_num_ex(args[0]);
    double std = val_as_num_ex(args[1]);
    
    if (std < 0) {
        native_throw_error("gauss() 标准差不能为负数");
        return val_null();
    }
    
    // Box-Muller 算法生成标准正态分布 N(0,1)
    double u1 = random_double();
    double u2 = random_double();
    
    // 避免 u1 为 0
    while (u1 == 0.0) {
        u1 = random_double();
    }
    
    double z0 = sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
    
    // 转换为 N(mean, std^2)
    double result = mean + std * z0;
    
    return val_float(result);
}

// ============================================================================
// 初始化
// ============================================================================

void rands_init_module(void) {
    TypeKind om_params[] = {};
    native_register_module_method("rands", "om", rands_om, 0, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, om_params);

    TypeKind ints_params[] = {TYPE_INT, TYPE_INT};
    native_register_module_method("rands", "ints", rands_ints, 2, -1, -1, TYPE_INT, TYPE_UNKNOWN, ints_params);

    TypeKind floats_params[] = {TYPE_FLOAT, TYPE_FLOAT};
    native_register_module_method("rands", "floats", rands_floats, 2, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, floats_params);

    TypeKind bools_params[] = {TYPE_FLOAT};
    native_register_module_method("rands", "bools", rands_bools, 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, bools_params);

    TypeKind choice_params[] = {TYPE_ARRAY};
    native_register_module_method("rands", "choice", rands_choice, 1, -1, -1, TYPE_ANY, TYPE_UNKNOWN, choice_params);

    TypeKind sample_params[] = {TYPE_ARRAY, TYPE_INT};
    native_register_module_method("rands", "sample", rands_sample, 2, -1, -1, TYPE_ARRAY, TYPE_ANY, sample_params);

    TypeKind shuffle_params[] = {TYPE_ARRAY};
    native_register_module_method("rands", "shuffle", rands_shuffle, 1, -1, -1, TYPE_ARRAY, TYPE_ANY, shuffle_params);

    TypeKind str_params[] = {TYPE_INT, TYPE_STRING};
    native_register_module_method("rands", "str", rands_str, 2, -1, -1, TYPE_STRING, TYPE_UNKNOWN, str_params);

    TypeKind int_array_params[] = {TYPE_INT, TYPE_INT};
    native_register_module_method("rands", "int_array", rands_int_array, 2, -1, -1, TYPE_ARRAY, TYPE_INT, int_array_params);

    TypeKind seed_params[] = {TYPE_INT};
    native_register_module_method("rands", "seed", rands_seed, 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, seed_params);

    // 新增：批量生成随机数数组
    TypeKind array_params[] = {TYPE_INT, TYPE_INT, TYPE_INT};
    native_register_module_method("rands", "array", rands_array, 3, -1, -1, TYPE_ARRAY, TYPE_INT, array_params);

    // 新增：正态分布随机数
    TypeKind gauss_params[] = {TYPE_FLOAT, TYPE_FLOAT};
    native_register_module_method("rands", "gauss", rands_gauss, 2, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, gauss_params);
}
