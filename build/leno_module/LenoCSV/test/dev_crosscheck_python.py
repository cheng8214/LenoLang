# dev_crosscheck_python.py —— 与 Python 标准库 `csv` 交叉验证（**开发期工具**）
#
# 为什么值得单独留一把尺子：CSV 的坑全在"引号/换行/逗号/空字段"的组合上，
# 自己写的解析器 + 自己的生成器容易"两边一起犯同一个错"，必须拿独立实现比对。
#
# 做法（双向）：
#   ① Python csv.writer 写一个含各种刁钻内容的文件 → LenoCSV 读进来再写出去
#      → 再用 Python csv.reader 读回，逐格比对 ⇒ 证明"读得对"
#   ② 同一个文件让 LenoCSV 直接解析（dev_roundtrip 内部就是 load） ⇒ 列数/行数一致
#
# 用法（仓库根目录）：
#     python build/leno_module/LenoCSV/test/dev_crosscheck_python.py
import csv
import os
import subprocess
import sys
import tempfile

# Windows 控制台默认 GBK：中文/箭头字符会 UnicodeEncodeError ⇒ 统一按 UTF-8 输出
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
EXE = os.path.join(ROOT, "build", "leno.exe")
SCRIPT = os.path.join(HERE, "dev_roundtrip.leno")

ROWS = [
    ["name", "note", "score"],
    ["Alice", 'has "quotes" inside', "1"],
    ["Bob", "comma, inside", "2"],
    ["Carol", "line1\nline2", "3"],
    ["张三", "中文、逗号，还有\"引号\"", "4"],
    ["", "", "5"],                      # 全空字段
    ["trailing space ", " leading space", "6"],
    ["emoji", "😀🚀", "7"],
]


def main():
    tmpdir = tempfile.mkdtemp(prefix="lenocsv_")
    py_out = os.path.join(tmpdir, "from_python.csv")
    leno_out = os.path.join(tmpdir, "from_leno.csv")

    # ① Python 生成（默认 CRLF + 最小引号）
    with open(py_out, "w", encoding="utf-8", newline="") as f:
        csv.writer(f, lineterminator="\r\n").writerows(ROWS)

    # ② LenoCSV 读 + 写
    r = subprocess.run([EXE, SCRIPT, py_out, leno_out], capture_output=True)
    out = r.stdout.decode("utf-8", "replace").strip()
    if not out.startswith("OK"):
        print("[FAIL] LenoCSV 往返失败：", out, r.stderr.decode("utf-8", "replace")[:300])
        return 1

    # ③ Python 读回 Leno 写出的文件，逐格比对
    with open(leno_out, "r", encoding="utf-8", newline="") as f:
        back = list(csv.reader(f))

    fails = 0
    if len(back) != len(ROWS):
        print(f"[FAIL] 行数：leno={len(back)} py={len(ROWS)}")
        fails += 1
    for i, (a, b) in enumerate(zip(ROWS, back)):
        if a != b:
            fails += 1
            print(f"[DIFF] 第 {i} 行\n  py  : {a!r}\n  leno: {b!r}")

    print(f"Python csv <-> LenoCSV：{len(ROWS)} 行，{'全一致' if fails == 0 else str(fails) + ' 处不一致'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
