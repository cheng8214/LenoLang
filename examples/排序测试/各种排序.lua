-- ============================================================================
-- 与 各种排序.leno **同结构、同算法**的 Lua 版（用于 Leno vs Lua 对照）
-- ----------------------------------------------------------------------------
-- 对照口径（与 .leno 版逐条对齐）：
--   * 同样 4 个算法：冒泡 / 原始快速排序（递归+频繁分配数组）/ 原地快速排序 /
--     优化快速排序（小数组 < 20 走插入排序 + 三数取中）
--   * 同样 5 个规模：2000 / 5000 / 20000 / 100000 / 400000
--   * 同样的跳过规则：冒泡只在 ≤5000、原始快排只在 ≤20000 跑
--   * 计时口径（**近似对齐**，2026-09-24 更正）：都在**同一进程内**依次测（避免跨时段 CPU
--     频率/热漂移）、单位微秒；但两者**时钟源不同** —— Leno 用 times.us()（单调**挂钟**，
--     Windows QPC），Lua 用 os.clock()（**进程 CPU 时间**，Windows 上粒度约 1ms）。
--     单线程 CPU 密集时两者接近，但不要当成严格同口径。
--   * 小规模取平均（2026-09-24 加）：每个算法按 **R 次重复取平均**，R 表两侧完全一致
--     （≤2000 → 20、≤5000 → 8、≤20000 → 2、其余 1）；复制数据的开销放在计时区之外。
--     不加这一层时 Lua 侧 2000 档会因 1ms 粒度报出 "0微秒"。
--     为了压掉粒度误差，小规模（≤2000 / ≤5000 / ≤20000）**两侧用同一张 R 表重复取平均**
--     （R = 20 / 8 / 2；复制数据的开销放在计时区之外），大规模 R = 1。
--   * 同样的正确性校验（verify_sorted）
--   * 数据（2026-09-24 更正）：Leno 的 rands.int_array(0, size) 是 **0..size 的无重复随机排列**
--     （元素个数 = size+1、全部互不相同），**不是** size 个可重复随机数。
--     这里改成同口径：`int_array(size)`（Fisher-Yates）洗出 0..size 的排列 —— 同样 size+1 个、
--     同样全不重复，固定种子 42。
--     具体取值不同（Lua 与 Leno 的 RNG 不同），但"均匀随机排列"这个分布在比较型排序下等价：
--     比较/交换次数只取决于元素的**相对大小关系**，与具体数值无关。
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
-- 生成 0..max 的随机排列（Fisher-Yates）——与 Leno 的 rands.int_array(0, size) 同口径：
-- 元素个数 = max+1、全部互不相同。
local function int_array(max)
    local a = {}
    for i = 0, max do a[i + 1] = i end
    for i = #a, 2, -1 do
        local j = math.random(1, i)
        a[i], a[j] = a[j], a[i]
    end
    return a
end

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

    -- 生成测试数据（0..size 的随机排列：元素个数 size+1、全部互不相同 —— 与 .leno 版同口径）
    local original = int_array(size)

    -- ★ 2026-09-24：小规模按 **R 次重复取平均** —— os.clock() 在本机粒度约 1ms，
    --   小规模的单次耗时会被量化（实测 2000 档原地快排报出 "0微秒"）。
    --   R 表与 .leno 版**完全一致**；复制数据的开销放在计时区之外（每次单独计时再求和）。
    local reps = 1
    if size <= 2000 then
        reps = 20
    elseif size <= 5000 then
        reps = 8
    elseif size <= 20000 then
        reps = 2
    end
    if reps > 1 then
        print(string.format("注：本规模每个算法重复 %d 次取平均（抵消时钟粒度）", reps))
    end

    if size <= 5000 then
        local total1, ok1 = 0.0, true
        for _ = 1, reps do
            local arr1 = copy_array(original)
            local s1 = now_us()
            local result1 = bubble_sort(arr1)
            local s2 = now_us()
            total1 = total1 + (s2 - s1)
            ok1 = verify_sorted(result1)
        end
        print(string.format("冒泡排序: %d微秒 (正确性: %s)", math.floor(total1 / reps), tostring(ok1)))
    end

    if size <= 20000 then
        local total2, ok2 = 0.0, true
        for _ = 1, reps do
            local arr2 = copy_array(original)
            local s3 = now_us()
            local result2 = quick_sort_original(arr2)
            local s4 = now_us()
            total2 = total2 + (s4 - s3)
            ok2 = verify_sorted(result2)
        end
        print(string.format("原始快速排序: %d微秒 (正确性: %s)", math.floor(total2 / reps), tostring(ok2)))
    end

    local total3, ok3 = 0.0, true
    for _ = 1, reps do
        local arr3 = copy_array(original)
        local s5 = now_us()
        local result3 = quick_sort(arr3)
        local s6 = now_us()
        total3 = total3 + (s6 - s5)
        ok3 = verify_sorted(result3)
    end
    print(string.format("原地快速排序: %d微秒 (正确性: %s)", math.floor(total3 / reps), tostring(ok3)))

    local total4, ok4 = 0.0, true
    for _ = 1, reps do
        local arr4 = copy_array(original)
        local s7 = now_us()
        local result4 = quick_sort_optimized(arr4)
        local s8 = now_us()
        total4 = total4 + (s8 - s7)
        ok4 = verify_sorted(result4)
    end
    print(string.format("优化快速排序: %d微秒 (正确性: %s)", math.floor(total4 / reps), tostring(ok4)))

    if size > 5000 then
        print("注：冒泡排序 O(n²) 在此规模下过慢，已跳过测试")
    end
    if size > 20000 then
        print("注：原始快速排序（递归 + 频繁分配数组）在此规模下过慢，已跳过测试")
    end
end

print("\n========== 测试完成 ==========")
print("计时口径：同一进程内跑完全部规模，避免跨时段 CPU 频率/热漂移污染")
