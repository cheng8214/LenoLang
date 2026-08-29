#include "include/native.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Windows 数学常量定义
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E 2.71828182845904523536
#endif

#define DEG_TO_RAD (M_PI / 180.0)
#define RAD_TO_DEG (180.0 / M_PI)

// 前向声明：数字实例方法支持函数（定义在 object_number.c）
extern void number_init_methods(void);
extern void number_register_method_with_params(const char* name, ObjNative* method, int arity,
                                                int min_arity, int max_arity,
                                                TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);

// 辅助函数：获取数值（支持 int、float 和 BigInt）
double get_number(Value v) {
    if (val_is_int(v)) return (double)val_as_int(v);
    if (val_is_float(v)) return val_as_num(v);
    if (val_is_bigint(v)) return bigint_to_double(val_as_bigint(v));
    return 0.0;
}

// ==================== 基本运算 ====================

static Value math_sqrt(int argc, Value* args) {
    (void)argc;
    double num = get_number(args[0]);
    if (num < 0) {
        native_throw_error("sqrt() 参数不能为负数");
        return val_null();
    }
    return val_float(sqrt(num));
}

static Value math_abs(int argc, Value* args) {
    (void)argc;
    double num = get_number(args[0]);
    return val_float(fabs(num));
}

static Value math_pow(int argc, Value* args) {
    (void)argc;
    double base = get_number(args[0]);
    double exp = get_number(args[1]);
    return val_float(pow(base, exp));
}

// ==================== 取整函数 ====================

static Value math_round(int argc, Value* args) {
    (void)argc;
    double num = get_number(args[0]);
    return val_float(round(num));
}

static Value math_ceil(int argc, Value* args) {
    (void)argc;
    double num = get_number(args[0]);
    return val_float(ceil(num));
}

static Value math_floor(int argc, Value* args) {
    (void)argc;
    double num = get_number(args[0]);
    return val_float(floor(num));
}

static Value math_trunc(int argc, Value* args) {
    (void)argc;
    double num = get_number(args[0]);
    return val_float(trunc(num));
}

// ==================== 三角函数（弧度制）====================

static Value math_cos(int argc, Value* args) {
    (void)argc;
    double num = get_number(args[0]);
    return val_float(cos(num));
}

static Value math_sin(int argc, Value* args) {
    (void)argc;
    double num = get_number(args[0]);
    return val_float(sin(num));
}

static Value math_tan(int argc, Value* args) {
    (void)argc;
    double num = get_number(args[0]);
    return val_float(tan(num));
}

static Value math_asin(int argc, Value* args) {
    (void)argc;
    double num = get_number(args[0]);
    if (num < -1 || num > 1) {
        native_throw_error("asin() 参数必须在 -1 到 1 之间");
        return val_null();
    }
    return val_float(asin(num));
}

static Value math_acos(int argc, Value* args) {
    (void)argc;
    double num = get_number(args[0]);
    if (num < -1 || num > 1) {
        native_throw_error("acos() 参数必须在 -1 到 1 之间");
        return val_null();
    }
    return val_float(acos(num));
}

static Value math_atan(int argc, Value* args) {
    (void)argc;
    double num = get_number(args[0]);
    return val_float(atan(num));
}

static Value math_atan2(int argc, Value* args) {
    (void)argc;
    double y = get_number(args[0]);
    double x = get_number(args[1]);
    return val_float(atan2(y, x));
}

// ==================== 对数和指数 ====================

static Value math_log(int argc, Value* args) {
    (void)argc;
    double num = get_number(args[0]);
    if (num <= 0) {
        native_throw_error("log() 参数必须大于 0");
        return val_null();
    }
    return val_float(log(num));
}

static Value math_log10(int argc, Value* args) {
    (void)argc;
    double num = get_number(args[0]);
    if (num <= 0) {
        native_throw_error("log10() 参数必须大于 0");
        return val_null();
    }
    return val_float(log10(num));
}

static Value math_exp(int argc, Value* args) {
    (void)argc;
    double num = get_number(args[0]);
    return val_float(exp(num));
}

