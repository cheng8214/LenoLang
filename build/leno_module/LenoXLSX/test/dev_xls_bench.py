# dev_xls_bench.py -- 同一个 .xls 的 Python 参考实现计时（对照 xls_read.leno）
#   用法：python build\leno_module\LenoXLSX\test\dev_xls_bench.py [file.xls] [重复次数]
#   只用标准库（struct / io），不依赖 xlrd / openpyxl / olefile
import struct, sys, time, statistics

PATH = sys.argv[1] if len(sys.argv) > 1 else r"D:\CLeno\1月份.xls"
N = int(sys.argv[2]) if len(sys.argv) > 2 else 100


def read_file(path):
    with open(path, "rb") as f:
        return f.read()


def ole_stream(d, wanted=("Workbook", "Book")):
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
            entries.append((e[:max(0, nameLen - 2)].decode("utf-16-le", "replace"), typ, start, size))

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

    for (n, t, s, z) in entries:
        if t == 2 and n in wanted:
            return read_mini(s, z) if z < miniCut else read_chain(s, z)
    return b""


def parse(wb):
    recs = []
    off = 0
    while off + 4 <= len(wb):
        rid, rl = struct.unpack_from("<HH", wb, off)
        recs.append((off, rid, rl))
        off += 4 + rl

    rd16 = lambda o: struct.unpack_from("<H", wb, o)[0]
    rd32 = lambda o: struct.unpack_from("<I", wb, o)[0]

    sheets = []
    for (o, r, l) in recs:
        if r == 0x0085:
            bof = rd32(o + 4)
            cch = wb[o + 10]
            fl = wb[o + 11]
            raw = wb[o + 12:o + 12 + (cch * 2 if fl & 1 else cch)]
            sheets.append((raw.decode("utf-16-le" if fl & 1 else "latin-1", "replace"), bof))

    def parse_sst(idx):
        blocks, j, first = [], idx, True
        while j < len(recs) and (first or recs[j][1] == 0x003C):
            o, r, l = recs[j]
            blocks.append(wb[o + 4:o + 4 + l])
            first = False
            j += 1
        data = b"".join(blocks)
        bounds, acc = [], 0
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

    sheetPos = [p for (n, p) in sheets]
    rows_out = []
    for _ in sheets:
        rows_out.append({})
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
            rows_out[si].setdefault(row, {})[col] = sst[isst] if isst < len(sst) else ""
        elif r == 0x027E:
            row, col, xf, v = struct.unpack_from("<HHHI", wb, b)
            rows_out[si].setdefault(row, {})[col] = numtext(rk(v))
        elif r == 0x00BD:
            row, c0 = struct.unpack_from("<HH", wb, b)
            n = (l - 6) // 6
            for q in range(n):
                xf, v = struct.unpack_from("<HI", wb, b + 4 + q * 6)
                rows_out[si].setdefault(row, {})[c0 + q] = numtext(rk(v))
        elif r == 0x0203:
            row, col, xf = struct.unpack_from("<HHH", wb, b)
            rows_out[si].setdefault(row, {})[col] = numtext(struct.unpack_from("<d", wb, b + 6)[0])
    return sheets, sst, rows_out


def bench(name, fn, n):
    fn()  # 预热
    ts = []
    for _ in range(n):
        t = time.perf_counter()
        fn()
        ts.append((time.perf_counter() - t) * 1e6)
    ts.sort()
    print("%-24s median=%7.0fus  min=%7.0fus  avg=%7.0fus" % (name, ts[n // 2], ts[0], statistics.mean(ts)))
    return ts[n // 2]


d = read_file(PATH)
print("文件：%s（%d 字节）" % (PATH, len(d)))
print("")
bench("open+read", lambda: read_file(PATH), N)
bench("OLE2 打开+取流", lambda: ole_stream(d), N)
wb = ole_stream(d)
bench("xls 完整读取", lambda: parse(ole_stream(d)), N)
print("")
print("Python " + sys.version.split()[0])
