# Leno 写 Web：能力缺口与修复路线（2026-10-18）

> 起因：用 `LenoHTTP` 写了演示站（`build/leno_module/LenoHTTP/examples/site.leno`），
> 实测路由 / 查询串 / 表单 / JSON / 静态文件 / 404 全通，但离"能对外服务"还有明确缺口。
> 本文把缺口、优先级、修法和验收标准定下来，避免"想到哪补到哪"。

## 0. 已验证的现状（基线）

| 能力 | 状态 |
|---|---|
| HTTP/1.1 请求解析（请求行/头/查询串/urlencoded 表单/Content-Length 体） | ✅ |
| 路由（精确匹配）、405、静态目录（含目录穿越防护） | ✅ |
| keep-alive（单连接 100 请求、5s 空闲超时） | ✅ |
| 响应助手（text/html/json/bytes/status/redirect/notFound） | ✅ |
| 单连接 100 请求 | **0.27 ms/请求 ≈ 3686 req/s**（Python `http.client` 测） |
| 并发 | ✅ **线程池**（2026-10-20）：`sock.detach()` → `Channel[int]` → `sockets.from_fd()`，见 §3.1 |
| TLS | ❌ 无 |
| 文件上传（multipart） | ❌ 无 |
| Range / 缓存头 / gzip 响应 | ❌ 无（gzip 已在 2026-10-18 补上，见 §2.1） |

## 1. 功能缺口（按"能不能上线"排序）

### P0 —— 决定"能不能对外"

| # | 缺口 | 现状影响 | 修法 | 验收 |
|---|---|---|---|---|
| 1.1 | **并发** | ✅ **已修（2026-10-20）**：`serveWorkers(app, host, port, n, worker)` 线程池 | 见 §3.1（`detach/from_fd` 方案已落地） | 3 个 slowloris 慢连接挂着时，5 个正常请求共 **2.3ms**（1 worker 对照 **3009ms**） |
| 1.2 | **TLS** | 公网不可用 | **建议不做**：前面挂 Caddy/nginx 反代（一行配置）；真要内建再 FFI OpenSSL | 文档写明"仅内网/反代" |

### P1 —— 决定"好不好用"

| # | 缺口 | 修法 | 验收 |
|---|---|---|---|
| 2.1 | gzip 响应 | ✅ **本轮已做**：`Accept-Encoding: gzip` + 体积 ≥ 1KB + 可压缩类型 ⇒ `Content-Encoding: gzip` + `Vary` | 客户端带 gzip 头时体积降 ≥ 50%，`Content-Length` 与解压后长度分别正确 |
| 2.2 | 模板自动转义 | ✅ **本轮已做**：`http.esc()` / `http.render(tpl, vars)`（`{{x}}` 转义、`{{{x}}}` 原样） | XSS 输入渲染成 `&lt;script&gt;`；缺键给空串 |
| 2.3 | 路径参数路由 | ✅ **已做（2026-10-20）**：`http.get(app, "/user/:id", h)` ⇒ `req["params"]["id"]`；精确路由优先、段数必须相同、方法不符仍 405 | `test_route_params.leno` 六组：单/多参数、段数不匹配 404、精确优先、405、与鉴权叠加 |
| 2.4 | 中间件链 | ✅ **部分已做（2026-10-20）**：`http.before(app, fn)` 全局前置管道（返回 `null` 放行、返回响应短路）+ **声明式鉴权** `http.auth(app, fn)` + 路由标 `{auth: true}`。**不做** `next()` 洋葱模型（同步 handler 用不上，改响应走 `_write_response`） | `test_middleware.leno` 六组：公开路由 / 401 / 带 token 拿到 `req["user"]` / 缺检查器 500 / `before` 短路 503 / 中间件抛异常后同一连接仍正常 |
| 2.5 | multipart/form-data | ✅ **已做（2026-10-21）**：`http_parse.parse_multipart`（全程 Array[int] 字节扫描）+ `req["files"]` + `http.file(req,name)` / `http.files(req)`；文本字段并入 `form`（formValue 通用） | `test_multipart_upload.leno` 八组：文本字段 / 2 个文件 / 中文文件名 / 二进制 10 字节逐字节 / 伪边界不误切 / 缺失字段为空 Dict |
| 2.6 | Range + 缓存头 | ✅ **已做（2026-10-21）**：`ETag`（size-mtime）+ `If-None-Match` ⇒ 304；`Range: bytes=a-b` ⇒ 206 + `Content-Range`；越界 ⇒ 416 + `bytes */total`；**未做** `Last-Modified`（需 GMT 格式化，`times.format` 是本地时区 ⇒ 宁可不发也不发错） | `test_static_cache.leno` 五组（304 等）+ `test_static_range.leno` 五组（1MB 完整 / 206 片段 / 206 到结尾 / 416 / 小文件回归） |
| 2.7 | 大文件不整读内存 | ✅ **已做（2026-10-21）**：> 256KB 走分块读发（64KB 一块，`files.open(path,"rb")` + `seek` + 循环 `send`），内存峰值恒定；分块路径不 gzip | 下载 1MB 文件内容与原文逐字节一致（首尾都验），`test_static_range.leno` ① |

