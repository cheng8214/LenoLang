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
- 队列出队从 `remove(0)`（O(n)）改为头下标 + 定期回收
- **CSV 落地**：`web.writeCsv(path, rows, header)` / `web.toCsv(rows)`（RFC 4180 转义 + UTF-8 BOM，Excel 直接认中文）

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

## 示例与测试

见 [`examples/README.md`](examples/README.md)。一键跑测试：

```bash
build\leno.exe build\leno_module\LenoWeb\examples\tests\run_tests.leno
```
