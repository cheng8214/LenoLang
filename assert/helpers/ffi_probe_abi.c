/* ffi_probe_abi.c —— FFI 调用桩的**边界与压力**探针（被调方）
 *
 * 用途：给 `assert/test_ffi_abi.leno` 当被调方。三个平台的 FFI 后端现在都是
 *   "按 ABI 精确分类 + 汇编调用桩"（Win64 / SysV / AAPCS64）⇒ 本探针**跨平台通用** ✓
 *   （测试侧按平台选 .dll / .so / .dylib，并用 gcc/cc 现编 ✓）
 *
 * 判据约定：多参数用例一律返回**逐参数的位掩码**（第 i 位 = 第 i+1 个参数不对）
 *   ⇒ 0 = 全部正确 ✓，而且能一眼看出**是哪一位**错了（比算校验和强得多 ✓）
 * 单值用例（返回/0 参数）直接返回可对照的常量。
 * 被调方另外往 stderr 打一行，便于"有没有被调到""实际收到什么"直接可见。
 *
 * 编译：Windows: gcc -shared -O2 -o ffi_probe_abi.dll ffi_probe_abi.c
 *      Linux/macOS: cc -shared -fPIC -O2 -o ffi_probe_abi.so ffi_probe_abi.c */
#include <stdio.h>
#include <stdint.h>

#if defined(_WIN32)
#  define PROBE_EXPORT __declspec(dllexport)
#else
#  define PROBE_EXPORT __attribute__((visibility("default")))
#endif

/* 对齐金丝雀只在 x86 上有意义：`movaps`（16 字节对齐存储）未对齐会**直接崩**，
 * 而 AArch64 的 NEON 存取不要求对齐 ⇒ 那边退化为参数值校验（见 sse_probe ✓） */
#if defined(__x86_64__) || defined(_M_X64)
#  include <emmintrin.h>
#  define PROBE_HAS_SSE 1
#else
#  define PROBE_HAS_SSE 0
#endif

static long long bite(long long mask, int bit, int ok) {
    return ok ? mask : (mask | (1LL << bit));
}

/* ================= 基础回归（曾经的三种坏法） ================= */
PROBE_EXPORT long long p_idid(int a, double b, int c, double d) {
    fprintf(stderr, "[DLL] p_idid: a=%d b=%g c=%d d=%g\n", a, b, c, d);
    return 123456789LL + (long long)a * 10000 + (long long)c * 100 + (long long)(b * 100) + (long long)d;
}
PROBE_EXPORT long long p_iFiF(int a, float b, int c, float d) {
    fprintf(stderr, "[DLL] p_iFiF: a=%d b=%g c=%d d=%g\n", a, b, c, d);
    return 123456789LL + (long long)a * 10000 + (long long)c * 100 + (long long)(b * 100) + (long long)d;
}
PROBE_EXPORT long long p_Fdii(float a, double b, int c, int d) {
    fprintf(stderr, "[DLL] p_Fdii: a=%g b=%g c=%d d=%d\n", a, b, c, d);
    return 999000000LL + (long long)(a * 10) + (long long)(b * 10) + (long long)c * 10 + (long long)d;
}
PROBE_EXPORT long long p_5iFiFi(int a, float b, int c, float d, int e) {
    fprintf(stderr, "[DLL] p_5iFiFi: a=%d b=%g c=%d d=%g e=%d\n", a, b, c, d, e);
    long long m = 0;
    m = bite(m, 0, a == 1);  m = bite(m, 1, b == 2.5f);
    m = bite(m, 2, c == 3);  m = bite(m, 3, d == 4.5f);
    m = bite(m, 4, e == 5);
    return m;   /* 0 = 五个参数全对 ✓ */
}

/* ================= 1. 0 参数：nstk==0 分支 ================= */
PROBE_EXPORT long long p_0(void) {
    fprintf(stderr, "[DLL] p_0 (无参数)\n");
    return 777;
}

