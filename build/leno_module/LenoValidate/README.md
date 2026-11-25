# LenoValidate

声明式数据校验（**纯 Leno**）：必填 / 类型 / 范围 / 长度 / 枚举 / 正则 / 自定义，一次校验整个 Dict、一次报出所有错误。

```leno
import "Validate" as v
use v.(Rule, Schema)

Schema s = new Schema()
s.fields["name"]  = new Rule(required = true, minLen = 1, maxLen = 20)
s.fields["age"]   = new Rule(required = true, typ = "int", minNum = 0, maxNum = 150)
s.fields["role"]  = new Rule(oneOf = ["admin", "user"])
s.fields["email"] = new Rule(pattern = "^[^@]+@[^@]+$")

Array[string] errs = v.validate(data, s)
if errs.len() == 0 { /* 通过 */ }
```

## 规则

| 字段 | 含义 | 缺省 = 不检查 |
|---|---|---|
| `required` | 缺失 / 显式 `null` 报错（**空串不算缺失**，要非空加 `notEmpty`） | `false` |
| `notEmpty` | 空串 / 空数组 / 空 Dict 报错 | `false` |
| `typ` | `"string"/"int"/"float"/"number"/"bool"/"array"/"dict"/"any"` | `""` |
| `minNum` / `maxNum` | 数值范围（int/float 都认）；**值不是数值时明确报错**（不再静默放过） | `null` |
| `minLen` / `maxLen` | 长度：string 按字符、Array/Dict 按元素数 | `-1` |
| `oneOf` | 字符串枚举（值转字符串比较） | `[]` |
| `pattern` | 正则（`regexs.match`，从头部锚定；全串匹配写 `^…$`） | `""` |
| `check` | 自定义 `func(any): string?`，返回 null = 通过 | `null` |
| `sub` | 嵌套：值是 Dict 时按子 `Schema` 再验一层，错误字段名 `父.子` | `null` |
| `item` | 数组元素：值是 Array 时逐个按该 `Rule` 验，错误字段名 `父[i]` | `null` |

## API

| 函数 | 说明 |
|---|---|
| `validate(data, schema)` | 校验整个 Dict → `Array[string]`（`"字段: 消息"`，空 = 通过） |
| `validateDetailed(data, schema)` | 校验 → `Array[FieldError]`（`field` / `message` / `kind`，按字段标红用这个） |
| `checkValue(val, rule)` | 校验单值 → `Array[string]` |
| `ok(errs)` | 错误列表是否为空 |

`FieldError.kind` 取值：`required` / `empty` / `type` / `range` / `len` / `enum` / `pattern` / `check`。

- 只检查 schema 里声明的字段，**未声明的字段不报错**（容忍多余字段）。
- `typ = "number"` 是宽松类型：int/float 都收；`typ = "int"` 只认 int。
- 嵌套 / 数组的字段路径：`addr.city`、`tags[2]`（顶层就是字段名本身）。

## 变更记录

- **0.2.0**（2026-11-25）：新增 `notEmpty`、嵌套 `sub`、数组元素 `item`、结构化错误
  `validateDetailed` + `FieldError`；范围规则遇到非数值**不再静默**（明确报错）。
  `validate` / `checkValue` 的返回格式与旧行为保持一致。
- **0.1.0**：必填 / 类型 / 范围 / 长度 / 枚举 / 正则 / 自定义。

## 跑测试

```bat
build\leno.exe build\leno_module\LenoValidate\test\test_validate.leno
```
