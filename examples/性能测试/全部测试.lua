local csnum = 10000000

local function ms()
    return os.clock() * 1000
end

-- ==================== 辅助函数 ====================

local function myfunc()
end

local function test1()
    print("======== LenoC VM 性能基准测试 ========")
    print("")

    -- 基础运算
    print("--- 基础运算 ---")
    local i = 0
    local t1 = ms()
    for _ = 1, csnum do
        i = i + 1
    end
    local t2 = ms()
    local elapsed = t2 - t1
    print(csnum .. "次 i++: " .. elapsed .. "ms")

    -- 算术运算
    local a = 1
    local b = 2
    local c = 0
    t1 = ms()
    for _ = 1, csnum do
        c = a + b
    end
    t2 = ms()
    elapsed = t2 - t1
    print(csnum .. "次加法: " .. elapsed .. "ms")

    -- 函数调用
    print("")
    print("--- 函数调用 ---")
    t1 = ms()
    for _ = 1, csnum do
        myfunc()
    end
    t2 = ms()
    elapsed = t2 - t1
    print(csnum .. "次空函数调用: " .. elapsed .. "ms")

    -- 数组操作
    print("")
    print("--- 数组操作 ---")
    local arr = {}
    t1 = ms()
    for _ = 1, csnum do
        table.insert(arr, 1)
    end
    t2 = ms()
    elapsed = t2 - t1
    print(csnum .. "次 arr.add(): " .. elapsed .. "ms")

    -- 数组访问
    local arr1 = {}
    for _ = 1, 1000 do
        table.insert(arr1, 1)
    end
    local k = 0
    t1 = ms()
    for _ = 1, csnum do
        k = arr1[1]
    end
    t2 = ms()
    elapsed = t2 - t1
    print(csnum .. "次 arr[index]: " .. elapsed .. "ms")

    -- 字典操作
    print("")
    print("--- 字典操作 ---")
    local d = {}
    t1 = ms()
    for _ = 1, csnum do
        d["key"] = 1
    end
    t2 = ms()
    elapsed = t2 - t1
    print(csnum .. "次 dict[key]=value: " .. elapsed .. "ms")

    print("")
    print("======== 下一个测试 ========")
end

local function no_try_test()
    local sum = 0
    for _ = 1, csnum do
        sum = sum + 1
    end
    return sum
end

local function with_try_test()
    local sum = 0
    for _ = 1, csnum do
        local ok, err = pcall(function()
            sum = sum + 1
        end)
    end
    return sum
end

local function trytest()
    print("")
    print("======== try 测试 ========")
    local t1 = ms()
    local d1 = no_try_test()
    local t2 = ms()
    print(" " .. (t2 - t1) .. "ms 结果:" .. d1)

    local t3 = ms()
    local d2 = with_try_test()
    local t4 = ms()
    print(" " .. (t4 - t3) .. "ms 结果:" .. d2)
    print("======== 下一个测试 ========")
end