/* ================= 2. 窄整型 / 全宽 返回值 ================= */
PROBE_EXPORT unsigned char p_ret_u8(void)  { return 200; }
PROBE_EXPORT signed char   p_ret_i8(void)  { return -100; }
PROBE_EXPORT unsigned short p_ret_u16(void){ return 60000; }
PROBE_EXPORT short         p_ret_i16(void){ return -30000; }
PROBE_EXPORT unsigned int  p_ret_u32(void){ return 4000000000u; }
PROBE_EXPORT int           p_ret_i32(void){ return -2000000000; }
PROBE_EXPORT long long     p_ret_i64(void){ return 0x123456789ABLL; }       /* 44 位 */
PROBE_EXPORT long long     p_ret_i64big(void){ return 0x123456789ABCDEFLL; } /* 57 位 ⇒ bigint */
PROBE_EXPORT int           p_ret_bool(void){ return 1; }

/* ================= 3. 窄整型作参数（4 槽 + 1 栈） ================= */
PROBE_EXPORT long long p_widths(unsigned char a, signed char b, unsigned short c, short d, unsigned int e) {
    fprintf(stderr, "[DLL] p_widths: a=%u b=%d c=%u d=%d e=%u\n", a, b, c, d, e);
    long long m = 0;
    m = bite(m, 0, a == 200); m = bite(m, 1, b == -100);
    m = bite(m, 2, c == 60000); m = bite(m, 3, d == -30000);
    m = bite(m, 4, e == 4000000000u);
    return m;
}

/* ================= 4. i64 全宽参数 ================= */
PROBE_EXPORT long long p_i64args(long long a, long long b) {
    fprintf(stderr, "[DLL] p_i64args: a=%lld b=%lld\n", a, b);
    long long m = 0;
    m = bite(m, 0, a == 0x123456789ABLL);
    m = bite(m, 1, b == -0x123456789ABLL);
    return m;
}

/* ================= 5. 寄存器/栈边界（位掩码判据） ================= */
PROBE_EXPORT long long p_4i(int a, int b, int c, int d) {
    long long m = 0;
    m = bite(m,0,a==1); m = bite(m,1,b==2); m = bite(m,2,c==3); m = bite(m,3,d==4);
    return m;
}
PROBE_EXPORT long long p_8i(int a, int b, int c, int d, int e, int f, int g, int h) {
    long long m = 0;
    m = bite(m,0,a==1); m = bite(m,1,b==2); m = bite(m,2,c==3); m = bite(m,3,d==4);
    m = bite(m,4,e==5); m = bite(m,5,f==6); m = bite(m,6,g==7); m = bite(m,7,h==8);
    return m;
}
PROBE_EXPORT long long p_12i(int a1,int a2,int a3,int a4,int a5,int a6,
                                      int a7,int a8,int a9,int a10,int a11,int a12) {
    long long m = 0;
    m = bite(m,0,a1==1);  m = bite(m,1,a2==2);  m = bite(m,2,a3==3);
    m = bite(m,3,a4==4);  m = bite(m,4,a5==5);  m = bite(m,5,a6==6);
    m = bite(m,6,a7==7);  m = bite(m,7,a8==8);  m = bite(m,8,a9==9);
    m = bite(m,9,a10==10); m = bite(m,10,a11==11); m = bite(m,11,a12==12);
    return m;
}
/* ★ 全浮点参数 + **整数返回**（最小复现：曾经从 XMM0 取返回值 ⇒ 拿到残留浮点垃圾）
 *   修复前它稳定返回"某个无意义但确定"的值，且给被调方加一行日志就换个值 ✗ */
PROBE_EXPORT long long p_dd_reti(double a, double b) {
    long long m = 0;
    m = bite(m,0,a==1.5); m = bite(m,1,b==2.5);
    return m;   /* 期望 0：两个参数都对，且返回值确实从 RAX 读到 ✓ */
}