### P2 —— 锦上添花

| # | 缺口 | 说明 |
|---|---|---|
| 3.1 | SSE（`text/event-stream`） | 成本最低的"实时"方案，长连接 + `data:` 帧，适合进度/日志推送 |
| 3.2 | Cookie / Session | ✅ **已做（2026-10-20）**：`http.cookie/setCookie/clearCookie`；Session 由应用建表（见 `examples/board.leno`、`music_api.leno`） |
| 3.3 | HEAD / OPTIONS / CORS 助手 | 预检与跨域开发常用 |
| 3.4 | WebSocket | 握手 + 帧解析，工作量明显大于 SSE，按需 |
| 3.5 | 请求日志中间件 | 与 2.4 一起做：`方法 路径 状态 耗时 字节` |

## 2. 本轮已修（2026-10-18）

### 2.1 gzip 响应
- 位置：`http_server.leno` 的 `_write_response`（新增 `acceptGzip` 参数）
- 判据：`Accept-Encoding` 含 `gzip` + 响应体 ≥ 1024 字节 + 未自带 `Content-Encoding` +
  类型属于 `text/*`、`application/json|javascript|xml`、`image/svg+xml`
- 头：`Content-Encoding: gzip`、`Vary: Accept-Encoding`；`Content-Length` 用**压缩后**长度
- 依赖：`LenoCompress`（同目录 `leno_module` 下）

### 2.2 模板自动转义
- 位置：`http_app.leno` 新增 `esc()` / `render()`
- 语法：`{{name}}` **转义**输出；`{{{name}}}` **原样**输出（明确信任时才用）；缺键输出空串
- 对外：`HTTP.leno` 暴露 `esc` / `render` / `htmlTemplate(tpl, vars)`

## 3. 语言 / 运行时层（比补库更值钱）

### 3.1 `Socket` 跨线程（P0 的根因）— ✅ 已落地（路线 1）
`threads.start` 实参白名单不含 Socket ⇒ 写不出"每连接一线程"。三条路线：

1. **`fd()` + `from_fd()`**（✅ 2026-10-20 落地）：Socket → int → 线程内重建。
   - `sockets.c` 新增 `sock.fd()` / `sock.detach()`（交出所有权，原对象 fd 置 INVALID ⇒ 不 double close）
     与 `sockets.from_fd(fd)`（新对象负责 close）。
   - LenoHTTP 侧：`serve_workers/worker_loop`（`lib/http_server.leno`）+ `HTTP.serveWorkers/workerLoop`。
   - ⚠ **app 不能传进线程**：路由表里是 handler 闭包，`value_clone_for_channel` 拒绝闭包
     ⇒ worker 入口必须自己 `buildApp()`（每线程一份路由表）。
   - ⚠ 模块级 `var` 跨线程共享且无锁 ⇒ 可变状态走 Channel 单点化。
   - 验收：3 个 slowloris 慢连接（各挂 3s）+ 5 个正常请求，8 workers 时正常请求 **2.3ms**；
     1 worker 对照 **3009ms**（`test/test_thread_server.leno`）。
2. 把 Socket 加入跨线程白名单：要处理引用计数、双端关闭竞争 —— **不做**（路线 1 已够）。
3. 单线程事件循环：非阻塞 + `select` + **HTTP 增量解析** —— 暂不做；连接数远超核数时再考虑。

### 3.2 字符串累加是 O(n²)
实测：XLSX 1 万格逐格 `s = s + x` **1.8 秒**，`Array[int]` 累积后 **80ms**。
建议：`StringBuilder` 类型，或编译器把**循环内**的 `s = s + x` 自动降级为 builder。

### 3.3 模板字符串缺"上下文转义"
`$"..."` 有插值但不转义。建议加 `html"..."` 前缀：插值点自动 HTML 转义，从"靠自觉"变"默认安全"。

### 3.4 错误模型不统一
`throw` / `null` / `false` / `Dict{ok}` 四种并存。Web 端至少要保证 handler 抛异常不会杀进程：
dispatcher 统一 `try/catch` → 500 页面（本轮未做，列为 2.4 的配套）。

### 3.5 默认参数求值时机
跨模块已能取值（96fa42a），但同模块按调用时、跨模块按定义时 ⇒ 建议统一为**定义时**。

## 4. 明确不做（有意的取舍）

- **TLS**：交给反代。理由：证书轮换、ALPN、会话复用这些细节不值得在语言里重写一遍。
- **HTTP/2、WebSocket（短期）**：收益/成本比低；SSE 已能覆盖大多数"推送"需求。
- **模板引擎的大语法**（继承/宏/过滤器链）：保持"最小可用"，避免长成第二个 Jinja。

## 5. 下一步顺序

1. ~~2.4 中间件~~ ✅ 已做（2026-10-20：声明式鉴权 `{auth: true}` + 全局管道 `before` + 500 兜底）；**剩下 2.3 路径参数**
2. 2.6 Range/缓存头 + 2.7 分块发送 —— 静态资源是每个站都有的
3. 2.5 multipart —— 有上传需求时立刻做
4. ~~3.1 并发~~ ✅ 已完成（2026-10-20，线程池 `serveWorkers`，见 §3.1）
