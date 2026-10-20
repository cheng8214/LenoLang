# LenoHTTP v0.2.1

Leno 的 **HTTP/1.1 服务端**（纯 Leno，零原生依赖，基于内置 `sockets`）。
路由、查询串、表单、静态文件、keep-alive、**线程池并发**、**视图文件**。

```leno
import "HTTP" as http

var app = http.newApp()

http.get(app, "/", handler_home)                 // 处理函数可以是具名函数…
http.get(app, "/echo", func(Dict req): Dict {    // …也可以是闭包
    return http.json({"q": req["query"]["q"]})
})
http.post(app, "/form", handler_form)            // req["form"] 已解析好
http.static(app, "/assets", "D:/www")            // 静态目录

http.serve(app, "0.0.0.0", 8080)                 // 阻塞式服务（单线程）
```

多线程并发（0.2）：

```leno
func buildApp(): Dict {                          // 每个 worker 线程各调一次
    var app = http.newApp()
    http.get(app, "/", handler_home)
    return app
}
func worker(int idx, Channel conns): int {       // 线程入口：具名模块函数，arity=2
    return http.workerLoop(buildApp(), conns)
}
main() {
    var app = buildApp()                         // 主线程这份只当 running 开关
    http.serveWorkers(app, "0.0.0.0", 8080, 8, worker)
}
```

完整可跑示例：

| 示例 | 内容 |
|---|---|
| `examples/hello_server.leno` | 最小单线程服务 |
| `examples/threaded_server.leno` | 线程池（`serveWorkers`） |
| `examples/site.leno` | 演示站：路由 / 表单 / JSON / 静态 / 404（用 `$@"..."` 写 HTML） |
| `examples/music_api.leno` | **鉴权 + SQLite 检索 + JSON 接口**（Bearer/cookie 两种鉴权、参数绑定防注入） |
| `examples/board.leno` | 注册 / 登录 / 留言（会话 + CSRF + 模板转义） |

```bat
build\leno.exe build\leno_module\LenoHTTP\examples\hello_server.leno 8080 D:\www
build\leno.exe build\leno_module\LenoHTTP\examples\threaded_server.leno 8080 8
build\leno.exe build\leno_module\LenoHTTP\examples\music_api.leno 8090
```

## API

| 函数 | 说明 |
|---|---|
| `newApp()` | 新建应用 `{routes, statics, running}` |
| `get/post/put/del(app, path, handler)` | 注册路由（精确匹配） |
| `route(app, method, path, handler)` | 任意方法（`"*"` = 全部） |
| `static(app, prefix, dir)` | 挂静态目录（`/assets` → `dir`） |
| `listen(host, port)` | 监听 → `{ok, sock, host, port, err}`；`port=0` 由系统分配 |
| `run(server, app)` | 在已有监听器上跑主循环（避免二次 bind） |
| `serve(app, host, port)` / `serveOn(app, port)` | 监听 + 主循环（阻塞，单线程） |
| `serveWorkers(app, host, port, n, entry, queue=0, maxConns=0)` | **线程池**：主线程只 accept，连接交给 n 个 worker |
| `workerLoop(app, conns)` | worker 侧循环：拿 fd → `from_fd` → `handle_conn` |
| `acceptOne(server, app)` | 只处理一个连接（测试/自定义循环） |
| `stop(app)` | 让 `serve/run` 退出（`serveWorkers` 用 `maxConns` 或进程退出） |
| `text/html/json/bytes/status/redirect/notFound` | 响应助手 |
| `esc(s)` | HTML 转义（`& < > " '`） |
| `render(tpl, vars)` | 模板：`{{x}}` **转义**输出、`{{{x}}}` 原样输出、缺键给空串 |
| `htmlTemplate(tpl, vars)` | 模板 → `text/html` 响应 |
| `query(req, name, def="")` | 取查询参数（缺键/空串 ⇒ 默认值）**0.2.1 新增** |
| `formValue(req, name, def="")` | 取表单字段（同上） |
| `header(req, name, def="")` | 取请求头（名字不区分大小写） |
| `views(app, dir)` | 指定视图目录 **0.2.1 新增** |
| `view(app, name, vars)` | 渲染 `dir/name.html`，自动套 `dir/layout.html`（`{{{body}}}`） |
| `cookie(req, name, def)` | 读 cookie（`Cookie: a=1; b=2`） |
| `setCookie(res, name, value, path, maxAge)` | 写 `Set-Cookie`（`maxAge=0` = 会话 cookie；可多次调用，逐行发出） |
| `clearCookie(res, name)` | 删除 cookie |

