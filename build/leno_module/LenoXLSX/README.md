# LenoXLSX v0.2.0

Leno 的 Excel（.xlsx）读写模块（**纯 Leno，零原生依赖**）。建立在 `LenoZip` 之上，
读 zip 内的 deflate 条目时走 `LenoCompress`。

```leno
import "XLSX" as xlsx

// ① 写
var wb = xlsx.blank()
xlsx.addSheet(wb, "数据", rows)              // rows: Array[Array[string]]（按文本写）
xlsx.addSheet(wb, "数值", nums, true)        // numeric=true：纯整数写成数字单元格
xlsx.save("out.xlsx", wb)

// ② 读
var r = xlsx.read("out.xlsx")                // {ok, err, sheets:[{name, rows}]}
if r.ok {
    for r.sheets to s { print(s.name + "  " + _str(s.rows.len()) + " 行") }
}
```

## API

| 函数 | 说明 |
|---|---|
| `blank()` | 空工作簿 `{sheets: []}` |
| `addSheet(wb, name, rows, numeric=false)` | 追加一个 sheet（名字会按 Excel 规则净化：`:\/?*[]` → `_`，≤31 字符） |
| `encode(wb)` | → xlsx 字节（字符串载体） |
| `save(path, wb)` | 写文件，返回是否落盘 |
| `read(path)` / `readBytes(raw)` | → `{ok, err, sheets:[{name, rows}]}` |
| `sheetNames(book)` / `rowsOf(book, i)` | 便捷取值 |

读出来的单元格**一律是 string**（数字不转 float：Excel 里 `1` 与 `1.0` 语义不同，
该不该转是业务判断）。

## 生成的 xlsx 长什么样

最小合法工作簿，5 个部件：

```
[Content_Types].xml            ← 声明 workbook / 每个 worksheet
_rels/.rels                    ← → xl/workbook.xml
xl/workbook.xml                ← <sheets><sheet name r:id/></sheets>
xl/_rels/workbook.xml.rels     ← 每个 sheet 一条 Relationship
xl/worksheets/sheetN.xml       ← <sheetData><row><c>…
```

- 文本用 `t="inlineStr"`（**不用 sharedStrings**：省一张表与一轮索引）
- 纯整数（≤15 位、无前导零、可选负号）在 `numeric=true` 时写成 `<v>42</v>` 数字单元格
- 前后空格写 `xml:space="preserve"`
- 空串**不落单元格**（文件更小；读回仍是 `""`）
- zip 条目是 **deflate**（`LenoZip` 的默认；XML 重复度高，通常能压到 1/10 以下）

## 边界（v0.1 明确不做）

| 不做 | 说明 |
|---|---|
| 样式 | 字体/颜色/边框/列宽/行高/合并单元格/冻结窗格 —— 全没有（默认样式） |
| 数字格式 | 小数、日期都按**文本**写（日期需要样式与序列号换算） |
| 公式 | 只写值，不写 `<f>` |
| 图片/图表/数据透视表 | 读时忽略，写时不生成 |
| 大文件 | 整包在内存里拼（字符串拼接）；v0.1 适合几千行级别的导出 |

> 读「别人写的 xlsx」时支持 `sharedStrings` / `inlineStr` / 布尔 / 数字；
> 带样式与格式的数字**拿不到原始显示串**（读到的是序列号/原值），这是"无样式解析"的固有限制。

## 实测（Windows x64）

| 规模 | 写（deflate） | 读 | 文件体积 |
|---|---|---|---|
| 100×10 = 1000 格 | 8 ms | 20 ms | — |
| 1000×10 = 10000 格 | 428 ms | 231 ms | **58 KB**（stored 时约 1.5 MB） |

写里约 **350ms 花在压缩**上（10k 格 × 每格 deflate 的 LZ77 搜索）；要更快可以
`Zip.save(..., compress=false)` 换体积（xlsx_write 目前固定压缩）。
读约 23µs/格，瓶颈在按标签切块时的字符串拷贝。

两处已修掉的 O(n²)：逐格 `s = s + …` 拼接（万格 1.8 秒 → 80ms）、`find_from`
每次 `to_bytes()` 整份 XML（92ms → 24ms）。

## 验证

```bat
:: 自测：单/多 sheet、数字模式、中文、特殊字符、前后空格、空单元格、异常输入
build\leno.exe build\leno_module\LenoXLSX\test\test_xlsx.leno

:: 与 Python 标准库双向交叉验证（开发期工具，需要 python；不依赖 openpyxl）
python build\leno_module\LenoXLSX\test\dev_crosscheck_python.py
```

交叉验证做了什么：

| 方向 | 判据 | 结果 |
|---|---|---|
| LenoXLSX 写 → Python | `zipfile.testzip()`（CRC）+ 必备部件齐全 + **每个 XML 部件用 ElementTree 解析** + 用 ET 独立取单元格值比对（条目是 deflate，Python 正常解压） | 全过 |
| Python 独立造（sharedStrings + inlineStr + bool + 数字，`ZIP_DEFLATED`）→ LenoXLSX 读 | 逐格比对（含中文、`&`/`<` 转义、前后空格、布尔） | 逐格一致 |

> ⚠ 没有在真实的 Excel / WPS / LibreOffice 里打开验证过（本机没有 Office）。
> 结构与值已通过独立 XML 解析器校验；如果发现某个阅读器不认，优先怀疑缺 `styles.xml`。
