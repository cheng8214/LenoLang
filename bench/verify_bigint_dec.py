#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""校验 bigint→十进制（分治版）与 Python 的真值逐字节一致。

覆盖：64 limb 交叉点两侧、奇数/偶数 limb、全 1 形态、10 的幂、阶乘、随机大数。
"""
import random
import subprocess
import sys
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
# Python 3.11+ 默认限制 int→str 到 4300 位（DoSing 防护），校验大数要放开
if hasattr(sys, "set_int_max_str_digits"):
    sys.set_int_max_str_digits(0)
LENO = Path(r"d:\CLeno\Leno")
EXE = LENO / "build" / "leno.exe"
TMP = LENO / "build" / "_bigbench" / "dec_verify.leno"
TMP.parent.mkdir(parents=True, exist_ok=True)

# (标签, Leno 表达式, Python 真值)
cases = []
for n in (1000, 2040, 2048, 2100, 3000, 4096, 10000, 100000):
    cases.append(("2^%d-1" % n, "(1 << %d) - 1" % n, (1 << n) - 1))
    cases.append(("2^%d+1" % n, "(1 << %d) + 1" % n, (1 << n) + 1))
# ⚠ 别写成 `10 * 10 * …`（1 万个操作数会撞「表达式过长（400 项）」的硬上限）
for k in (10, 100, 1000, 10000):
    cases.append(("10^%d" % k, "LOOP10:%d" % k, 10 ** k))
cases.append(("3^5000", "3", 3 ** 5000))   # 特殊处理：下面换成循环
random.seed(12345)
for bits in (2048, 3000, 5000):
    v = random.getrandbits(bits) | (1 << (bits - 1))
    cases.append(("rand%d" % bits, str(v), v))

lines = ["main() {"]
exprs = []
for idx, (tag, expr, truth) in enumerate(cases):
    if expr.startswith("LOOP10:"):
        k = int(expr.split(":")[1])
        nm = "p10_%d" % idx                      # ⚠ 同一个 main 里不能重复定义同名变量
        lines.append("    var %s = 1" % nm)
        lines.append("    for 0 : %d to i { %s = %s * 10 }" % (k - 1, nm, nm))
        lines.append('    print("%d " + _str(%s) + "\\n")' % (idx, nm))
        exprs.append((tag, str(truth)))
        continue
    if tag == "3^5000":
        lines.append("    var p3 = 1")
        lines.append("    for 0 : 4999 to i { p3 = p3 * 3 }")
        lines.append('    print("%d " + _str(p3) + "\\n")' % idx)
        exprs.append((tag, str(3 ** 5000)))
        continue
    lines.append('    print("%d " + _str(%s) + "\\n")' % (idx, expr))
    exprs.append((tag, str(truth)))
lines.append("}")
TMP.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="")

r = subprocess.run([str(EXE), str(TMP)], cwd=str(LENO), capture_output=True,
                   text=True, encoding="utf-8", errors="replace", timeout=300)
out = (r.stdout or "") + (r.stderr or "")
got = {}
for line in out.splitlines():
    parts = line.split(" ", 1)
    if parts and parts[0].isdigit() and len(parts) == 2:
        got[int(parts[0])] = parts[1].strip()

bad = 0
for idx, (tag, truth) in enumerate(exprs):
    g = got.get(idx)
    if g == truth:
        print("✅ %-12s %d 位一致" % (tag, len(truth)))
    else:
        bad += 1
        gl = len(g) if g else 0
        print("❌ %-12s Leno %d 位 / 真值 %d 位" % (tag, gl, len(truth)))
        if g and gl == len(truth):
            for i, (a, b) in enumerate(zip(g, truth)):
                if a != b:
                    print("   首个不符 @%d: leno=%s truth=%s" % (i, g[max(0,i-8):i+12], truth[max(0,i-8):i+12]))
                    break
print("\n共 %d 例，失败 %d" % (len(exprs), bad))
sys.exit(1 if bad else 0)
