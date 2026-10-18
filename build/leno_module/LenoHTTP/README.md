# LenoHTTP v0.1.0

Leno 的 **HTTP/1.1 服务端**（纯 Leno，零原生依赖，基于内置 `sockets`）。
路由、查询串、表单、静态文件、keep-alive。

```leno
import "HTTP" as http

var app = http.newApp()

http.get(app, "/", handler_home)                 // 处理函数可以是具名函数…
http.get(app, "/echo", func(Dict req): Dict {    // …也可以是闭包
    return http.json({"q": req["query"]["q"]})
})
http.post(app, "/form", handler_form)            // req["form"] 已解析好
http.static(app, "/assets", "D:/www")            // 静态目录

http.serve(app, "0.0.0.0", 8080)                 // 阻塞式服务
```

完整可跑示例：`examples/hello_server.leno`

```bat
build\leno.exe build\leno_module\LenoHTTP\examples\hello_server.leno 8080 D:\www
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
| `serve(app, host, port)` / `serveOn(app, port)` | 监听 + 主循环（阻塞） |
| `acceptOne(server, app)` | 只处理一个连接（测试/自定义循环） |
| `stop(app)` | 让 `serve/run` 退出 |
| `text/html/json/bytes/status/redirect/notFound` | 响应助手 |
| `esc(s)` | HTML 转义（`& < > " '`） |
| `render(tpl, vars)` | 模板：`{{x}}` **转义**输出、`{{{x}}}` 原样输出、缺键给空串 |
| `htmlTemplate(tpl, vars)` | 模板 → `text/html` 响应 |

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

## 行为与设计

- **路由顺序**：精确路径（注册顺序）→ 静态目录 → 404；**路径命中但方法不符 ⇒ 405**
- **keep-alive**：HTTP/1.1 默认保持（`Connection: close` 关闭）、HTTP/1.0 需显式
  `keep-alive`；单连接最多 100 个请求，空闲 5s 超时
- **上限**：请求头 64KB、请求体 8MB（超出报 413/400）
- **静态文件**：拒绝任何含 `..` 的路径段（防目录穿越）；目录请求补 `index.html`；
  按扩展名给 Content-Type；文件整读进内存
- **并发模型**：**单线程顺序处理**。原因不是偷懒 —— `Socket` **不能跨线程传递**
  （`threads.start` 的实参只允许标量/string/Array/Dict/struct/Channel，Socket 是对象
  ⇒ 运行期直接拒绝），所以"每连接一个线程"在本语言里做不到。慢客户端会占住后续连接。
- **debug 日志**：`app.set("debug", true)` 会为每个请求打印一行
  （方法 / target / 状态 / keep / carry 字节数），排查"请求收不到"很快。

## 顺带修掉的两个 sockets 问题（本模块的副产品）

1. **`sock.send()` 原来只调一次 `send()` 且忽略返回值** ⇒ 大响应被**静默截断**
   （实测发 1MB 静态文件，客户端只收到前几十 KB，而 Leno 侧拿到的是 `true`）。
   现在改成**循环发完**（EINTR 重试、EWOULDBLOCK 有限重试），见 `src/module/sockets/sockets.c`。
2. **`accept()` 出来的 socket 会继承监听端的 nonblocking**（Windows 上明显）⇒
   不显式转回阻塞，`recv` 在"数据还没到"时立刻返回 null，keep-alive 的第二个请求
   会被误判成"对端关闭"（症状：第一个请求正常、第二个永远收不到）。
   LenoHTTP 在 `handle_conn` 里显式 `set_nonblocking(false)` 再 `set_timeout(5000)`。

## 验证

```bat
:: Leno 侧自测：路由 / 查询串 / 表单 / 404 / 405 / 静态文件 / keep-alive / 200KB 大响应
build\leno.exe build\leno_module\LenoHTTP\test\test_http.leno

:: Python 独立客户端压测（开发期工具；http.client + socket，与 Leno 实现完全无关）
python build\leno_module\LenoHTTP\test\dev_stress_python.py
```

Python 压测（Windows x64，同一台机器）：

| 场景 | 结果 |
|---|---|
| keep-alive 单连接 100 请求 | **0.27 ms/请求（≈3686 req/s）** |
| 每次新建连接 100 请求 | 8.2–9.3 ms/请求 —— **本机 Python→Python 的 connect/close 基线就是 7.0ms**（安全软件/回环开销），服务端只多 1–2ms |
| 10 线程 × 10 请求并发 | 122 ms 总计（≈823 req/s），0 错误 |
| POST 表单（中文 urlencoded） | 字段回显正确 |
| POST JSON 4KB | 字节数正确 |
| 404 / 405 / 畸形请求 | 404 / 405 / 400 |
| 200KB 静态文件 | sha256 逐字节一致（验证 `send` 循环修复） |
| keep-alive 20 个请求 | 全 200 |

## 边界（v0.1 明确不做）

| 不做 | 说明 |
|---|---|
| 并发处理 | 单线程（原因见上）；压测里并发请求靠 backlog 排队，不会出错但会串行 |
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
- 连接处理下沉到线程（**前提**：运行期允许 Socket 跨线程，或改成"线程内自建连接"的
  模型 —— 当前语言限制下做不了）
- 简单中间件（日志/超时/鉴权）与 `:param` 路由
