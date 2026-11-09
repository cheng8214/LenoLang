# LenoResult —— `Result[T]` / `Option[T]`（统一错误与空值契约，P2）

纯 Leno，无原生库。19 个导出：2 个泛型类型、2 个枚举相关、5 个构造、3 个组合、2 个严格解析、1 个码名转换。

```leno
import "Result" as res
use res.(Result, Option)
```

## 为什么是它（问题定义）

评估报告 §4.5.1 统计：标准库与四个项目里并存 **8 种错误表达**（`Dict{ok,err}` / `bool` /
`int -1` / typed `Result` struct / `error` 字段 / `throw` / `Result[T]` / `Response.ok()`）与
**4 类空值哨兵**（`null` / `""` / `0` / `[]`）。代价不是难看，是**无法判断**：

```leno
// 修前：NULL 和 0 完全一样
int n = stmt.column_int(3)     // NULL ⇒ 0，值就是 0 也是 0

// 修后：失败/缺值有各自的形态，判断依据只有一处
Result[int] r = res.parseInt(stmt.column_text(3))
```

## 三条规则

| 场景 | 用 |
|---|---|
| 可能失败的操作（IO / 网络 / 解析 / DB） | `Result[T]` —— `ok` / `code` / `msg` / `val` |
| 可能"没有值"但不算失败 | `Option[T]` —— `some` / `val` |
| 函数内部临时判空 | 继续用 `T?`（便宜；**跨模块边界**才换 Option） |

## 用法

```leno
Result[int] a = res.ok(42)
if a.isOk() { print(_str(a.val)) }

Result[int] b = res.parseInt("12a")            // PARSE: 不是整数（含非数字字符）：12a
print(b.errText())                              // "PARSE: 不是整数…"
print(_str(b.orElse(0)))                        // 默认值必须**显式**给（这就是不吃"静默 0"的关键）

// 组合：失败短路，code/msg 一路带过去
Result[string] s = res.mapResult(a, func(int v) { return _str(v) })
Result[int] c = res.flatMap(a, func(int v) { return res.ok(v + 1) })

// T 只在返回类型里出现 ⇒ 调用点写 [T]
Result[int] e = res.err[int](res.ErrCode.NOT_FOUND, "没有这一行")
Option[string] o = res.none[string]()
```

## 错误码

`ErrCode`：`OK / INVALID_ARG / NOT_FOUND / PARSE / IO / NET / DB / AUTH / CONFLICT / UNSUPPORTED / INTERNAL`。
码只回答"哪一类"，具体原因放 `msg`；`codeName(code)` 用于日志与响应体。

## 严格解析

`parseInt` / `parseFloat` 是**严格**的：不 trim、不认 `nan/inf`、不认全角、不认十六进制。
要宽松就在调用点 `strings.trim(s)` —— 别把宽松塞进解析器里。

| 输入 | `parseInt` | `parseFloat` |
|---|---|---|
| `"42"` / `"-7"` / `"+7"` / `"007"` | 42 / -7 / 7 / 7 | 42.0 / -7.0 / 7.0 / 7.0 |
| `"1.5"` | ❌ PARSE | 1.5 |
| `".5"` / `"1e3"` / `"2E-2"` | ❌ | 0.5 / 1000.0 / 0.02 |
| `" 12"` / `"12a"` / `"１２"` / `""` | ❌ | ❌ |

## 测试

```bat
build\leno.exe build\leno_module\LenoResult\test\test_result.leno
build\leno.exe build\leno_module\run_all_tests.leno --only LenoResult
```
