# LenoXLSX v0.3.0

Leno 的 Excel 模块（**纯 Leno，零原生依赖**）：`.xlsx` 读/写、`.xls`（97-2003）读。
`.xlsx` 建立在 `LenoZip` 之上，读 zip 内的 deflate 条目时走 `LenoCompress`；
`.xls` 走自带的 OLE2 + BIFF8 解析（不需要 zip）。

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
| `read(path)` / `readBytes(raw)` | → `{ok, err, sheets:[{name, rows}]}`；**按文件头**自动分派 .xlsx / .xls |
| `readXls(path)` | 强制按 .xls 解析（错误信息更具体，排查时用） |
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

## .xls（Excel 97-2003，v0.3 起只读）

```leno
import "XLSX" as xlsx

var r = xlsx.read("旧表.xls")        // 按文件头识别：D0 CF 11 E0 ⇒ .xls
if r.ok {
    for r.sheets to s { print(s.name + "  " + _str(s.rows.len()) + " 行") }
}
```

`.xls` 不是 zip（是 OLE2 复合文档），所以和上面的 xlsx 路径完全分开：

```
[OLE2]  扇区 → DIFAT/FAT → 目录项 → 解出 "Workbook" 流（小流走 miniFAT + mini 流）
[BIFF8] 记录流 → BOUNDSHEET（sheet 名 + 各自 BOF 偏移）
                 SST (+CONTINUE) → 共享字符串
                 每个 sheet 区间内的 LABELSST / LABEL / RSTRING /
                 RK / MULRK / NUMBER / BOOLERR → 单元格
```

- 数值：`RK` 的三种编码（30 位整数、整数/100、double 高 30 位）都还原；
  输出时**整数不带 `.0`**（`2.0` ⇒ `"2"`），其余原样
- `SST` 的字符数据**可以跨 CONTINUE**：每跨一块，新块首字节是新的 `fHighByte`
  （读错的典型症状就是中文变乱码/丢字，这里按块边界处理）
- 行号是**稀疏**的：读出来是 `Array[Array[string]]`，缺的行/列补 `""`
- sheet 顺序按 BOUNDSHEET（= Excel 里的标签顺序），空表返回 `rows: []`

**边界**：日期只能拿到**序列号**（没有样式就没有数字格式，和 xlsx 读同一口径）；
只支持 BIFF8（Excel 97-2003），BIFF5 及更早报错；加密工作簿（FILEPASS）不支持；
公式只跳过（不返回缓存值）；图表/图片/嵌入对象忽略。

**验证**（2026-11-26，`D:\CLeno\1月份.xls`，3 sheet / 612 单元格 / 142 个共享字符串）：

```bat
build\leno.exe build\leno_module\LenoXLSX\test\dev_xls_read.leno          :: 人工看前几行
build\leno.exe build\leno_module\LenoXLSX\test\dev_xls_dump_tsv.leno      :: 导出 .tmp_leno.tsv
python build\leno_module\LenoXLSX\test\dev_xls_crosscheck.py "D:\CLeno\1月份.xls" .tmp_py.tsv
```

两份 TSV **逐行逐格完全一致**（187 行），Python 那份是独立实现（`struct` 手解 OLE2 +
BIFF，不依赖 xlrd/openpyxl），所以不是"同源自我确认"。

**速度**（2026-11-28 实测，i5-3450 / Windows x64，100 次中位数，同机同轮。样本
`test/sample.xls`：15 KB / 3 sheet / 601 非空格 / **269 个共享字符串**，由
`test/gen_sample_xls.py` 生成 ⇒ 随时可复跑）：

| 阶段 | Leno（本模块） | Python 参考实现 |
|---|---|---|
| 读文件 `files.read` | 103 µs | 94 µs（`open().read()`） |
| `to_bytes()` | 68 µs | —（`bytes` 本身就是 C 数组） |
| OLE2 取 Workbook 流 | **46 µs** | 57 µs |
| BIFF8 记录解析 | 2188 µs | ~2133 µs |
| **完整读取** | **2234 µs** | **2190 µs** |

> **差距 1.02 倍（≈2%）**：OLE2 层已经**反超** Python（46 vs 57 µs —— `bytes` 模块的
> `read_u32` 走 string 载体免 `to_bytes`，比 Python 的 `struct.unpack_from` 还省一次
> 元组分配）；BIFF8 段只慢 2.5%；剩下的就是 `to_bytes` 那 68 µs（Python 不需要这一步）。
> 在**共享字符串更多**（269 vs 旧样本 142）的负载上打平，说明 SST 解码换
> `bytes.decode_utf16le` 之后已不是短板。

