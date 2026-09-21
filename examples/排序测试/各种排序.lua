-- ============================================================================
-- 与 各种排序.leno **同结构、同算法**的 Lua 版（用于 Leno vs Lua 对照）
-- ----------------------------------------------------------------------------
-- 对照口径（与 .leno 版逐条对齐）：
--   * 同样 4 个算法：冒泡 / 原始快速排序（递归+频繁分配数组）/ 原地快速排序 /
--     优化快速排序（小数组 < 20 走插入排序 + 三数取中）
--   * 同样 5 个规模：2000 / 5000 / 20000 / 100000 / 400000
--   * 同样的跳过规则：冒泡只在 ≤5000、原始快排只在 ≤20000 跑
--   * 同样的计时口径：**同一进程内**依次测（避免跨时段 CPU 频率/热漂移），单位微秒
--     Leno 用 times.us()；Lua 用 os.clock()（CPU 时间）×1e6
--   * 同样的正确性校验（verify_sorted）
--   * 数据：Leno 是 rands.int_array(0, size)（size 个 [0,size) 的随机整数），
--     这里用固定种子的 math.random(0, size-1) —— 分布相同、规模相同；
--     具体取值不同（Lua 与 Leno 的 RNG 不同），比较型排序的工作量对随机数据是统计等价的。
--   * **算法工作量等价**：比较/交换次数、递归结构、阈值、枢轴选择方式都一致；
--     只做 1-based 下标等语言层面的适配，不改变算法本身。
-- ============================================================================

math.randomseed(42)

local function copy_array(a)
    local b = {}
    for i = 1, #a do b[i] = a[i] end
    return b
end

local function bubble_sort(a)
    local n = #a
    for i = 1, n - 1 do
        for j = 1, n - i do
            if a[j] > a[j + 1] then
                local temp = a[j]
                a[j] = a[j + 1]
                a[j + 1] = temp
            end
        end
    end
    return a
end

