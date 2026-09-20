// bench_bitwise_loop.cs — bench_bitwise_loop.leno 的 C# 对照实现（.NET）
//
// 语义对齐：Leno 的 int 是 int48，这里用 long（64 位有符号）。本基准里值始终落在
// 0..1200（>>8 后 &7 夹到 0..7，再 ^1125 &1135 +77），位宽不影响结果。
// C# 的 >> 对 long 是算术右移，与 Leno 的 OP_SHR 一致。
//
// 运行（.NET SDK）：
//   dotnet run -c Release            （需一个引用本文件的 .csproj）
// 计时用 Stopwatch（墙钟毫秒，与 Leno 的 times.ms() 同一量纲）。
//
// AggressiveOptimization：跳过 .NET 的分层 JIT（tier0 先跑、之后才升 tier1），
// 让被测循环一开始就在优化后的代码上跑 —— 否则前若干轮会被 tier0 拖慢。
using System;
using System.Diagnostics;
using System.Runtime.CompilerServices;

class BenchBitwiseLoop
{
    [MethodImpl(MethodImplOptions.AggressiveOptimization)]
    static long Run(long iterations)
    {
        long inp = 12345;
        for (long i = 0; i < iterations; i++)
        {
            inp = inp >> 8;
            inp = inp & 7;
            inp = 1125 ^ inp;
            inp = inp & 1135;
            inp = inp + 77;
        }
        return inp;
    }

    static void Main()
    {
        const long iterations = 2000000000L;
        var sw = Stopwatch.StartNew();
        long inp = Run(iterations);
        sw.Stop();
        Console.WriteLine("结果: " + inp);
        Console.WriteLine("耗时: " + sw.ElapsedMilliseconds + " ms");
    }
}
