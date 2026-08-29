import time

# 递归斐波那契：帧建立开销基准（对比 fib.leno）

def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

t1 = time.perf_counter()
r = fib(32)
t2 = time.perf_counter()
print(f"fib(32) = {r}, 耗时 {(t2 - t1) * 1000:.0f}ms")
