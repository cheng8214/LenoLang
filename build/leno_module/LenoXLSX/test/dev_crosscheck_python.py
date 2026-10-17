# dev_crosscheck_python.py —— 与 Python 独立实现双向交叉验证（**开发期工具**）
#
# 为什么需要：xlsx 的坑在"XML 结构 + zip 容器 + 共享字符串/内联字符串/布尔/数字"，
#   自己写、自己读容易两边一起错。这里用 Python 标准库（zipfile + xml.etree）当尺子：
#
#   ① LenoXLSX 写 → Python 校验：
#        zip 完整性（testzip）→ 必备部件齐全 → 每个 XML 部件**能被 ET 解析**
#        → 用 ET 独立取出单元格值与期望比对
#   ② Python 独立造 xlsx（sharedStrings + inlineStr + bool + 数字，ZIP_DEFLATED）
#        → LenoXLSX 读取 → 导出 TSV → 与期望比对
#      （① 证明我们写的文件别的实现能读；② 证明我们读得懂别人写的）
#
# 用法（仓库根目录）：
#     python build/leno_module/LenoXLSX/test/dev_crosscheck_python.py
import os
import subprocess
import sys
import tempfile
import zipfile
import xml.etree.ElementTree as ET

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
EXE = os.path.join(ROOT, "build", "leno.exe")
MAKE = os.path.join(HERE, "dev_make_xlsx.leno")
DUMP = os.path.join(HERE, "dev_dump.leno")

# Leno 写出的工作簿（与 dev_make_xlsx.leno 对应）
LENO_EXPECT = {
    "数据": [["name", "note", "id"],
             ["张三", "含 & < > \" '", "00123"],
             ["Alice", " padded ", "42"]],
    "数值": [["n1", "n2"], ["7", "3.5"]],
}

# Python 独立造的工作簿（见 make_ref_xlsx）
PY_EXPECT = {
    "S1": [[("A", "Alpha"), ("B", "42"), ("C", "A&B")],
           [("A", "共享"), ("C", "3.5")]],
    "S2": [[("A", "内联"), ("B", "true"), ("C", " padded ")],
           [("A", "x<y")]],
}

NS_MAIN = "http://schemas.openxmlformats.org/spreadsheetml/2006/main"


def run(cmd):
    r = subprocess.run(cmd, capture_output=True)
    return r.stdout.decode("utf-8", "replace").strip()


def local(tag):
    return tag.split("}")[-1]


def col_index(ref):
    n = 0
    for ch in ref:
        if ch.isalpha():
            n = n * 26 + (ord(ch.upper()) - 64)
        else:
            break
    return n - 1


def parse_sheet(xml_bytes):
    """ET 独立解析工作表 → {(r,c): value}"""
    root = ET.fromstring(xml_bytes)
    cells = {}
    for c in root.iter():
        if local(c.tag) != "c":
            continue
        ref = c.get("r", "")
        t = c.get("t", "")
        val = ""
        if t == "inlineStr":
            texts = [n.text or "" for n in c.iter() if local(n.tag) == "t"]
            val = "".join(texts)
        else:
            vs = [n.text or "" for n in c.iter() if local(n.tag) == "v"]
            v = vs[0] if vs else ""
            if t == "s":
                val = ("__shared__", int(v))
            elif t == "b":
                val = "true" if v == "1" else "false"
            else:
                val = v
        cells[(int(ref[1:]) - 1, col_index(ref))] = val
    return cells


def check_leno_output(path, shared_strings):
    """① LenoXLSX 写出的文件：zip/XML/值三重校验"""
    fails = 0
    with zipfile.ZipFile(path) as zf:
        bad = zf.testzip()
        if bad is not None:
            print("[FAIL] zip CRC 损坏：", bad)
            fails += 1
        names = set(zf.namelist())
        required = {"[Content_Types].xml", "_rels/.rels", "xl/workbook.xml",
                    "xl/_rels/workbook.xml.rels", "xl/worksheets/sheet1.xml",
                    "xl/worksheets/sheet2.xml"}
        missing = required - names
        if missing:
            print("[FAIL] 缺部件：", sorted(missing))
            fails += 1
        # 所有 XML 部件必须能被 ET 解析（合法性）
        for n in names:
            if n.endswith(".xml") or n.endswith(".rels"):
                try:
                    ET.fromstring(zf.read(n))
                except Exception as e:
                    print(f"[FAIL] XML 不合法 {n}: {e}")
                    fails += 1
        sheets = {}
        for idx, name in ((1, "数据"), (2, "数值")):
            cells = parse_sheet(zf.read(f"xl/worksheets/sheet{idx}.xml"))
            grid = []
            max_r = max((k[0] for k in cells), default=-1)
            for r in range(max_r + 1):
                row = []
                width = max((k[1] for k in cells if k[0] == r), default=-1)
                for c in range(width + 1):
                    row.append(cells.get((r, c), ""))
                grid.append(row)
            sheets[name] = grid
    for name, want in LENO_EXPECT.items():
        got = sheets.get(name)
        if got is None:
            print(f"[FAIL] 缺 sheet：{name}")
            fails += 1
            continue
        for r, want_row in enumerate(want):
            got_row = got[r] if r < len(got) else []
            for c, want_v in enumerate(want_row):
                got_v = got_row[c] if c < len(got_row) else ""
                if got_v != want_v:
                    print(f"[FAIL] {name} R{r+1}C{c+1}: py={want_v!r} leno={got_v!r}")
                    fails += 1
    print(f"① LenoXLSX -> Python(ET)：zip/XML/值{'全过' if fails == 0 else '有差异'}")
    return fails


