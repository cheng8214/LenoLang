# bench_py.py — 与 bench_leno.leno / bench_node.js 同工作量的 CPython 对照基准
#
# 为什么 Python 比 Node 更有参考价值：CPython 也是**字节码解释器**（无 JIT），
# 和 LenoC 同一类实现，差距反映的是"VM 每指令开销"，而不是"解释器 vs JIT"。
import time
import json


def fib(n):
    return n if n < 2 else fib(n - 1) + fib(n - 2)


def ms(t0):
    return round((time.perf_counter() - t0) * 1000)


t = time.perf_counter()
s = 0
for i in range(20000000):
    s += i
print(f"int_loop_20m    = {ms(t)} ms   sum={s}")

t = time.perf_counter()
small = 0
for i in range(20000000):
    small += 1
print(f"int_loop_small  = {ms(t)} ms   sum={small}")

t = time.perf_counter()
fv = fib(32)
print(f"fib_32          = {ms(t)} ms   fib={fv}")

t = time.perf_counter()
arr = []
for i in range(1000000):
    arr.append(i)
s2 = 0
for v in arr:
    s2 += v
print(f"array_1m_forin  = {ms(t)} ms   sum={s2}")

t = time.perf_counter()
s2b = 0
for i in range(len(arr)):
    s2b += arr[i]
print(f"array_1m_index  = {ms(t)} ms   sum={s2b}")

t = time.perf_counter()
d = {}
for i in range(200000):
    d["k" + str(i)] = i
s3 = 0
for i in range(200000):
    s3 += d.get("k" + str(i), 0)
print(f"dict_200k       = {ms(t)} ms   sum={s3}")

t = time.perf_counter()
acc = ""
for i in range(20000):
    acc += "x"
print(f"str_concat_20k  = {ms(t)} ms   len={len(acc)}")

t = time.perf_counter()
parts = []
for i in range(20000):
    parts.append("x")
joined = "".join(parts)
print(f"str_join_20k    = {ms(t)} ms   len={len(joined)}")

t = time.perf_counter()
hits = 0
for i in range(10000):
    if "xx" in acc:
        hits += 1
print(f"str_find_10k    = {ms(t)} ms   hits={hits}")

t = time.perf_counter()
js = '{"a":[1,2,3,{"b":"text"}],"n":123.456}'
cnt = 0
for i in range(50000):
    o = json.loads(js)
    if isinstance(o, dict):
        cnt += 1
print(f"json_parse_50k  = {ms(t)} ms   cnt={cnt}")

t = time.perf_counter()
fs = 0.0
for i in range(20000000):
    fs += 0.5
print(f"float_loop_20m  = {ms(t)} ms   sum={fs}")