/* 4 个浮点槽全占（f32） */
PROBE_EXPORT long long p_4f32(float a, float b, float c, float d) {
    long long m = 0;
    m = bite(m,0,a==1.5f); m = bite(m,1,b==2.5f); m = bite(m,2,c==3.5f); m = bite(m,3,d==4.5f);
    return m;
}
/* 5 个 f32：4 槽 + 1 栈 */
PROBE_EXPORT long long p_5f32(float a, float b, float c, float d, float e) {
    long long m = 0;
    m = bite(m,0,a==1.5f); m = bite(m,1,b==2.5f); m = bite(m,2,c==3.5f);
    m = bite(m,3,d==4.5f); m = bite(m,4,e==5.5f);
    return m;
}
/* 6 / 7 个 f64：4 槽 + 2 / 3 栈（覆盖 nstk 偶/奇两种对齐补位） */
PROBE_EXPORT long long p_6d(double a, double b, double c, double d, double e, double f) {
    long long m = 0;
    m = bite(m,0,a==1.5); m = bite(m,1,b==2.5); m = bite(m,2,c==3.5);
    m = bite(m,3,d==4.5); m = bite(m,4,e==5.5); m = bite(m,5,f==6.5);
    return m;
}
PROBE_EXPORT long long p_7d(double a, double b, double c, double d, double e, double f, double g) {
    long long m = 0;
    m = bite(m,0,a==1.5); m = bite(m,1,b==2.5); m = bite(m,2,c==3.5); m = bite(m,3,d==4.5);
    m = bite(m,4,e==5.5); m = bite(m,5,f==6.5); m = bite(m,6,g==7.5);
    return m;
}
/* 4 浮点槽 + 4 个整数**在栈上** */
PROBE_EXPORT long long p_4d_4i(double a, double b, double c, double d,
                                        int e, int f, int g, int h) {
    long long m = 0;
    m = bite(m,0,a==1.5); m = bite(m,1,b==2.5); m = bite(m,2,c==3.5); m = bite(m,3,d==4.5);
    m = bite(m,4,e==5); m = bite(m,5,f==6); m = bite(m,6,g==7); m = bite(m,7,h==8);
    return m;
}
/* 4 整数槽 + 4 个 f64**在栈上**（这条专门钉"浮点走栈"） */
PROBE_EXPORT long long p_4i_4d(int a, int b, int c, int d,
                                        double e, double f, double g, double h) {
    long long m = 0;
    m = bite(m,0,a==1); m = bite(m,1,b==2); m = bite(m,2,c==3); m = bite(m,3,d==4);
    m = bite(m,4,e==5.5); m = bite(m,5,f==6.5); m = bite(m,6,g==7.5); m = bite(m,7,h==8.5);
    return m;
}
/* f32 溢出到栈（第 5、6 个） */
PROBE_EXPORT long long p_mix_f32stack(int a, float b, float c, float d, float e, float f) {
    long long m = 0;
    m = bite(m,0,a==1); m = bite(m,1,b==2.5f); m = bite(m,2,c==3.5f);
    m = bite(m,3,d==4.5f); m = bite(m,4,e==5.5f); m = bite(m,5,f==6.5f);
    return m;
}
/* 8 参数混合（4 寄存器 + 4 栈，参数类型交错） */
PROBE_EXPORT long long p_8(int a, double b, int c, double d, int e, double f, int g, double h) {
    long long m = 0;
    m = bite(m,0,a==1); m = bite(m,1,b==2.5); m = bite(m,2,c==3); m = bite(m,3,d==4.5);
    m = bite(m,4,e==5); m = bite(m,5,f==6.5); m = bite(m,6,g==7); m = bite(m,7,h==8.5);
    return m;
}
/* 12 参数（上限）：6 整数 + 6 f64 ⇒ 4 寄存器槽 + 8 栈参数 */
PROBE_EXPORT long long p_12(int a1, double b1, int a2, double b2, int a3, double b3,
                                     int a4, double b4, int a5, double b5, int a6, double b6) {
    long long m = 0;
    m = bite(m,0,a1==1);  m = bite(m,1,b1==1.5); m = bite(m,2,a2==2);  m = bite(m,3,b2==2.5);
    m = bite(m,4,a3==3);  m = bite(m,5,b3==3.5); m = bite(m,6,a4==4);  m = bite(m,7,b4==4.5);
    m = bite(m,8,a5==5);  m = bite(m,9,b5==5.5); m = bite(m,10,a6==6); m = bite(m,11,b6==6.5);
    return m;
}
/* 指针形状 7 参数（SDL_RenderTextureRotated 那种） */
PROBE_EXPORT long long p_7mix(long long p1, long long p2, long long p3, long long p4,
                                       double ang, long long p5, int flip) {
    long long m = 0;
    m = bite(m,0,p1==11); m = bite(m,1,p2==22); m = bite(m,2,p3==33); m = bite(m,3,p4==44);
    m = bite(m,4,ang==55.5); m = bite(m,5,p5==66); m = bite(m,6,flip==7);
    return m;
}

