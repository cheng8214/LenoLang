# LenoWeb

Leno 的 Web 模块：**HTTP 客户端（libcurl）+ HTML 解析 + 爬虫框架**。
对外只需 `import "Web"` 一个入口（也可按需 import 子模块 `web_net` / `web_html` / `web_crawler` / `web_charset`）。

```leno
import "Web" as web

main() {
    var r = web.get("https://example.com")
    print(r.statusCode, r.charset, r.body.len())
}
```

## 模块组成（`lib/`）

| 文件 | 职责 |
| --- | --- |
| `Web.leno` | 统一入口（转发 + 重新导出类型） |
| `web_curl_core.leno` | libcurl 的 FFI 绑定（常量 / Easy / slist / MIME） |
| `web_net.leno` | `HttpClient` / `Response` / `Session` / 表单 / multipart |
| `web_charset.leno` | **字符集探测与转码**（GBK/BIG5/… ↔ UTF-8） |
| `web_html.leno` | HTML 解析 + CSS 选择器 + 文本/表格/表单提取 |
| `web_crawler.leno` | `Crawler` 调度 + URL 工具 + robots/sitemap + CSV |
| `web_generic.leno` | `Result[T]` 等泛型小工具 |

原生依赖：`lib/libcurl-x64.dll`（Windows）/ `lib/libcurl.so`（Linux），已在 `leno.toml` 的 `[native-libs]` 声明。

## 字符集：响应体自动转 UTF-8（2026-10-06 新增）

libcurl 只搬字节、不认识 `charset`，所以 GBK/GB2312 页面以前进 `body` 就是乱码。现在：

- **`Response.body` 始终是 UTF-8**；原始字节在 **`Response.rawBody()`**
- **`Response.charset`** = 探测到的字符集（小写；`""` = 未声明且无法推断）
- 探测顺序：`BOM` → `Content-Type` → `<meta charset>` → UTF-8 合法性 → **系统 ANSI 兜底**
  （中文 Windows 上 = `gbk`，所以"GBK 页面忘了声明编码"也能救回来）
- **只对文本响应动手**：`image/*` 等二进制原样保留（`body == rawBody()`）
- `Response.totalSize` 是**字节数**（不是字符数；字符数用 `body.len()`）
- **落盘请用 `rawBody()`**：`download()` 已改用它，否则会把 GBK/二进制写坏

需要自己处理原始字节时：

```leno
web.decodeCharset(raw, "gbk")        // 目标编码 → UTF-8
web.encodeCharset("中文", "gbk")     // UTF-8 → GBK（往 GBK 站点提交表单必须用）
web.detectCharset(raw, contentType)  // 只探测不转换
web.isValidUtf8(s)                   // UTF-8 合法性（Windows 走 C，快）
web.codepage("gb18030")              // → 54936
```

转码后端：Windows 用 `kernel32`（系统自带），POSIX 用 `iconv`；都拿不到时**原样返回**（不破坏内容）。

> ⚠ **光有转码还不够**：2026-10-06 同时修掉了 `web_html` / `web_crawler` 里一批"把**字符数**当
> **字节下标上界**"的写法（`int n = html.len()` 配 `html.byte(i)`）。纯 ASCII 时两者恰好相等，
> 所以一直没暴露；含中文的页面会被**截断/切碎**（`parse("<p>中文</p>")` 取文本只剩 2 字节坏字符，
> `allText` 得到 `"标标"`）。也就是说：转码把 GBK 页面正确变成 UTF-8 之后，**解析这一步又会把中文毁掉**。
> 现已全部改为 `byte_len()`，回归用例 `examples/tests/test_utf8_text.leno`。

## HTML 解析与选择器

解析器是手写的、对畸形 HTML 容错（自动闭合、忽略注释/CDATA/DOCTYPE），选择器语法如下：

| 语法 | 例子 |
| --- | --- |
| 标签 / 通配 | `div`、`*` |
| id / class | `#main`、`.item`、`p.a.b` |
| 属性 | `[href]`（存在性）、`[href='/x']`、`[href^='http']`、`[href$='.png']`、`[href*='x']`、`[class~='x']`（词）、`[lang\|='zh']`（前缀） |
| 组合器 | 后代（空格）、子元素 `>`、紧随兄弟 `+`、后续兄弟 `~` |
| 结构伪类 | `:first-child`、`:last-child`、`:only-child`、`:nth-child(2n+1)`、`:nth-child(odd)`、`:nth-child(-n+2)`、`:nth-last-child(n)` |
| 类型伪类 | `:first-of-type`、`:last-of-type`、`:only-of-type`、`:nth-of-type(2n)`、`:nth-last-of-type(n)` |
| 否定 / 关系 | `:not(.x)`（可嵌套 `:not(:nth-child(2))`）、`:has(> .price)`、`:has(+ span)`、`:has(~ li)` |
| 其他 | `:empty`、`:root`；伪类**可叠加**：`td:not(.hidden):nth-child(2)` |

