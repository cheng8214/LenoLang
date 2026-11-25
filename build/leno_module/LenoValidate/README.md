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
| `required` | 缺失 / 显式 `null` 报错 | `false` |
| `typ` | `"string"/"int"/"float"/"number"/"bool"/"array"/"dict"/"any"` | `""` |
| `minNum` / `maxNum` | 数值范围（int/float 都认） | `null` |
| `minLen` / `maxLen` | 长度：string 按字符、Array/Dict 按元素数 | `-1` |
| `oneOf` | 字符串枚举（值转字符串比较） | `[]` |
| `pattern` | 正则（`regexs.match`，从头部锚定；全串匹配写 `^…$`） | `""` |
| `check` | 自定义 `func(any): string?`，返回 null = 通过 | `null` |

## API

| 函数 | 说明 |
|---|---|
| `validate(data, schema)` | 校验整个 Dict → `Array[string]`（空 = 通过） |
| `checkValue(val, rule)` | 校验单值 → `Array[string]` |
| `ok(errs)` | 错误列表是否为空 |

- 只检查 schema 里声明的字段，**未声明的字段不报错**（容忍多余字段）。
- `typ = "number"` 是宽松类型：int/float 都收；`typ = "int"` 只认 int。

## 跑测试

```bat
build\leno.exe build\leno_module\LenoValidate\test\test_validate.leno
```
