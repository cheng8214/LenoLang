# LenoWeb 示例与测试

> 2026-10-06 整理：原先 46 个 `.leno` 平铺在 `examples/` 根目录，现在按用途归类。

## 目录

| 目录 | 内容 | 需要外网 |
| --- | --- | --- |
| [`tests/`](tests/) | **断言式用例**（`check` + 退出码），可离线、可当门禁 | 否 |
| [`01_HTTP基础/`](01_HTTP基础/) | GET/POST/PUT/DELETE、自定义请求头、JSON、HTTPS、文件下载 | 是 |
| [`02_会话与Cookie/`](02_会话与Cookie/) | `Session` 跨请求保持 cookie / 认证 | 是 |
| [`03_HTML解析/`](03_HTML解析/) | 选择器、伪类、属性选择器、表格/表单/meta 提取、转文本、节点遍历 | 否 |
| [`04_URL与合规/`](04_URL与合规/) | URL 解析/重组/工具、编解码、robots.txt、sitemap | 否 |
| [`05_爬虫实战/`](05_爬虫实战/) | 完整爬虫（名言列表、翻页、站内爬取、酷我音乐） | 是 |
| [`06_语言特性/`](06_语言特性/) | 泛型、类型收窄、调试输出 —— 顺带验证模块与语言类型系统的配合 | 否 |
| [`07_综合测试/`](07_综合测试/) | 全量测试、新特性、压力测试、JSON 单测、基础 API | 是 |

> `tests/` 之外的例子大多是**演示脚本**（只 `print`、不断言，部分需要外网），
> 所以不放进门禁；要验证正确性请以 `tests/` 与仓库根的 `assert/` 为准。

## 跑测试

```bash
# 一键跑完 tests/ 下全部用例（离线，全绿时退出码 0）
build\leno.exe build\leno_module\LenoWeb\examples\tests\run_tests.leno

# 也可以单跑某一个
build\leno.exe build\leno_module\LenoWeb\examples\tests\test_charset_http.leno
```

## tests/ 用例说明

| 用例 | 覆盖 |
| --- | --- |
| `test_charset_unit.leno` | 字符集名→代码页、BOM/Content-Type/`<meta>` 探测、UTF-8 校验、GBK↔UTF-8 转码与编码（纯函数） |
| `test_charset_http.leno` | HTTP 层自动转码：声明/未声明的 GBK、UTF-8 不被改动、二进制原样保留、`totalSize` 为字节数 |
| `test_http_repeat.leno` | 300 次连续请求（回调不泄漏）+ 复用客户端（累加器每轮清空） |
| `test_multipart.leno` | `multipart/form-data` 文本字段 + 文件上传 + PUT + 边界 |
| `test_crawler_queue.leno` | URL 归一化去重、限速自动生效、robots `crawl-delay`、CSV 落地 |
| `test_ffi_callback_thread.leno` | **运行时回归**：子线程里的 FFI 回调（`qsort` 驱动，曾经只成功第一次） |
| `test_thread_http.leno` | **多线程并发 HTTP**：8 线程 × 6 轮 × 3 种模式，含并发抓 GBK 页面 |
| `test_download_resume.leno` | **断点续传下载**：续传拼接逐字节校验、已完整不重下、本地过大重下、**服务端忽略 Range 时回退**、404 ⇒ -1 |
| `test_crawler_state.leno` | **断点续爬**：进度落盘/恢复（页数 + 已访问 + 待抓队列）、自动落盘、坏存档不崩 |
| `test_utf8_text.leno` | **非 ASCII 口径回归**：中文页面的解析/属性/聚合/清洗不被截断；命名实体表 |
| `test_selector_ext.leno` | **扩展选择器**：`an+b` / `of-type` / `:not` / `:has` / 兄弟组合器 / 属性运算符 / 中文类名 |
| `test_priority_queue.leno` | **优先队列**：优先级出队 / 同级 FIFO 稳定 / 与去重共存 / 存档往返 / 兼容旧存档 |
| `test_sitemap_deep.leno` | **sitemap**：索引递归 / 元数据 / URL 去重 / 跨站忽略 / 层数与数量上限 / `.gz` 真解压与降级 |
| `test_gzip.leno` | **gzip/DEFLATE 解压**（纯 Leno）：三种块类型 / 空正文 / 二进制保真 / 头可选字段 / 错误路径 / RFC 表核对 |
| `test_cdp.leno` | **动态渲染**：WS 握手（SHA-1/Base64 向量）、浏览器发现与启动、CDP 往返、渲染后 DOM 接解析器、复用与关闭（**无浏览器时自动跳过**） |
| `test_antibot.leno` | **反爬基础**：代理池轮询/失败冷却/恢复/剔除法、**真走代理**（本机假代理回显绝对 URI）、Session 自动轮换（用死代理端口可观测地证明换了）、浏览器头顺序、TLS 选项探测 |
| `test_fetch_all.leno` | **并发批抓**：用 5 个独立延迟服务端把并发**量出来**（并发 253ms vs 串行 1357ms）、结果顺序对齐输入、单条失败不拖累其它、40×2 条回调不泄漏、GBK/代理协同 |

### `_testkit.leno`

公共工具：断言（`check/checkTrue/checkHas/checkNotHas/summary`）+ **本机 HTTP 服务端**
（`serve` / `waitUp` / `shutdown`），让用例不依赖外网。

服务端跑在**子线程**里（范式来自仓库根 `assert/test_sockets_io.leno`）：

```leno
// ⚠ threads.start 的入口必须是**本文件**的函数 —— 不能直接传 tk.serve
func serverEntry(int port, int maxReqs) { tk.serve(port, maxReqs) }

main() {
    var srv = threads.start(serverEntry, 39611, 512)
    tk.waitUp(39611)
    // ... 用 web.get("http://127.0.0.1:39611/xxx") 做请求 ...
    tk.shutdown(39611)
    srv.join()
}
```

服务端路由（`_testkit.leno` 内）刻意覆盖爬虫最容易踩的编码场景：
`/gbk`、`/gbk-nod`（不声明 charset）、`/utf8`、`/bin`（非法 UTF-8 的二进制）、
`/echo/<token>`（正文=`BODY:<token>`，用于自证没串台）、`/post`（回显完整请求）、`/quit`。

## 多线程

**HTTP 请求可以从 `threads.start()` 的子线程发起**（2026-10-06 起，见 `tests/test_thread_http.leno`
—— 8 线程 × 6 轮 × 3 种模式全绿）。约定与 `requests.Session` 一致：

- 每线程各用各的 `HttpClient`，或用模块级 `web.get(...)`（内部每次新建）⇒ 安全 ✓
- 同一个 `HttpClient` 跨线程共用 ⇒ 不安全（用户责任）

> 这条曾经走不通（子线程里 FFI 回调"只能成功第一次"、并发直接崩），根因在运行时
> `src/vm/vm.c` 的 `vm_call_value` 写错了对象，外加两处并发竞态；三处均已修。
> 详见 `docs/多线程struct与模块全局变量问题记录.md` 第八节。
> 回归用例：`tests/test_ffi_callback_thread.leno`（运行时层）、`tests/test_thread_http.leno`（LenoWeb 层）。
