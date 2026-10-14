# module_web —— LenoWeb（HTTP / HTML / 字符集 / gzip / 爬虫 / 无头浏览器）

> **位置**：`build/leno_module/LenoWeb/lib/`；**入口**：`import "Web" as web`
> **本文补于 2026-10-14**：这是 LenoWeb 的**第一份参考文档**。此前它的能力只写在模块源码头注释与
> 几份开发记录里（`移植音乐下载器实录与待办.md` / `LenoHtml_开发问题记录.md` / `LenoNet开发问题记录.md`），
> 从 `docs/` 入口**根本发现不了** —— 实际后果之一是"Leno 有没有 gzip"被误判成"没有"
> （`web_gzip.leno` 635 行就在那儿，纯 Leno 实现，但文档层没有任何入口提及它）。

## 目录

- [一、入口与子模块](#一入口与子模块)
- [二、快速上手](#二快速上手)
- [三、API](#三api)
  - [web_net：HTTP 客户端 / 会话 / 代理池 / 批抓](#web_nethttp-客户端--会话--代理池--批抓)
  - [web_gzip：gzip / DEFLATE / zlib 解压（纯 Leno）](#web_gzipgzip--deflate--zlib-解压纯-leno)
  - [web_charset：字符集探测与转码](#web_charset字符集探测与转码)
  - [web_html：HTML 解析与 CSS 选择器](#web_htmlhtml-解析与-css-选择器)
  - [web_crawler：URL / robots / sitemap / 爬虫框架 / CSV](#web_crawlerurl--robots--sitemap--爬虫框架--csv)
  - [web_ws：WebSocket 客户端](#web_wswebsocket-客户端)
  - [web_cdp：无头浏览器（Chrome / Edge）](#web_cdp无头浏览器chrome--edge)
  - [web_generic：Result / Stack / LinkedList](#web_genericresult--stack--linkedlist)
- [四、注意事项：平台差异 · 外部依赖 · 坑](#四注意事项平台差异--外部依赖--坑)
- [五、已知缺口 / 待办](#五已知缺口--待办)

---

## 一、入口与子模块

`Web.leno`（266 行）本身就是一行"转发层"：`import` 子模块 → `use` 重新导出类型 → `export func` 转发函数。

| 文件 | 角色 | 主要导出 |
| --- | --- | --- |
| `Web.leno` | **统一入口**（用户面） | 上面那套 `get/post/...`、HTML、爬虫、gzip、渲染 |
| `web_curl_core.leno` | libcurl 低层绑定（FFI） | `Code` / `Opt` / `Info` / `Global` / `HttpVersion` / `SslVersion` 枚举 + `setopt_*` / `perform` / `multi*` / `mime*` |
| `web_net.leno` | HTTP 客户端与会话 | `HttpClient` / `Response` / `Session` / `ProxyPool` / `BatchResult` / `fetchAll` |
| `web_html.leno` | HTML 解析 + CSS 选择器（纯 Leno） | `HtmlNode` / `parse` / `select` / `extractTable` / `htmlToText` |
| `web_charset.leno` | 字符集探测与转码 | `detect` / `decode` / `encode` / `toUtf8` / `isValidUtf8` |
| `web_gzip.leno` | **gzip / DEFLATE / zlib 解压（纯 Leno）** | `GzipResult` / `gunzip` / `inflateRaw` / `inflateZlib` / `isGzip` / `rfcTableCheck` |
| `web_crawler.leno` | URL / robots / sitemap / 爬虫框架 / CSV | `Url` / `parseUrl` / `Crawler` / `createCrawler` / `writeCsv` |
| `web_ws.leno` | WebSocket 客户端（自写，供 CDP 用） | `WsClient` / `connect` / `sendText` / `recvMessage` |
| `web_cdp.leno` | 无头 Chrome/Edge 驱动（CDP） | `Browser` / `launch` / `render*` / `evaluate` / 截图 |
| `web_generic.leno` | 泛型小工具 | `Result[T]` / `Stack[T]` / `LinkedList[T]` |

### 入口**没有**转发的通道

`import "Web" as web` 拿不到的东西，要直接引子模块：

| 想要 | 写法 |
| --- | --- |
| gzip 自检 `rfcTableCheck()` | `import "web_gzip" as gz`（入口只转发了 `GzipResult` + 4 个解压函数）|
| **WebSocket 的任何函数** | `import "web_ws" as ws`（入口只转发了 3 个类型，函数一个都没转）|
| CDP 低层（`evaluate` / `clickReal` / `captureScreenshot` / `createTargets` …）| `import "web_cdp" as cdp` |
| HTML 的选择器部件 `SelectorPart` | `import "web_html" as htmls` |
| libcurl 低层绑定 | `import "web_curl_core" as core` |

---

## 二、快速上手

```leno
import "Web" as web

// ① 一次 GET（模块级便捷函数：每次新建/销毁 client，开销换线程安全）
var r = web.get("https://example.com")
if r.ok() {
    print(r.statusCode, r.body.len())
} else {
    print("失败:", r.error, "curlCode=", r.curlCode)
}

// ② 会话（仿 requests.Session）：一次设头，多次复用
var s = web.createSession()
web.sessionSetTimeout(s, 10000)
web.sessionSetHeader(s, "Authorization", "Bearer xxx")
var r1 = web.sessionGet(s, "https://example.com/a")
var r2 = web.sessionPostJson(s, "https://example.com/b", "{\"k\":1}")
web.sessionClose(s)

// ③ HTML 解析
var doc = web.parse(r.body)
print(web.getTitle(doc))
for web.select(doc, "div.item > a[href]") to a {
    print(web.attr(a, "href"), web.text(a))
}

// ④ 响应体是 gzip 字节（服务端没带 Content-Encoding，libcurl 不会自动解）
var gz = web.gunzip(web.get("https://example.com/sitemap.xml.gz").rawBody())
if gz.ok { print("解出 " + _str(gz.outBytes) + " 字节") }
```

---

## 三、API

### web_net：HTTP 客户端 / 会话 / 代理池 / 批抓

**`Response`**（`web_net.leno`；字段 + 方法）

| 字段 | 含义 |
| --- | --- |
| `statusCode` / `body` / `headers` / `contentType` | 状态码 / 正文（可能已按字符集转码）/ 原始响应头 / Content-Type |
| `_raw` / `charset` / `effectiveUrl` | 原始字节 / 探测到的字符集 / 最终 URL（跟随重定向后）|
| `totalSize` / `totalTime` / `connectTime` / `downloadSpeed` | 字节数 / 总耗时 / 连接耗时 / 速度 |
| `error` / `curlCode` | 失败原因 / libcurl 错误码 |

方法：`rawBody()` / `bytes()`（**原始字节，下载/落盘必须用它**）、`wasDecoded()`、`ok()`、`isRedirect()`、
`isClientError()`、`isServerError()`、`isError()`、`json()`、`getHeader(name)`、`str()`、`reset()`。

**`HttpClient`**：`init()` / `close()` / `reset()`、`setTimeout(ms)`、`setConnectTimeout(ms)`、`setUserAgent(ua)`、
`setFollowRedirect(follow, max)`、`setSSLVerify(peer, host)`、`setHttpVersion(v)`、`setProxy(p)`、
`setCredentials(user, pass)`、`replaceHeaders(arr)`（**整表替换**）/ `addHeaders(arr)`（追加）、`headers()`、
`setVerbose(b)`、`setCustomRequest(m)`、`setCAInfo(path)`、`setCipherList(l)`、`setTls13Ciphers(l)`、
`setEcCurves(l)`、`setBrowserHeaders(ua, lang)`、`setResumeFrom(offset)`、
`get/post/postJson/put/patch/del/head(url)`、`setCookie` / `setCookieFile` / `setCookieJar` / `setBearerToken`、
`getHandle()`、`isOk()`、`lastError()`、`downloadResume(url, savePath)`。

**模块级函数**

| 分类 | 函数 |
| --- | --- |
| 便捷动词 | `get` / `post` / `postJson` / `postJSON` / `put` / `patch` / `del` / `getWithParams` / `postForm` |
| 重试与下载 | `getWithRetry(url, retries, delayMs)` / `postWithRetry` / `download(url, path)` / `remoteSize(url)` / `downloadResume` |
| 多部分表单 | `formField(name, value)` / `formFile(name, path, fileName, mime)` / `postMultipart` / `putMultipart` |
| 会话 | `createSession` / `sessionGet|Post|PostJson|PostJSON|Put|Del|Patch|Head` / `sessionSetHeader|Cookie|Credentials|Timeout|Proxy` / `sessionLoadCookies` / `sessionSaveCookies` / `sessionClose` |
| 并发批抓 | `fetchAll(urls, concurrency = 8, userAgent = "", proxy = "", timeoutMs = 30000): Array[BatchResult]` |
| 反爬基础 | `chromeUA()` / `browserHeaders(ua, lang)` / `getWithBrowserHeaders(url, ua)` / `getVia(url, proxy)` / `createProxyPool` / `proxyPoolFromFile` / `sessionSetProxyPool` / `sessionReportResult` / `sessionCoolDownProxy` |
| URL / 全局 | `urlEncode` / `urlDecode` / `formEncode(dict)` / `buildQuery(dict)` / `version()` / `globalInit(flags)` / `globalCleanup()` |

> `ProxyPool`：`next()`（全在冷却 ⇒ `""`）、`reportOk` / `reportFail`、`coolDown(proxy, ms)`、`add` / `remove`、
> `available()`（当前可用数）、`stats()`。有池的场景**别硬打**：池全冷却时 `fetchAll` / 爬虫当轮直接跳过。

### web_gzip：gzip / DEFLATE / zlib 解压（纯 Leno）

```leno
export struct GzipResult {
    bool ok; string data;          // ok=true 时 data 是解压后的**字节**（当字符串用）
    string error;                  // ok=false 时的可读原因
    int inBytes; int outBytes;     // 输入 / 输出字节数
}
```

| 函数 | 认什么 |
| --- | --- |
| `gunzip(raw)` | gzip（RFC1952）：10 字节头（含可选字段）+ DEFLATE + 8 字节 trailer |
| `inflateRaw(raw)` | **裸 DEFLATE**（没有头/trailer）|
| `inflateZlib(raw)` | zlib（RFC1950）：2 字节头 + DEFLATE + 4 字节 adler32 |
| `isGzip(raw)` | 只看魔数 `1f 8b` ⇒ **只作答"像不像"，不做完整性判断** |
| `rfcTableCheck()` | 自检：算出的 Huffman 表逐项对照 RFC1951 数值（`""` = 全对）|

**解码范围与取舍**（改它之前先读这段）

- ✅ 支持：`stored(00)` / `fixed(01)` / `dynamic(10)` 三种块、**多块流**、
  gzip 头的可选字段 `FEXTRA` / `FNAME` / `FCOMMENT` / `FHCRC` 跳过。
- ✅ 校验：头部魔数与保留位、stored 块的 `LEN/NLEN`、Huffman 码长合法性、回拷距离越界、
  gzip trailer 的 `ISIZE` ⇒ 截断与大部分损坏**都能报出来**（报错可读，不静默返回空）。
- ❌ **不做 CRC32 校验**（ISIZE 已能挡住截断与大部分损坏）。
- ❌ **不支持多成员的"连接式" gzip**：只解第一个成员，尾部多余字节不参与。
- ❌ `inflateZlib` **不校验 adler32**，也**不查** RFC1950 的 FCHECK（`(CMF*256+FLG)%31`）。
- ❌ **没有压缩产出侧**（只解不压）。

**为什么是纯 Leno 而不是 zlib**：系统里没有独立 zlib（PATH / System32 / Git usr/bin 都查过）；
libcurl 也不导出 `gzopen` / `inflate*`（它自己的 gzip 解码是内部静态链接的）；随模块分发 zlib 要维护
两套二进制 ⇒ 自写反而**零依赖、跨平台、可重入**（状态全挂 struct，无模块级变量 ⇒ 多线程可同时解）。

**性能**：Huffman 解码走 9 位快查表 + 慢路径兜底；解 1 MB 左右 XML 约几百毫秒。

**典型用法：`base64(gzip(json))` 这种本地文件**

```leno
import strings
import files
import "web_gzip" as gz

var b64 = strings.trim(files.read("some.info"))
var r = gz.gunzip(b64.from_base64())
if r.ok { print(r.data) } else { print("解压失败: " + r.error) }
```

> ⚠ **二进制串上 `len()` 是字符数、不是字节数**：错的字节序列经 `len()` 会得到一个偏小的数字
> （实测：146 字节的 gzip 数据 `len()` 报 127，而 `byte_len()` 是 146、与 `gunzip(...).inBytes`
> 对得上）⇒ 处理这些解出来的字节时，**一律用 `byte_len()` / `byte()` / `byte_slice()`**。
> 这与 `web_charset.leno` 文件头第 13-18 行记的是同一条口径（那边同样全程走 `byte_*`）。

### web_charset：字符集探测与转码

API：`backend()`（`"windows"` / `"iconv"` / `"none"`）、`normalize` / `codepage` / `needsDecode`、
`charsetFromBom` / `charsetFromContentType` / `charsetFromMeta`、`isValidUtf8`、`isTextual`、
`defaultCharset`、`detect(raw, contentType)`、`decode(raw, charset)`、`toUtf8(raw, contentType)`、
`encode(text, charset)`、`canEncode(charset)`。

探测顺序：**BOM → Content-Type → `<meta charset>` → UTF-8 校验 → 系统 ANSI 兜底**（与 requests 对齐）。

- `charsetFromMeta` **只在 `<meta>` 标签内部**认 charset：GBK 双字节的尾字节可能落在 `0x40~0x7E`，
  全文裸搜必然误判。
- 后端：Windows = `kernel32`（自带、零依赖）；POSIX = libc `iconv`（macOS/musl 回退 `libiconv.so`）；
  **两者都没有 ⇒ 原样返回**（宁可不转，也不搞坏内容）。
- ⚠ `isValidUtf8` **不能用 Leno 逐字节循环**（1 MB ≈ 38 ms）；无后端时退回"只扫前 8 KB"。

### web_html：HTML 解析与 CSS 选择器

`HtmlNode`：`tag`（普通元素小写、文本节点 `"#text"`、根 `"#document"`）、`attrs`、`children`、`text`、`_parent`；
方法 `attr` / `hasAttr` / `text()` / `allText()` / `isTextNode()` / `isElement()` / `tagName()` / `classes()` /
`hasClass` / `childCount()` / `select` / `selectOne` / `remove(selector)` / `parent` / `closest` /
`siblings` / `nextSibling` / `prevSibling`。

模块级：`parse(html)`、`tag` / `text` / `allText` / `textContent` / `cleanText` / `htmlToText` / `stripTags`、
`extractTable(root)`（th 优先，与 pandas `read_html` 一致）、`extractForm(root)`（input/select/textarea）、
`getTitle` / `getMeta` / `getMetas`、`getLinks(html)`、`getElementById` / `getElementsByTag` /
`getElementsByClass` / `findAll`、`select` / `selectOne` / `removeElement`。

- 纯 Leno 实现；支持实体解码（XML 基本 5 个 + ISO-8859-1 全集，含 `nbsp` = U+00A0）、DOCTYPE、CDATA、容错解析。
- 选择器组合器只支持 `" "`（后代）/ `">"`（子）/ `"+"`（紧随兄弟）/ `"~"`（后续兄弟）；
  `[ ]` 与 `( )` 内部的空格不算分隔符；伪类可叠加、位置类伪类只数**元素**子节点；
  **未知伪类不拦**（写错的伪类不会静默返回空）。
- ⚠ `select` 结果去重是 O(m²)（调用点已按"是否可能重复"过滤）。
- ⚠ 解析状态用**模块级全局变量**管理 ⇒ 多线程同时解析同一份 HTML 需谨慎（见 `多线程struct与模块全局变量问题记录.md`）。

### web_crawler：URL / robots / sitemap / 爬虫框架 / CSV

`Url`：`scheme` / `host` / `port` / `path` / `query` / `fragment` / `isAbsolute` + `str()` / `authority()` /
`effectivePort()` / `normalizedPath()` / `withoutQuery()` / `queryParam(k)` / `resolve(ref)` / `normalize()`。

`Crawler`（`createCrawler(config)`）：字段含 `start_urls` / `allowed_domains` / `max_pages` / `max_depth` /
`delay_ms` / `user_agent` / `respect_robots` 以及一组渲染回退配置（`render` / `render_mode`（`auto|always|never`）/
`render_selector` / `render_stealth` / `render_timeout_ms` / `render_proxy` / `render_user_data_dir`）。
方法：`start()` / `nextUrl()` / `markVisited(url)` / `enqueue(url, depth, priority)` / `shouldStop()` / `stop()` /
`throttle()` / `effectiveDelay()`（`crawl-delay` 与 `delay_ms` 取较大者）/ `setRobots(text)` / `saveState(path)` /
`loadState(path)` / `resume()` / `fetchItem(item)` / `closeBrowser()` / `setProxyPool(pool)`。

URL / robots / sitemap：`parseUrl`（=`urlParse`）/ `buildUrl` / `buildUrlDict` / `domain` / `baseUrl` /
`resolveUrl` / `normalizeUrl` / `getQueryParam` / `stripQuery`；`parseRobots` / `parseCrawlDelay` /
`isRobotsAllowed`；`parseSitemapEntries` / `parseSitemap` / `parseSitemapIndex` / `isSitemapIndex` /
`sitemapUrl` / `robotsUrl` / `fetchRobots` / `fetchSitemapUrls` / `fetchSitemapUrlsDeep` / `fetchSitemapReport`。

CSV：`csvField(v)` / `toCsv(rows)`（CRLF，Excel 友好）/ `writeCsv(path, rows, header, withBom = true)` /
`writeCsvRows(path, rows, withBom = true)`。

- 依赖链：`web_crawler` → `web_net` + `web_html` + `web_gzip`（`.gz` sitemap）+ `web_cdp`（SPA 回退渲染）。
- `SitemapReport` 刻意**不用模块级状态** ⇒ 多线程安全；单个 sitemap 抓失败不中断整体（错误进 `errors`）。
- ⚠ **爬完记得 `closeBrowser()`**（否则留下浏览器进程与临时 profile 目录）。

### web_ws：WebSocket 客户端

`connect(url, timeoutMs): WsClient`、`sendText(c, text)`、`recvMessage(c): WsMessage`（自动拼延续帧、
自动回 pong、忽略 pong）、`recvText(c)`、`setTimeout(c, ms)`、`close(c)`、`parseWsUrl(url)`、
`lastError()`，以及自检用 `sha1Hex` / `base64` / `base64Decode`。

- 支持：客户端握手（校验 `Sec-WebSocket-Accept`）、文本/二进制/延续帧、分片重组、ping 自动回 pong、
  close 帧、出站帧加掩码、读超时。**不做**扩展协商（permessage-deflate 等）。
- 自写理由：语言没有 websockets 模块，而 libcurl 的 `curl_ws_*` 要配 multi 驱动 + `CURLE_AGAIN` 轮询
  ⇒ 与 `web_gzip` 同一取舍（零依赖、可离线测试，底层用 `sockets`）。
- ⚠ `WsUrl` / `WsMessage` 用 struct 而不是数组（数组元素类型不齐会被判成 `Array[any]`，与声明冲突）。

### web_cdp：无头浏览器（Chrome / Edge）

`findBrowser(): string`（按 `LENO_BROWSER` 环境变量 → Edge → Chrome → 常见路径）、
`launch(BrowserConfig)`、`render(url, timeoutMs)`（一次性：起浏览器→渲染→关掉）、
`renderWith(b, url, waitLoadMs, settleMs)`、`renderAdvanced(b, url, RenderOptions)`、
`renderOn(b, targetId, url, opt)`、`renderMany(b, urls, opt, tabs)`；客户端交互：`evaluate` / `waitForSelector` /
`clickSelector` / `clickReal` / `typeText` / `scrollToBottom` / `captureScreenshot` / `createTarget` /
`closeTarget` / `waitNetworkIdle`；收尾 `close(b)`。

- ⚠ **需要系统装 Chrome/Edge**；`Chrome/Edge 136+` 起 `--remote-debugging-port` 对**默认用户目录不再生效**，
  必须配 `--user-data-dir=<独立目录>`（本模块本就建临时目录）。
- ⚠ **绝不能 `taskkill /IM msedge.exe`**（会连用户自己的浏览器一起杀）——模块按 `--user-data-dir`
  核对"这是不是自己拉起的那个"。
- 进程管理走 `_spawn(exe, args)` + `_kill(handle)`（Job Object）⇒ 只杀自己 spawn 的；
  Leno 进程退出（含崩溃/强杀）时内核连它一起收。
- `BrowserConfig.minFreeCommitMb`（默认 1024 MB）是**可用提交内存闸门**：低于闸门**拒绝启动**
  （2026-10-07 两次 `0xEF CRITICAL_PROCESS_DIED` 都发生在批量拉起浏览器那一下）。
- `renderMany` 结果**按输入顺序**返回，但它是**顺序**执行多 tab 轮转；真并发要自己起线程。

### web_generic：Result / Stack / LinkedList

`Result[T]`（`isOk()` / `isError()` / `unwrapOr(v)` / `getError()` / `getData()`）+ `Ok(v)` / `Err(msg)`、
`map` / `filter` / `reduce` / `forEach`、`Stack[T]`（`push` / `pop` / `peek` / `isEmpty`）、
`LinkedList[T]`（`append` / `prepend` / `shift` / `first` / `contains` / `get` / `toArray`）。

⚠ 空栈 / 越界的 `pop` / `peek` / `shift` / `first` / `get` **返回 `null`**（不是抛错）；
`LinkedList.contains` / `get` 是 O(n)。`Web.leno` 也转发了一套同名函数（见"已知缺口"）。

---

## 四、注意事项：平台差异 · 外部依赖 · 坑

| 项 | 说明 |
| --- | --- |
| **libcurl 动态库** | Windows 用随模块分发的 `libcurl-x64.dll`；POSIX 用随模块分发的 `libcurl.so`（curl 8.21.0，TLS 静态链 mbedTLS 3.5.2），失败回退系统 `libcurl.so.4` |
| **TLS 指纹** | 想要真浏览器指纹需换 **curl-impersonate** 版 libcurl；`setCipherList` / `setEcCurves` 只是"看起来不像脚本"那层 |
| **brotli** | 浏览器头里声明 `br` 时依赖 libcurl 后端支持 brotli，否则收到解不开的正文 |
| **重定向后正文** | 下载/落盘一律用 `rawBody()` / `bytes()`（`body` 可能已被字符集转码） |
| **`remoteSize`** | 返回 `-1` 是"**未知**"不是错误 ⇒ 未知时不要做完整性校验 |
| **`fetchAll`** | 结果按**输入顺序**返回；`concurrency` 建议 4~16；`multiInit` 失败时整批返回带 `resp.error` 的占位结果 |
| **回调上限** | `ffi.callback` 同时存活上限 **256**；`HttpClient.close()` 必须释放自己建的 2 个回调，否则第 128 次请求起失败 |
| **线程安全** | 每线程各用各的 `HttpClient` ✓；同一 client 跨线程共用**不安全**（用户责任）；`curl_global_init` 已改为模块加载时做一次 |
| **模块级全局** | `web_html` 用模块级全局管解析状态；`ObjModule.globals` **不随 `threads.start()` 深拷贝**（见 `多线程struct与模块全局变量问题记录.md`）⇒ 多线程共用一个模块的全局要当心 |
| **charset 后端** | Windows `kernel32` / POSIX `iconv`；**两者都不可用 ⇒ 原样返回**；POSIX（iconv）路径**未在 Windows 实测**，需在 Linux/macOS 跑一遍 |
| **sitemap 接口分两种** | `parseSitemap` 只取 `<loc>`（兼容旧接口）；要 lastmod/changefreq/priority 用 `parseSitemapEntries`；索引文件先用 `isSitemapIndex` 判 |
| **CSV** | `toCsv` 行尾是 CRLF（Excel 友好）；`writeCsv` 默认带 BOM |

---

## 五、已知缺口 / 待办

1. **`Web.leno` 有两处 `version()`**（70 行返回 `core.version()`、126 行返回 `net.version()`，行为等价）
   —— 同一入口重复定义，建议只留一个。
2. **`Ok` / `Err` / `map` / `filter` / `reduce` / `forEach` 双重转发**：既 `use generic.(...)` 又写了一遍
   `export func`（237-266 行），同样是重复来源。
3. **入口未转发的函数**（见上表）：WebSocket 的**全部函数**、CDP 低层、`web_gzip.rfcTableCheck`、
   `web_html.SelectorPart` —— 用户面想看齐"一个 import 全都有"就得补齐转发。
4. **gzip 只解不压**：需要"把结果写回 gzip 文件"的场景目前没有产出侧。
5. **`web_cdp` 需要系统装浏览器**：CI / 干净机器上要显式 `LENO_BROWSER` 指定，否则只能走静态抓取。
6. 本文件是 LenoWeb 的**首份参考文档**，API 清单以 2026-10-14 的源码为准；改模块时请同步本文件
   （尤其新函数要补 `export` 与转发）。

---

## 相关文档

- [`LenoHtml_开发问题记录.md`](LenoHtml_开发问题记录.md) —— HTML 解析器的问题与取舍
- [`LenoNet开发问题记录.md`](LenoNet开发问题记录.md) / [`LenoNet_崩溃问题排查记录.md`](LenoNet_崩溃问题排查记录.md) —— libcurl 绑定与崩溃排查
- [`移植音乐下载器实录与待办.md`](移植音乐下载器实录与待办.md) —— 实战用例（zlib + gb18030 那类）
- [`语言陷阱速查.md`](语言陷阱速查.md) —— `len()` vs `byte_len()` 等语言层坑