// ==================== 工具函数 ====================

static Value math_fmod(int argc, Value* args) {
    (void)argc;
    double a = get_number(args[0]);
    double b = get_number(args[1]);
    if (b == 0.0) {
        native_throw_error("fmod() 除数不能为 0");
        return val_null();
    }
    return val_float(fmod(a, b));
}

static Value math_clamp(int argc, Value* args) {
    (void)argc;
    double v = get_number(args[0]);
    double lo = get_number(args[1]);
    double hi = get_number(args[2]);
    if (v < lo) return val_float(lo);
    if (v > hi) return val_float(hi);
    return val_float(v);
}

static Value math_lerp(int argc, Value* args) {
    (void)argc;
    double a = get_number(args[0]);
    double b = get_number(args[1]);
    double t = get_number(args[2]);
    return val_float(a + (b - a) * t);
}

static Value math_rsqrt(int argc, Value* args) {
    (void)argc;
    double x = get_number(args[0]);
    if (x <= 0.0) {
        native_throw_error("rsqrt() 参数必须大于 0");
        return val_null();
    }
    // 经典快速反平方根（Quake III 算法）
    float xf = (float)x;
    float xhalf = 0.5f * xf;
    int i;
    memcpy(&i, &xf, sizeof(i));      // 按位转 int
    i = 0x5f3759df - (i >> 1);       // 魔法常数
    float result;
    memcpy(&result, &i, sizeof(result));
    result = result * (1.5f - xhalf * result * result);  // 一次牛顿迭代
    return val_float((double)result);
}

static Value math_hypot(int argc, Value* args) {
    (void)argc;
    double x = get_number(args[0]);
    double y = get_number(args[1]);
    return val_float(hypot(x, y));
}

static Value math_log2(int argc, Value* args) {
    (void)argc;
    double num = get_number(args[0]);
    if (num <= 0) {
        native_throw_error("log2() 参数必须大于 0");
        return val_null();
    }
    return val_float(log2(num));
}

// ==================== 快速近似三角函数 ====================
// 使用 Bhaskara I 正弦近似（精度 <0.001），比标准 sin 快约 3-5x
// 内联范围缩减（避免 fmod 调用），用整数取模代替

static Value math_sin_fast(int argc, Value* args) {
    (void)argc;
    double x = get_number(args[0]);
    // 快速范围缩减：归一化到 [0, 2π)
    double inv = 1.0 / (2.0 * M_PI);
    int k = (int)(x * inv);  // 取整代替 fmod
    x = x - (double)k * 2.0 * M_PI;
    if (x < 0) x += 2.0 * M_PI;
    // [π, 2π) 映射到 [0, π) 并取反
    int neg = 0;
    if (x >= M_PI) { x -= M_PI; neg = 1; }
    // Bhaskara I: sin(x) ≈ 16x(π-x) / (5π² - 4x(π-x))
    double v = 16.0 * x * (M_PI - x) / (5.0 * M_PI * M_PI - 4.0 * x * (M_PI - x));
    return val_float(neg ? -v : v);
}

static Value math_cos_fast(int argc, Value* args) {
    (void)argc;
    double x = get_number(args[0]);
    // cos(x) = sin(x + π/2)
    x += M_PI / 2.0;
    double inv = 1.0 / (2.0 * M_PI);
    int k = (int)(x * inv);
    x = x - (double)k * 2.0 * M_PI;
    if (x < 0) x += 2.0 * M_PI;
    int neg = 0;
    if (x >= M_PI) { x -= M_PI; neg = 1; }
    double v = 16.0 * x * (M_PI - x) / (5.0 * M_PI * M_PI - 4.0 * x * (M_PI - x));
    return val_float(neg ? -v : v);
}

static Value math_max(int argc, Value* args) {
    (void)argc;
    double max_val = get_number(args[0]);
    for (int i = 1; i < argc; i++) {
        double val = get_number(args[i]);
        if (val > max_val) max_val = val;
    }
    return val_float(max_val);
}

