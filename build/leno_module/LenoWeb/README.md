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
| `web_gzip.leno` | **纯 Leno 的 gzip / DEFLATE 解压**（零依赖，`.gz` sitemap 用） |
| `web_ws.leno` | **纯 Leno 的 WebSocket 客户端**（CDP 的通道） |
| `web_cdp.leno` | **无头浏览器 / 动态渲染**（CDP 驱动 Chrome/Edge + 进程管理） |
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
- `.gz` **能解**：响应的 `Content-Encoding: gzip` 由 libcurl 自动解；服务端把 `.gz` 当静态文件
  返回（不带 `Content-Encoding`）时，由模块自己的 `web_gzip` 解（纯 Leno，不需要 zlib）。
  真解不开（截断/损坏）才计入 `gzip_skipped` 并给出原因——**不会**把压缩字节当 XML 解析出乱码"URL"
- `web.robotsUrl(base)` 单独给出（原先是从 `sitemapUrl().replace(...)` 拼的，前辍一变就失效）

### gzip / DEFLATE 解压（`web_gzip`，2026-10-06 新增）

```leno
import "Web" as web

var r = web.gunzip(rawBytes)          // 解 gzip（含头/trailer）
print(r.ok, r.outBytes, r.error)      // 失败时 error 是可读原因
if web.isGzip(body) { ... }           // 只判魔数 1f 8b（不解压）
var d = web.inflateRaw(deflateOnly)   // 裸 DEFLATE（无 gzip 头/trailer）
```

**为什么自己写**：系统里没有独立的 zlib（PATH / System32 / Git 都查过），`libcurl` 也不导出
`gzopen` / `inflate*`；随模块分发 DLL 又要维护 Windows/POSIX 两套二进制。这块逻辑不长，
所以直接用 Leno 实现——零依赖、跨平台、**可重入**（无模块级状态，多线程可同时调用）。

覆盖与取舍：

| 项 | 说明 |
| --- | --- |
| 块类型 | stored(00) / fixed(01) / dynamic(10) 全支持，多块流 |
| gzip 头 | FEXTRA / FNAME / FCOMMENT / FHCRC 可选字段都能跳过 |
| 校验 | 魔数、保留位、stored 的 LEN/NLEN、Huffman 码长合法性、回拷距离越界、trailer 的 `ISIZE` |
| 不解 | 多成员"连接式" gzip（只解第一个成员）；不做 CRC32（见「已知限制」） |
| 性能 | 2.39MB 明文 / 57KB 压包 → **约 96ms**（≈25MB/s；高度重复的 sitemap 数据，普通数据会慢些） |

长度/距离表是用公式算的，测试里对着 RFC1951 的原始数值逐项核过（`rfcTableCheck()`）；
测试向量的字节来自 .NET `GZipStream`，三种块类型各一份。

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

## 并发批抓（`curl_multi`，新增）

爬虫吞吐的大头在这：N 个请求在**同一个线程**上同时飞。

为什么是 `curl_multi` 而不是线程池：Leno 的子线程要求"入口必须是**当前文件**的函数"
（见 [`docs/threads使用指南.md`](../../../docs/threads使用指南.md)）⇒ 库函数自己起不了线程；
而多路复用是 libcurl 的原生能力，没有锁、也不跨线程（所有回调都在调用线程上执行）。

```leno
Array[string] urls = [...]                     // 几百个详情页
Array[BatchResult] rs = web.fetchAll(urls, 8)  // 并发 8
for rs.len() to i {
    if rs[i].resp.ok() { save(rs[i].resp.body) }
    else { log(rs[i].url + " 失败: " + rs[i].resp.error) }
}
```

- **结果按输入顺序返回**（不是完成顺序）⇒ 可直接与 `urls` 对齐；每条带 `index` / `url` / `resp`
- **单条失败不拖累其它**：`resp.error` 非空即该条失败，其余照常完成（`resp.curlCode` 给错误码）
- `concurrency` 建议 4~16：再高对单站点只是压力，而且更容易触发风控
- 可选参数 `userAgent` / `proxy`（应用到本批每个请求）、`timeoutMs`（默认 30s）
- 与字符集层打通：GBK 页在结果里同样是**转好码**的 `resp.body`（`resp.charset` 给出探测结果）

