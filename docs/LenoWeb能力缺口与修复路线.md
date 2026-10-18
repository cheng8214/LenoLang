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
| 并发 | ❌ 单线程顺序处理（`Socket` 不能跨线程） |
| TLS | ❌ 无 |
| 文件上传（multipart） | ❌ 无 |
| Range / 缓存头 / gzip 响应 | ❌ 无（gzip 已在 2026-10-18 补上，见 §2.1） |

## 1. 功能缺口（按"能不能上线"排序）

### P0 —— 决定"能不能对外"

| # | 缺口 | 现状影响 | 修法 | 验收 |
|---|---|---|---|---|
| 1.1 | **并发** | 慢客户端占住后续所有连接；压测/真实流量下不可用 | 见 §3.1 三条路线（推荐 `Socket.fd()/from_fd()`） | 10 个慢连接并行时，普通请求仍 < 50ms |
| 1.2 | **TLS** | 公网不可用 | **建议不做**：前面挂 Caddy/nginx 反代（一行配置）；真要内建再 FFI OpenSSL | 文档写明"仅内网/反代" |

### P1 —— 决定"好不好用"

| # | 缺口 | 修法 | 验收 |
|---|---|---|---|
| 2.1 | gzip 响应 | ✅ **本轮已做**：`Accept-Encoding: gzip` + 体积 ≥ 1KB + 可压缩类型 ⇒ `Content-Encoding: gzip` + `Vary` | 客户端带 gzip 头时体积降 ≥ 50%，`Content-Length` 与解压后长度分别正确 |
| 2.2 | 模板自动转义 | ✅ **本轮已做**：`http.esc()` / `http.render(tpl, vars)`（`{{x}}` 转义、`{{{x}}}` 原样） | XSS 输入渲染成 `&lt;script&gt;`；缺键给空串 |
| 2.3 | 路径参数路由 | `http.get(app, "/user/:id", h)` ⇒ `req["params"]["id"]` | `/user/42` 命中并取到 `"42"`；`/user/a/b` 不命中 |
| 2.4 | 中间件链 | `http.use(app, fn)`，`fn(app, req, next)`；内置请求日志/计时 | 两条中间件按注册顺序执行，`next()` 能短路 |
| 2.5 | multipart/form-data | 按 boundary 切分，解析 `name` / `filename` / 体 | 上传 1 个中文名文本文件，能取到名字与内容 |
| 2.6 | Range + 缓存头 | 静态文件支持 `Range: bytes=a-b`（206）、`ETag`/`If-None-Match`（304）、`Last-Modified` | 断点续传拿到正确片段；二次请求返回 304 |
| 2.7 | 大文件不整读内存 | 静态文件 > 4MB 走分块发送 | 下载 100MB 文件进程内存不暴涨 |

### P2 —— 锦上添花

| # | 缺口 | 说明 |
|---|---|---|
| 3.1 | SSE（`text/event-stream`） | 成本最低的"实时"方案，长连接 + `data:` 帧，适合进度/日志推送 |
| 3.2 | Cookie / Session | `Cookie` 解析助手 + 内存/文件 Session；`Set-Cookie` 构造 |
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

### 3.1 `Socket` 跨线程（P0 的根因）
`threads.start` 实参白名单不含 Socket ⇒ 写不出"每连接一线程"。三条路线：

1. **`fd()` + `from_fd()`**（推荐，改动最小）：Socket → int → 线程内重建。
   需要保证：`from_fd` 出来的 Socket 不再重复 close（所有权明确）。
2. 把 Socket 加入跨线程白名单：要处理引用计数、双端关闭竞争。
3. 单线程事件循环：非阻塞 + `select` + **HTTP 增量解析**（现在的解析是"阻塞读到完整请求"，
   要改状态机，工作量中等，但可彻底解决"一个慢连接卡所有人"）。

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

1. 2.3 路径参数 + 2.4 中间件（含 3.4 的 500 兜底）—— 写大站点前必须先有
2. 2.6 Range/缓存头 + 2.7 分块发送 —— 静态资源是每个站都有的
3. 2.5 multipart —— 有上传需求时立刻做
4. 3.1 并发 —— 决定能不能对外，投入最大，建议单独排期
