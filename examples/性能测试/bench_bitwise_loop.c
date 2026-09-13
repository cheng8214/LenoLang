/* bench_bitwise_loop.c — bench_bitwise_loop.leno 的 C 对照实现
 *
 * 语义对齐：Leno 的 int 是 int48（有符号 48 位），这里用 int64_t。本基准里值始终
 * 落在 0..1200（>>8 后 &7 夹到 0..7，再 ^1125 &1135 +77），所以位宽不影响结果，
 * 也不会有溢出/提升 BigInt 的差异。
 *
 * 编译/运行（mingw-w64）：
 *   gcc -O2 -o bench_bitwise_loop_c.exe bench_bitwise_loop.c ; bench_bitwise_loop_c.exe
 * 计时用 QueryPerformanceCounter（与 Leno 的 times.ms() 一样是墙钟毫秒）。
 */
#include <stdio.h>
#include <windows.h>

int main(void) {
    long long inp = 12345;
    const long long iterations = 2000000000LL;
    LARGE_INTEGER freq, t0, t1;
    long long i;

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    for (i = 0; i < iterations; i++) {
        inp = inp >> 8;
        inp = inp & 7;
        inp = 1125 ^ inp;
        inp = inp & 1135;
        inp = inp + 77;
    }

    QueryPerformanceCounter(&t1);
    double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;

    printf("结果: %lld\n", inp);
    printf("耗时: %.0f ms\n", ms);
    return 0;
}
