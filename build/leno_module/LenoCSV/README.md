# LenoCSV v0.1.0

Leno 的 CSV 模块（**纯 Leno，零原生依赖**）。RFC 4180 的容错实现 + 表头/文件/记录助手。

```leno
import "CSV" as csv

// ① 文本 ⇄ 行×列
var rows = csv.parse("a,b\n1,2\n")          // [["a","b"], ["1","2"]]
var text = csv.encode(rows)                  // "a,b\r\n1,2"（默认 CRLF，Excel 友好）

// ② 带表头 → 记录
var t = csv.parseTable("name,age\nAlice,30\n")
var recs = csv.toDicts(t.header, t.rows)     // [{name:"Alice", age:"30"}]

// ③ 文件
var d = csv.load("data.csv", ",", true)      // {ok, header, rows, err}
csv.save("out.csv", d.rows, ",", true)
```

## API

| 函数 | 说明 |
|---|---|
| `parse(text, delim=",")` | → `Array[Array[string]]`（行×列，含表头行） |
| `encode(rows, delim=",", crlf=true)` | 行×列 → 文本（**最小引号**） |
| `encodeRecords(header, rows, ...)` | 表头 + 记录 → 文本 |
| `parseTable(text, delim=",", hasHeader=true)` | → `{ok, header, rows, err}` |
| `toDicts(header, rows)` | → `Array[Dict]`（缺列补 `""`，多列忽略） |
| `load(path, delim=",", hasHeader=true)` | 读文件，同上 Dict；文件不存在 ⇒ `ok=false` + 原因 |
| `save(path, rows, delim=",", crlf=true)` | 写文件，返回是否落盘 |
| `widths(rows)` | 每行列数（排查"第 3 行少一列"） |
| `delimByte(delim)` | 分隔符首字节（自检用） |

## 行为口径

**解析（容错，但绝不静默改数据）**
- 引号内可含分隔符、换行；`""` 表示一个字面的 `"`
- 引号只在**字段开头**才特殊；闭合引号后的杂字符**保留**
- 行尾 `CRLF` / `LF` / 单独 `CR` 都认；文件开头 UTF-8 BOM 去掉
- **空行跳过**（`a\n\nb` 是 2 行），但 `a,,` 的**空字段保留**
- 读 `a,b,c` 后面某行只有 `1` ⇒ 如实返回 1 个字段（不补齐、不报错）；要按表头取值用 `toDicts`

**生成（最小引号）**
- 只有含分隔符 / 双引号 / CR / LF / 以引号开头时才加引号；`"` 翻倍转义
- 默认 `\r\n`；`crlf=false` 用 `\n`
- 不做"前后空格自动加引号"（那会改变"空格是数据"的语义）

**不做**
- 值全部是 `string`：**不做数字/日期推断**（猜错比不猜更难查）
- 分隔符只支持**单字节**（`,` `;` `\t` `|` 都行）
- 不做编码转换（按 UTF-8 文本处理；其它编码请先转）

## 验证

```bat
:: ① 15 组断言：基本/引号/换行/BOM/空行/自定义分隔符/往返/表头/文件/中文/不齐/空输入
build\leno.exe build\leno_module\LenoCSV\test\test_csv.leno

:: ② 与 Python 标准库 csv 双向交叉验证（开发期工具，需要 python）
python build\leno_module\LenoCSV\test\dev_crosscheck_python.py
```

交叉验证的内容包括：引号内逗号、引号内换行、`""` 转义、中文+emoji、全空字段、
前后空格、CRLF —— **8 行 8 列逐格一致**（Python 写 → LenoCSV 读 → 写回 → Python 读）。

为什么需要第 ② 条：自己写的解析器 + 自己的生成器容易"两边一起犯同一个错"，
必须拿独立实现比对（与 LenoQR 的"矩阵逐格对拍 python qrcode"同一取向）。
