# LenoCrypto

Leno 通用加密库：**MD5 / SHA-256 / SHA-512 / HMAC-SHA256 / AES-128（ECB 单块 + CBC + PKCS7）**，
纯 Leno 实现，无外部依赖。

> **base64 与"文本 ⇄ 字节"不在这里**：它们是标量编解码、不是密码学原语（无密钥、无安全性质），
> 已归**核心标准库** `strings.to_base64` / `strings.from_base64` / `strings.to_bytes` /
> `strings.from_bytes`（2026-10-07，与 `to_hex` / `from_hex` 同族）✓

> 缘起：用 Leno 移植 TraeSign 时，为了"派生密钥 + AES-128-CBC 解密登录态"，
> 不得不在应用里**搬 800 行纯 Leno 的 AES/SHA-512** ✗（对照 C# 只写 33 行，因为它自带
> `SHA512.Create()` / `Aes.Create()`）。与其每个项目都搬一遍，不如做成标准库 ✓。

## 用法

```leno
import strings                                      // base64 / 文本⇄字节 ⇒ 核心标准库
import "Crypto" as crypto

// Base64（二进制安全：走字节版）
var bs = strings.to_bytes(strings.from_base64("AAEC/f7/"))           // ⇒ [0,1,2,253,254,255]
var s  = strings.to_base64(strings.from_bytes([0,1,2,253,254,255]))  // ⇒ "AAEC/f7/"

// 哈希（返回十六进制小写；*_bytes 返回原始字节）
var h   = crypto.sha256("abc")
var h2  = crypto.sha512("abc")
var raw = crypto.sha512_bytes(strings.to_bytes("abc"))

// MD5（**只为服务端协议签名**收录，如酷狗 API；别拿它做口令存储/数字签名 ✗）
var m   = crypto.md5("abc")                            // ⇒ "900150983cd24fb0d6963f7d28e17f72"
var m16 = crypto.md5_bytes(strings.to_bytes("abc"))    // ⇒ 16 原始字节

// HMAC-SHA256
var mac = crypto.hmac_sha256("key", "msg")

// AES-128-CBC（**内置 PKCS7**：加密自动补、解密自动去）
var key = strings.to_bytes("0123456789abcdef")   // 16 字节
var iv  = strings.to_bytes("abcdef0123456789")   // 16 字节
var ct  = crypto.aes128_cbc_encrypt_bytes(key, iv, strings.to_bytes("hello leno crypto"))
var pt  = crypto.aes128_cbc_decrypt_str(key, iv, ct)   // ⇒ "hello leno crypto"
```

> `strings.from_base64` 比手工实现**更宽容**：两种字母表都认（`+/` 与 `-_`）、缺 `=`
> padding 也收、空白字符忽略；非法字符则**报错并指出第几位**（不静默截断 ✓）。

## 设计约定

- **加解密一律走 `Array[int]`（字节）** ✓；字符串只是糖（Leno 的 string 是字节串，二进制经它往返易失真 ✗）。
  ⇒ 文本 ⇄ 字节用 `strings.to_bytes` / `strings.from_bytes` ✓。
- **子模块互不依赖**（`crypto_md5` / `crypto_sha256` / `crypto_sha512` / `crypto_hmac_sha256` / `crypto_aes`），
  组合只发生在门面 `Crypto.leno` ⇒ 换实现不影响别人 ✓。
- 五个子模块都是 `examples/crypto/*.leno` 的**机械搬运**（截断各自 `main()`、一律 `export`，
  算法代码逐字保留 ✓）⇒ 库里不含"新写但没验证"的实现 ✓；`crypto_aes` 末尾追加了 bytes 版
  接口与 CBC/PKCS7 ✓。
- `crypto_base64`（原第 5 个子模块）已于 2026-10-07 **删除**：base64 归核心标准库，
  `examples/crypto/base64.leno` 同步删除，调用方（音乐下载器 / Trae签到 / 本库测试）全部改走 `strings.*` ✓

## 验证

`test/test_crypto_vectors.leno`：断言值全部由 **node:crypto 生成**（权威），作为绝对期望值 ✓

| 项 | 向量 |
| --- | --- |
| `md5("abc")` | `900150983cd24fb0d6963f7d28e17f72`（另有 RFC 1321 全套 + 跨块 + 中文 UTF-8 + `md5_bytes` 长度用例）|
| `sha256("abc")` | `ba7816bf…20015ad` |
| `sha512("abc")` | `ddaf35a1…fa54ca49f` |
| `hmac_sha256("key","msg")` | `2d93cbc1…bb1b8c628` |
| `strings.to_base64(strings.from_bytes([0,1,2,253,254,255]))` | `AAEC/f7/` |
| AES-128-CBC 往返 | key=`0123456789abcdef`、iv=`abcdef0123456789`、明文=`hello leno crypto`、密文=`fd0ebb65…f6fb4a09` |

```
build\leno.exe leno_module\LenoCrypto\test\test_crypto_vectors.leno
```

## 现状 / 待做

- ✅ 已收录：MD5、SHA-256、SHA-512、HMAC-SHA256、AES-128（ECB/CBC/PKCS7）；
  base64 与文本⇄字节归核心标准库 `strings`
- ⏳ 待收录（`examples/crypto/` 里已有实现，可直接搬）：SHA-1、PBKDF2、ChaCha20、RC4、
  RC5、TEA/XTEA/XXTEA、Speck、Vigenere、RSA
  （MD5 于 2026-10-08 收录，触发点是**酷狗 API 的签名就是 md5** —— 见音乐下载器的酷狗源）
- ⏳ 性能：`sha512_bytes` 里两个循环目前较慢（每轮都有 `OP_CAST_INT` 的开销，结果正确）⇒
  见 `docs/待办_易用性痛点（TraeSign 移植实录）.md` 的 J1 ✓
