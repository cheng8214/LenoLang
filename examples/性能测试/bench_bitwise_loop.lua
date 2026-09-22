-- bench_bitwise_loop.lua —— 与 bench_bitwise_loop.leno 同口径的位运算循环
--   （对齐 Java 的位运算跑分测试：移位 / 与 / 异或 / 与 / 加，每轮 5 个整数操作）
--
-- 迭代次数取 **2 亿**：20 亿在 Lua 侧要跑一百多秒，对表没必要。
-- 轮数口径与 .leno 的 `for 0 : iterations to i` 严格对齐 —— Leno 的 `:` 是**闭区间**，
--   所以那边是 iterations + 1 轮；Lua 的数值 for `for i = 0, N` 同样是闭区间 ⇒ 两边一致。
--
-- ⚠ Lua 的位运算符与 C 不同：**异或是 `~`（不是 `^`）**，`^` 在 Lua 里是乘方。
--   `>>` 在 Lua 是逻辑右移、Leno 是算术右移 —— 本基准的 inp 全程为正，两者一致。

local inp = 12345
local iterations = 200000000  -- 2 亿次（含下标 0 ⇒ 实际 200000001 轮）

local start = os.clock() * 1000  -- 与其它 Lua 基准同口径：CPU 时间 × 1000 = 毫秒

for i = 0, iterations do
    inp = inp >> 8
    inp = inp & 7
    inp = 1125 ~ inp
    inp = inp & 1135
    inp = inp + 77
end

local finish = os.clock() * 1000
print("结果: " .. inp)
print("耗时: " .. (finish - start) .. " ms")
