# dev_crosscheck_python.py —— 与 Python zipfile 双向交叉验证（**开发期工具**）
#
# 为什么必须做：ZIP 的坑集中在"偏移/长度/CRC/压缩方法"上，自己写、自己读容易
#   两边一起犯同一个错；必须让独立实现（Python zipfile，deflate 压缩）来验。
#
# 两条方向：
#   ① Python 写（ZIP_DEFLATED）→ LenoZip 解到目录 → 逐字节比对
#      ⇒ 这条会真正走到 LenoCompress 的裸 DEFLATE 解码（stored 之外的方法）
#   ② LenoZip 写（stored）→ Python zipfile 读 + testzip()（CRC）→ 逐字节比对
#
# 用法（仓库根目录）：
#     python build/leno_module/LenoZip/test/dev_crosscheck_python.py
import os
import subprocess
import sys
import tempfile
import zipfile

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
EXE = os.path.join(ROOT, "build", "leno.exe")
UNZIP = os.path.join(HERE, "dev_unzip.leno")
MAKEZIP = os.path.join(HERE, "dev_makezip.leno")

PY_ENTRIES = {
    "hello.txt": "hello from Python zipfile\n".encode("utf-8"),
    "dir/中文.txt": "中文内容\n第二行\n".encode("utf-8"),
    "bin.dat": bytes([0, 1, 2, 127, 128, 255, 10, 13]),
    "empty.txt": b"",
    "big.txt": ("LenoZip " * 500).encode("utf-8"),      # 4KB，逼出 deflate 多块
}
LENO_ENTRIES = {
    "hello.txt": b"hello from LenoZip",
    "dir/中文.txt": "中文内容\n第二行".encode("utf-8"),
    "bin.dat": bytes([0, 1, 2, 127, 128, 255, 10, 13]),
    "empty.txt": b"",
}


def run(cmd):
    r = subprocess.run(cmd, capture_output=True)
    return r.stdout.decode("utf-8", "replace").strip()


def main():
    tmp = tempfile.mkdtemp(prefix="lenozip_")
    fails = 0

    # ---------- ① Python 写 → LenoZip 读 ----------
    py_zip = os.path.join(tmp, "from_python.zip")
    with zipfile.ZipFile(py_zip, "w", zipfile.ZIP_DEFLATED) as zf:
        for name, data in PY_ENTRIES.items():
            zf.writestr(name, data)

    out_dir = os.path.join(tmp, "extracted")
    out = run([EXE, UNZIP, py_zip, out_dir])
    if not out.startswith("OK"):
        print("[FAIL] LenoZip 解包失败：", out)
        return 1
    got_count = int(out.split()[1])
    if got_count != len(PY_ENTRIES):
        print(f"[FAIL] 条目数：leno={got_count} py={len(PY_ENTRIES)}")
        fails += 1

    for name, data in PY_ENTRIES.items():
        path = os.path.join(out_dir, *name.split("/"))
        if not os.path.exists(path):
            print(f"[FAIL] 缺文件：{name}")
            fails += 1
            continue
        got = open(path, "rb").read()
        if got != data:
            print(f"[FAIL] 内容不符：{name}（py {len(data)}B / leno {len(got)}B）")
            fails += 1
    print(f"① Python(deflate) -> LenoZip：{got_count} 条，{'全一致' if fails == 0 else '有差异'}")

    # ---------- ② LenoZip 写 → Python 读 ----------
    leno_zip = os.path.join(tmp, "from_leno.zip")
    out = run([EXE, MAKEZIP, leno_zip])
    if not out.startswith("OK"):
        print("[FAIL] LenoZip 打包失败：", out)
        return 1

    try:
        with zipfile.ZipFile(leno_zip) as zf:
            bad = zf.testzip()          # CRC 校验；返回第一个坏文件名或 None
            if bad:
                print(f"[FAIL] Python 报告 CRC 损坏：{bad}")
                fails += 1
            names = zf.namelist()
            if set(names) != set(LENO_ENTRIES):
                print(f"[FAIL] 条目名不符：{names}")
                fails += 1
            for name, want in LENO_ENTRIES.items():
                got = zf.read(name)
                if got != want:
                    print(f"[FAIL] Python 读到内容不符：{name}")
                    fails += 1
    except Exception as e:
        print("[FAIL] Python 打不开 LenoZip 写的包：", e)
        return 1
    print(f"② LenoZip(stored) -> Python：{len(LENO_ENTRIES)} 条，CRC 与内容{'全一致' if fails == 0 else '有差异'}")

    print("FAILS =", fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