/* ================= 6. 纯浮点（含 >6 个：旧实现静默返回 0） ================= */
PROBE_EXPORT double p_8d(double a, double b, double c, double d,
                                  double e, double f, double g, double h) {
    double v = a + b*10 + c*100 + d*1000 + e*10000 + f*100000 + g*1000000 + h*10000000;
    fprintf(stderr, "[DLL] p_8d: %g %g %g %g %g %g %g %g -> %g\n", a,b,c,d,e,f,g,h,v);
    return v;
}

/* ================= 7. f32 返回（XMM0 低 4 字节） ================= */
PROBE_EXPORT float p_retf32(int a, float b, int c, float d) {
    fprintf(stderr, "[DLL] p_retf32: a=%d b=%g c=%d d=%g\n", a, b, c, d);
    return b + d;   /* 2.5 + 4.5 = 7 */
}

/* ================= 8. 对齐金丝雀（3 种 nstk 奇偶） ================= */
/* 局部 __m128 + 16 字节对齐存储：rsp 未按 16 对齐 ⇒ movaps 直接崩 ✗ */
static long long sse_probe(const char* tag, int a, double b, int c, double d, int e, int n) {
    long long m = 0;
    fprintf(stderr, "[DLL] %s: a=%d b=%g c=%d d=%g e=%d (nstk=%d)\n", tag, a, b, c, d, e, n);
#if PROBE_HAS_SSE
    /* x86：局部 __m128 + 16 字节对齐存储（movaps）⇒ 调用者没把 rsp 摆到 16 对齐就**直接崩** ✓ */
    float arr[4];
    __m128 v = _mm_setr_ps((float)a, (float)b, (float)c, (float)d);
    _mm_store_ps(arr, _mm_add_ps(v, _mm_set1_ps(1.0f)));
    m = bite(m,0,arr[0]==2.0f); m = bite(m,1,arr[1]==3.0f);
    m = bite(m,2,arr[2]==4.0f); m = bite(m,3,arr[3]==5.0f);
    m = bite(m,4,e==5);
#else
    /* 非 x86（AArch64 等）：NEON 存取**不要求对齐** ⇒ 没有"未对齐就崩"的等价金丝雀 ✗
     *   ⇒ 这里退化为**参数值校验**；对齐由桩自身保证 + 别处人工复核 ⚠ */
    m = bite(m,0,a==1); m = bite(m,1,b==2.0);
    m = bite(m,2,c==3); m = bite(m,3,d==4.0);
    m = bite(m,4,e==5);
#endif
    return m;
}
PROBE_EXPORT long long p_sse_n1(int a, double b, int c, double d, int e) {
    return sse_probe("p_sse_n1", a, b, c, d, e, 1);
}
PROBE_EXPORT long long p_sse_n2(int a, double b, int c, double d, int e, double f) {
    long long m = sse_probe("p_sse_n2", a, b, c, d, e, 2);
    return bite(m, 5, f == 6.5);
}
PROBE_EXPORT long long p_sse_n3(int a, double b, int c, double d, int e, double f, int g) {
    long long m = sse_probe("p_sse_n3", a, b, c, d, e, 3);
    m = bite(m, 5, f == 6.5);
    m = bite(m, 6, g == 7);
    return m;
}

/* ================= 9. 回调（C → Leno）与重入 ================= */
typedef int (*LenoCbI)(int);
PROBE_EXPORT int p_invoke_cb(LenoCbI cb, int x) {
    int r = cb(x);
    fprintf(stderr, "[DLL] p_invoke_cb: cb(%d) = %d\n", x, r);
    return r;
}

/* ================= 10. 压力：混合签名循环调用 ================= */
/* 每次校验 5 个参数（2 整 2 浮 + 1 整）的槽位与值；返回位掩码 0=对 */
PROBE_EXPORT long long p_stress(int a, double b, int c, double d, int e) {
    long long m = 0;
    m = bite(m,0,b == (double)a);       /* 同值不同类：抓"浮点被当整数传" */
    m = bite(m,1,c == a + 1);           /* 抓顺序错位 */
    m = bite(m,2,d == (double)(a + 2));
    m = bite(m,3,e == a + 3);
    return m;
}
