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
extern void number_register_method(const char* name, ObjNative* method,
                                  TypeKind return_type, TypeKind return_element_type,
                                  NativeParamSpec params);

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

// smoothstep(t)：0..1 的平滑缓动 3t²-2t³（t 先夹到 [0,1]）
//   用途：动画的"起手慢/收尾慢"曲线。线性插值起步和停止都很硬（"嗒"地一下），
//   指数逼近则是"一上来最快、尾巴拖很长" ⇒ 两边都不好看，UI 动画基本都用这条 ✓
//   边缘不在 0..1 时先自己归一化：maths.smoothstep(maths.clamp((x-e0)/(e1-e0), 0, 1))
//   （不做成 GLSL 那种三参数版本：本语言同一名字只能有一种定长签名，
//     而 UI 侧 99% 都是"进度 0~1"，单参数更省心 ✓）
static Value math_smoothstep(int argc, Value* args) {
    (void)argc;
    double t = get_number(args[0]);
    if (t <= 0.0) return val_float(0.0);      // 省掉死区里的乘法（也顺手夹取）
    if (t >= 1.0) return val_float(1.0);
    return val_float(t * t * (3.0 - 2.0 * t));
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

// ==================== 整数版工具 (2026-11-01) ====================
// 为什么单独一组：上面的 round / min / max / clamp 一律返回 **float**，而 GUI 控件里的尺寸、
//   行列号、像素坐标全是 int ⇒ 调用点还得 `_int(...)` 转回来，比手写 `if` 还长。
//   实测 LenoSDL3/lib 的 58 个文件里只有 11 个 import maths，其余全是手写夹取；
//   `_int(x + 0.5)` 这种手写四舍五入也有 24 处（sdl_renderer / sdl_imageview / sdl_card…）。
//   ⇒ 补**同语义的 int 版**（名字加 i 后缀），不动原有 float 版的返回类型
//     （改返回类型会动到类型推断，风险远大于加新函数 ✓）。
//   int 版接受任意数字实参：小数按**四舍五入**取整（与 roundi 一致，符合"像素边界"的直觉）。

static int64_t math_to_i64(Value v) {
    // ★ 整数快路径：GUI 控件的尺寸/行列号**全是 int**，先判类型再决定要不要走 double
    //   （实测：一律 get_number + llround 时 `clampi` 比手写 if 慢 2.5 倍；加这条快路径后
    //     降到与手写同量级 —— 见 tools/bench_maths_native.leno 的对比）
    if (val_is_int(v)) return val_as_int(v);
    return (int64_t)llround(get_number(v));
}

static Value math_roundi(int argc, Value* args) {
    (void)argc;
    return val_int(math_to_i64(args[0]));
}

static Value math_mini(int argc, Value* args) {
    int64_t m = math_to_i64(args[0]);
    for (int i = 1; i < argc; i++) {
        int64_t v = math_to_i64(args[i]);
        if (v < m) m = v;
    }
    return val_int(m);
}

static Value math_maxi(int argc, Value* args) {
    int64_t m = math_to_i64(args[0]);
    for (int i = 1; i < argc; i++) {
        int64_t v = math_to_i64(args[i]);
        if (v > m) m = v;
    }
    return val_int(m);
}

static Value math_clampi(int argc, Value* args) {
    (void)argc;
    int64_t v = math_to_i64(args[0]);
    int64_t lo = math_to_i64(args[1]);
    int64_t hi = math_to_i64(args[2]);
    // 边界写反时不抛异常：按"空区间"收成 lo（GUI 里 hi/lo 常来自布局计算，画错比崩了好）
    if (hi < lo) hi = lo;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return val_int(v);
}

// 正取模：结余符号跟**除数**走（b > 0 时返回 0..b-1）
//   为什么需要：`%` 是 C 语义（负数得负余数），日历取星期、环形索引都要自己补偏移
//   （LenoSDL3 的 sdl_calendar.leno:226 就是 `(firstDow - weekStart + 7) % 7`）
static Value math_mod(int argc, Value* args) {
    (void)argc;
    int64_t a = math_to_i64(args[0]);
    int64_t b = math_to_i64(args[1]);
    if (b == 0) {
        native_throw_error("mod() 除数不能为 0");
        return val_null();
    }
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return val_int(r);
}

// 夹到 0..1（GUI 里出现频率最高的夹取：透明度、进度、归一化值）
static Value math_saturate(int argc, Value* args) {
    (void)argc;
    double v = get_number(args[0]);
    if (v < 0.0) return val_float(0.0);
    if (v > 1.0) return val_float(1.0);
    return val_float(v);
}

// (v - a) / (b - a)；a == b 时返回 0（不抛异常：GUI 里尺寸为 0 是常态）
static Value math_inverse_lerp(int argc, Value* args) {
    (void)argc;
    double a = get_number(args[0]);
    double b = get_number(args[1]);
    double v = get_number(args[2]);
    if (b == a) return val_float(0.0);
    return val_float((v - a) / (b - a));
}

// 区间映射：v 在 [inLo, inHi] 中的比例（**先夹到 0..1**）线性映射到 [outLo, outHi]
//   为什么夹输入：UI 侧几乎都是"内容长度 → 滑块长度"（LenoSDL3 里同一公式复制了 6 处），
//   越界时必须夹住，否则滑块会长出轨道 ✗
static Value math_remap(int argc, Value* args) {
    (void)argc;
    double v = get_number(args[0]);
    double in_lo = get_number(args[1]);
    double in_hi = get_number(args[2]);
    double out_lo = get_number(args[3]);
    double out_hi = get_number(args[4]);
    double t = (in_hi == in_lo) ? 0.0 : (v - in_lo) / (in_hi - in_lo);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return val_float(out_lo + (out_hi - out_lo) * t);
}

// 环形夹取：把 v 绕回 [lo, hi)（轮播、色相环、环形索引）
static Value math_wrap(int argc, Value* args) {
    (void)argc;
    double v = get_number(args[0]);
    double lo = get_number(args[1]);
    double hi = get_number(args[2]);
    if (hi <= lo) return val_float(lo);
    double span = hi - lo;
    double r = fmod(v - lo, span);
    if (r < 0.0) r += span;
    return val_float(lo + r);
}

// ==================== 缓动曲线 (2026-11-01) ====================
// 为什么一次给这么多条：`smoothstep` 只是"起手慢 + 收尾慢"，而 UI 动画里真正高频的是
//   ① 进入/弹出 = **只收尾慢**（ease_out_*）② 展开/切换 = 两头都慢（ease_in_out_*）
//   ③ 退场 = 只起手慢（ease_in_*）。LenoSDL3 的歌词滚动、Toast 淡入、面板展开已经在各写
//   一份（Aria 最近两笔提交就是"缓动改用 maths.smoothstep / 自己补 ease"）。
//   ⚠ 入口 t 一律先夹到 0..1（动画进度越界是常态，不该由调用方每次自己夹）。
static double ease_clamp01(double t) {
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    return t;
}

static Value math_smootherstep(int argc, Value* args) {
    (void)argc;
    double t = ease_clamp01(get_number(args[0]));
    return val_float(t * t * t * (t * (t * 6.0 - 15.0) + 10.0));   // 5t⁴-6t⁵ 的展开式
}

static Value math_ease_in_quad(int argc, Value* args) {
    (void)argc;
    double t = ease_clamp01(get_number(args[0]));
    return val_float(t * t);
}

static Value math_ease_out_quad(int argc, Value* args) {
    (void)argc;
    double t = ease_clamp01(get_number(args[0]));
    return val_float(1.0 - (1.0 - t) * (1.0 - t));
}

static Value math_ease_in_out_quad(int argc, Value* args) {
    (void)argc;
    double t = ease_clamp01(get_number(args[0]));
    if (t < 0.5) return val_float(2.0 * t * t);
    double u = 1.0 - t;
    return val_float(1.0 - 2.0 * u * u);
}

static Value math_ease_in_cubic(int argc, Value* args) {
    (void)argc;
    double t = ease_clamp01(get_number(args[0]));
    return val_float(t * t * t);
}

static Value math_ease_out_cubic(int argc, Value* args) {
    (void)argc;
    double t = ease_clamp01(get_number(args[0]));
    double u = 1.0 - t;
    return val_float(1.0 - u * u * u);
}

static Value math_ease_in_out_cubic(int argc, Value* args) {
    (void)argc;
    double t = ease_clamp01(get_number(args[0]));
    if (t < 0.5) return val_float(4.0 * t * t * t);
    double u = 1.0 - t;
    return val_float(1.0 - 4.0 * u * u * u);
}

// ease_out_back：**会越过 1 再回弹**（k 是回弹幅度，UI 弹窗/卡片出现常用）
static Value math_ease_out_back(int argc, Value* args) {
    (void)argc;
    double t = ease_clamp01(get_number(args[0]));
    double k = (argc >= 2) ? get_number(args[1]) : 1.70158;
    double u = t - 1.0;
    return val_float(1.0 + (k + 1.0) * u * u * u + k * u * u);
}

// ==================== 角度工具 (2026-11-01) ====================
// 为什么要单独三条：角度的"最短路径"不是普通减法。341° → 19° 应该走 +38°，直接相减是 -322°，
//   按后者插值会转一大圈（spinner / ring / 指针类动画都会踩）。

static double angle_norm(double d) {
    double two_pi = 2.0 * M_PI;
    d = fmod(d + M_PI, two_pi);
    if (d < 0.0) d += two_pi;
    return d - M_PI;                 // [-π, π)
}

// 从 from 转到 to 的最短角差（[-π, π)）
static Value math_angle_delta(int argc, Value* args) {
    (void)argc;
    double from = get_number(args[0]);
    double to = get_number(args[1]);
    return val_float(angle_norm(to - from));
}

// 按最短路径插值（from/to 可以是任意弧度，不必先归一化）
static Value math_lerp_angle(int argc, Value* args) {
    (void)argc;
    double from = get_number(args[0]);
    double to = get_number(args[1]);
    double t = get_number(args[2]);
    return val_float(from + angle_norm(to - from) * t);
}

// 把任意弧度绕回 [-π, π)
static Value math_wrap_angle(int argc, Value* args) {
    (void)argc;
    return val_float(angle_norm(get_number(args[0])));
}

// ==================== 数值小工具 (2026-11-01) ====================

// 小数部分（着色、滚动偏移、进度取余都用它；负数结果也落在 [0,1)）
static Value math_fract(int argc, Value* args) {
    (void)argc;
    double v = get_number(args[0]);
    double f = v - floor(v);
    return val_float(f);
}

// GLSL 式 step：x < edge ⇒ 0，否则 1
static Value math_step(int argc, Value* args) {
    (void)argc;
    double edge = get_number(args[0]);
    double x = get_number(args[1]);
    return val_float(x < edge ? 0.0 : 1.0);
}

// 吸附到步长网格（对齐到整数像素/格子）；step <= 0 时原样返回
static Value math_snap(int argc, Value* args) {
    (void)argc;
    double v = get_number(args[0]);
    double s = get_number(args[1]);
    if (s <= 0.0) return val_float(v);
    return val_float(round(v / s) * s);
}

// 浮点近似相等（默认 eps = 1e-6）：布局/动画断言不该用 ==
static Value math_approx_eq(int argc, Value* args) {
    double a = get_number(args[0]);
    double b = get_number(args[1]);
    double eps = (argc >= 3) ? get_number(args[2]) : 1e-6;
    if (eps < 0.0) eps = -eps;
    double d = a - b;
    if (d < 0.0) d = -d;
    return val_bool(d <= eps);
}

// 两点距离（控件里两两算坐标差时，比手动 hypot(dx,dy) 少两行）
static Value math_dist(int argc, Value* args) {
    (void)argc;
    double x1 = get_number(args[0]);
    double y1 = get_number(args[1]);
    double x2 = get_number(args[2]);
    double y2 = get_number(args[3]);
    return val_float(hypot(x2 - x1, y2 - y1));
}

// 2 的幂判断 / 向上取到 2 的幂（扩容、网格 layout 分块）
static Value math_is_pow2(int argc, Value* args) {
    (void)argc;
    int64_t n = math_to_i64(args[0]);
    return val_bool(n > 0 && (n & (n - 1)) == 0);
}

static Value math_next_pow2(int argc, Value* args) {
    (void)argc;
    int64_t n = math_to_i64(args[0]);
    if (n <= 1) return val_int(1);
    int64_t p = 1;
    while (p < n && p < (int64_t)1 << 62) p <<= 1;
    return val_int(p);
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
    native_register_module_method("maths", "sqrt", math_sqrt, &NATIVE_T_FLOAT, NATIVE_FIXED(sqrt_params));

    TypeKind abs_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "abs", math_abs, &NATIVE_T_FLOAT, NATIVE_FIXED(abs_params));

    TypeKind pow_params[] = {TYPE_FLOAT, TYPE_FLOAT};
    native_register_module_method("maths", "pow", math_pow, &NATIVE_T_FLOAT, NATIVE_FIXED(pow_params));

    // 取整函数
    TypeKind round_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "round", math_round, &NATIVE_T_FLOAT, NATIVE_FIXED(round_params));

    TypeKind ceil_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "ceil", math_ceil, &NATIVE_T_FLOAT, NATIVE_FIXED(ceil_params));

    TypeKind floor_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "floor", math_floor, &NATIVE_T_FLOAT, NATIVE_FIXED(floor_params));

    TypeKind trunc_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "trunc", math_trunc, &NATIVE_T_FLOAT, NATIVE_FIXED(trunc_params));

    // 三角函数
    TypeKind cos_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "cos", math_cos, &NATIVE_T_FLOAT, NATIVE_FIXED(cos_params));

    TypeKind sin_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "sin", math_sin, &NATIVE_T_FLOAT, NATIVE_FIXED(sin_params));

    TypeKind tan_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "tan", math_tan, &NATIVE_T_FLOAT, NATIVE_FIXED(tan_params));

    TypeKind asin_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "asin", math_asin, &NATIVE_T_FLOAT, NATIVE_FIXED(asin_params));

    TypeKind acos_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "acos", math_acos, &NATIVE_T_FLOAT, NATIVE_FIXED(acos_params));

    TypeKind atan_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "atan", math_atan, &NATIVE_T_FLOAT, NATIVE_FIXED(atan_params));

    TypeKind atan2_params[] = {TYPE_FLOAT, TYPE_FLOAT};
    native_register_module_method("maths", "atan2", math_atan2, &NATIVE_T_FLOAT, NATIVE_FIXED(atan2_params));

    // 对数和指数
    TypeKind log_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "log", math_log, &NATIVE_T_FLOAT, NATIVE_FIXED(log_params));

    TypeKind log10_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "log10", math_log10, &NATIVE_T_FLOAT, NATIVE_FIXED(log10_params));

    TypeKind exp_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "exp", math_exp, &NATIVE_T_FLOAT, NATIVE_FIXED(exp_params));

    // 工具函数
    native_register_module_method("maths", "max", math_max, &NATIVE_T_FLOAT, NATIVE_VARARG(1, NATIVE_ARITY_ANY, 0, NULL, TYPE_FLOAT));

    native_register_module_method("maths", "min", math_min, &NATIVE_T_FLOAT, NATIVE_VARARG(1, NATIVE_ARITY_ANY, 0, NULL, TYPE_FLOAT));

    // ---- 可变参数的参数类型（v3.2.7）----
    // max/min 是**同质可变参数**：每个实参都该是数字（v3.2.7 显式声明）。
    //   此前 `arity == -1` 让 param_types 被整份忽略 ⇒ `maths.max("x")` 编译期不报错 ✗。
    // 尾部声明 FLOAT：本语言的 `int → float` 是**允许**的隐式转换（实测 `maths.max(1,2,3)` ⇒ 3.0 ✓），
    //   所以既拦住了非数字，又不会误伤纯整数调用。
    // (2026-10-02) 原先这两行是 `native_set_method_vararg_params("maths","max"/"min",0,NULL,TYPE_FLOAT)`
    //   —— "两步式"的第二步。现已并入上面各自的 `native_register_module_method_vararg(...)` ✓

    TypeKind deg_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "deg", math_deg, &NATIVE_T_FLOAT, NATIVE_FIXED(deg_params));

    TypeKind rad_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "rad", math_rad, &NATIVE_T_FLOAT, NATIVE_FIXED(rad_params));

    TypeKind sign_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "sign", math_sign, &NATIVE_T_FLOAT, NATIVE_FIXED(sign_params));

    // 新增工具函数
    TypeKind fmod_params[] = {TYPE_FLOAT, TYPE_FLOAT};
    native_register_module_method("maths", "fmod", math_fmod, &NATIVE_T_FLOAT, NATIVE_FIXED(fmod_params));

    TypeKind clamp_params[] = {TYPE_FLOAT, TYPE_FLOAT, TYPE_FLOAT};
    native_register_module_method("maths", "clamp", math_clamp, &NATIVE_T_FLOAT, NATIVE_FIXED(clamp_params));

    // ---- 整数版（2026-11-01，见文件上方"整数版工具"的说明）----
    TypeKind roundi_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "roundi", math_roundi, &NATIVE_T_INT, NATIVE_FIXED(roundi_params));

    native_register_module_method("maths", "mini", math_mini, &NATIVE_T_INT, NATIVE_VARARG(1, NATIVE_ARITY_ANY, 0, NULL, TYPE_FLOAT));
    native_register_module_method("maths", "maxi", math_maxi, &NATIVE_T_INT, NATIVE_VARARG(1, NATIVE_ARITY_ANY, 0, NULL, TYPE_FLOAT));

    TypeKind clampi_params[] = {TYPE_FLOAT, TYPE_FLOAT, TYPE_FLOAT};
    native_register_module_method("maths", "clampi", math_clampi, &NATIVE_T_INT, NATIVE_FIXED(clampi_params));

    TypeKind mod_params[] = {TYPE_FLOAT, TYPE_FLOAT};
    native_register_module_method("maths", "mod", math_mod, &NATIVE_T_INT, NATIVE_FIXED(mod_params));

    // ---- 归一化 / 区间映射 ----
    TypeKind saturate_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "saturate", math_saturate, &NATIVE_T_FLOAT, NATIVE_FIXED(saturate_params));

    TypeKind inverse_lerp_params[] = {TYPE_FLOAT, TYPE_FLOAT, TYPE_FLOAT};
    native_register_module_method("maths", "inverse_lerp", math_inverse_lerp, &NATIVE_T_FLOAT, NATIVE_FIXED(inverse_lerp_params));

    TypeKind remap_params[] = {TYPE_FLOAT, TYPE_FLOAT, TYPE_FLOAT, TYPE_FLOAT, TYPE_FLOAT};
    native_register_module_method("maths", "remap", math_remap, &NATIVE_T_FLOAT, NATIVE_FIXED(remap_params));

    TypeKind wrap_params[] = {TYPE_FLOAT, TYPE_FLOAT, TYPE_FLOAT};
    native_register_module_method("maths", "wrap", math_wrap, &NATIVE_T_FLOAT, NATIVE_FIXED(wrap_params));

    // ---- 缓动曲线（2026-11-01，见文件里"缓动曲线"的说明）----
    TypeKind ease1_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "smootherstep", math_smootherstep, &NATIVE_T_FLOAT, NATIVE_FIXED(ease1_params));
    native_register_module_method("maths", "ease_in_quad", math_ease_in_quad, &NATIVE_T_FLOAT, NATIVE_FIXED(ease1_params));
    native_register_module_method("maths", "ease_out_quad", math_ease_out_quad, &NATIVE_T_FLOAT, NATIVE_FIXED(ease1_params));
    native_register_module_method("maths", "ease_in_out_quad", math_ease_in_out_quad, &NATIVE_T_FLOAT, NATIVE_FIXED(ease1_params));
    native_register_module_method("maths", "ease_in_cubic", math_ease_in_cubic, &NATIVE_T_FLOAT, NATIVE_FIXED(ease1_params));
    native_register_module_method("maths", "ease_out_cubic", math_ease_out_cubic, &NATIVE_T_FLOAT, NATIVE_FIXED(ease1_params));
    native_register_module_method("maths", "ease_in_out_cubic", math_ease_in_out_cubic, &NATIVE_T_FLOAT, NATIVE_FIXED(ease1_params));
    // 第 2 参可选（回弹幅度 k，默认 1.70158）
    native_register_module_method("maths", "ease_out_back", math_ease_out_back, &NATIVE_T_FLOAT, NATIVE_VARARG(1, 2, 0, NULL, TYPE_FLOAT));

    // ---- 角度工具 ----
    TypeKind angle_delta_params[] = {TYPE_FLOAT, TYPE_FLOAT};
    native_register_module_method("maths", "angle_delta", math_angle_delta, &NATIVE_T_FLOAT, NATIVE_FIXED(angle_delta_params));
    TypeKind lerp_angle_params[] = {TYPE_FLOAT, TYPE_FLOAT, TYPE_FLOAT};
    native_register_module_method("maths", "lerp_angle", math_lerp_angle, &NATIVE_T_FLOAT, NATIVE_FIXED(lerp_angle_params));
    native_register_module_method("maths", "wrap_angle", math_wrap_angle, &NATIVE_T_FLOAT, NATIVE_FIXED(ease1_params));

    // ---- 数值小工具 ----
    native_register_module_method("maths", "fract", math_fract, &NATIVE_T_FLOAT, NATIVE_FIXED(ease1_params));
    TypeKind step_params[] = {TYPE_FLOAT, TYPE_FLOAT};
    native_register_module_method("maths", "step", math_step, &NATIVE_T_FLOAT, NATIVE_FIXED(step_params));
    native_register_module_method("maths", "snap", math_snap, &NATIVE_T_FLOAT, NATIVE_FIXED(step_params));
    // eps 可选（默认 1e-6）
    native_register_module_method("maths", "approx_eq", math_approx_eq, &NATIVE_T_BOOL, NATIVE_VARARG(2, 3, 0, NULL, TYPE_FLOAT));
    TypeKind dist_params[] = {TYPE_FLOAT, TYPE_FLOAT, TYPE_FLOAT, TYPE_FLOAT};
    native_register_module_method("maths", "dist", math_dist, &NATIVE_T_FLOAT, NATIVE_FIXED(dist_params));
    native_register_module_method("maths", "is_pow2", math_is_pow2, &NATIVE_T_BOOL, NATIVE_FIXED(ease1_params));
    native_register_module_method("maths", "next_pow2", math_next_pow2, &NATIVE_T_INT, NATIVE_FIXED(ease1_params));

    TypeKind lerp_params[] = {TYPE_FLOAT, TYPE_FLOAT, TYPE_FLOAT};
    native_register_module_method("maths", "lerp", math_lerp, &NATIVE_T_FLOAT, NATIVE_FIXED(lerp_params));

    TypeKind rsqrt_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "rsqrt", math_rsqrt, &NATIVE_T_FLOAT, NATIVE_FIXED(rsqrt_params));

    // 动画缓动：3t²-2t³（0..1，t 自动夹取）—— 见 math_smoothstep 的注释 ✓
    TypeKind smoothstep_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "smoothstep", math_smoothstep, &NATIVE_T_FLOAT, NATIVE_FIXED(smoothstep_params));

    TypeKind hypot_params[] = {TYPE_FLOAT, TYPE_FLOAT};
    native_register_module_method("maths", "hypot", math_hypot, &NATIVE_T_FLOAT, NATIVE_FIXED(hypot_params));

    TypeKind log2_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "log2", math_log2, &NATIVE_T_FLOAT, NATIVE_FIXED(log2_params));

    TypeKind sin_fast_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "sin_fast", math_sin_fast, &NATIVE_T_FLOAT, NATIVE_FIXED(sin_fast_params));

    TypeKind cos_fast_params[] = {TYPE_FLOAT};
    native_register_module_method("maths", "cos_fast", math_cos_fast, &NATIVE_T_FLOAT, NATIVE_FIXED(cos_fast_params));

    // 常量
    native_register_module_method("maths", "pi", math_pi, &NATIVE_T_FLOAT, NATIVE_FIXED_NONE(0));

    native_register_module_method("maths", "e", math_e, &NATIVE_T_FLOAT, NATIVE_FIXED_NONE(0));
}

