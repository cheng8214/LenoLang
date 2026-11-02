// leno_math_inline.h — maths 纯数值函数的**唯一实现**（2026-11-01）
// ----------------------------------------------------------------------------
// 为什么要有这个文件：
//   同一批函数（abs / clamp / min / max / lerp / smoothstep / saturate / fract /
//   roundi / clampi / mini / maxi / mod）现在有**两个入口**：
//     · native  ：`maths.c` 的 math_xxx（模块方法，走 native 调用协议）
//     · 内联    ：`OP_MATH_FAST`（codegen 把 `maths.clamp(...)` 直接编成一条指令）
//   两份逻辑各写一遍迟早漂移（改了一个忘了另一个，行为就分叉）。
//   ⇒ 纯计算**只在这里写一次**，两个入口各自只负责「取值」与「装 Value」。
//
// 命名约定：
//   _d 结尾 = 输入输出 double；_i 结尾 = 输入输出 int64_t。
//
// ⚠ 语义以本文件为准，改动时两个调用方都不用动。
//   · clamp：**不**处理 lo > hi（与既有 native 行为一致；lo>hi 时返回 lo）
//   · clampi：处理 hi < lo ⇒ 收成 lo（GUI 里上下界常由布局算出，写反了画错比崩了好）
//   · mod：**要求 b != 0**，除零由调用方报错（native 抛异常、VM 走 runtime error，文案各自不同）
// ============================================================================

#ifndef LENO_MATH_INLINE_H
#define LENO_MATH_INLINE_H

#include <math.h>
#include <stdint.h>

// ---- 浮点 ----

static inline double leno_math_abs(double v) {
    return fabs(v);
}

static inline double leno_math_clamp(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline double leno_math_min2(double a, double b) {
    return (a < b) ? a : b;
}

static inline double leno_math_max2(double a, double b) {
    return (a > b) ? a : b;
}

static inline double leno_math_lerp(double a, double b, double t) {
    return a + (b - a) * t;                       // 不夹 t（与 maths.lerp 一致）
}

// 3t²-2t³，入口先夹到 0..1（死区直接返回，省掉乘法）
static inline double leno_math_smoothstep(double t) {
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    return t * t * (3.0 - 2.0 * t);
}

static inline double leno_math_saturate(double v) {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

static inline double leno_math_fract(double v) {
    return v - floor(v);                          // 负数也落在 [0,1)
}

// 四舍五入到整数（远离 0）
//   注：调用方若能从 Value 认出「本来就是 int」，应直接走整数分支（省一次 double 往返，
//   也避免 >2^53 的整数在 double 里丢精度）；这里只负责 double → int64 的那一半。
static inline int64_t leno_math_roundi(double v) {
    return (int64_t)llround(v);
}

// ---- 整数 ----

static inline int64_t leno_math_absi(int64_t v) {
    return (v < 0) ? -v : v;
}

static inline int64_t leno_math_clampi(int64_t v, int64_t lo, int64_t hi) {
    if (hi < lo) hi = lo;                         // 边界写反 ⇒ 收成 lo（不抛异常）
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline int64_t leno_math_mini(int64_t a, int64_t b) {
    return (a < b) ? a : b;
}

static inline int64_t leno_math_maxi(int64_t a, int64_t b) {
    return (a > b) ? a : b;
}

// 正取模：结余符号跟**除数**走（b > 0 时返回 0..b-1）；b 必须非 0
static inline int64_t leno_math_mod(int64_t a, int64_t b) {
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}

#endif // LENO_MATH_INLINE_H
