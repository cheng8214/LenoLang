#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""校验"十进制文本 → bigint"（分治解析）与 Python 一致。

覆盖：400 位阈值两侧、9/10/18 位边界、全 9、1 后跟零、随机、负号、前导零。
做法：把十进制串**原文**塞进 Leno 源码，用 `_int(...)` 解析后再 `_str(...)` 打回，
与 Python 的 `str(int(s))` 逐字节比较。
"""
import random
import subprocess
import sys
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys, "set_int_max_str_digits"):
    sys.set_int_max_str_digits(0)

LENO = Path(r"d:\CLeno\Leno")
EXE = LENO / "build" / "leno.exe"
TMP = LENO / "build" / "_bigbench" / "parse_verify.leno"
TMP.parent.mkdir(parents=True, exist_ok=True)

random.seed(2026)
tokens = []
tokens.append(("9位", "999999999"))
tokens.append(("10位", "1234567890"))
tokens.append(("18位", "123456789012345678"))
tokens.append(("399位", "9" * 399))
tokens.append(("400位(阈值)", "9" * 400))
tokens.append(("401位", "9" * 401))
tokens.append(("617位", str((1 << 2048) - 1)))
tokens.append(("1后跟500零", "1" + "0" * 500))
tokens.append(("前导零", "0" * 50 + "123456789"))
for digits in (1000, 3010, 5000):
    v = random.getrandbits(int(digits * 3.32)) | (1 << int(digits * 3.32) - 1)
    tokens.append(("rand%d位" % digits, str(v)[:digits]))
tokens.append(("负数1000位", "-" + "7" * 1000))

lines = ["main() {"]
for idx, (tag, lit) in enumerate(tokens):
    lines.append('    var x%d = _int("%s")' % (idx, lit.replace("-", "", 1) if lit.startswith("-") else lit))
    if lit.startswith("-"):
        lines.append('    x%d = 0 - x%d' % (idx, idx))
    lines.append('    print("%d " + _str(x%d) + "\\n")' % (idx, idx))
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
for idx, (tag, lit) in enumerate(tokens):
    expect = str(int(lit))           # 顺手把前导零规范化（"000123" ⇒ "123"）
    g = got.get(idx)
    if g == expect:
        print("✅ %-10s %d 位" % (tag, len(expect)))
    else:
        bad += 1
        print("❌ %-10s Leno=%s 期望=%s" % (tag, (g or "<空>")[:40], expect[:40]))
print("\n共 %d 例，失败 %d" % (len(tokens), bad))
sys.exit(1 if bad else 0)