同一份数据读成 `Array[Array[string]]`。参照：同内容的 `.xlsx` 读一次约 **35 ms**
（要 inflate + 扫 XML），`.xls` 快约 **16 倍**。

> **对照口径要说明**：Python 那边是"Python 逻辑 + C 原语"——
> `struct.unpack_from`（一次调用解 4/8 字节，还能 `<109I` 一次解 109 个整数）、
> `bytes` 切片（C memcpy）、`decode("utf-16-le")`（C 整段解码）。
> Leno 这边**控制流同为解释执行**，但每个字段是"函数调用 + 手写位运算"，
> 字符串构造是 `str_new`。所以差距是**原语粒度**的差距，不是逻辑写得差。

优化过程（都是实测数字，不是估计）：

| 写法 | Workbook 流（39 KB） |
|---|---|
| `Array[int]` 逐字节 `add` | 3.5 ms |
| 字符串切片 + `out = out + slice`（**O(n²)**） | 1.7 ms |
| 80 段 `byte_slice` + `join("")` | 465 µs |
| 连续扇区合并成 1 段（现用） | 225 µs → **165 µs**（见下条） |

SST 解码同样换过写法（10 字中文串）：

| 写法 | 耗时 |
|---|---|
| `parts.add(strings.from_codepoint(u))` + `join` | 9 µs |
| 手写 UTF-8 编码到 `Array[int]` + `strings.from_bytes`（现用） | **2 µs** |

最后一个大头是**字段解码的函数调用开销**：记录表 3000+ 次 + 单元格 1800+ 次 `_u16/_u32`。
把它们在热点循环里**手工内联**（`wb[o] | (wb[o+1] << 8)`，并加 `rl >= N` 判据替代逐字段
越界检查）后：BIFF8 段 3500 → **2850 µs**，整体 3880 → **3200 µs**，而 `_u16/_u32` 仍保留给
非热点路径（BOUNDSHEET、FAT、SST 头）。

**顺带改了一条编译器侧**（`src/object/object_string.c` 的 `str_new`）：
长串（> `INTERN_MAX_SHORT_LEN` = 40）进不了内化表，原来却仍调 `intern_find` 白算一遍
整串 hash 才能确定"查不到"。改成先按长度判据过滤再查表（与 `str_concat` 的既有写法一致，
语义完全等价）。实测收益：

| 指标 | 改前 | 改后 |
|---|---|---|
| `files.read` 44 KB | 351 µs | **235 µs** |
| OLE2 取 Workbook 流 | 584 µs | **378 µs** |
| 完整读取 | 4185 µs | **3880 µs** |

（`assert\run_tests.leno` 670 条全过。）这同时抬高了所有二进制读盘路径
（zip / 打包 exe / 图片）。

BIFF8 解析段 2.85 ms vs Python 2.95 ms —— **已经持平甚至反超**。也就是说"逐记录 + 逐字符"
这套控制流在 Leno 的字节码解释下和 CPython 同量级，两边真正的差别只在原语粒度上，
而这一层已经被上面的手工内联补回来了。整体剩下的 4% 差距是 I/O（`files.read` 240 vs 95 µs）
与 `to_bytes`（190 µs，Python 不需要这一步）。

> **还能往哪优化**（2026-11-26 评估，未做）：
> `str_new` 对二进制数据仍要做 `utf8_char_len` + `hash_string` 两遍全扫。惰性化可再省
> ~100 µs（本文件约 3%），但要改 60+ 处 `->char_len` / `->hash` 读取点，漏一处就是
> 静默错值，**不划算**。更值得的是加"批量解包"原语（一次 C 调用解一组整数），
> 那能让 OLE2 + BIFF 的字段解码整体提速，且是**新增** API、不动现有语义。

```bat
:: 自己的基准（同时给 xlsx 对照）
build\leno.exe build\leno_module\LenoXLSX\test\dev_xls_bench.leno
:: Python 参考实现（同算法、只用标准库）
python build\leno_module\LenoXLSX\test\dev_xls_bench.py "D:\CLeno\1月份.xls" 100
```

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
