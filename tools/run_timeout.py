# run_timeout.py <timeout_seconds> <cmd...>
# 带硬超时地跑一个命令：超时立刻 kill 子进程并返回 124。
# 用途：网络/线程/async 类测试可能死锁，绝不在前台裸跑。
#   例：python tools/run_timeout.py 15 build/leno.exe assert/test_async_timeout.leno
import subprocess
import sys
import os

if len(sys.argv) < 3:
    print("usage: run_timeout.py <seconds> <cmd...>")
    sys.exit(2)

timeout = float(sys.argv[1])
cmd = sys.argv[2:]
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

try:
    p = subprocess.run(cmd, cwd=root, timeout=timeout,
                       capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
except subprocess.TimeoutExpired:
    print("TIMEOUT: %s 超过 %.1fs 未结束，已 kill" % (" ".join(cmd), timeout))
    sys.exit(124)

if p.stdout:
    print(p.stdout, end="")
if p.stderr:
    print(p.stderr, end="", file=sys.stderr)
sys.exit(p.returncode)
