# LenoCrypto

Leno 通用加密库：**Base64 / SHA-256 / SHA-512 / HMAC-SHA256 / AES-128（ECB 单块 + CBC + PKCS7）**，
纯 Leno 实现，无外部依赖。

> 缘起：用 Leno 移植 TraeSign 时，为了"派生密钥 + AES-128-CBC 解密登录态"，
> 不得不在应用里**搬 800 行纯 Leno 的 AES/SHA-512** ✗（对照 C# 只写 33 行，因为它自带
> `SHA512.Create()` / `Aes.Create()`）。与其每个项目都搬一遍，不如做成标准库 ✓。

## 用法

```leno
import "leno_module/LenoCrypto/lib/Crypto.leno" as crypto

// Base64（bytes 友好：二进制安全）
var bs = crypto.b64_to_bytes("AAEC/f7/")          // ⇒ [0,1,2,253,254,255]
var s  = crypto.bytes_to_b64([0,1,2,253,254,255]) // ⇒ "AAEC/f7/"

// 哈希（返回十六进制小写；*_bytes 返回原始字节）
var h  = crypto.sha256("abc")
var h2 = crypto.sha512("abc")
var raw = crypto.sha512_bytes(crypto.str_to_bytes("abc"))

// HMAC-SHA256
var mac = crypto.hmac_sha256("key", "msg")

// AES-128-CBC（**内置 PKCS7**：加密自动补、解密自动去）
var key = crypto.str_to_bytes("0123456789abcdef")   // 16 字节
var iv  = crypto.str_to_bytes("abcdef0123456789")   // 16 字节
var ct  = crypto.aes128_cbc_encrypt_bytes(key, iv, crypto.str_to_bytes("hello leno crypto"))
var pt  = crypto.aes128_cbc_decrypt_str(key, iv, ct)   // ⇒ "hello leno crypto"
```

## 设计约定

- **加解密一律走 `Array[int]`（字节）** ✓；字符串只是糖（Leno 的 string 是字节串，二进制经它往返易失真 ✗）。
  ⇒ 需要 `str_to_bytes` / `bytes_to_str` / `b64_to_bytes` / `bytes_to_b64` 这四件套 ✓。
- **子模块互不依赖**（`crypto_base64` / `crypto_sha256` / `crypto_sha512` / `crypto_hmac_sha256` /
  `crypto_aes`），组合只发生在门面 `Crypto.leno` ⇒ 换实现不影响别人 ✓。
- 五个子模块都是 `examples/crypto/*.leno` 的**机械搬运**（截断各自 `main()`、一律 `export`，
  算法代码逐字保留 ✓）⇒ 库里不含"新写但没验证"的实现 ✓；`crypto_base64` / `crypto_aes` 末尾
  追加了 bytes 版接口与 CBC/PKCS7 ✓。

## 验证

`test/test_crypto_vectors.leno`：断言值全部由 **node:crypto 生成**（权威），作为绝对期望值 ✓

| 项 | 向量 |
| --- | --- |
| `sha256("abc")` | `ba7816bf…20015ad` |
| `sha512("abc")` | `ddaf35a1…fa54ca49f` |
| `hmac_sha256("key","msg")` | `2d93cbc1…bb1b8c628` |
| `bytes_to_b64([0,1,2,253,254,255])` | `AAEC/f7/` |
| AES-128-CBC 往返 | key=`0123456789abcdef`、iv=`abcdef0123456789`、明文=`hello leno crypto`、密文=`fd0ebb65…f6fb4a09` |

```
build\leno.exe leno_module\LenoCrypto\test\test_crypto_vectors.leno
```

## 现状 / 待做

- ✅ 已收录：Base64、SHA-256、SHA-512、HMAC-SHA256、AES-128（ECB/CBC/PKCS7）
- ⏳ 待收录（`examples/crypto/` 里已有实现，可直接搬）：SHA-1、MD5、PBKDF2、ChaCha20、RC4、
  RC5、TEA/XTEA/XXTEA、Speck、Vigenere、RSA
- ⏳ 性能：`sha512_bytes` 里两个循环目前会被 JIT 在 `OP_CAST_INT` 处 bail 拉黑（掉回解释器跑，
  结果正确）⇒ 见 `docs/待办_易用性痛点（TraeSign 移植实录）.md` 的 J1 ✓