**慢连接保护（2026-10-20）**：单个请求的读入总时长上限 `MAX_REQUEST_SECONDS = 10`（秒），
用 `select` 等"剩余时间"实现 ⇒ 滴水式客户端（每 4.9s 送一个字节）也不会把连接占死。
超时返回 400 并关闭连接。线程池下每个 worker 仍受这条约束（最多被占 10s，不会永久卡住）。

**gzip 响应（2026-10-18，自动）**：请求带 `Accept-Encoding: gzip` 且响应体 ≥ 1KB、
类型可压（`text/*` / json / javascript / xml / svg）时自动压缩，头里给
`Content-Encoding: gzip` 与 `Vary: Accept-Encoding`，`Content-Length` 是**压缩后**长度。
要关掉就自己设 `headers["Content-Encoding"]`（有值即跳过压缩）。

**请求** `req`：

```leno
{method, target, path, query: Dict, headers: Dict, body, form: Dict,
 version, keepAlive, remote}
```

**响应**（handler 返回；也可以直接返回 `string`）：

```leno
{status: 200, body: "...", contentType: "text/plain; charset=utf-8", headers: Dict}
```

> handler 直接 `return "<h1>hi</h1>"` 时按 **`text/html`** 发出（0.2.1 起；旧版是 text/plain）。
> 要纯文本请显式 `return http.text(...)`。

## HTML 怎么写才不啰嗦（0.2.1）

用**原始插值字符串** `$@"..."`：属性引号写 `""`、反斜杠原样、`{expr}` 插值，HTML 不需要 `+` 拼接：

```leno
func hello(Dict req): string {
    var name = http.query(req, "name", "世界")        // 一行取参数
    return $@"<main><section class=""card"">
  <h1>你好 {http.esc(name)}</h1>
  <p>注意：<b>插值不会自动转义</b>，用户输入一律 http.esc(...)</p>
</section></main>"
}
```

> `""` = 一个引号；换行直接写在字符串里（支持跨行）。
> 也可以改用 `$"..."`（这时引号要写 `\"`），两者结果一致。
> 想彻底不碰引号就用**视图文件**：HTML 放 `views/hello.html`，handler 里一行
> `return http.view(app, "hello", {name: name})`。

视图文件约定：

```
views/
  layout.html   ← 用 {{{body}}} 插入页面；title 等变量也能直接用
  hello.html    ← <h1>你好 {{name}}</h1>
```

```leno
http.views(app, "views")
http.get(app, "/hello", func(Dict req): Dict {
    return http.view(app, "hello", {name: "Leno", title: "问候"})
})
```

`{{x}}` 默认**转义**（防 XSS），`{{{x}}}` 原样输出（只对可信内容用）。

## 行为与设计

- **路由顺序**：精确路径（注册顺序）→ 静态目录 → 404；**路径命中但方法不符 ⇒ 405**
- **keep-alive**：HTTP/1.1 默认保持（`Connection: close` 关闭）、HTTP/1.0 需显式
  `keep-alive`；单连接最多 100 个请求，空闲 5s 超时
- **上限**：请求头 64KB、请求体 8MB（超出报 413/400）
- **静态文件**：拒绝任何含 `..` 的路径段（防目录穿越）；目录请求补 `index.html`；
  按扩展名给 Content-Type；文件整读进内存
- **并发模型**：两条路 —— `run/serve` 单线程顺序处理；`serveWorkers` 是**线程池**：
  主线程只 `accept`，`sock.detach()` 拿到 fd（整数）经 `Channel` 交给 worker，
  worker 用 `sockets.from_fd(fd)` 在自己堆上重建 Socket 再处理（`Socket` 对象本身仍不能跨线程）。
  - ⚠ **app 必须在 worker 线程内构建**：路由表里存的是 handler 闭包，闭包不能跨线程传，
    所以入口要自己 `buildApp()`（每个线程一份路由表）。
  - worker 入口建议写成**同模块的具名函数**（`func(int, Channel)`）：`threads.start` 只认真实闭包，
    个别路径取到的裸 `ObjFunction` 会被拒绝，具名函数最稳。
  - ⚠ **可变共享状态**：模块级 `var` 是所有线程同一份且无锁，计数/缓存/session 请用
    `Channel` 汇到单点线程处理，或每线程一份。
  - 队列满（默认 `workers*4`）时直接回 503 关连接，不阻塞 accept。
- **debug 日志**：`app.set("debug", true)` 会为每个请求打印一行
  （方法 / target / 状态 / keep / carry 字节数），排查"请求收不到"很快。

## 顺带修掉的三个问题（本模块的副产品）

