# LenoUUID v0.1.0

Leno 的 UUID 模块（**纯 Leno**，随机数走内置 `rands`）。

```leno
import "UUID" as uuid

var id  = uuid.v4()        // 550e8400-e29b-41d4-a716-446655440000 风格（随机）
var ord = uuid.v7()        // 按时间排序（前 48 位是时间戳）
var sid = uuid.short()     // 12 位十六进制短 ID
uuid.isValid(id)           // true
uuid.versionOf(ord)        // 7
```

## API

| 函数 | 说明 |
|---|---|
| `v4()` | RFC 4122 v4 随机 UUID（version=4，variant=10xx） |
| `v7()` | RFC 9562 v7：48 位**秒级**时间戳 + 随机位（适合做可排序主键/日志关联） |
| `short(n = 12)` | n 位十六进制短 ID（`n<=0` 按 1 处理） |
| `nil()` | 全零 UUID |
| `isValid(s)` | 36 位格式 + 四个 `-` 位置 + 十六进制（大小写都收）+ version 1..8、variant 8/9/a/b；全零 UUID 也算合法 |
| `versionOf(s)` | 取 version 数字；非法返回 -1（nil 返回 0） |

## 边界

- **v7 的时间精度是秒**：`times.now()` 是秒级墙钟，本语言没有毫秒墙钟 ⇒ 同一秒内多个
  v7 的前 48 位相同（靠随机位区分）。仍可排序，但"同秒内严格递增"不保证；
  需要严格单调请在调用方自己加计数器。
- **`short()` 不保证全局唯一**：12 位十六进制 ≈ 2^48 空间，日志/临时文件名够用；
  当数据库主键请用 v4/v7。
- 随机源是 `rands`（非密码学安全）。做 token/密钥请用 `LenoCrypto` 的随机字节。

## 验证

```bat
build\leno.exe build\leno_module\LenoUUID\test\test_uuid.leno
```

6 组断言：v4 格式与版本/变体位、500 个不重复、v7 版本位与前缀单调、校验正反例
（长度/非十六进制/大写/全零）、短 ID 长度与字符集、nil。
