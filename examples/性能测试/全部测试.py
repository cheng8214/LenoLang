
import time

csnum = 10_000_000

def myfunc():
    pass

def no_try_test():
    sum_val = 0
    for _ in range(csnum):
        sum_val += 1
    return sum_val

def with_try_test():
    sum_val = 0
    for _ in range(csnum):
        try:
            sum_val += 1
        except Exception:
            pass
    return sum_val

def fib_recursive(n):
    if n <= 1:
        return n
    return fib_recursive(n - 2) + fib_recursive(n - 1)

def fib_memo_helper(n, memo):
    if n <= 1:
        return n
    if n in memo:
        return memo[n]
    result = fib_memo_helper(n - 1, memo) + fib_memo_helper(n - 2, memo)
    memo[n] = result
    return result

def fib_memo(n):
    memo = {}
    return fib_memo_helper(n, memo)

def fib_iterative(n):
    if n <= 1:
        return n
    a = 0
    b = 1
    for _ in range(2, n + 1):
        temp = a + b
        a = b
        b = temp
    return b

def fib_tail_helper(n, a, b):
    if n == 0:
        return a
    if n == 1:
        return b
    return fib_tail_helper(n - 1, b, a + b)

def fib_tail(n):
    return fib_tail_helper(n, 0, 1)

# ==================== 算术运算与赋值性能分析 ====================

print("======== 算术运算与赋值性能分析 ========")
print()

# 测试1: 纯整数自增
print("--- 测试1: 纯整数自增 (i++) ---")
t1 = time.time() * 1000
i = 0
for _ in range(csnum):
    i += 1
t2 = time.time() * 1000
print(f"{csnum}次 i++: {t2 - t1:.0f}ms")

# 测试2: 局部变量赋值
print()
print("--- 测试2: 局部变量赋值 (a = b) ---")
t1 = time.time() * 1000
a = 0
b = 1
for _ in range(csnum):
    a = b
t2 = time.time() * 1000
print(f"{csnum}次 a = b: {t2 - t1:.0f}ms")

# 测试3: 常量赋值
print()
print("--- 测试3: 常量赋值 (a = 1) ---")
t1 = time.time() * 1000
a = 0
for _ in range(csnum):
    a = 1
t2 = time.time() * 1000
print(f"{csnum}次 a = 1: {t2 - t1:.0f}ms")

# 测试4: 简单加法
print()
print("--- 测试4: 加法 a = 1 + 2 ---")
t1 = time.time() * 1000
a = 0
for _ in range(csnum):
    a = 1 + 2
t2 = time.time() * 1000
print(f"{csnum}次 a = 1 + 2: {t2 - t1:.0f}ms")

# 测试5: 变量加法
print()
print("--- 测试5: 变量加法 a = b + c ---")
a = 0
b = 1
c = 2
t1 = time.time() * 1000
for _ in range(csnum):
    a = b + c
t2 = time.time() * 1000
print(f"{csnum}次 a = b + c: {t2 - t1:.0f}ms")

# 测试6: 复合加法赋值
print()
print("--- 测试6: 复合加法 a += 1 ---")
a = 0
t1 = time.time() * 1000
for _ in range(csnum):
    a += 1
t2 = time.time() * 1000
print(f"{csnum}次 a += 1: {t2 - t1:.0f}ms")

# 测试7: 自增 vs 加1
print()
print("--- 测试7: i++ vs i = i + 1 ---")
t1 = time.time() * 1000
i = 0
for _ in range(csnum):
    i = i + 1
t2 = time.time() * 1000
add1_time = t2 - t1
print(f"{csnum}次 i = i + 1: {add1_time:.0f}ms")

t1 = time.time() * 1000
i = 0
for _ in range(csnum):
    i += 1
t2 = time.time() * 1000
inc_time = t2 - t1
print(f"{csnum}次 i++: {inc_time:.0f}ms")
print(f"比值: i++ / (i=i+1) = {inc_time * 100 / add1_time:.0f}%")

# 测试8: 乘法
print()
print("--- 测试8: 乘法 a = b * c ---")
a = 0
b = 2
c = 3
t1 = time.time() * 1000
for _ in range(csnum):
    a = b * c
t2 = time.time() * 1000
print(f"{csnum}次 a = b * c: {t2 - t1:.0f}ms")

