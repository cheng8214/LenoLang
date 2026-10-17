# LenoCompress v0.1.0

Leno 的压缩模块（**纯 Leno，零原生依赖**）。当前提供**解压**（gzip / zlib / 裸 DEFLATE）
与 **CRC32**；压缩（DEFLATE 编码）是下一步。

```leno
import "Compress" as z

var r = z.gunzip(bytes)        // {ok, data, error, inBytes, outBytes}
var r2 = z.inflateRaw(deflate) // 裸 DEFLATE（zip 条目、PNG IDAT）
var r3 = z.inflateZlib(bytes)  // RFC1950
int c = z.crc32("123456789")   // 3421780262
```

## API

| 函数 | 说明 |
|---|---|
| `gunzip(raw)` | RFC1952；校验 ISIZE（截断/损坏会报），**不校验** CRC32（历史取舍） |
| `inflateZlib(raw)` | RFC1950；只查压缩方法，不查 FCHECK/adler32（"能解就解"） |
| `inflateRaw(raw)` | RFC1951 裸 DEFLATE；zip / PNG IDAT 用的就是它 |
| `isGzip(raw)` | 魔数判断 |
| `crc32(s)` / `crc32Bytes(Array[int])` | IEEE CRC32（查表实现） |
| `rfcTableCheck()` | 内部 Huffman 码表自检（返回空串表示正常） |

解压结果统一是 `GzipResult`：`{ok, data, error, inBytes, outBytes}`。

## 范围与取舍

- 支持：stored(00) / fixed(01) / dynamic(10) 三种块、多块流；gzip 头的
  FEXTRA / FNAME / FCOMMENT / FHCRC 跳过
- 不校验 CRC32 / adler32（按字节跑位运算，收益不抵成本；损坏由解码阶段的具体错误兜住）
- 不做：多成员"连接式" gzip（只解第一个成员）、zstd/brotli 等其它算法

## 来源与去重

本模块的 inflate 是从 `LenoWeb/lib/web_gzip.leno` **抽出来的**（2026-10-17），
因为 zip（以及 xlsx）也需要裸 DEFLATE，不能让归档功能反过来依赖 HTTP 模块。

**已收编（同日）**：`LenoWeb/lib/web_gzip.leno` 现在是对本模块的**转发层**
（函数名/签名不变，`Web.gunzip` 一族照旧可用）。LenoWeb 因此**依赖本包存在**；
它的 gzip 自检（`examples/tests/test_gzip.leno`，51 项）在收编后仍全过。

## 验证

```bat
build\leno.exe build\leno_module\LenoCompress\test\test_inflate_crc.leno
```

固定输入全部由 Python `zlib`/`gzip` 生成（权威期望值），断言 6 组：

1. CRC32 向量：`crc32("123456789") = 3421780262`、`crc32("") = 0`、`crc32("abc") = 891568578`、表项 `0x77073096`
2. gzip 49B → 28B，内容逐字一致
3. zlib 37B 解压正确
4. 裸 DEFLATE 31B 解压正确
5. 故意损坏的 gzip ⇒ `ok=false` 且有可读原因（不静默给半截）
6. 空输入的边界

## 下一步

- **DEFLATE 压缩**（fixed + dynamic Huffman + LZ77，先 fixed 保底）
  ⇒ 有了它，LenoZip 才能把条目压小、LenoCompress 才能写 gzip
- 压缩端落地后：给 `LenoZip` 加 `level` 参数；把 `LenoWeb/web_gzip.leno` 收编为 re-export
