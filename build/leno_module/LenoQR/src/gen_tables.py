# gen_tables.py —— 生成 lib/qr_tables.leno（**开发期工具，不参与运行时/打包**）
#
# 规格表（版本×纠错 的分块结构、对齐图案中心）抄错一位，症状是"某些版本扫不出来"，
# 而且很难肉眼发现 ⇒ 不从 PDF 手抄，改成从 python qrcode 的数据表导出。
#
# 用法（需要 `pip install qrcode`）：
#     python src/gen_tables.py
#
# 导出的字段与 qr_encode.leno 的注释一一对应：
#   EC      = [每块纠错码字数, 组1块数, 组1数据码字, 组2块数, 组2数据码字] × 160
#   ALIGN   = 对齐中心坐标（扁平），ALIGN_COUNT = 每版本的个数
import os

import qrcode.base as base
import qrcode.util as util

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib", "qr_tables.leno")

ec = []
for v in range(40):
    for e in range(4):  # 顺序与 qrcode 一致：L, M, Q, H
        ent = base.RS_BLOCK_TABLE[v * 4 + e]
        if len(ent) == 3:
            blocks, total, data = ent
            ecpb = total - data
            g1b, g1d, g2b, g2d = blocks, data, 0, 0
        else:
            b1, t1, d1, b2, t2, d2 = ent
            ecpb = t1 - d1
            assert t1 - d1 == t2 - d2, (v, e, ent)
            g1b, g1d, g2b, g2d = b1, d1, b2, d2
        ec += [ecpb, g1b, g1d, g2b, g2d]

align = []
counts = []
for pos in util.PATTERN_POSITION_TABLE:
    counts.append(len(pos))
    align += list(pos)

lines = []
lines.append("// qr_tables.leno —— QR 规格表（**自动生成，勿手改**）")
lines.append("//")
lines.append("// 生成器：src/gen_tables.py（从 python qrcode 的 RS_BLOCK_TABLE /")
lines.append("//   PATTERN_POSITION_TABLE 导出；数据本身出自 ISO/IEC 18004 的容量表）")
lines.append("//")
lines.append("// EC：160 组 = 40 版本 × 4 档纠错（顺序 L,M,Q,H），每组 5 个数：")
lines.append("//   [每块纠错码字数, 组1块数, 组1每块数据码字, 组2块数, 组2每块数据码字]")
lines.append("//   组2块数为 0 表示该版本/档只有一组（组2数据码字也应为 0）。")
lines.append("export var EC = [")
for i in range(0, len(ec), 10):
    lines.append("    " + ", ".join(str(x) for x in ec[i:i + 10]) + ",")
lines.append("]")
lines.append("")
lines.append("// 对齐图案中心坐标（扁平表 + 每版本个数）：v1 没有对齐图案；同一版本内取所有 x×y 组合，")
lines.append("// 但**与三个定位图案重叠的角落要跳过**（见 qr_encode.leno 的 _place_function_patterns）。")
lines.append("export var ALIGN = [")
for i in range(0, len(align), 10):
    lines.append("    " + ", ".join(str(x) for x in align[i:i + 10]) + ",")
lines.append("]")
lines.append("")
lines.append("export var ALIGN_COUNT = [")
for i in range(0, len(counts), 20):
    lines.append("    " + ", ".join(str(x) for x in counts[i:i + 20]) + ",")
lines.append("]")

with open(OUT, "w", encoding="utf-8", newline="\n") as f:
    f.write("\n".join(lines) + "\n")

print("wrote", os.path.normpath(OUT), "EC", len(ec), "ALIGN", len(align))