local function whileandfor()
    print("======== While vs For 性能对比 ========")
    print("")

    -- 测试1: 纯空循环对比
    print("--- 测试1:" .. csnum .. "次 空循环 ---")

    local t1 = ms()
    for _ = 1, csnum do
    end
    local t2 = ms()
    local for_empty = t2 - t1
    print("for N {}: " .. for_empty .. "ms")

    t1 = ms()
    local i = 0
    while i < csnum do
        i = i + 1
    end
    t2 = ms()
    local while_empty = t2 - t1
    print("while i < N: " .. while_empty .. "ms")
    print("比值: for/while = " .. (for_empty * 100 / while_empty) .. "%")
    print("")

    -- 测试2: 带简单操作的循环
    print("--- 测试2: " .. csnum .. "次 i++ ---")

    t1 = ms()
    for _ = 1, csnum do
        i = i + 1
    end
    t2 = ms()
    local for_inc = t2 - t1
    print("for N: " .. for_inc .. "ms")

    t1 = ms()
    i = 0
    while i < csnum do
        i = i + 1
    end
    t2 = ms()
    local while_inc = t2 - t1
    print("while i < N: " .. while_inc .. "ms")
    print("比值: for/while = " .. (for_inc * 100 / while_inc) .. "%")
    print("")

    -- 测试3: 带循环变量的 for vs while
    print("--- 测试3: for to var vs while (带循环变量访问) ---")

    t1 = ms()
    local sum = 0
    for j = 1, csnum do
        sum = j
    end
    t2 = ms()
    local for_var = t2 - t1
    print("for N to j: " .. for_var .. "ms")

    t1 = ms()
    local jj = 0
    while jj < csnum do
        jj = jj + 1
    end
    t2 = ms()
    local while_var = t2 - t1
    print("while i < N: " .. while_var .. "ms")
    print("比值: for/while = " .. (for_var * 100 / while_var) .. "%")
    print("")

    -- 测试4: 数组操作场景
    print("--- 测试4: 数组添加操作 ---")

    local arr = {}
    t1 = ms()
    for _ = 1, csnum do
        table.insert(arr, 1)
    end
    t2 = ms()
    local for_arr = t2 - t1
    print("for N: " .. for_arr .. "ms, 长度: " .. #arr)

    local arr1 = {}
    t1 = ms()
    i = 0
    while i < csnum do
        table.insert(arr1, 1)
        i = i + 1
    end
    t2 = ms()
    local while_arr = t2 - t1
    print("while i < N: " .. while_arr .. "ms, 长度: " .. #arr1)
    print("比值: for/while = " .. (for_arr * 100 / while_arr) .. "%")
    print("")

    -- 测试5: for 步长2 (各自最自然的写法)
    print("--- 测试5: for 步长2 (各自最自然的写法) ---")

    local arr2 = {}
    t1 = ms()
    for k = 0, csnum - 1, 2 do
        table.insert(arr2, k)
    end
    t2 = ms()
    local for_step = t2 - t1
    print("for 0:csnum:2 to k: " .. for_step .. "ms, 长度: " .. #arr2)

    print("")

    -- 测试5b: while 步长2 (完全公平对比)
    print("--- 测试5b: while 步长2 (完全公平对比) ---")

    local arr3 = {}
    t1 = ms()
    local kk = 0
    while kk < csnum do
        kk = kk + 1
        table.insert(arr3, kk)
        kk = kk + 1
    end
    t2 = ms()
    local while_step = t2 - t1
    print("while + 两次i++: " .. while_step .. "ms, 长度: " .. #arr3)
    print("")

    -- 测试6: 嵌套循环场景
    print("--- 测试6: 嵌套循环 (1000 * 1000) ---")

    t1 = ms()
    local count = 0
    for _ = 1, 1000 do
        for _ = 1, 1000 do
            count = count + 1
        end
    end
    t2 = ms()
    local for_nested = t2 - t1
    print("for + for: " .. for_nested .. "ms, count=" .. count)

    count = 0
    t1 = ms()
    i = 0
    while i < 1000 do
        local m = 0
        while m < 1000 do
            count = count + 1
            m = m + 1
        end
        i = i + 1
    end
    t2 = ms()
    local while_nested = t2 - t1
    print("while + while: " .. while_nested .. "ms, count=" .. count)
    print("比值: for/while = " .. (for_nested * 100 / while_nested) .. "%")

    print("")
    print("======== 总结 ========")
end

-- 版本1：经典递归
local function fib_recursive(n)
    if n <= 1 then
        return n
    end
    return fib_recursive(n - 2) + fib_recursive(n - 1)
end

-- 版本2：记忆化递归
local function fib_memo_helper(n, memo)
    if n <= 1 then
        return n
    end
    local str = tostring(n)
    if memo[str] ~= nil then
        return memo[str]
    end
    local result = fib_memo_helper(n - 1, memo) + fib_memo_helper(n - 2, memo)
    memo[str] = result
    return result
end

local function fib_memo(n)
    local memo = {}
    return fib_memo_helper(n, memo)
end

-- 版本3：迭代实现
local function fib_iterative(n)
    if n <= 1 then
        return n
    end
    local a = 0
    local b = 1
    for i = 2, n do
        local temp = a + b
        a = b
        b = temp
    end
    return b
end

-- 版本4：尾递归优化
local function fib_tail_helper(n, a, b)
    if n == 0 then
        return a
    end
    if n == 1 then
        return b
    end
    return fib_tail_helper(n - 1, b, a + b)
end

local function fib_tail(n)
    return fib_tail_helper(n, 0, 1)
end

local function fibtest()
    local n = 30

    print("斐波那契数列优化对比 (n=" .. n .. "):")
    print("")

    -- 测试1：经典递归
    local t1 = ms()
    local r1 = fib_recursive(n)
    local t2 = ms()
    print("经典递归: " .. r1 .. "  耗时: " .. (t2 - t1) .. "ms")

    -- 测试2：记忆化递归
    local t3 = ms()
    local r2 = fib_memo(n)
    local t4 = ms()
    print("记忆化递归: " .. r2 .. "  耗时: " .. (t4 - t3) .. "ms")

    -- 测试3：迭代实现
    local t5 = ms()
    local r3 = fib_iterative(n)
    local t6 = ms()
    print("迭代实现: " .. r3 .. "  耗时: " .. (t6 - t5) .. "ms")

    -- 测试4：尾递归
    local t7 = ms()
    local r4 = fib_tail(n)
    local t8 = ms()
    print("尾递归: " .. r4 .. "  耗时: " .. (t8 - t7) .. "ms")

    print("")
    print("测试更大的数 (n=1000):")
    local t9 = ms()
    local r5 = fib_iterative(1000)
    local t10 = ms()
    print("迭代实现 fib(1000): " .. r5 .. "  耗时: " .. (t10 - t9) .. "ms")
end

-- ==================== main ====================

print("======== 算术运算与赋值性能分析 ========")
print("")

-- 测试1: 纯整数自增 (baseline)
print("--- 测试1: 纯整数自增 (i++) ---")
local t1 = ms()
local i = 0
for _ = 1, csnum do
    i = i + 1
end
local t2 = ms()
print(csnum .. "次 i++: " .. (t2 - t1) .. "ms")

-- 测试2: 局部变量赋值
print("")
print("--- 测试2: 局部变量赋值 (a = b) ---")
t1 = ms()
local a = 0
local b = 1
for _ = 1, csnum do
    a = b
end
t2 = ms()
print(csnum .. "次 a = b: " .. (t2 - t1) .. "ms")

-- 测试3: 常量赋值
print("")
print("--- 测试3: 常量赋值 (a = 1) ---")
t1 = ms()
a = 0
for _ = 1, csnum do
    a = 1
end
t2 = ms()
print(csnum .. "次 a = 1: " .. (t2 - t1) .. "ms")

-- 测试4: 简单加法 (两个常量)
print("")
print("--- 测试4: 加法 a = 1 + 2 ---")
t1 = ms()
a = 0
for _ = 1, csnum do
    a = 1 + 2
end
t2 = ms()
print(csnum .. "次 a = 1 + 2: " .. (t2 - t1) .. "ms")

-- 测试5: 变量加法
print("")
print("--- 测试5: 变量加法 a = b + c ---")
a = 0
b = 1
local c = 2
t1 = ms()
for _ = 1, csnum do
    a = b + c
end
t2 = ms()
print(csnum .. "次 a = b + c: " .. (t2 - t1) .. "ms")

-- 测试6: 复合加法赋值
print("")
print("--- 测试6: 复合加法 a += 1 ---")
a = 0
t1 = ms()
for _ = 1, csnum do
    a = a + 1
end
t2 = ms()
print(csnum .. "次 a += 1: " .. (t2 - t1) .. "ms")

-- 测试7: 自增 vs 加1 对比
print("")
print("--- 测试7: i++ vs i = i + 1 ---")
t1 = ms()
i = 0
for _ = 1, csnum do
    i = i + 1
end
t2 = ms()
local add1_time = t2 - t1
print(csnum .. "次 i = i + 1: " .. add1_time .. "ms")

t1 = ms()
i = 0
for _ = 1, csnum do
    i = i + 1
end
t2 = ms()
local inc_time = t2 - t1
print(csnum .. "次 i++: " .. inc_time .. "ms")
print("比值: i++ / (i=i+1) = " .. (inc_time * 100 / add1_time) .. "%")

-- 测试8: 乘法
print("")
print("--- 测试8: 乘法 a = b * c ---")
a = 0
b = 2
c = 3
t1 = ms()
for _ = 1, csnum do
    a = b * c
end
t2 = ms()
print(csnum .. "次 a = b * c: " .. (t2 - t1) .. "ms")

-- 测试9: 减法
print("")
print("--- 测试9: 减法 a = b - c ---")
a = 0
b = 10
c = 3
t1 = ms()
for _ = 1, csnum do
    a = b - c
end
t2 = ms()
print(csnum .. "次 a = b - c: " .. (t2 - t1) .. "ms")

-- 测试10: 连续操作
print("")
print("--- 测试10: 连续操作 a = b + c + d ---")
a = 0
b = 1
c = 2
local d = 3
t1 = ms()
for _ = 1, csnum do
    a = b + c + d
end
t2 = ms()
print(csnum .. "次 a = b + c + d: " .. (t2 - t1) .. "ms")

print("")
print("======== 下一个测试 ========")
test1()
trytest()
whileandfor()
fibtest()
io.read()