void maths_init_instance_methods(void) {
    number_init_methods();
    
    // 基本运算
    TypeKind one_params[] = {TYPE_FLOAT};

    number_register_method("sqrt", make_native(math_sqrt, 1, "sqrt"), TYPE_FLOAT, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    number_register_method("abs", make_native(math_abs, 1, "abs"), TYPE_FLOAT, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    number_register_method("pow", make_native(math_pow, 2, "pow"), TYPE_FLOAT, TYPE_UNKNOWN, NATIVE_FIXED(one_params));

    // 取整函数
    number_register_method("round", make_native(math_round, 1, "round"), TYPE_FLOAT, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    number_register_method("ceil", make_native(math_ceil, 1, "ceil"), TYPE_FLOAT, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    number_register_method("floor", make_native(math_floor, 1, "floor"), TYPE_FLOAT, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    number_register_method("trunc", make_native(math_trunc, 1, "trunc"), TYPE_FLOAT, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));

    // 三角函数
    number_register_method("cos", make_native(math_cos, 1, "cos"), TYPE_FLOAT, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    number_register_method("sin", make_native(math_sin, 1, "sin"), TYPE_FLOAT, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    number_register_method("tan", make_native(math_tan, 1, "tan"), TYPE_FLOAT, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    number_register_method("asin", make_native(math_asin, 1, "asin"), TYPE_FLOAT, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    number_register_method("acos", make_native(math_acos, 1, "acos"), TYPE_FLOAT, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    number_register_method("atan", make_native(math_atan, 1, "atan"), TYPE_FLOAT, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    number_register_method("atan2", make_native(math_atan2, 2, "atan2"), TYPE_FLOAT, TYPE_UNKNOWN, NATIVE_FIXED(one_params));

    // 对数和指数
    number_register_method("log", make_native(math_log, 1, "log"), TYPE_FLOAT, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    number_register_method("log10", make_native(math_log10, 1, "log10"), TYPE_FLOAT, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    number_register_method("exp", make_native(math_exp, 1, "exp"), TYPE_FLOAT, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));

    // 工具函数
    number_register_method("deg", make_native(math_deg, 1, "deg"), TYPE_FLOAT, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    number_register_method("rad", make_native(math_rad, 1, "rad"), TYPE_FLOAT, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    number_register_method("sign", make_native(math_sign, 1, "sign"), TYPE_FLOAT, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    number_register_method("fmod", make_native(math_fmod, 2, "fmod"), TYPE_FLOAT, TYPE_UNKNOWN, NATIVE_FIXED(one_params));
    // ⚠ 这两个是**两个**实参（非接收者 2 个）：旧注册写 `2, -1, -1` 却给了 1 元素的 `one_params`
    //   ⇒ 老代码按 arity 复制 2 项 = 越界读第 2 项（UB，恰好在"值"上没炸）✗。
    //   统一成 NATIVE_FIXED(types) 后 arity 由表长推出 ⇒ 必须给**长度正确**的表 ✓
    TypeKind two_params[] = {TYPE_FLOAT, TYPE_FLOAT};
    number_register_method("clamp", make_native(math_clamp, 3, "clamp"), TYPE_FLOAT, TYPE_UNKNOWN, NATIVE_FIXED(two_params));
    number_register_method("lerp", make_native(math_lerp, 3, "lerp"), TYPE_FLOAT, TYPE_UNKNOWN, NATIVE_FIXED(two_params));
    number_register_method("rsqrt", make_native(math_rsqrt, 1, "rsqrt"), TYPE_FLOAT, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    // 数字上的方法形式：`t.smoothstep()`（与 `maths.smoothstep(t)` 同一个实现 ✓）
    number_register_method("smoothstep", make_native(math_smoothstep, 1, "smoothstep"), TYPE_FLOAT, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    number_register_method("hypot", make_native(math_hypot, 2, "hypot"), TYPE_FLOAT, TYPE_UNKNOWN, NATIVE_FIXED(one_params));
    number_register_method("log2", make_native(math_log2, 1, "log2"), TYPE_FLOAT, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    number_register_method("sin_fast", make_native(math_sin_fast, 1, "sin_fast"), TYPE_FLOAT, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    number_register_method("cos_fast", make_native(math_cos_fast, 1, "cos_fast"), TYPE_FLOAT, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
}
