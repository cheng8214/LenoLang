# LenoCrypto

Leno 通用加密库：**MD5 / SHA-256 / SHA-512 / HMAC-SHA256 / AES-128 与 AES-256（ECB 单块 + CBC + PKCS7 + GCM/AEAD）**，
纯 Leno 实现，无外部依赖。

> **base64 与"文本 ⇄ 字节"不在这里**：它们是标量编解码、不是密码学原语（无密钥、无安全性质），
> 已归**核心标准库** `strings.to_base64` / `strings.from_base64` / `strings.to_bytes` /
> `strings.from_bytes`（2026-10-07，与 `to_hex` / `from_hex` 同族）✓

> 缘起：用 Leno 移植 TraeSign 时，为了"派生密钥 + AES-128-CBC 解密登录态"，
> 不得不在应用里**搬 800 行纯 Leno 的 AES/SHA-512** ✗（对照 C# 只写 33 行，因为它自带
> `SHA512.Create()` / `Aes.Create()`）。与其每个项目都搬一遍，不如做成标准库 ✓。
>
> 追加缘起（2026-10-14）：给 WorkBuddy 自动签到做"纯 Leno 解凭据"时，撞上的是
> **AES-256-GCM + 自定义 AAD** 的信封（`sym-v1`：key=sha256(密钥串)、nonce 12 字节、
> AAD 里带 keyId 的分帧头）—— 本库当时只有 AES-128 的 ECB/CBC，于是又得回去借 Node ✗。
> 触发点和上次一模一样：**与其每个项目各搬一份，不如进标准库** ✓。

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

// AES-256 单块（32 字节密钥）；这条就是 FIPS-197 附录 C.3 的例子
var k32 = crypto.hex_to_bytes("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f")
var blk = crypto.aes256_encrypt_block(crypto.hex_to_bytes("00112233445566778899aabbccddeeff"), k32)

// GCM / AEAD：密钥 16 字节 ⇒ AES-128-GCM，32 字节 ⇒ AES-256-GCM（**按长度自动选**）
//   seal 返回 **密文 || 标签(16)**；open 先校验标签，不符**抛错**（AEAD 不交付未认证的明文）
var nonce = strings.to_bytes("0123456789ab")     // 12 字节（推荐；其它长度走规范的 GHASH 构造，见下）
var aad   = strings.to_bytes("WB-AAD" + strings.char(0) + strings.char(1))
var sealed = crypto.aes_gcm_seal(k32, nonce, aad, strings.to_bytes("payload"))
var back   = crypto.aes_gcm_open(k32, nonce, aad, sealed)              // ⇒ 原字节
// 信封把密文与标签拆成两个字段时（如 WorkBuddy 的 sym-v1）：
var plain  = crypto.gcm_open_parts(k32, nonce, aad, ct_bytes, tag_bytes)
var tag    = crypto.gcm_tag_bytes(k32, nonce, aad, ct_bytes)           // 只算标签、不取明文
```

> **GCM 的 nonce 长度**：12 字节走 `nonce || 0x00000001` 这条推荐路径；
> 其它长度按 NIST SP 800-38D 用 GHASH 构造 J0（兼容老实现，但性能与安全性都不如 12 字节）。
> 两条路径都已用权威向量锁住 —— 它们的差别不只是"少几个字节"，**分块边界完全不同**。

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
- **例外（2026-10-14）**：AES-256 与 GCM **不是**搬运来的（`examples/crypto/` 里没有对应实现），
  是本模块内新写的 —— 因此它们**必须有权威向量兜底**（FIPS-197 单块 + NIST SP 800-38D 用例，
  用 node:crypto 复算一致），不接受"自洽往返"当验收 ✓。
  AES-128 那三个老函数（`key_expansion` / `aes_encrypt_block` / `aes_decrypt_block`）**逐字未动**：
  它们已被现有调用方依赖、也过了向量校验，不做"顺手重构"。
  GCM 用规范里的**右移型参考算法**（大端位串口径）而不是软件常见的"小端整数"版 ——
  位序写错的典型症状是"空输入/单块对得上、一多块全错"，正好躲过小样例。
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
| AES-128 / AES-256 单块 | FIPS-197 附录 C.1 / C.3：`69c4e0d8…b4c55a` / `8ea2b7ca…4b496089`（**单向**期望值，自洽往返测不出密钥扩展写错）|
| GCM（AES-256） | SP 800-38D 用例 13/14（全零 key/iv：空明文与 16 字节明文）、用例 15（60 字节明文 + 20 字节 AAD，密文与标签都锁）|
| GCM（AES-128） | 同一组参数的用例 3（`42831ec2…` / 标签 `5bc94fbc…`）—— 证明"按密钥长度自动选"两条都对 |
| GCM 非 12 字节 IV | 16 字节 IV 的通用 J0 路径（key/iv 全零、16 字节明文）⇒ 密文 + 标签 |
| GCM 应用形态 | key=sha256(密钥串)、12 字节 nonce、AAD=`WB-AAD\0\|0x01\|lp('WBEV1')\|lp('sym-v1')\|0,0,0,1\|lp(keyId)\|2,0,0`、275 字节明文 ⇒ 密文/标签/往返 |
| GCM 拒绝篡改 | 标签 / 密文 / AAD / 密钥 任一被改、密钥长度非法 ⇒ **必须抛错**（不许交付明文）|

```
build\leno.exe leno_module\LenoCrypto\test\test_crypto_vectors.leno
```

## 现状 / 待做

- ✅ 已收录：MD5、SHA-256、SHA-512、HMAC-SHA256、AES-128（ECB/CBC/PKCS7）、
  **AES-256（ECB 单块 + GCM）**；base64 与文本⇄字节归核心标准库 `strings`
- ⏳ 待收录（`examples/crypto/` 里已有实现，可直接搬）：SHA-1、PBKDF2、ChaCha20、RC4、
  RC5、TEA/XTEA/XXTEA、Speck、Vigenere、RSA
  （MD5 于 2026-10-08 收录，触发点是**酷狗 API 的签名就是 md5** —— 见音乐下载器的酷狗源）
- ⏳ 性能：`sha512_bytes` 里两个循环目前较慢（每轮都有 `OP_CAST_INT` 的开销，结果正确）⇒
  见 `docs/待办_易用性痛点（TraeSign 移植实录）.md` 的 J1 ✓
- ⏳ 性能（GCM）：实测 **1365 字节 seal ≈ 27 ms**（50 次合计 1373 ms；其中 GHASH 约 10 ms，
  其余是 AES-256 的 14 轮 × 86 个块，逐轮都在新建 `Array[int]`）。对"每次运行解一次凭据"
  这种用量完全够用；真要提速，方向是**表驱动 + 就地改 state**（现在每轮 `sub_bytes`/`shift_rows`
  都分配新数组），但那属于重写核，得先有基准再动 —— 与 J1 同一条思路：别为"看起来快"改算法 ✓
- ⏳ 可用性：GCM 目前只输出 16 字节标签（规范允许 4~16 字节截断），需要短标签时再补参数 ✓