> 想要"每条请求换一个代理"：按代理数把 `urls` 分块，每块调一次 `fetchAll(urls_i, n, ua, proxy_i)`
> 即可（顺序与并发都不受影响）。批内共用一个出口是刻意的 —— 一次批抓 = 一个 multi 句柄 + 一个出口，
> 语义更好推理；需要复杂轮换时用 Session 的代理池（见下）。

**并发是量出来的，不是"设了参数就算"**：`examples/tests/test_fetch_all.leno` 起 5 个独立延迟
服务端（各延迟 250ms），并发 5 实测 **253ms**、串行实测 **1357ms** —— 同一批 URL 的两端对比。

## 反爬基础（新增）

只解决"看起来不像脚本"这一层：**浏览器式请求头（含顺序）**、**代理池**、**TLS 套件选项**。

```leno
// ① 浏览器式请求头：数组顺序就是发出去的顺序（libcurl 按 slist 顺序发）
var c = web.createClient()
c.setBrowserHeaders()                        // ua 省略 ⇒ web.chromeUA()
var r = web.getWithBrowserHeaders(url)       // 一次性：新建客户端 + 浏览器头

// ② 代理池：轮询 + 失败冷却
var pool = web.createProxyPool(["http://u:p@host:8080", "socks5h://host:1080"])
var s = web.createSession()
web.sessionSetProxyPool(s, pool)             // 之后每个 sessionXxx 请求自动换代理
var resp = web.sessionGet(s, url)
web.sessionReportResult(s, resp.ok())        // 回报成败 ⇒ 坏代理自动冷却
web.sessionCoolDownProxy(s, 60000)           // 服务端明确 403/429 时立刻冷却（比等连续失败快）

// 爬虫侧：挂上池子后 fetchItem() 自动"取代理 → 抓 → 回报"
web.setProxyPool(crawler, pool)
var page = crawler.fetchItem(item)

// ③ TLS 套件/曲线：**如实返回是否被支持**（不支持就 false，不静默）
c.setCipherList("HIGH:!aNULL")

// ④ 代理列表也可以从文件读（一行一个；`#` 注释、空行、行尾空白都容忍）
var pool2 = web.proxyPoolFromFile("proxies.txt")   // 文件不存在 ⇒ 空池，不抛错
```

两个设计点值得说明：

- **失败要冷却，不只是轮询**：坏代理留在轮换里，每次轮到它都要等一个超时 ——
  一个坏 IP 就能把整体吞吐拖死。连续失败达 `fail_threshold`（默认 3）⇒ 冷却
  `cool_ms`（默认 60s），到点**自动放回**（代理也可能只是临时抽风）；
  `pool.remove(p)` 才是永久剔除（被封）。
- **池子耗尽不偷偷直连**：全在冷却时 `next()` 返回 `""`，Session 保持现状并置
  `s.pool_exhausted = true` —— 直连会暴露真实 IP，比"失败"更糟，所以退避还是收工交给调用方。

`Accept-Encoding` **有意不写进模板**：建客户端时设的是 `ACCEPT_ENCODING = ""`，由 libcurl
按**自身实际支持**的编码声明并自动解压；硬写 `gzip, deflate, br` 会在后端没有 brotli 时
收到解不开的正文（静默乱码）。

### TLS 指纹（JA3/JA4）：只改套件不够

`setCipherList` / `setTls13Ciphers` / `setEcCurves` 能调套件与曲线顺序，但 JA3/JA4 还包含
**扩展顺序、ALPN、GREASE**，libcurl 原生没有这些开关。真要对齐 Chrome 指纹，需要把
`lib/libcurl-x64.dll` 换成 [curl-impersonate](https://github.com/lwthiker/curl-impersonate)
的 Windows 构建（导出符号与 libcurl 兼容，本模块**无需改代码**即可加载）。

本包**不预置**该 DLL（体积、来源可信度、许可证都要单独交代）。替换后这样验证：

```leno
print(web.version())          // 应显示替换后的版本串
```

本仓库自带 DLL 的实测结论（`examples/tests/test_antibot.leno` 第 ⑧ 段会打印）：
`setCipherList = true`、`setTls13Ciphers = true`、**`setEcCurves = false`**（后端不接受该选项）。

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

## 动态渲染：无头浏览器（2026-10-07 新增）

SPA / JS 注入内容抓不到是静态 HTTP 爬虫的硬缺陷。本模块用 **CDP（Chrome DevTools Protocol）
驱动本机真实浏览器**，把**渲染后**的 DOM 拿回来，再交给 `web.parse` / `web.select`：

```leno
import "Web" as web

