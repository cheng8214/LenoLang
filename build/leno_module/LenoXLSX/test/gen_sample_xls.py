# gen_sample_xls.py -- 生成一个可复现的 .xls 测试样本（OLE2 + BIFF8，纯标准库）
#
# 为什么需要：基准/交叉验证原来依赖 D:\CLeno\1月份.xls（外部文件，随时可能被清掉）。
# 这里用 Python 的 struct 手写一个最小但合法的 BIFF8 工作簿，让 dev_xls_bench / crosscheck
# 随时可复跑。
#
# 用法：python gen_sample_xls.py <输出路径> [行数]
#   默认 3 个 sheet：Sheet1 是 200 行 × 3 列混合数据（字符串 + 数字），另两个是空表。
import struct
import sys


def rec(rid, data):
    return struct.pack('<HH', rid, len(data)) + data


def bof(kind):
    # BIFF8 BOF：版本 0x0600，type（5=globals，0x10=worksheet），build，year，历史，最低版本
    return rec(0x0809, struct.pack('<HHHHII', 0x0600, kind, 0x0DBB, 0x07CC, 0, 0x0006))


def sst(strings):
    """SST：总引用数、唯一数，然后每个字符串 cch(2) + grbit(1) + 字符数据"""
    data = struct.pack('<II', len(strings), len(strings))
    for s in strings:
        u = s.encode('utf-16-le')
        # grbit 0x01 = fHighByte（用 16 位字符），高位字节在前？不——BIFF8 是 little-endian UTF-16
        data += struct.pack('<HB', len(s), 0x01) + u
    return rec(0x00FC, data)


def label_sst(row, col, idx):
    return rec(0x00FD, struct.pack('<HHHI', row, col, 0x0F, idx))


def number(row, col, val):
    return rec(0x0203, struct.pack('<HHH', row, col, 0x0F) + struct.pack('<d', val))


def boundsheet(offset, name):
    nb = name.encode('ascii')
    return rec(0x0085, struct.pack('<I', offset) + struct.pack('<BB', 0, 0)
               + struct.pack('<BB', len(nb), 0) + nb)


def build(rows):
    """rows 行 × 3 列：第 0 列字符串、第 1 列整数、第 2 列小数（夹杂文本）"""
    strings = []
    sidx = {}
    cells = b''

    def intern(s):
        if s not in sidx:
            sidx[s] = len(strings)
            strings.append(s)
        return sidx[s]

    # 表头
    for c, h in enumerate(['材料', '数量', '单价']):
        cells += label_sst(0, c, intern(h))

    for r in range(1, rows + 1):
        name = '项目-%d' % r
        cells += label_sst(r, 0, intern(name))
        cells += number(r, 1, float(r))
        if r % 3 == 0:
            cells += label_sst(r, 2, intern('备注%d' % r))
        else:
            cells += number(r, 2, r * 1.5)

    sheet1 = bof(0x0010) + cells + rec(0x000A, b'')
    sheet2 = bof(0x0010) + rec(0x000A, b'')
    sheet3 = bof(0x0010) + rec(0x000A, b'')

    # globals = BOF + 3×BOUNDSHEET + SST + EOF；BOUNDSHEET 里的偏移要回填
    globals_header = bof(0x0005)
    # 先按"偏移占位"拼一次，算出真实大小
    bs_len = len(boundsheet(0, 'Sheet1'))
    sst_blob = sst(strings)
    globals_size = len(globals_header) + bs_len * 3 + len(sst_blob) + 4  # 末尾 EOF 4 字节

    off1 = globals_size
    off2 = off1 + len(sheet1)
    off3 = off2 + len(sheet2)

    globals_blob = (globals_header
                    + boundsheet(off1, 'Sheet1')
                    + boundsheet(off2, 'Sheet2')
                    + boundsheet(off3, 'Sheet3')
                    + sst_blob
                    + rec(0x000A, b''))
    assert len(globals_blob) == globals_size, (len(globals_blob), globals_size)

    return globals_blob + sheet1 + sheet2 + sheet3


def ole2(stream):
    """把 Workbook 流包成 OLE2 复合文档（>= 4096 字节 ⇒ 用常规 FAT，不碰 miniFAT）"""
    if len(stream) < 4096:
        stream = stream + b'\x00' * (4096 - len(stream))
    SS = 512
    n_stream_sectors = (len(stream) + SS - 1) // SS
    stream = stream + b'\x00' * (n_stream_sectors * SS - len(stream))

    # 扇区分配：0=FAT，1=目录，2..2+n-1=Workbook
    first_stream_sector = 2
    n_sectors = 2 + n_stream_sectors

    # FAT
    fat = [0xFFFFFFFF] * 128
    fat[0] = 0xFFFFFFFD          # FATSECT
    fat[1] = 0xFFFFFFFE          # 目录扇区：链结束
    for i in range(n_stream_sectors):
        sid = first_stream_sector + i
        fat[sid] = 0xFFFFFFFE if i == n_stream_sectors - 1 else (sid + 1)
    fat_blob = b''.join(struct.pack('<I', x) for x in fat)

    # 目录项
    def dirent(name, typ, start, size):
        nb = name.encode('utf-16-le') + b'\x00\x00'
        e = nb + b'\x00' * (64 - len(nb))
        e += struct.pack('<H', len(nb))
        e += struct.pack('<BB', typ, 1)              # type, color=black
        e += struct.pack('<III', 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF)
        e += b'\x00' * 16                            # CLSID
        e += struct.pack('<I', 0)                    # state bits
        e += b'\x00' * 16                            # creation/modified
        e += struct.pack('<I', start)
        e += struct.pack('<Q', size)
        return e

    dir_blob = (dirent('Root Entry', 5, 0, 0)
                + dirent('Workbook', 2, first_stream_sector, len(stream))
                + dirent('', 0, 0, 0)
                + dirent('', 0, 0, 0))

    # 头
    header = struct.pack('<8s', b'\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1')
    header += b'\x00' * 16                            # CLSID
    header += struct.pack('<HH', 0x003E, 0x0003)      # minor, major
    header += struct.pack('<H', 0xFFFE)               # byte order
    header += struct.pack('<HH', 9, 6)                # sector shift, mini sector shift
    header += b'\x00' * 6
    header += struct.pack('<IIIIIIIII', 0, 1, 1, 0, 4096,
                          0xFFFFFFFE, 0, 0xFFFFFFFE, 0)
    difat = [0] + [0xFFFFFFFF] * 108
    header += b''.join(struct.pack('<I', x) for x in difat)
    assert len(header) == 512, len(header)

    return header + fat_blob + dir_blob + stream


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else 'sample.xls'
    rows = int(sys.argv[2]) if len(sys.argv) > 2 else 200
    blob = ole2(build(rows))
    with open(out, 'wb') as f:
        f.write(blob)
    print('wrote %s  %d bytes  rows=%d  sectors=%d'
          % (out, len(blob), rows, (len(blob) - 512) // 512))


if __name__ == '__main__':
    main()