```leno
var rows = web.select(web.parse(html), "table tr:nth-child(2n+1) td:not(.hidden)")
```

两处语义细节（与 CSS 一致）：

- 位置类伪类与 `nth-*` **只数元素**（`#text` 不计入编号）
- 返回结果**已去重**（`p ~ span` 不会因为前面有多个 `p` 而重复给出同一个 `span`）

`:has()` 支持前导组合器（`> S` 子、`+ S` 紧随兄弟、`~ S` 后续兄弟），不带前导组合器时按"有后代匹配"处理。

### 命名实体

`&nbsp;` → `U+00A0`（**不是**普通空格）、`&hearts;` → `♥`、`&euro;` → `€`、`&alpha;` → `α`
等 ~250 个常用命名实体已收录（ISO-8859-1 全集 + 排版标点 + 货币 + 希腊字母 + 数学/箭头 + 符号）。
未收录的（多码点实体、冷门符号）会**原样保留** `&name;`，不会吃掉内容；数字实体
`&#20013;` / `&#x4E2D;` 一直支持。

## 上传：multipart/form-data（新增）

```leno
var parts = [
    web.formField("user", "zhangsan"),
    web.formFile("avatar", "a.png", "a.png", "image/png")
]
web.postMultipart(url, parts)
// 也支持 client.postMultipart / client.putMultipart / web.sessionPostMultipart
```

## 爬虫

```leno
var c = web.createCrawler({
    start_urls: ["https://example.com"], allowed_domains: ["example.com"],
    max_pages: 100, max_depth: 3, delay_ms: 500,
    user_agent: "MyBot/1.0", respect_robots: true
})
c.setRobots(web.fetchRobots("https://example.com", c.user_agent))
c.start()
while not c.shouldStop() {
    var item = c.nextUrl()          // 已自动去重 + 限速（delay_ms 与 robots 的 crawl-delay 取大）
    if item == null { break }
    var page = web.fetchPage(item.url, c.user_agent)
    if page != null {
        for web.extractLinks(page) to link { c.enqueue(link, item.depth + 1) }
    }
    c.markVisited(item.url)
}
```

2026-10-06 的改进：

- **入队/判重前先 `normalizeUrl`**（`#fragment`、`/a/./b`、host 大小写不再算两页），并新增"队列内去重"
- **限速由框架生效**：`nextUrl()` 自动按 `delay_ms` 补睡，不必在循环里手写 `sleep`
- **解析 robots 的 `crawl-delay`**（`web.parseCrawlDelay`），并与 `delay_ms` 取较大者
- `max_pages = 0` 表示**不限**（原实现会立刻停）
- **优先队列**（见下）：`enqueue(url, depth, priority)`，入队/出队都是 O(log n)
- **CSV 落地**：`web.writeCsv(path, rows, header)` / `web.toCsv(rows)`（RFC 4180 转义 + UTF-8 BOM，Excel 直接认中文）
- **断点续爬**：进度落盘 / 恢复（见下）
- **sitemap**：索引递归 + 元数据 + 跨站防护（见下）

### 优先队列

`priority` 越大越先被 `nextUrl()` 取出；同优先级保持入队先后（FIFO 稳定）。

```leno
var item = c.nextUrl()
// 详情页优先（1），翻页往后放（-1）——典型的两级抓取策略
for page.detailLinks() to href { c.enqueue(href, item.depth + 1, 1) }
c.enqueue(page.nextPageUrl(), item.depth, -1)
```

实现是**二叉大顶堆**（`_push` / `_pop` 都是 O(log n)，排序键 = `priority` 降序 + `seq` 升序）。
为什么不是"排序数组"：数组插入要搬移 O(n)，出队也要搬移；队列上千时差别明显。
注意 **同一 URL 第二次入队会被丢弃**（优先级不会"升级"）——与判重口径一致。

### Sitemap

```leno
// 只要地址列表（自动下钻 sitemapindex，默认 2 层）
Array[string] urls = web.fetchSitemapUrlsDeep("https://example.com", "MyBot/1.0")

// 要诊断信息（为什么少了一些地址？）
var rep = web.fetchSitemapReport("https://example.com/sitemap_index.xml", "MyBot/1.0", 2, 50)
print(rep.sitemaps_fetched, rep.gzip_skipped, rep.errors)

// 单条记录的元数据
for web.parseSitemapEntries(xml) to e { print(e.url, e.lastmod, e.changefreq, e.priority) }
```

- `sitemapindex` **递归下钻**（去重 + 层数上限 `maxDepth` + 请求数上限 `maxSitemaps`）
- **同站点限制**：索引里指向别的站点的子 sitemap 一律忽略并记入 `errors`
  （否则一个第三方索引就能把爬虫引到任意站点）
- `<lastmod>` / `<changefreq>` / `<priority>` 可读（`SitemapEntry`）
- `.gz`：响应的 `Content-Encoding: gzip` 由 libcurl 自动解开；**仍是 gzip 魔数时只计入
  `gzip_skipped`**，不会把压缩字节当 XML 解析出一堆乱码"URL"（原因见「已知限制」）