# 测试9: 减法
print()
print("--- 测试9: 减法 a = b - c ---")
a = 0
b = 10
c = 3
t1 = time.time() * 1000
for _ in range(csnum):
    a = b - c
t2 = time.time() * 1000
print(f"{csnum}次 a = b - c: {t2 - t1:.0f}ms")

# 测试10: 连续操作
print()
print("--- 测试10: 连续操作 a = b + c + d ---")
a = 0
b = 1
c = 2
d = 3
t1 = time.time() * 1000
for _ in range(csnum):
    a = b + c + d
t2 = time.time() * 1000
print(f"{csnum}次 a = b + c + d: {t2 - t1:.0f}ms")

print()
print("======== 下一个测试=======")

# ==================== LenoC VM 性能基准测试 ====================

print("======== LenoC VM 性能基准测试 ========")
print()

# 基础运算
print("--- 基础运算 ---")
i = 0
t1 = time.time() * 1000
for _ in range(csnum):
    i += 1
t2 = time.time() * 1000
elapsed = t2 - t1
print(f"{csnum}次 i++: {elapsed:.0f}ms")

# 算术运算
a = 1
b = 2
c = 0
t1 = time.time() * 1000
for _ in range(csnum):
    c = a + b
t2 = time.time() * 1000
elapsed = t2 - t1
print(f"{csnum}次加法: {elapsed:.0f}ms")

# 函数调用
print()
print("--- 函数调用 ---")
t1 = time.time() * 1000
for _ in range(csnum):
    myfunc()
t2 = time.time() * 1000
elapsed = t2 - t1
print(f"{csnum}次空函数调用: {elapsed:.0f}ms")

# 数组操作
print()
print("--- 数组操作 ---")
arr = []
t1 = time.time() * 1000
for _ in range(csnum):
    arr.append(1)
t2 = time.time() * 1000
elapsed = t2 - t1
print(f"{csnum}次 arr.add(): {elapsed:.0f}ms")

# 数组访问
arr1 = [1] * 1000
t1 = time.time() * 1000
for _ in range(csnum):
    _ = arr1[0]
t2 = time.time() * 1000
elapsed = t2 - t1
print(f"{csnum}次 arr[index]: {elapsed:.0f}ms")

# 字典操作
print()
print("--- 字典操作 ---")
d = {}
t1 = time.time() * 1000
for _ in range(csnum):
    d["key"] = 1
t2 = time.time() * 1000
elapsed = t2 - t1
print(f"{csnum}次 dict[key]=value: {elapsed:.0f}ms")

print()
print("======== 下一个测试=======")

# ==================== try 测试 ====================

print()
print("======== try 测试=======")
t1 = time.time() * 1000
d1 = no_try_test()
t2 = time.time() * 1000
print(f" {t2 - t1:.0f}ms 结果:{d1}\n")
t3 = time.time() * 1000
d2 = with_try_test()
t4 = time.time() * 1000
print(f" {t4 - t3:.0f}ms 结果:{d2}\n")
print("======== 下一个测试=======")

# ==================== While vs For 性能对比 ====================

print("======== While vs For 性能对比 ========")
print()

# 测试1: 纯空循环对比
print(f"--- 测试1:{csnum}次 空循环 ---")

t1 = time.time() * 1000
for _ in range(csnum):
    pass
t2 = time.time() * 1000
for_empty = t2 - t1
print(f"for N: {for_empty:.0f}ms")

t1 = time.time() * 1000
i = 0
while i < csnum:
    i += 1
t2 = time.time() * 1000
while_empty = t2 - t1
print(f"while i < N: {while_empty:.0f}ms")
print(f"比值: for/while = {for_empty * 100 / while_empty:.0f}%")
print()

# 测试2: 带简单操作的循环
print("--- 测试2: 1000万次 i++ ---")

t1 = time.time() * 1000
for _ in range(csnum):
    i += 1
t2 = time.time() * 1000
for_inc = t2 - t1
print(f"for N: {for_inc:.0f}ms")

t1 = time.time() * 1000
i = 0
while i < csnum:
    i += 1
t2 = time.time() * 1000
while_inc = t2 - t1
print(f"while i < N: {while_inc:.0f}ms")
print(f"比值: for/while = {for_inc * 100 / while_inc:.0f}%")
print()

