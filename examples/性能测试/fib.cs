// fib.cs — fib.leno 的 C# 对照实现（.NET）
//
// 语义对齐：Leno 的 int 是 int48，这里用 long（64 位有符号）。fib(32) 的结果
// 2178309 远在 int48 范围内，位宽不影响结果。
//
// N 与 fib.leno / fib.py 保持一致（都是 32）。
//
// 运行（.NET SDK 10，文件级程序）：
//   dotnet run -c Release fib.cs
// 计时用 Stopwatch（墙钟毫秒，与 Leno 的 times.ms() 同一量纲）。
//
// AggressiveOptimization：跳过 .NET 的分层 JIT（tier0 先跑、之后才升 tier1），
// 让被测递归一开始就在优化后的代码上跑 —— 否则前若干轮会被 tier0 拖慢。
//
// 注意：递归 fib 是「调用帧建立 + 整数算术」基准。fib 是自递归，JIT 无法把它
// 内联成一个循环，所以衡量的正是帧成本本身 —— 与 fib.leno 同口径。
using System;
using System.Diagnostics;
using System.Runtime.CompilerServices;

class BenchFib
{
    [MethodImpl(MethodImplOptions.AggressiveOptimization)]
    static long Fib(long n)
    {
        if (n < 2)
        {
            return n;
        }
        return Fib(n - 1) + Fib(n - 2);
    }

    static void Main()
    {
        const long n = 32;

        // 预热：确保 Fib 已在优化后的代码上（AggressiveOptimization 下可省，留着更稳）
        Fib(30);

        var sw = Stopwatch.StartNew();
        long r = Fib(n);
        sw.Stop();

        Console.WriteLine("fib(" + n + ") = " + r + ", 耗时 " + sw.ElapsedMilliseconds + "ms");
    }
}
