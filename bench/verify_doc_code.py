#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""校验文档里的 ```leno 代码块能否编译/运行（比官方 ps1 版可靠：ps1 会把 stderr 当异常）。

分类输出（关键：把"片段"和"过期示例"分开）：
  · FRAGMENT —— 只报「未定义函数 / 未定义变量」⇒ 依赖上面块的上下文，无法单独跑
  · STALE    —— 报语法/语义错误 ⇒ 文档过期（语言演进了），需要改文档
  · CRASH    —— 非 0 退出且没有 error 行（段错误等）

用法：
    python bench/verify_doc_code.py                    # module_*.md
    python bench/verify_doc_code.py --glob "*.md"
    python bench/verify_doc_code.py --only stale       # 只看过期示例
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
LENO = Path(r"d:\CLeno\Leno")
EXE = LENO / "build" / "leno.exe"
DOCS = Path(r"d:\CLeno\LenoDocsSrc")

BLOCK = re.compile(r"```leno\r?\n(.*?)```", re.S)
SKIP_IMPORT = re.compile(r'import\s+"[^"]+\.leno"')
HAS_MAIN = re.compile(r"\bmain\s*\(")
FRAGMENT_ERR = re.compile(r"\[未定义函数\]|\[未定义变量\]|未定义的模块或变量")


def classify(out: str, rc: int):
    errs = re.findall(r"error: \[([^\]]+)\]", out)
    if rc == 0:
        return "ok", errs
    if not errs:
        return ("timeout" if rc == 124 else "crash"), errs
    if all(e in ("未定义函数", "未定义变量") for e in errs):
        return "fragment", errs
    return "stale", errs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--glob", default="module_*.md")
    ap.add_argument("--timeout", type=int, default=20)
    ap.add_argument("--only", default="", help="stale / fragment / crash")
    args = ap.parse_args()

    files = sorted(DOCS.glob(args.glob))
    if not files:
        print("没有匹配的文档：" + args.glob)
        return 2

    counts = {"ok": 0, "fragment": 0, "stale": 0, "crash": 0, "timeout": 0, "skip": 0}
    for md in files:
        blocks = BLOCK.findall(md.read_text(encoding="utf-8", errors="replace"))
        rows = []
        with tempfile.TemporaryDirectory(prefix="doccode_") as td:
            work = Path(td)
            for i, code in enumerate(blocks, 1):
                if not HAS_MAIN.search(code) or SKIP_IMPORT.search(code):
                    counts["skip"] += 1
                    continue
                f = work / ("blk%02d.leno" % i)
                f.write_text(code, encoding="utf-8", newline="")
                try:
                    r = subprocess.run([str(EXE), str(f)], cwd=str(work), capture_output=True,
                                       text=True, encoding="utf-8", errors="replace",
                                       timeout=args.timeout)
                    rc, out = r.returncode, ((r.stdout or "") + (r.stderr or ""))
                except subprocess.TimeoutExpired:
                    rc, out = 124, "TIMEOUT"
                kind, errs = classify(out, rc)
                counts[kind] = counts.get(kind, 0) + 1
                if kind != "ok":
                    first = ""
                    for line in out.splitlines():
                        if "error:" in line or "ERROR" in line:
                            first = line.strip()
                            break
                    rows.append((i, kind, errs[:2], first, code.strip().splitlines()[0][:70]))
        if rows:
            print("=" * 78)
            print(md.name)
            for (i, kind, errs, first, head) in rows:
                if args.only and kind != args.only:
                    continue
                print("  #%-3d %-8s %s" % (i, kind.upper(), head))
                if first:
                    print("        " + first.replace(str(md), "")[:170])

    print("-" * 78)
    print("ok=%d stale=%d fragment=%d crash=%d timeout=%d skip=%d"
          % (counts["ok"], counts["stale"], counts["fragment"],
             counts["crash"], counts["timeout"], counts["skip"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
