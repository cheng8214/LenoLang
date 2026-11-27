# dev_xls_crosscheck.py -- 与 xls_read.leno 交叉验证：独立解析 .xls（OLE2 + BIFF8）并 dump TSV
#   用法：python build\leno_module\LenoXLSX\test\dev_xls_crosscheck.py <file.xls> <out.tsv>
import struct, sys, collections

src = sys.argv[1] if len(sys.argv) > 1 else r"D:\CLeno\1月份.xls"
out = sys.argv[2] if len(sys.argv) > 2 else r"C:\Users\Administrator\AppData\Local\Temp\py_xls.tsv"

d = open(src, "rb").read()
assert d[:8] == b"\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", "not OLE2"

ssz = 1 << struct.unpack_from("<H", d, 30)[0]
mssz = 1 << struct.unpack_from("<H", d, 32)[0]
nFAT = struct.unpack_from("<I", d, 44)[0]
dirStart = struct.unpack_from("<I", d, 48)[0]
miniCut = struct.unpack_from("<I", d, 56)[0]
miniFATStart = struct.unpack_from("<I", d, 60)[0]
nMiniFAT = struct.unpack_from("<I", d, 64)[0]
difatStart = struct.unpack_from("<I", d, 68)[0]
nDIFAT = struct.unpack_from("<I", d, 72)[0]


def sector(i):
    off = 512 + i * ssz
    return d[off:off + ssz]


