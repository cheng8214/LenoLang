#!/usr/bin/env python3
"""
往已存在的 APK（zip）里追加条目，用于手工打包链路里的"dex + .so"注入。
为什么不用 zip 命令：Git Bash 不带 zip；aapt 的 add 会重排/压缩不可控。
.stored = True 时按 STORED 存（.so 不压缩，安装时 extractNativeLibs 直接落盘）。
用法: apkadd.py <apk> <stored|deflate> <src_path> <entry_name>
"""
import sys
import zipfile

apk, mode, src, entry = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]

data = open(src, 'rb').read()
comp = zipfile.ZIP_STORED if mode == 'stored' else zipfile.ZIP_DEFLATED

# 只允许追加：先读旧条目，再整体重写（zipfile 不支持原地追加后保留顺序/对齐）
with zipfile.ZipFile(apk, 'r') as zin:
    items = [(i, zin.read(i.filename)) for i in zin.infolist()]

with zipfile.ZipFile(apk, 'w') as zout:
    for info, payload in items:
        zi = zipfile.ZipInfo(info.filename, date_time=info.date_time)
        zi.compress_type = info.compress_type
        zi.external_attr = info.external_attr
        zout.writestr(zi, payload)
    zi = zipfile.ZipInfo(entry)
    zi.compress_type = comp
    zi.external_attr = 0o644 << 16
    zout.writestr(zi, data)

print(f"  + {entry} ({len(data)} bytes, {'stored' if mode == 'stored' else 'deflate'})")
