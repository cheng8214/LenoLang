# dev_diff_vs_python.py —— 编码器矩阵与 python qrcode 逐格对拍（**开发期工具**）
#
# 为什么单独留一份：QR 的坑几乎都在"规格搬运"上——模式指示符、字符计数位宽、
#   交织顺序、掩码/format 位、version info。这类错**自编码→自解码往往照样能过**
#   （同一个错两边一起犯），必须拿独立实现逐格比。
#
# 依赖：`pip install qrcode`
# 用法（仓库根目录）：
#     python build/leno_module/LenoQR/test/dev_diff_vs_python.py
#
# 判据两条：
#   ① 同 version / ecc / mask 下，矩阵**逐格相同**
#   ② 我们选的掩码 == 用 qrcode 罚分独立算出的最优掩码
import os
import subprocess
import sys

import qrcode
import qrcode.util
from qrcode.constants import ERROR_CORRECT_H, ERROR_CORRECT_L, ERROR_CORRECT_M, ERROR_CORRECT_Q

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
EXE = os.path.join(ROOT, "build", "leno.exe")
SCRIPT = os.path.join(HERE, "dev_emit_matrix.leno")

ECC = {"L": ERROR_CORRECT_L, "M": ERROR_CORRECT_M, "Q": ERROR_CORRECT_Q, "H": ERROR_CORRECT_H}

CASES = [
    # v1
    ("LENOQR TEST 001", "M"),
    ("1234567890", "L"),
    ("HELLO WORLD 123", "Q"),
    # v3（含 UTF-8 字节模式）
    ("https://gitee.com/cheng8214/LenoLang", "M"),
    ("你好，Leno 二维码", "H"),
    # v5
    ("a" * 100, "L"),
    ("The quick brown fox jumps over the lazy dog. 0123456789", "Q"),
    # 大版本：v7+ 考 version info（BCH 18,6），v10+ 考 16 位字符计数，v27+ 考 14/13 位计数
    ("A" * 300, "M"),
    ("x" * 1200, "L"),
    ("9" * 5000, "L"),
    ("Z" * 2900, "L"),
]


def run_leno(text, ecc):
    out = subprocess.run([EXE, SCRIPT, text, ecc], capture_output=True)
    lines = out.stdout.decode("utf-8", "replace").splitlines()
    if not lines or lines[0].startswith("ERR"):
        raise RuntimeError("leno 编码失败：" + "\n".join(lines) + out.stderr.decode("utf-8", "replace"))
    meta = {"rows": []}
    for ln in lines:
        if ln.startswith("VERSION "):
            meta["version"] = int(ln.split()[1])
        elif ln.startswith("ECC "):
            meta["ecc"] = ln.split()[1]
        elif ln.startswith("MASK "):
            meta["mask"] = int(ln.split()[1])
        elif ln.startswith("SIZE "):
            meta["size"] = int(ln.split()[1])
        elif ln and ln[0] in "01":
            meta["rows"].append(ln)
    return meta


def py_matrix(text, ecc, version, mask):
    qr = qrcode.QRCode(version=version, error_correction=ECC[ecc], box_size=1, border=0, mask_pattern=mask)
    qr.add_data(text)
    qr.make(fit=False)
    return ["".join("1" if c else "0" for c in row) for row in qr.get_matrix()]


def penalty(modules):
    return qrcode.util.lost_point(modules)


def py_best_mask(text, ecc, version):
    pens = []
    for m in range(8):
        qr = qrcode.QRCode(version=version, error_correction=ECC[ecc], box_size=1, border=0, mask_pattern=m)
        qr.add_data(text)
        qr.make(fit=False)
        pens.append(penalty(qr.modules))
    return pens.index(min(pens))


def main():
    fails = 0
    for text, ecc in CASES:
        lm = run_leno(text, ecc)
        pm = py_matrix(text, ecc, lm["version"], lm["mask"])
        best = py_best_mask(text, ecc, lm["version"])
        matrix_ok = lm["rows"] == pm
        mask_ok = best == lm["mask"]
        tag = "OK  " if (matrix_ok and mask_ok) else "DIFF"
        print(f"[{tag}] {ecc} v{lm['version']} mask={lm['mask']} best={best} size={lm['size']} "
              f"{text[:36]!r}")
        if not matrix_ok:
            fails += 1
            for i, (a, b) in enumerate(zip(lm["rows"], pm)):
                if a != b:
                    print("  first diff row", i)
                    print("   leno:", a)
                    print("   py  :", b)
                    break
        elif not mask_ok:
            fails += 1
            print("  掩码不是最优：ours", lm["mask"], "best", best)
    print("FAILS =", fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