difat = list(struct.unpack_from("<109I", d, 76))
nxt = difatStart
while nDIFAT > 0 and nxt not in (0xFFFFFFFE, 0xFFFFFFFF):
    vals = struct.unpack_from("<%dI" % (ssz // 4), sector(nxt), 0)
    difat.extend(vals[:-1])
    nxt = vals[-1]
fatSecs = [x for x in difat if x not in (0xFFFFFFFE, 0xFFFFFFFF)][:nFAT]

fat = []
for fs in fatSecs:
    fat.extend(struct.unpack_from("<%dI" % (ssz // 4), sector(fs), 0))


def chain(start):
    out, cur, seen = [], start, set()
    while cur not in (0xFFFFFFFE, 0xFFFFFFFF) and cur not in seen and cur < len(fat):
        seen.add(cur)
        out.append(cur)
        cur = fat[cur]
    return out


def read_chain(start, size=None):
    b = b"".join(sector(i) for i in chain(start))
    return b if size is None else b[:size]


dirBytes = read_chain(dirStart)
entries = []
for i in range(0, len(dirBytes), 128):
    e = dirBytes[i:i + 128]
    if len(e) < 128:
        break
    nameLen = struct.unpack_from("<H", e, 64)[0]
    typ = e[66]
    start = struct.unpack_from("<I", e, 116)[0]
    size = struct.unpack_from("<I", e, 120)[0]
    if typ:
        name = e[:max(0, nameLen - 2)].decode("utf-16-le", "replace")
        entries.append((name, typ, start, size))

miniFAT = []
if miniFATStart not in (0xFFFFFFFE, 0xFFFFFFFF) and nMiniFAT > 0:
    b = read_chain(miniFATStart)
    miniFAT = list(struct.unpack_from("<%dI" % (len(b) // 4), b, 0))

rootStart, rootSize = [(s, z) for (n, t, s, z) in entries if t == 5][0]
ministream = read_chain(rootStart, rootSize)


def read_mini(start, size):
    out, cur, seen = b"", start, set()
    while cur not in (0xFFFFFFFE, 0xFFFFFFFF) and cur not in seen and cur < len(miniFAT):
        seen.add(cur)
        out += ministream[cur * mssz:(cur + 1) * mssz]
        cur = miniFAT[cur]
    return out[:size]


wb = b""
for (n, t, s, z) in entries:
    if t == 2 and n in ("Workbook", "Book"):
        wb = read_mini(s, z) if z < miniCut else read_chain(s, z)

recs = []
off = 0
while off + 4 <= len(wb):
    rid, rl = struct.unpack_from("<HH", wb, off)
    recs.append((off, rid, rl))
    off += 4 + rl


def rd16(o):
    return struct.unpack_from("<H", wb, o)[0]


def rd32(o):
    return struct.unpack_from("<I", wb, o)[0]


# BOUNDSHEET
sheets = []
for (o, r, l) in recs:
    if r == 0x0085:
        bof = rd32(o + 4)
        cch = wb[o + 10]
        fl = wb[o + 11]
        name = wb[o + 12:o + 12 + (cch * 2 if fl & 1 else cch)]
        name = name.decode("utf-16-le" if fl & 1 else "latin-1", "replace")
        sheets.append((name, bof))


# SST
def parse_sst(idx):
    blocks = []
    j = idx
    first = True
    while j < len(recs) and (first or recs[j][1] == 0x003C):
        o, r, l = recs[j]
        blocks.append(wb[o + 4:o + 4 + l])
        first = False
        j += 1
    data = b"".join(blocks)
    bounds = []
    acc = 0
    for b in blocks:
        acc += len(b)
        bounds.append(acc)

    def next_bound(pos):
        for b in bounds:
            if b > pos:
                return b
        return 1 << 30

    unique = struct.unpack_from("<I", data, 4)[0]
    pos = 8
    out = []
    for _ in range(unique):
        cch = struct.unpack_from("<H", data, pos)[0]
        flags = data[pos + 2]
        pos += 3
        cRun = cbExt = 0
        if flags & 8:
            cRun = struct.unpack_from("<H", data, pos)[0]
            pos += 2
        if flags & 4:
            cbExt = struct.unpack_from("<I", data, pos)[0]
            pos += 4
        high = flags & 1
        need = cch
        parts = []
        while need > 0:
            nb = next_bound(pos)
            avail = nb - pos
            if avail <= 0:
                break
            if high:
                can = min(need, avail // 2)
                parts.append(data[pos:pos + can * 2].decode("utf-16-le", "replace"))
                pos += can * 2
                need -= can
            else:
                can = min(need, avail)
                parts.append(data[pos:pos + can].decode("latin-1"))
                pos += can
                need -= can
            if need > 0:
                high = data[pos] & 1
                pos += 1
        out.append("".join(parts))
        pos += 4 * cRun + cbExt
    return out


sstIdx = [i for i, (o, r, l) in enumerate(recs) if r == 0x00FC]
sst = parse_sst(sstIdx[0]) if sstIdx else []


def rk(v):
    f100, fint = v & 1, v & 2
    v2 = v & 0xFFFFFFFC
    if fint:
        n = v2 >> 2
        if n & 0x20000000:
            n -= 0x40000000
        val = float(n)
    else:
        val = struct.unpack("<d", struct.pack("<Q", v2 << 32))[0]
    return val / 100 if f100 else val


def numtext(v):
    if abs(v) < 1e15 and v == int(v):
        return str(int(v))
    return repr(v)


sheets_rows = collections.defaultdict(lambda: collections.defaultdict(dict))
sheetPos = [p for (n, p) in sheets]
for (o, r, l) in recs:
    si = -1
    for i, p in enumerate(sheetPos):
        if o >= p:
            si = i
    if si < 0:
        continue
    b = o + 4
    if r == 0x00FD:
        row, col, xf, isst = struct.unpack_from("<HHHI", wb, b)
        sheets_rows[si][row][col] = sst[isst] if isst < len(sst) else ""
    elif r == 0x027E:
        row, col, xf, v = struct.unpack_from("<HHHI", wb, b)
        sheets_rows[si][row][col] = numtext(rk(v))
    elif r == 0x00BD:
        row, c0 = struct.unpack_from("<HH", wb, b)
        n = (l - 6) // 6
        for q in range(n):
            xf, v = struct.unpack_from("<HI", wb, b + 4 + q * 6)
            sheets_rows[si][row][c0 + q] = numtext(rk(v))
    elif r == 0x0203:
        row, col, xf = struct.unpack_from("<HHH", wb, b)
        val = struct.unpack_from("<d", wb, b + 6)[0]
        sheets_rows[si][row][col] = numtext(val)
    elif r in (0x0204, 0x00D6):
        row, col, xf, cch, fl = struct.unpack_from("<HHHBB", wb, b)
        raw = wb[b + 9:b + 9 + (cch * 2 if fl & 1 else cch)]
        sheets_rows[si][row][col] = raw.decode("utf-16-le" if fl & 1 else "latin-1", "replace")
    elif r == 0x0205:
        row, col, xf, bv, iserr = struct.unpack_from("<HHHBB", wb, b)
        sheets_rows[si][row][col] = "#ERR" if iserr else ("false" if bv == 0 else "true")

lines = []
for si, (name, _) in enumerate(sheets):
    rows = sheets_rows.get(si, {})
    lines.append("S%d\t%s" % (si, name))
    for r in sorted(rows):
        cells = rows[r]
        mx = max(cells) if cells else 0
        vals = [cells.get(c, "") for c in range(mx + 1)]
        lines.append("R%d\t%s" % (r, "\t".join(vals)))

with open(out, "w", encoding="utf-8", newline="\n") as f:
    f.write("\n".join(lines) + "\n")
print("wrote", out, "lines", len(lines), "sheets", [n for n, _ in sheets], "sst", len(sst))