-- 原始快速排序（每层递归都新建 left/right 数组并逐个 append —— 与 .leno 版同写法）
local function quick_sort_original(a)
    if #a <= 1 then
        return a
    end
    local pivot = a[1]
    local left = {}
    local right = {}
    for i = 2, #a do
        if a[i] <= pivot then
            left[#left + 1] = a[i]
        else
            right[#right + 1] = a[i]
        end
    end
    local sorted_left = quick_sort_original(left)
    local sorted_right = quick_sort_original(right)
    local result = sorted_left
    result[#result + 1] = pivot
    for i = 1, #sorted_right do
        result[#result + 1] = sorted_right[i]
    end
    return result
end

local partition
local quick_sort_recursive
local quick_sort

quick_sort = function(a)
    return quick_sort_recursive(a, 1, #a)
end

quick_sort_recursive = function(a, left, right)
    if left >= right then return a end
    local pivot_index = partition(a, left, right)
    quick_sort_recursive(a, left, pivot_index - 1)
    quick_sort_recursive(a, pivot_index + 1, right)
    return a
end

partition = function(a, left, right)
    -- 选中间元素作为基准值，避免最坏情况
    local mid = math.floor((left + right) / 2)
    local pivot = a[mid]
    -- 把基准值换到最右边
    local temp = a[mid]
    a[mid] = a[right]
    a[right] = temp
    local i = left
    for j = left, right - 1 do
        if a[j] <= pivot then
            local t = a[i]
            a[i] = a[j]
            a[j] = t
            i = i + 1
        end
    end
    local t1 = a[i]
    a[i] = a[right]
    a[right] = t1
    return i
end

local insertion_sort_range

local partition_opt
local quick_sort_opt

local function quick_sort_optimized(a)
    return quick_sort_opt(a, 1, #a)
end

quick_sort_opt = function(a, left, right)
    -- 小数组用插入排序
    if right - left < 20 then
        return insertion_sort_range(a, left, right)
    end
    if left < right then
        local pivot_index = partition_opt(a, left, right)
        quick_sort_opt(a, left, pivot_index - 1)
        quick_sort_opt(a, pivot_index + 1, right)
    end
    return a
end

partition_opt = function(a, left, right)
    local mid = math.floor((left + right) / 2)
    -- 三数取中：保证 a[left] <= a[mid] <= a[right]
    if a[left] > a[mid] then
        local t = a[left]; a[left] = a[mid]; a[mid] = t
    end
    if a[mid] > a[right] then
        local t = a[mid]; a[mid] = a[right]; a[right] = t
    end
    if a[left] > a[mid] then
        local t = a[left]; a[left] = a[mid]; a[mid] = t
    end
    local pivot = a[mid]
    local temp = a[mid]
    a[mid] = a[right - 1]
    a[right - 1] = temp
    local i = left
    local j = right - 1
    while true do
        i = i + 1
        while a[i] < pivot do i = i + 1 end
        j = j - 1
        while a[j] > pivot do j = j - 1 end
        if i < j then
            local t = a[i]; a[i] = a[j]; a[j] = t
        else
            break
        end
    end
    local t1 = a[i]
    a[i] = a[right - 1]
    a[right - 1] = t1
    return i
end

insertion_sort_range = function(a, left, right)
    if left >= right then
        return a
    end
    for i = left + 1, right do
        local key = a[i]
        local j = i - 1
        while j >= left and a[j] > key do
            a[j + 1] = a[j]
            j = j - 1
        end
        a[j + 1] = key
    end
    return a
end

local function verify_sorted(a)
    for i = 1, #a - 1 do
        if a[i] > a[i + 1] then
            return false
        end
    end
    return true
end

-- ============================================================================
-- 测试驱动（与 .leno 版同一结构、同一输出格式）
-- ============================================================================
local test_cases = {
    { size = 2000,   name = "小规模" },
    { size = 5000,   name = "中规模" },
    { size = 20000,  name = "大规模" },
    { size = 100000, name = "超大规模" },
    { size = 400000, name = "巨大规模" },
}

local function now_us()
    return os.clock() * 1000000.0
end

print("========== 排序算法性能对比 ==========\n")

for _, test_case in ipairs(test_cases) do
    local size = test_case.size
    local name = test_case.name
    print(string.format("\n=== %s测试 (数组大小: %d) ===", name, size))

    -- 生成测试数据（size 个 [0, size-1] 随机整数；分布与 .leno 版一致）
    local original = {}
    for i = 1, size do original[i] = math.random(0, size - 1) end

    if size <= 5000 then
        local arr1 = copy_array(original)
        local s1 = now_us()
        local result1 = bubble_sort(arr1)
        local s2 = now_us()
        local us1 = math.floor(s2 - s1)
        print(string.format("冒泡排序: %d微秒 (正确性: %s)", us1, tostring(verify_sorted(result1))))
    end

    if size <= 20000 then
        local arr2 = copy_array(original)
        local s3 = now_us()
        local result2 = quick_sort_original(arr2)
        local s4 = now_us()
        local us2 = math.floor(s4 - s3)
        print(string.format("原始快速排序: %d微秒 (正确性: %s)", us2, tostring(verify_sorted(result2))))
    end

    local arr3 = copy_array(original)
    local s5 = now_us()
    local result3 = quick_sort(arr3)
    local s6 = now_us()
    local us3 = math.floor(s6 - s5)
    print(string.format("原地快速排序: %d微秒 (正确性: %s)", us3, tostring(verify_sorted(result3))))

    local arr4 = copy_array(original)
    local s7 = now_us()
    local result4 = quick_sort_optimized(arr4)
    local s8 = now_us()
    local us4 = math.floor(s8 - s7)
    print(string.format("优化快速排序: %d微秒 (正确性: %s)", us4, tostring(verify_sorted(result4))))

    if size > 5000 then
        print("注：冒泡排序 O(n²) 在此规模下过慢，已跳过测试")
    end
    if size > 20000 then
        print("注：原始快速排序（递归 + 频繁分配数组）在此规模下过慢，已跳过测试")
    end
end

print("\n========== 测试完成 ==========")
print("计时口径：同一进程内跑完全部规模，避免跨时段 CPU 频率/热漂移污染")