def make_ref_xlsx(path):
    """Python 独立造一个 xlsx：sharedStrings + inlineStr + bool + 数字，deflate 压缩"""
    ct = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
          '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
          '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
          '<Default Extension="xml" ContentType="application/xml"/>'
          '<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>'
          '<Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>'
          '<Override PartName="/xl/worksheets/sheet2.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>'
          '<Override PartName="/xl/sharedStrings.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml"/>'
          '</Types>')
    root_rels = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
                 '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
                 '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>'
                 '</Relationships>')
    workbook = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
                '<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" '
                'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">'
                '<sheets>'
                '<sheet name="S1" sheetId="1" r:id="rId1"/>'
                '<sheet name="S2" sheetId="2" r:id="rId2"/>'
                '</sheets></workbook>')
    wb_rels = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
               '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
               '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>'
               '<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet2.xml"/>'
               '<Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings" Target="sharedStrings.xml"/>'
               '</Relationships>')
    shared = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
              f'<sst xmlns="{NS_MAIN}" count="3" uniqueCount="3">'
              '<si><t>Alpha</t></si>'
              '<si><t>共享</t></si>'
              '<si><t>A&amp;B</t></si>'
              '</sst>')
    sheet1 = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
              f'<worksheet xmlns="{NS_MAIN}"><sheetData>'
              '<row r="1"><c r="A1" t="s"><v>0</v></c><c r="B1"><v>42</v></c><c r="C1" t="s"><v>2</v></c></row>'
              '<row r="2"><c r="A2" t="s"><v>1</v></c><c r="C2"><v>3.5</v></c></row>'
              '</sheetData></worksheet>')
    sheet2 = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
              f'<worksheet xmlns="{NS_MAIN}"><sheetData>'
              '<row r="1"><c r="A1" t="inlineStr"><is><t>内联</t></is></c>'
              '<c r="B1" t="b"><v>1</v></c>'
              '<c r="C1" t="inlineStr"><is><t xml:space="preserve"> padded </t></is></c></row>'
              '<row r="2"><c r="A2" t="inlineStr"><is><t>x&lt;y</t></is></c></row>'
              '</sheetData></worksheet>')
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("[Content_Types].xml", ct)
        zf.writestr("_rels/.rels", root_rels)
        zf.writestr("xl/workbook.xml", workbook)
        zf.writestr("xl/_rels/workbook.xml.rels", wb_rels)
        zf.writestr("xl/sharedStrings.xml", shared)
        zf.writestr("xl/worksheets/sheet1.xml", sheet1)
        zf.writestr("xl/worksheets/sheet2.xml", sheet2)


def check_leno_reading(path):
    """② Python 造的 xlsx → Leno 读 → TSV 比对"""
    fails = 0
    dump = path + ".tsv"
    out = run([EXE, DUMP, path, dump])
    if not out.startswith("OK"):
        print("[FAIL] LenoXLSX 读取失败：", out)
        return 1

    cur = None
    got = {}
    for line in open(dump, encoding="utf-8").read().splitlines():
        parts = line.split("\t")
        if parts[0] == "SHEET":
            cur = parts[1]
            got[cur] = {}
        elif parts[0] == "CELL":
            got[cur][(int(parts[1]), int(parts[2]))] = parts[3]

    for name, rows in PY_EXPECT.items():
        if name not in got:
            print(f"[FAIL] 没读到 sheet {name}")
            fails += 1
            continue
        for r, cells in enumerate(rows):
            for col_letter, want in cells:
                c = col_index(col_letter + "1")
                got_v = got[name].get((r, c), "")
                if got_v != want:
                    print(f"[FAIL] {name} R{r+1}{col_letter}: py={want!r} leno={got_v!r}")
                    fails += 1
    print(f"② Python -> LenoXLSX：{'逐格一致' if fails == 0 else '有差异'}")
    return fails


def main():
    tmp = tempfile.mkdtemp(prefix="lenoxlsx_")
    fails = 0

    leno_xlsx = os.path.join(tmp, "from_leno.xlsx")
    out = run([EXE, MAKE, leno_xlsx])
    if not out.startswith("OK"):
        print("[FAIL] LenoXLSX 写文件失败：", out)
        return 1
    fails += check_leno_output(leno_xlsx, [])

    py_xlsx = os.path.join(tmp, "from_python.xlsx")
    make_ref_xlsx(py_xlsx)
    fails += check_leno_reading(py_xlsx)

    print("FAILS =", fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