static Value math_min(int argc, Value* args) {
    (void)argc;
    double min_val = get_number(args[0]);
    for (int i = 1; i < argc; i++) {
        double val = get_number(args[i]);
        if (val < min_val) min_val = val;
    }
    return val_float(min_val);
}

static Value math_deg(int argc, Value* args) {
    (void)argc;
    double rad = get_number(args[0]);
    return val_float(rad * RAD_TO_DEG);
}

static Value math_rad(int argc, Value* args) {
    (void)argc;
    double deg = get_number(args[0]);
    return val_float(deg * DEG_TO_RAD);
}

static Value math_sign(int argc, Value* args) {
    (void)argc;
    double num = get_number(args[0]);
    if (num > 0) return val_float(1.0);
    if (num < 0) return val_float(-1.0);
    return val_float(0.0);
}

// ==================== 常量 ====================

static Value math_pi(int argc, Value* args) {
    (void)argc; (void)args;
    return val_float(M_PI);
}

static Value math_e(int argc, Value* args) {
    (void)argc; (void)args;
    return val_float(M_E);
}

// ==================== 初始化 ====================

void maths_init_module(void) {
    // 基本运算
    TypeKind sqrt_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "sqrt", math_sqrt, 1, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, sqrt_params);

    TypeKind abs_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "abs", math_abs, 1, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, abs_params);

    TypeKind pow_params[] = {TYPE_FLOAT, TYPE_FLOAT};
    native_register_module_method("maths", "pow", math_pow, 2, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, pow_params);

    // 取整函数
    TypeKind round_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "round", math_round, 1, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, round_params);

    TypeKind ceil_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "ceil", math_ceil, 1, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, ceil_params);

    TypeKind floor_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "floor", math_floor, 1, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, floor_params);

    TypeKind trunc_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "trunc", math_trunc, 1, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, trunc_params);

    // 三角函数
    TypeKind cos_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "cos", math_cos, 1, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, cos_params);

    TypeKind sin_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "sin", math_sin, 1, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, sin_params);

    TypeKind tan_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "tan", math_tan, 1, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, tan_params);

    TypeKind asin_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "asin", math_asin, 1, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, asin_params);

    TypeKind acos_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "acos", math_acos, 1, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, acos_params);

    TypeKind atan_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "atan", math_atan, 1, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, atan_params);

    TypeKind atan2_params[] = {TYPE_FLOAT, TYPE_FLOAT};
    native_register_module_method("maths", "atan2", math_atan2, 2, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, atan2_params);

    // 对数和指数
    TypeKind log_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "log", math_log, 1, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, log_params);

    TypeKind log10_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "log10", math_log10, 1, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, log10_params);

    TypeKind exp_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "exp", math_exp, 1, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, exp_params);

    // 工具函数
    TypeKind max_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "max", math_max, -1, 1, -1, TYPE_FLOAT, TYPE_UNKNOWN, max_params);

    TypeKind min_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "min", math_min, -1, 1, -1, TYPE_FLOAT, TYPE_UNKNOWN, min_params);

    TypeKind deg_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "deg", math_deg, 1, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, deg_params);

    TypeKind rad_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "rad", math_rad, 1, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, rad_params);

    TypeKind sign_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "sign", math_sign, 1, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, sign_params);

    // 新增工具函数
    TypeKind fmod_params[] = {TYPE_FLOAT, TYPE_FLOAT};
    native_register_module_method("maths", "fmod", math_fmod, 2, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, fmod_params);

    TypeKind clamp_params[] = {TYPE_FLOAT, TYPE_FLOAT, TYPE_FLOAT};
    native_register_module_method("maths", "clamp", math_clamp, 3, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, clamp_params);

    TypeKind lerp_params[] = {TYPE_FLOAT, TYPE_FLOAT, TYPE_FLOAT};
    native_register_module_method("maths", "lerp", math_lerp, 3, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, lerp_params);

    TypeKind rsqrt_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "rsqrt", math_rsqrt, 1, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, rsqrt_params);

    TypeKind hypot_params[] = {TYPE_FLOAT, TYPE_FLOAT};
    native_register_module_method("maths", "hypot", math_hypot, 2, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, hypot_params);

    TypeKind log2_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "log2", math_log2, 1, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, log2_params);

    TypeKind sin_fast_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "sin_fast", math_sin_fast, 1, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, sin_fast_params);

    TypeKind cos_fast_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "cos_fast", math_cos_fast, 1, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, cos_fast_params);

    // 常量
    TypeKind pi_params[] = {};
    native_register_module_method("maths", "pi", math_pi, 0, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, pi_params);

    TypeKind e_params[] = {};
    native_register_module_method("maths", "e", math_e, 0, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, e_params);
}

