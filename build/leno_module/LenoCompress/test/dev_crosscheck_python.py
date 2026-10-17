# dev_crosscheck_python.py —— 与 Python zlib/gzip 双向交叉验证（**开发期工具**）
#
# 为什么：DEFLATE 的坑在"位序 / Huffman 码方向 / 长度-距离表 / 校验和"。自己压、自己解
# 可能两边一起错；必须让独立实现（CPython 的 zlib）来验两个方向：
#   ① LenoCompress 压 → Python zlib/gzip 解 ⇒ 我们产出的是**合法**流
#   ② Python zlib（-15 裸流 / zlib / gzip）压 → LenoCompress 解 ⇒ 我们读得懂别人的流
#
# 用法（仓库根目录）：
#     python build/leno_module/LenoCompress/test/dev_crosscheck_python.py
import gzip
import os
import subprocess
import sys
import tempfile
import zlib

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
EXE = os.path.join(ROOT, "build", "leno.exe")
COMPRESS = os.path.join(HERE, "dev_compress.leno")
DECOMPRESS = os.path.join(HERE, "dev_decompress.leno")

CASES = {
    "empty": b"",
    "one": b"a",
    "short": b"hello world",
    "text": ("LenoCompress 压缩测试：中文、ASCII 与换行\n" * 200).encode("utf-8"),
    "binary": bytes(range(256)) * 16,
    "jsonish": ('{"k":"v","n":123,"arr":[1,2,3],"note":"重复的键值对"}\n' * 100).encode("utf-8"),
}


def run(cmd):
    r = subprocess.run(cmd, capture_output=True)
    return r.stdout.decode("utf-8", "replace").strip()


def main():
    tmp = tempfile.mkdtemp(prefix="lenozip_deflate_")
    fails = 0

    for name, data in CASES.items():
        raw_in = os.path.join(tmp, name + ".bin")
        with open(raw_in, "wb") as f:
            f.write(data)

        # ---------- ① Leno 压 → Python 解 ----------
        for mode in ("raw", "gzip", "zlib"):
            out = os.path.join(tmp, f"{name}.{mode}")
            r = run([EXE, COMPRESS, mode, raw_in, out])
            if not r.startswith("OK"):
                print(f"[FAIL] 压缩失败 {name}/{mode}: {r}")
                fails += 1
                continue
            blob = open(out, "rb").read()
            try:
                if mode == "raw":
                    back = zlib.decompress(blob, -15)
                elif mode == "gzip":
                    back = gzip.decompress(blob)
                else:
                    back = zlib.decompress(blob)
            except Exception as e:
                print(f"[FAIL] Python 解不开 {name}/{mode}: {e}")
                fails += 1
                continue
            if back != data:
                print(f"[FAIL] 内容不符 {name}/{mode}（py {len(data)}B / leno 解回 {len(back)}B）")
                fails += 1

        # ---------- ② Python 压 → Leno 解 ----------
        blobs = {
            "raw": zlib.compressobj(9, zlib.DEFLATED, -15),
        }
        co = zlib.compressobj(9, zlib.DEFLATED, -15)
        raw_blob = co.compress(data) + co.flush()
        gz_blob = gzip.compress(data, 9)
        zl_blob = zlib.compress(data, 9)
        for mode, blob in (("raw", raw_blob), ("gzip", gz_blob), ("zlib", zl_blob)):
            inp = os.path.join(tmp, f"{name}.py.{mode}")
            outp = os.path.join(tmp, f"{name}.py.{mode}.out")
            with open(inp, "wb") as f:
                f.write(blob)
            r = run([EXE, DECOMPRESS, mode, inp, outp])
            if not r.startswith("OK"):
                print(f"[FAIL] Leno 解不开 {name}/{mode}: {r}")
                fails += 1
                continue
            back = open(outp, "rb").read()
            if back != data:
                print(f"[FAIL] Leno 解压内容不符 {name}/{mode}")
                fails += 1

    # 压缩率汇总（只为肉眼确认"真的压了"，不作为断言阈值）
    for name, data in CASES.items():
        raw_in = os.path.join(tmp, name + ".bin")
        out = os.path.join(tmp, f"{name}.raw")
        if os.path.exists(out):
            c = os.path.getsize(out)
            n = len(data)
            pct = (c * 100.0 / n) if n else 0.0
            print(f"  {name:8s} {n:7d}B -> {c:6d}B ({pct:5.1f}%)")

    print("FAILS =", fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