- `web.robotsUrl(base)` 单独给出（原先是从 `sitemapUrl().replace(...)` 拼的，前辍一变就失效）

### 断点续爬

长跑任务中途挂掉不用从头再来：

```leno
var c = web.createCrawler({
    start_urls: ["https://example.com"],
    state_path: "crawl_state.json",   // 给了它就自动落盘
    save_every: 50                    // 每 50 页存一次（缺省 50）
})
if not c.resume() { c.start() }        // 有存档就续爬，没有就从头
while not c.shouldStop() {
    var item = c.nextUrl()
    if item == null { break }
    // ... 抓取 ...
    c.markVisited(item.url)            // 每 save_every 页自动存一次
}
c.saveState(c.statePath)              // 收尾再存一次
```

存档是普通 JSON（`visited` + 未出队的 `queue` + `page_count`），只记录了必要状态：

| 恢复的东西 | 含义 |
| --- | --- |
| `page_count` | 已抓页数（`max_pages` 判断不会因为重启而重置） |
| 已访问集合 | 已抓的 URL 不会被重复抓、也不会被重新入队 |
| 待抓队列 | 尚未出队的 URL 连同深度一起恢复（顺序保持） |

底层是 `saveState(path)` / `loadState(path)`（可用 `web.saveState/loadState` 直接调）；
坏存档 / 缺文件一律返回 `-1`，不抛异常。

### 断点续传下载

```leno
web.downloadResume(url, "big.zip")     // 本地已有前半段 ⇒ 只取剩余字节并追加
web.remoteSize(url)                    // HEAD 取 Content-Length；未知返回 -1
```

- 走 libcurl 的 `CURLOPT_RESUME_FROM_LARGE`：自动发 `Range: bytes=N-`
- **服务端不支持 Range 会自动回退整体重下**（不会把文件拼成"前半段 + 全量"）
- 本地文件比远端还大（上次下坏了）⇒ 从头重下
- 总量已知时会校验最终大小，对不上返回 `-1`
- 本地文件**已完整**时不再重复下载

## 多线程

**HTTP 请求可以从 `threads.start()` 的子线程发起**（2026-10-06 起）。

```leno
func worker(any idAny, Channel ch) {          // ⚠ 入口必须是**本文件**的函数
    var r = web.get("http://127.0.0.1:8080/echo/" + _str(idAny))
    ch.send(r.body)
}

main() {
    var ch = threads.channel(8)
    for 8 to i { threads.start(worker, i, ch) }
    for 8 to i { print(ch.receive()) }
}
```

约定（与 `requests.Session` 一致）：

- **每个线程各用各的 `HttpClient`**，或直接用模块级 `web.get/post/...`（内部每次新建）⇒ 安全 ✓
- **同一个 `HttpClient` 被多个线程同时用** ⇒ 不安全（和 `requests.Session` 一样，属用户责任）

> 这条曾经走不通：FFI 回调在子线程里"只能成功第一次"，并发时直接崩。根因是
> `src/vm/vm.c` 的 `vm_call_value` 把停止条件写到了**全局主 VM** 上（宏 `vm` 在该处已不在作用域），
> 另有"回调槽位分配竞态"与"每客户端调用 `curl_global_init`（非线程安全）"两处。
> 三处均已修；证据与验证见
> [`docs/多线程struct与模块全局变量问题记录.md`](../../../docs/多线程struct与模块全局变量问题记录.md) 第八节。
> 回归用例：`examples/tests/test_thread_http.leno`、`examples/tests/test_ffi_callback_thread.leno`。

## 已知限制

**POSIX 的 iconv 路径未经实测**（开发环境是 Windows）：`_iconvConvert` / `_iconvDoEx`
只做静态审查，请在 Linux/macOS 上跑一遍 `examples/tests/test_charset_unit.leno` 确认。

**`.gz` 的 sitemap 解压不了**：本模块只随包分发 `libcurl-x64.dll` / `libcurl.so`，
系统里既没有独立的 zlib，`libcurl` 也不导出 `gzopen` / `inflate*`
（2026-10-06 实测：`gzopen`/`inflateInit2`/`zlibVersion` 在 DLL 里都查不到）。
所以 `sitemap.xml.gz` 只有在服务端带 `Content-Encoding: gzip`（libcurl 会自动解）时才能用；
否则记入 `SitemapReport.gzip_skipped` 并给出原因——**不会**把压缩字节当 XML 解析出乱码。
要真正支持：随模块补一个 `zlib1.dll` / `libz.so.1`，再 FFI 绑 `gzopen`/`gzread`（改动很小）。

## 示例与测试

见 [`examples/README.md`](examples/README.md)。一键跑测试：

```bash
build\leno.exe build\leno_module\LenoWeb\examples\tests\run_tests.leno
```
