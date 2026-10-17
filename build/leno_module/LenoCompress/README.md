# LenoCompress v0.2.0

Leno 的压缩模块（**纯 Leno，零原生依赖**）：gzip / zlib / 裸 DEFLATE 的**压缩与解压**，
外加 CRC32 / Adler-32。

```leno
import "Compress" as z

// 压
string raw  = z.deflateRaw(data)   // 裸 DEFLATE（zip 条目、PNG IDAT）
string gz   = z.gzip(data)         // .gz
string zl   = z.zlib(data)         // RFC1950

// 解（返回 {ok, data, error, inBytes, outBytes}）
var r = z.gunzip(gz)
var r2 = z.inflateRaw(raw)
var r3 = z.inflateZlib(zl)

// 校验和
int c = z.crc32("123456789")       // 3421780262
int a = z.adler32("abc")           // 38600999
```

## API

| 函数 | 说明 |
|---|---|
| `deflateRaw(s)` / `deflateLevel(s, 1\|2)` | 裸 DEFLATE **压缩**（level 1 快、level 2 更小） |
| `gzip(s)` | gzip：10 字节头（MTIME=0，可复现）+ DEFLATE + CRC32/ISIZE |
| `zlib(s)` | zlib：`78 9C` + DEFLATE + adler32 |
| `gunzip(raw)` / `inflateZlib(raw)` / `inflateRaw(raw)` | 解压（见下） |
| `isGzip(raw)` | 魔数判断 |
| `crc32(s)` / `crc32Bytes(Array[int])` / `adler32(s)` | 校验和 |
| `rfcTableCheck()` | 内部 Huffman 码表自检（空串 = 正常） |

## 压缩实现（v0.2.0 新增）

- **fixed Huffman（BTYPE=01）+ LZ77**，单个 BFINAL 块
  - 窗口 32KB、最小匹配 3、最大 258、哈希 8192 槽（3 字节）
  - `level 1`：只试最近一次出现；`level 2`（默认）：顺链最多 32 个候选
- 为什么先不做 dynamic Huffman：要多算码长、构树、把码表写进流里，复杂度约 3 倍；
  而 fixed 对 XML/CSV/JSON 这类重复度高的文本已能到 **10–50 倍**（实测见下）。
  解码端本来就支持 fixed/dynamic 两种 ⇒ 我们压的流、别人压的流走同一套解码路径。
- 已知取舍：不做**惰性匹配**（lazy matching），所以对"半重复"文本比 zlib 略差；
  不做多块 / 字典 / 多线程。

## 解压实现（0.1.0）

- 支持 stored(00) / fixed(01) / dynamic(10) 三种块、多块流
- gzip 头可选字段 FEXTRA / FNAME / FCOMMENT / FHCRC 跳过
- 只查结构（魔数、块类型、码长合法性、回拷距离、ISIZE），**不校验 CRC32/adler32**
- 不做多成员"连接式" gzip（只解第一个成员）

## 实测（Windows x64）

| 输入 | 原文 | deflate | 比例 |
|---|---|---|---|
| 重复文本（20KB） | 19,500 B | 203 B | 1.0% |
| 中文重复段落 | 10,600 B | 145 B | 1.4% |
| 类 JSON 重复行 | 6,000 B | 112 B | 1.9% |
| 字节序列 0..255 × 16 | 4,096 B | 308 B | 7.5% |
| 78KB 混合文本 | 78,000 B | 685 B | **压缩耗时 ≈ 8 ms** |

> 空输入压出 2 字节、1 字节输入压出 3 字节 —— DEFLATE 的最小开销，正常现象。

## 验证

```bat
:: 自测：回环 ×6 / 压缩率底线 / gzip 头尾 / zlib 头 + adler32 / level 1·2 / CRC 向量
build\leno.exe build\leno_module\LenoCompress\test\test_deflate.leno
build\leno.exe build\leno_module\LenoCompress\test\test_inflate_crc.leno

:: 与 Python zlib/gzip 双向交叉验证（开发期工具）
python build\leno_module\LenoCompress\test\dev_crosscheck_python.py
```

交叉验证的判据（这才是"格式对不对"的标准）：

| 方向 | 内容 | 结果 |
|---|---|---|
| LenoCompress 压 → CPython `zlib` / `gzip` 解 | 6 种输入 × raw/gzip/zlib | 全部解开且逐字节一致 |
| CPython `zlib`（level 9，raw/zlib/gzip）压 → LenoCompress 解 | 同上 6 种 | 全部一致 |

## 来源与去重

解压部分是从 `LenoWeb/lib/web_gzip.leno` **抽出来的**（2026-10-17），因为 zip（以及 xlsx）
也需要裸 DEFLATE，不能让归档功能反过来依赖 HTTP 模块。**已收编**：`web_gzip.leno` 现在是
对本模块的转发层（函数名/签名不变），LenoWeb 因此依赖本包存在。

## 下一步

- dynamic Huffman（码长 RLE + 码表），预计比 fixed 再省 20–40%
- 惰性匹配 / 匹配长度上限调优
- 解压侧补 CRC32 校验（现在为性能省略）