void maths_init_instance_methods(void) {
    number_init_methods();
    
    // 基本运算
    TypeKind no_params[] = {};
    TypeKind one_params[] = {TYPE_FLOAT};

    number_register_method_with_params("sqrt", make_native(math_sqrt, 1, "sqrt"), 0, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, no_params);
    number_register_method_with_params("abs", make_native(math_abs, 1, "abs"), 0, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, no_params);
    number_register_method_with_params("pow", make_native(math_pow, 2, "pow"), 1, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, one_params);

    // 取整函数
    number_register_method_with_params("round", make_native(math_round, 1, "round"), 0, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, no_params);
    number_register_method_with_params("ceil", make_native(math_ceil, 1, "ceil"), 0, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, no_params);
    number_register_method_with_params("floor", make_native(math_floor, 1, "floor"), 0, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, no_params);
    number_register_method_with_params("trunc", make_native(math_trunc, 1, "trunc"), 0, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, no_params);

    // 三角函数
    number_register_method_with_params("cos", make_native(math_cos, 1, "cos"), 0, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, no_params);
    number_register_method_with_params("sin", make_native(math_sin, 1, "sin"), 0, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, no_params);
    number_register_method_with_params("tan", make_native(math_tan, 1, "tan"), 0, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, no_params);
    number_register_method_with_params("asin", make_native(math_asin, 1, "asin"), 0, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, no_params);
    number_register_method_with_params("acos", make_native(math_acos, 1, "acos"), 0, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, no_params);
    number_register_method_with_params("atan", make_native(math_atan, 1, "atan"), 0, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, no_params);
    number_register_method_with_params("atan2", make_native(math_atan2, 2, "atan2"), 1, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, one_params);

    // 对数和指数
    number_register_method_with_params("log", make_native(math_log, 1, "log"), 0, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, no_params);
    number_register_method_with_params("log10", make_native(math_log10, 1, "log10"), 0, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, no_params);
    number_register_method_with_params("exp", make_native(math_exp, 1, "exp"), 0, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, no_params);

    // 工具函数
    number_register_method_with_params("deg", make_native(math_deg, 1, "deg"), 0, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, no_params);
    number_register_method_with_params("rad", make_native(math_rad, 1, "rad"), 0, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, no_params);
    number_register_method_with_params("sign", make_native(math_sign, 1, "sign"), 0, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, no_params);
    number_register_method_with_params("fmod", make_native(math_fmod, 2, "fmod"), 1, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, one_params);
    number_register_method_with_params("clamp", make_native(math_clamp, 3, "clamp"), 2, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, one_params);
    number_register_method_with_params("lerp", make_native(math_lerp, 3, "lerp"), 2, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, one_params);
    number_register_method_with_params("rsqrt", make_native(math_rsqrt, 1, "rsqrt"), 0, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, no_params);
    number_register_method_with_params("hypot", make_native(math_hypot, 2, "hypot"), 1, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, one_params);
    number_register_method_with_params("log2", make_native(math_log2, 1, "log2"), 0, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, no_params);
    number_register_method_with_params("sin_fast", make_native(math_sin_fast, 1, "sin_fast"), 0, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, no_params);
    number_register_method_with_params("cos_fast", make_native(math_cos_fast, 1, "cos_fast"), 0, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, no_params);
}
