#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Leno bigint vs Python int：同一负载、同一台机器、各跑一遍取墙钟时间。

设计原则：
  · 计时区间里**不做十进制转换**（除非该用例本身就是测转换），避免把印刷时间混进来
  · 两边用**完全相同的算式**与迭代次数
  · 每个用例先跑一次热身（只跑 1 次，避免 JIT/页命中差异——Leno 是解释器，无 JIT 影响）
"""
import subprocess
import sys
import time
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
LENO = Path(r"d:\CLeno\Leno")
EXE = LENO / "build" / "leno.exe"
DIR = LENO / "build" / "_bigbench"
DIR.mkdir(parents=True, exist_ok=True)

# (名字, 说明, Leno 代码体, Python 代码体)
CASES = [
    ("mul_100k", "10 万比特乘法 ×20",
     """    var a = (1 << 100000) - 1
    var b = (1 << 100000) + 1
    var c = 0
    for 0 : 19 to i {
        c = a * b
    }
    print(_str(c > 0))
""",
     """a = (1 << 100000) - 1
b = (1 << 100000) + 1
c = 0
for _ in range(20):
    c = a * b
print(c > 0)
"""),

    ("mul_1m", "100 万比特乘法 ×4",
     """    var a = (1 << 1000000) - 1
    var b = (1 << 1000000) + 1
    var c = 0
    for 0 : 3 to i {
        c = a * b
    }
    print(_str(c > 0))
""",
     """a = (1 << 1000000) - 1
b = (1 << 1000000) + 1
c = 0
for _ in range(4):
    c = a * b
print(c > 0)
"""),

    ("fact_20000", "阶乘 20000（2 万次小乘）",
     """    var f = 1
    for 1 : 20000 to i {
        f = f * i
    }
    print(_str(f > 0))
""",
     """f = 1
for i in range(1, 20001):
    f = f * i
print(f > 0)
"""),

    ("add_1m", "100 万比特加法 ×20000",
     """    var a = (1 << 1000000) - 1
    var s = 0
    for 0 : 19999 to i {
        s = s + a
    }
    print(_str(s > 0))
""",
     """a = (1 << 1000000) - 1
s = 0
for _ in range(20000):
    s = s + a
print(s > 0)
"""),

    ("shift_pow2", "2^1000000000 移位（十亿比特）",
     """    var p = 1 << 1000000000
    print(_str(p > 0))
""",
     """p = 1 << 1000000000
print(p > 0)
"""),

    ("dec_300k", "2^1000000 转十进制（30 万位）",
     """    var s = _str(1 << 1000000)
    print(_str(s.len()))
""",
     """s = str(1 << 1000000)
print(len(s))
"""),
]


def run(cmd, cwd, limit=600):
    t0 = time.perf_counter()
    try:
        r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=limit)
        dt = time.perf_counter() - t0
        return dt, (r.stdout or "").strip().splitlines()[-1:] or [""], r.returncode
    except subprocess.TimeoutExpired:
        return float("inf"), ["TIMEOUT"], -1


print("%-12s %-26s %10s %10s %8s" % ("用例", "说明", "Leno(s)", "Python(s)", "倍数"))
print("-" * 74)
rows = []
for name, desc, leno_body, py_body in CASES:
    lp = DIR / (name + ".leno")
    pp = DIR / (name + ".py")
    lp.write_text("main() {\n" + leno_body + "}\n", encoding="utf-8", newline="")
    pp.write_text(py_body, encoding="utf-8", newline="")

    run([str(EXE), str(lp)], str(LENO), 30)          # 热身（含编译）
    lt, lout, lrc = run([str(EXE), str(lp)], str(LENO))
    run([sys.executable, str(pp)], str(DIR), 30)
    pt, pout, prc = run([sys.executable, str(pp)], str(DIR))

    ratio = (lt / pt) if pt > 0 and lt != float("inf") else float("inf")
    print("%-12s %-26s %10.3f %10.3f %7.1fx" % (name, desc, lt, pt, ratio))
    sys.stdout.flush()
    rows.append((name, lt, pt, ratio, lout, lt))

print()
print("（倍数 = Leno 用时 / Python 用时，越大表示 Leno 越慢）")
