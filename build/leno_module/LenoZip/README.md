# LenoZip v0.1.0

Leno 的 ZIP 归档模块（**纯 Leno，零原生依赖**）。读支持 stored + deflate，写当前是 stored。

```leno
import "Zip" as zip

// ① 打包
Array[Dict] entries = []
entries.add(zip.entry("readme.txt", "hello"))
entries.add(zip.entry("doc/说明.txt", "中文内容"))
string bytes = zip.encode(entries)        // zip 字节（字符串载体）
zip.save("out.zip", entries)

// ② 解包
var z = zip.load("in.zip")                // {ok, err, count, entries}
for z.entries to e {
    if e.isDir { continue }
    print(e.name + "  " + _str(e.size) + "B  crc=" + _str(e.crcOk))
}
var one = zip.read("in.zip", "readme.txt") // {ok, data, err}
```

## API

| 函数 | 说明 |
|---|---|
| `entry(name, data)` | 造一个条目 `{name, data}`（data 是任意字节，含 NUL） |
| `encode(entries)` | 条目数组 → zip 字节 |
| `save(path, entries)` | 写文件，返回是否落盘 |
| `load(path)` | 读文件 → `{ok, err, count, entries}` |
| `decode(bytes)` | 从字节读（同上） |
| `read(path, name)` | 取单条 → `{ok, data, err}` |
| `names(z)` | 条目名列表（按归档内顺序，目录条目也在） |
| `crcAllOk(z)` | 非目录条目是否**全部** CRC 通过 |

解出的条目：`{name, data, size, method, crcOk, isDir, err}`
（`method`：0=stored，8=deflate；`err` 非空表示这一条解不开，整包 `ok` 会置 false）

## 格式支持

| 能力 | 状态 |
|---|---|
| stored（method 0） | ✅ 读 + 写 |
| deflate（method 8） | ✅ 读（走 `LenoCompress` 的裸 DEFLATE）；写 ❌（等压缩端） |
| UTF-8 文件名（flag 0x0800） | ✅ 读写都置位，中文名不乱码 |
| CRC32 校验 | ✅ 读时逐条校验（`crcOk`），写时正确落头 |
| 目录条目（名字以 `/` 结尾） | ✅ 读时跳过、可识别（`isDir`） |
| zip64 / 加密 / bzip2 / lzma | ❌ 明确报错（不静默给半截） |
| 超大归档 | ⚠ v0.1 整包读进内存（流式接口以后再说） |

> **写为什么是 stored**：deflate **压缩端**还没实现（`LenoCompress` 目前只有解压）。
> stored 的好处是任何解压器都能读，代价是体积。等压缩端落地，这里只改 `method` 与
> 一行压缩调用，读写接口不变。

## 验证

```bat
:: 自往返（文本/中文名/嵌套/空文件/二进制/CRC/截断/非 zip）
build\leno.exe build\leno_module\LenoZip\test\test_zip.leno

:: 与 Python zipfile 双向交叉验证（开发期工具，需要 python）
python build\leno_module\LenoZip\test\dev_crosscheck_python.py
```

交叉验证实测：

| 方向 | 内容 | 结果 |
|---|---|---|
| Python `ZIP_DEFLATED` 写 → LenoZip 解 | 5 条：文本 / 中文名 / 4KB 长文本 / 二进制 / 空文件 | 逐字节一致 |
| LenoZip 写（stored）→ Python `zipfile` 读 | 4 条：同上（无 4KB 条） | `testzip()` CRC 全过、内容一致 |

第一条是**关键**：它真正走了 `LenoCompress` 的裸 DEFLATE 解码路径（stored 之外的方法），
第二条则证明我们写出的头/偏移/CRC 能被独立实现接受。