# 测试3: 带循环变量的 for vs while
print("--- 测试3: for to var vs while (带循环变量访问) ---")

t1 = time.time() * 1000
for j in range(csnum):
    j
t2 = time.time() * 1000
for_var = t2 - t1
print(f"for N to j: {for_var:.0f}ms")

t1 = time.time() * 1000
jj = 0
while jj < csnum:
    jj += 1
t2 = time.time() * 1000
while_var = t2 - t1
print(f"while i < N: {while_var:.0f}ms")
print(f"比值: for/while = {for_var * 100 / while_var:.0f}%")
print()

# 测试4: 数组操作场景
print("--- 测试4: 数组添加操作 ---")

arr = []
t1 = time.time() * 1000
for _ in range(csnum):
    arr.append(1)
t2 = time.time() * 1000
for_arr = t2 - t1
print(f"for N: {for_arr:.0f}ms, 长度: {len(arr)}")

arr1 = []
t1 = time.time() * 1000
i = 0
while i < csnum:
    arr1.append(1)
    i += 1
t2 = time.time() * 1000
while_arr = t2 - t1
print(f"while i < N: {while_arr:.0f}ms, 长度: {len(arr)}")
print(f"比值: for/while = {for_arr * 100 / while_arr:.0f}%")
print()

# 测试5: for 步长为2 (range 自带步长参数，与 Leno for 0:csnum-1:2 to k 语义等价)
print("--- 测试5: for 步长为2 (range(0, csnum, 2)) ---")

arr2 = []
t1 = time.time() * 1000
for k in range(0, csnum, 2):
    arr2.append(k)
t2 = time.time() * 1000
for_step = t2 - t1
print(f"for 步长2: {for_step:.0f}ms, 长度: {len(arr2)}")

# 测试5b: while 步长为2 (完全公平对比，体内两次自增)
print("--- 测试5b: while 步长为2 (两次i++) ---")

arr3 = []
t1 = time.time() * 1000
kk = 0
while kk < csnum:
    kk += 1
    arr3.append(kk)
    kk += 1
t2 = time.time() * 1000
while_step = t2 - t1
print(f"while 步长2: {while_step:.0f}ms, 长度: {len(arr3)}")
print()

# 测试6: 嵌套循环场景
print("--- 测试6: 嵌套循环 (1000 * 1000) ---")

t1 = time.time() * 1000
count = 0
for _ in range(1000):
    for _ in range(1000):
        count += 1
t2 = time.time() * 1000
for_nested = t2 - t1
print(f"for + for: {for_nested:.0f}ms, count={count}")

count = 0
t1 = time.time() * 1000
i = 0
while i < 1000:
    m = 0
    while m < 1000:
        count += 1
        m += 1
    i += 1
t2 = time.time() * 1000
while_nested = t2 - t1
print(f"while + while: {while_nested:.0f}ms, count={count}")
print(f"比值: for/while = {for_nested * 100 / while_nested:.0f}%")

print()
print("======== 总结 ========")

# ==================== 斐波那契测试 ====================

n = 30

print(f"\n斐波那契数列优化对比 (n={n}):\n")

# 测试1：经典递归
t1 = time.time() * 1000
r1 = fib_recursive(n)
t2 = time.time() * 1000
print(f"经典递归: {r1}  耗时: {t2 - t1:.0f}ms")

# 测试2：记忆化递归
t3 = time.time() * 1000
r2 = fib_memo(n)
t4 = time.time() * 1000
print(f"记忆化递归: {r2}  耗时: {t4 - t3:.0f}ms")

# 测试3：迭代实现
t5 = time.time() * 1000
r3 = fib_iterative(n)
t6 = time.time() * 1000
print(f"迭代实现: {r3}  耗时: {t6 - t5:.0f}ms")

# 测试4：尾递归
t7 = time.time() * 1000
r4 = fib_tail(n)
t8 = time.time() * 1000
print(f"尾递归: {r4}  耗时: {t8 - t7:.0f}ms")

print("\n测试更大的数 (n=1000):")
t9 = time.time() * 1000
r5 = fib_iterative(1000)
t10 = time.time() * 1000
print(f"迭代实现 fib(1000): {r5}  耗时: {t10 - t9:.0f}ms")