// 一次性：起浏览器 → 渲染 → 关掉
var r = web.renderPage("https://example.com/spa", 20000)
if r.ok {
    var node = web.parse(r.html)                       // 渲染后的 HTML
    print(web.allText(web.select(node, "#app")[0]))
}

// 批量：复用同一个浏览器（启动一次 ~2.5s，之后每页 ~0.5s）
var b = web.launchBrowser(web.BrowserConfig(headless=true, startUrl="about:blank"))
var r1 = web.renderWith(b, url1, 8000, 500)
var r2 = web.renderWith(b, url2, 8000, 500)
web.closeBrowser(b)                                    // 走协议 Browser.close
```

**为什么是这条路（而不是 WebView2）**：

| | 覆盖面 / 理由 |
| --- | --- |
| 覆盖面 | Chromium 家族占桌面 **≈84%**（StatCounter 2026-08：Chrome 73.28% + Edge 10.46%），且 **Edge 在 Win10/11 系统自带**；WebView2 只在 Windows |
| 生态 | Playwright / Puppeteer / Selenium 走的都是"外部浏览器 + CDP"这条同路 |
| 依赖 | 语言**自带 libcurl 已导出 `curl_ws_*`**，`sockets` 也够用 ⇒ WebSocket 用纯 Leno 写（`web_ws`），零第三方二进制 |
| 隔离 | 浏览器是独立进程：它崩了/被反爬干掉不会带走爬虫（WebView2 是同进程内嵌） |

实现要点与踩过的坑：

- **Chrome/Edge 136+**：`--remote-debugging-port` 对**默认用户目录不再生效**，必须配
  `--user-data-dir=<独立目录>`。本模块自动建临时目录（不碰用户的浏览器），并把它当作
  "这是我们拉起的浏览器"的标记，收尾时据此兜底清理（**绝不会** `taskkill /IM msedge.exe`）。
- 端口自选（连接探测找空闲端口），启动后轮询 `/json/version` 等就绪；`SystemInfo.getProcessInfo`
  取进程号；关闭优先走 `Browser.close`，失败才按 profile 目录兜底杀。
- 渲染等待：`Page.loadEventFired` + `settleMs`（默认 800ms，给前端异步取数留时间）；
  取值用 `Runtime.evaluate`（`returnByValue` + `awaitPromise`）。
- 没装 Chrome/Edge 时 `findBrowser()` 返回 `""`，可用环境变量 **`LENO_BROWSER`** 指定路径。

## 已知限制

**POSIX 的 iconv 路径未经实测**（开发环境是 Windows）：`_iconvConvert` / `_iconvDoEx`
只做静态审查，请在 Linux/macOS 上跑一遍 `examples/tests/test_charset_unit.leno` 确认。

**gzip 解压不做 CRC32 校验**：`web_gzip` 校验了头、块结构、`ISIZE`（原文字节数）与各种越界，
但不逐字节算 CRC32（要按字节跑 8 次位运算，收益不抵成本）。截断与大多数损坏都能报出来，
但"内容被改过、长度没变"的极端情况不会被发现。

**动态渲染的几处边界**（`web_cdp`）：

- 需要本机有 Chrome/Edge（Windows 上 Edge 自带）；**POSIX 路径未实测**（开发环境是 Windows）
- `web_ws` 只实现 `ws://`，不实现 `wss://`（CDP 走本机明文，用不到）
- 等待策略是"`load` 事件 + 固定 `settleMs`"，没有真正的"网络空闲"判定 ⇒
  对慢接口的页面可能取早了（P2 计划：`Network.*` 事件或 `waitForSelector`）
- **没有做反检测**：`--headless=new` 仍会暴露若干特征（`navigator.webdriver`、Headless Chrome
  的 UA 片段等）。要过强反爬需另做 stealth（`Page.addScriptToEvaluateOnNewDocument` 抹特征、
  用真实 profile 等），属 P2/P3

**TLS 指纹仍是原生 libcurl**：代理轮换与浏览器式请求头已具备，但 JA3/JA4 需要替换 DLL
（`setEcCurves` 在本包自带 DLL 上就返回 `false`）。见「反爬基础 → TLS 指纹」。

## 示例与测试

见 [`examples/README.md`](examples/README.md)。一键跑测试：

```bash
build\leno.exe build\leno_module\LenoWeb\examples\tests\run_tests.leno
```