1. **`sock.send()` 原来只调一次 `send()` 且忽略返回值** ⇒ 大响应被**静默截断**
   （实测发 1MB 静态文件，客户端只收到前几十 KB，而 Leno 侧拿到的是 `true`）。
   现在改成**循环发完**（EINTR 重试、EWOULDBLOCK 有限重试），见 `src/module/sockets/sockets.c`。
2. **`accept()` 出来的 socket 会继承监听端的 nonblocking**（Windows 上明显）⇒
   不显式转回阻塞，`recv` 在"数据还没到"时立刻返回 null，keep-alive 的第二个请求
   会被误判成"对端关闭"（症状：第一个请求正常、第二个永远收不到）。
   LenoHTTP 在 `handle_conn` 里显式 `set_nonblocking(false)` 再 `set_timeout(5000)`。
3. **状态短语表缺 401/409/429/502/503** ⇒ 这些状态码全被兜成 `HTTP/1.1 401 OK`
   （写鉴权接口时实测发现，客户端和日志都很别扭）。已补齐，并对未列出的码按
   `4xx → Client Error` / `5xx → Server Error` 兜底，不再把错误说成 OK。

## 验证

```bat
:: Leno 侧自测：路由 / 查询串 / 表单 / 404 / 405 / 静态文件 / keep-alive / 200KB 大响应
build\leno.exe build\leno_module\LenoHTTP\test\test_http.leno

:: 线程池并发：3 个 slowloris 慢连接挂着时，正常请求必须 < 1500ms（1 worker 对照会失败）
build\leno.exe build\leno_module\LenoHTTP\test\test_thread_server.leno

:: 视图文件：渲染 / layout 生效 / 默认转义 / 缺文件报错（不起网络）
build\leno.exe build\leno_module\LenoHTTP\test\test_view.leno

:: 鉴权接口：401 / 登录拿 token / 带 token 搜索 / SQL 注入当字符串 / 空参 400 / cookie 鉴权
build\leno.exe build\leno_module\LenoHTTP\test\test_music_api.leno

:: Python 独立客户端压测（开发期工具；http.client + socket，与 Leno 实现完全无关）
python build\leno_module\LenoHTTP\test\dev_stress_python.py
```

Python 压测（Windows x64，同一台机器）：

| 场景 | 结果 |
|---|---|
| keep-alive 单连接 100 请求 | **0.27 ms/请求（≈3686 req/s）** |
| 每次新建连接 100 请求 | 8.2–9.3 ms/请求 —— **本机 Python→Python 的 connect/close 基线就是 7.0ms**（安全软件/回环开销），服务端只多 1–2ms |
| 10 线程 × 10 请求并发 | 122 ms 总计（≈823 req/s），0 错误 —— 这是**单线程** serve 的数据（靠 backlog 排队） |
| **线程池**：20 线程 × 20 请求（keep-alive，8 workers） | **42 ms 总计（≈9500 req/s），0 错误** |
| **线程池**：3 个 slowloris 慢连接（各挂 3s）+ 5 个正常请求，8 workers | 正常请求共 **2.3 ms** |
| 同上，但只用 **1 worker**（对照，验证测试判别力） | 正常请求 **3009 ms** —— 被慢连接堵住 |
| POST 表单（中文 urlencoded） | 字段回显正确 |
| POST JSON 4KB | 字节数正确 |
| 404 / 405 / 畸形请求 | 404 / 405 / 400 |
| 200KB 静态文件 | sha256 逐字节一致（验证 `send` 循环修复） |
| keep-alive 20 个请求 | 全 200 |

## 边界（v0.1 明确不做）

| 不做 | 说明 |
|---|---|
| chunked 请求体 / multipart | 只按 `Content-Length` 读；文件上传后续版本再说 |
| TLS / HTTP/2 / WebSocket | 没有；需要时套反向代理（nginx/caddy） |
| Range / 断点续传 / gzip 响应 | 没有（`LenoCompress` 有压缩能力，但响应侧还没接） |
| 大文件流式发送 | 整读进内存（<几十 MB 还行） |

## 能力缺口与路线

见 [`docs/LenoWeb能力缺口与修复路线.md`](../../../docs/LenoWeb能力缺口与修复路线.md)：
并发（`Socket` 跨线程）、multipart、Range/缓存头、中间件、路径参数等，都写了修法与验收标准。

## 下一步

- `multipart/form-data`（文件上传）
- 响应 gzip（接 `LenoCompress.deflateRaw`）+ 静态文件 ETag/Last-Modified
- ~~连接处理下沉到线程~~ ✅ 0.2 已用 `fd` 交接 + 线程池落地（见 `serveWorkers`）
- 简单中间件（日志/超时/鉴权）与 `:param` 路由
