#!/usr/bin/env python3
"""
LenoC 自动化测试运行器 (Python 版)

用法：python assert/run_tests.py [leno_exe] [test_dir]
默认：python assert/run_tests.py build/leno.exe assert

每个测试独立子进程，无内存污染、无并发冲突。
"""
import subprocess, os, sys, time

LENO  = sys.argv[1] if len(sys.argv) > 1 else "build/leno.exe"
TDIR  = sys.argv[2] if len(sys.argv) > 2 else "assert"
print(f"LenoC 自动化测试运行器")
print(f"解释器: {LENO}")
print(f"测试目录: {TDIR}")
print("=" * 40)

# 递归收集所有 test_*.leno 文件
tests = []
for root, dirs, files in os.walk(TDIR):
    dirs.sort()
    for f in sorted(files):
        if f.startswith("test_") and f.endswith(".leno"):
            tests.append(os.path.join(root, f))

passed = failed = 0
for fp in tests:
    name = os.path.relpath(fp, TDIR).replace("\\", "/")
    start = time.time()
    try:
        r = subprocess.run([LENO, fp],
                           capture_output=True,
                           timeout=30,
                           encoding='utf-8', errors='replace')
    except subprocess.TimeoutExpired:
        print(f"  [FAIL] {name} - 超时(30s)")
        failed += 1
        continue

    elapsed = time.time() - start
    out = (r.stdout or "") + (r.stderr or "")

    if r.returncode == 0:
        print(f"  [PASS] {name}")
        passed += 1
    else:
        # 提取错误行
        err_lines = [l.strip() for l in out.split("\n")
                     if l.strip() and ("错误" in l or "error" in l.lower() or "Error" in l)]
        reason = err_lines[0] if err_lines else f"退出码 {r.returncode}"
        if len(reason) > 200:
            reason = reason[:200] + "..."
        print(f"  [FAIL] {name} - {reason}")
        failed += 1

print("=" * 40)
print(f"Results: {passed} passed, {failed} failed (total {passed+failed})")
sys.exit(0 if failed == 0 else 1)
