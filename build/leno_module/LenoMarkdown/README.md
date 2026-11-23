# LenoMarkdown

**Markdown → HTML**（纯 Leno，无原生依赖）。为「在 Leno 里渲染 Markdown」提供基础能力 ——
文档站、README 预览、日志/报告渲染都不必再外挂 Python 的 `markdown` 包。

```leno
import "Markdown" as md

main() {
    print(md.to_html("## 标题\n\n正文 **加粗** 与 `代码`。\n"))
    // <h2 id="标题">标题</h2>
    // <p>正文 <strong>加粗</strong> 与 <code>代码</code>。</p>

    for md.headings("# 一\n## 二") to h {
        print(_str(h.level) + " " + h.text + " → #" + h.id + "\n")
    }
}
```

## API

| 函数 | 说明 |
|---|---|
| `to_html(text)` | Markdown → HTML 片段（块级 + 行内） |
| `headings(text)` | 标题列表 `Array[Heading{level,text,id}]`，供目录/锚点用（代码块里的 `#` 不算） |
| `slug(text)` | 锚点名：小写；字母/数字/汉字/`_`/`-` 保留，其余压成 `-`；保留中文 |
| `escape(text)` | HTML 转义（`& < > "`） |
| `scan_unsupported(text)` | 扫描**本库没实现**的写法（`~~~` 围栏、引用式链接定义、下划线式标题…），返回人类可读的行号+说明。构建脚本用它把缺口打出来，避免"看着像正常文本"的静默错误 |

## 支持范围

块级：ATX 标题（`#`..`######`，带 id）/ 段落（软换行保留、行尾两空格 → `<br>`）/
围栏代码（```` ```lang ````，语言进 `class="language-xx"`，**只认裸 ```` ``` ```` 闭合**）/
**缩进代码块**（4 空格或 tab）/ 表格（GFM，含 `:` 对齐、单元格内 `\|` 转义）/
无序与有序列表（含嵌套；**紧凑**时 `<li>文字</li>`、**松散**（项间有空行）时 `<li><p>文字</p></li>`；
遵守 `sane_lists`：列表前要有空行）/ 引用（含嵌套、**懒续行**（不写 `>` 的续行仍属引用）、引用里也能放代码块）/
分隔线 / 原始 HTML 块透传。

行内：`` `code` ``（**开闭反引号等长**）/ `**粗**` / `*斜*` / `~~删除线~~` /
`[链接](url)` / `![图](src)` / **行内 HTML 透传**（`<br>` `<kbd>` `<sub>`…）/ `\` 转义。

## 刻意不支持

- 下划线斜体 `_x_`：Leno 代码里 `snake_case` 满地都是，误判代价远大于收益；
- 引用式链接 `[a][ref]`、脚注、定义列表、任务列表；
- `~~~` 围栏、下划线式（setext）标题 —— 用 `scan_unsupported()` 会明确报出来。

## 与 python-markdown 的关系

输出形状（`<th>` 一行一个、`<hr>` 不闭合、代码块结尾换行、行内 `<code>` 不转义引号…）
是**照它生成的 HTML 抄的**，可以直接逐字节对拍。已知差异：

| 场景 | python-markdown | 本库 |
|---|---|---|
| 引用里的围栏代码 `> ```leno` | 不认识，退化成段落 + 行内 `<code>`（文档站为此写了 `lift_blockquote_fences` 补丁） | 正常渲染成 `<pre><code>` |
| `~~删除线~~` | **不支持**，原样输出波浪线 | `<del>…</del>`（GFM 写法） |
| 围栏闭合 | 宽松：任意 ```` ``` ```` 都算闭合 ⇒ 漏写一个闭合就奇偶颠倒、整篇被吞 | 严格：只认**裸** ```` ``` ````（CommonMark）⇒ 出错只影响那一段 |

## 自测

```bat
build\leno.exe build\leno_module\LenoMarkdown\test\test_markdown.leno
```

或随模块套件一起跑：`build\leno.exe build\leno_module\run_all_tests.leno build\leno.exe --only LenoMarkdown`
