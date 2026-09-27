# lines_check.py — 用 Python 复算 leno-lines.leno 的统计，与报告逐文件对账
# 用法: py lines_check.py [目录] [报告文件]
# 口径与 Leno 版一致：按 b"\n" 分割、结尾换行不算一行、空白行 = strip 后为空
import os, re, sys

root = sys.argv[1] if len(sys.argv) > 1 else "."
report = sys.argv[2] if len(sys.argv) > 2 else r"d:\CLeno\Leno\build\lines_report.txt"

def count_file(path):
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(b"\n")
    n = len(parts)
    if n > 0 and parts[-1] == b"":
        n -= 1
    blk = sum(1 for i in range(n) if parts[i].strip() == b"")
    return n, blk

stats = {}   # 归一化路径 -> (行数, 空行数)
groups = {}  # 扩展名 -> [文件数, 总行, 空行]
for dirpath, _, filenames in os.walk(root):
    for fn in filenames:
        ext = os.path.splitext(fn)[1].lower()
        if ext not in (".c", ".h", ".leno"):
            continue
        p = os.path.join(dirpath, fn)
        n, blk = count_file(p)
        key = os.path.relpath(p, root).replace("/", "\\")
        stats[key] = (n, blk)
        g = groups.setdefault(ext, [0, 0, 0])
        g[0] += 1; g[1] += n; g[2] += blk

print("== Python 复算 ==")
tc = tt = 0
for ext in (".c", ".h", ".leno"):
    c, t, b = groups.get(ext, [0, 0, 0])
    tc += c; tt += t
    print(f"{ext:6s}: {c} 文件, {t} 行 (空 {b}, 代码 {t - b})")
print(f"总计  : {tc} 文件, {tt} 行")

# ---- 与 leno 报告逐文件比对 ----
if os.path.exists(report):
    with open(report, encoding="utf-8-sig") as f:
        rep_lines = f.read().splitlines()
    pat = re.compile(r"^\s*(\d+)\s\s(.+)$")   # "  " + 6宽行数 + "  " + 路径
    compared = bad = 0
    for l in rep_lines:
        m = pat.match(l)
        if not m:
            continue
        n_rep = int(m.group(1))
        key = m.group(2).strip()
        if key.startswith(".\\"):
            key = key[2:]
        if key not in stats:
            print(f"[缺失] 报告有而复算无: {key}")
            bad += 1
            continue
        compared += 1
        if stats[key][0] != n_rep:
            print(f"[不符] {key}: leno={n_rep} py={stats[key][0]}")
            bad += 1
    extra = len(stats) - (compared + bad)
    print(f"-- 逐文件比对: 核对 {compared} 个, 不符 {bad}, 复算多出 {max(extra,0)} 个(报告未含)")
