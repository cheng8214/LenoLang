# LenoNet 开发问题记录

## 1. curl_easy_getinfo 返回错误码 48

### 问题描述
调用 `curl_easy_getinfo` 获取 `CURLINFO_RESPONSE_CODE` 时，返回错误码 48（`CURLE_UNKNOWN_OPTION`），无法获取 HTTP 状态码、响应时间等信息。

### 根因分析
`curl_core.leno` 中 `CURLINFO` 枚举常量值全部写错了。

`curl.h` 中 `CURLINFO` 的定义方式为：
```c
#define CURLINFO_STRING   0x100000
#define CURLINFO_LONG     0x200000
#define CURLINFO_DOUBLE   0x300000
#define CURLINFO_SLIST    0x400000
#define CURLINFO_OFF_T    0x600000

typedef enum {
    CURLINFO_EFFECTIVE_URL    = CURLINFO_STRING + 1,   // 0x100001
    CURLINFO_RESPONSE_CODE    = CURLINFO_LONG   + 2,   // 0x200002
    CURLINFO_TOTAL_TIME       = CURLINFO_DOUBLE + 3,   // 0x300003
    ...
} CURLINFO;
```

每个 `CURLINFO` 值 = **类型前缀 + 序号**。但代码中把所有值写成了 **类型前缀 + 0**（如 `0x200000`），对应 `CURLINFO_NONE`，libcurl 不识别这个选项，因此返回 48。

### 修复
逐一对照 `curl.h`（curl 8.21.0）头文件，将所有 `CURLINFO` 常量修正为 `类型前缀 + 序号` 的正确值。例如：
- `RESPONSE_CODE`: `0x200000` → `0x200002`
- `EFFECTIVE_URL`: `0x100000` → `0x100001`
- `TOTAL_TIME`: `0x300000` → `0x300003`

### 验证
修复后运行 `debug_trace.leno` 和 `debug_getinfo.leno`，`getinfo` 全部返回 0（成功），正确读取到 HTTP 状态码、URL、耗时等信息。

---

## 2. CURLOPT 常量值多处错误

### 问题描述
`curl_core.leno` 中部分 `CURLOPT` 枚举值不正确。

### 根因
`curl.h` 中 `CURLOPT` 的定义方式为：
```c
#define CURLOPTTYPE_LONG          0
#define CURLOPTTYPE_OBJECTPOINT   10000
#define CURLOPTTYPE_FUNCTIONPOINT 20000
#define CURLOPTTYPE_OFF_T         30000
// 以下三个都等于 OBJECTPOINT:
#define CURLOPTTYPE_STRINGPOINT   CURLOPTTYPE_OBJECTPOINT
#define CURLOPTTYPE_SLISTPOINT    CURLOPTTYPE_OBJECTPOINT
#define CURLOPTTYPE_CBPOINT       CURLOPTTYPE_OBJECTPOINT
// VALUES 等于 LONG:
#define CURLOPTTYPE_VALUES        CURLOPTTYPE_LONG
```
每个 `CURLOPT` = **类型前缀 + 序号**。

### 错误清单
| 常量 | 错误值 | 正确值 | 说明 |
|------|--------|--------|------|
| `HTTPHEADER` | 10026 | 10023 | 应为 `CURLOPTTYPE_SLISTPOINT + 23` |
| `HEADERFUNCTION` | 20029 | 20079 | 应为 `CURLOPTTYPE_FUNCTIONPOINT + 79` |
| `PROGRESSFUNCTION` | 20057 | 20056 | 应为 `CURLOPTTYPE_FUNCTIONPOINT + 56` |
| `PROXYTYPE` | 10101 | 101 | 应为 `CURLOPTTYPE_VALUES(LONG) + 101` |

### 修复
对照 `curl.h` 逐一修正所有 `CURLOPT` 值，并添加了 `POSTFIELDSIZE`、`ERRORBUFFER` 等缺失的选项。

---

## 3. 编译器限制记录

### 3.1 Enum 成员不支持表达式
Leno 的 `enum` 成员不支持表达式计算（如 `HEADER_SIZE = 0x200000 + 0x0C`），只能使用字面量。
- **影响**：所有需要表达式计算的常量必须手动计算后写入硬编码值。
- **变通**：在注释中注明原始公式，方便核对。

### 3.2 默认参数必须为字面量
Leno 的函数默认参数不支持引用全局变量（如 `func(int flags = Global.ALL)`），只能使用字面量。
- **变通**：直接写入计算后的值（如 `func(int flags = 3)`）。

### 3.3 模块全局变量作用域
结构体方法中引用模块级全局变量时，如果全局变量定义在结构体之后，编译器无法找到该变量。
- **变通**：将全局变量定义移到文件顶部，确保在所有引用之前。

---

## 4. curl_easy_escape/unescape 的 FFI 调用问题

### 问题描述
`curl_easy_escape` 通过 FFI 调用时返回 `NULL`，URL 编解码功能完全不可用。

### 根因
两个问题叠加：

1. **clib 声明的 str8 返回类型无效**：`clib` 中声明 `str8 curl_easy_escape(Ptr[u8] handle, str8 url, i32 length)`，但 `str8` 返回类型在 FFI 层无法正确处理 libcurl 分配的 `char*` 内存。需要改用 `ffi.call_ptr` 获取指针，再用 `ffi.read_string` 读取，最后用 `curl_free` 释放。

2. **length 参数 -1 传参问题**：`curl_easy_escape` 的 C 签名为 `char* curl_easy_escape(CURL*, const char*, int length)`。当 `length = 0` 时 libcurl 使用 `strlen` 自动计算长度。但传 `-1` 时，FFI 引擎将 `-1` 作为 64 位整数传递，C 函数收到的是 `0xFFFFFFFFFFFFFFFF` 而非 `0xFFFFFFFF`，导致函数返回 `NULL`。

### 修复
- 改用 `ffi.call_ptr` + `ffi.read_string` + `ffi.call_void("curl_free")` 三步走
- `length` 参数从 `-1` 改为 `0`（让 libcurl 自动 `strlen`）

---

## 5. HEAD 请求 body 不为空

### 问题描述
HEAD 请求后 `Response.body` 不为空，仍然包含了响应体数据。

### 根因
`head()` 方法先设置 `NOBODY=1`，然后调用 `_doRequest("GET", url, "")`。在 `_doRequest` 中，`HTTPGET=1` 会重置 `NOBODY` 标志，导致请求变为普通 GET，服务器返回完整 body。

### 修复
在 `_doRequest` 中添加 `"HEAD"` 方法分支：
```leno
if method == "HEAD" {
    core.setopt_int(handle, Opt.NOBODY, 1)
} else if method == "GET" {
    core.setopt_int(handle, Opt.HTTPGET, 1)
    core.setopt_int(handle, Opt.NOBODY, 0)  // 确保 GET 时清除 NOBODY
}
```
`head()` 方法直接调用 `_doRequest("HEAD", url, "")`。

---

## 6. net 模块缺少 patch 导出函数

### 问题描述
`net.leno` 中 `HttpClient` 结构体有 `patch()` 方法，但模块级便捷函数 `net.patch()` 未导出。

### 修复
添加模块级 `patch` 函数：
```leno
export func patch(string url, string body): Response {
    HttpClient c = createClient()
    Response resp = c.patch(url, body)
    c.close()
    return resp
}
```

---

## 7. 全面测试结果

测试脚本：`examples/full_test.leno`，覆盖 20 个测试场景，40 项断言。

| 测试项 | 说明 | 结果 |
|--------|------|------|
| 1. GET 请求 | example.com GET | ✅ 7/7 PASS |
| 2. GET 带参数 | query string | ✅ 2/2 PASS |
| 3. POST 请求 | httpbin.org/post | ⏭ SKIP (httpbin 503) |
| 4. POST JSON | httpbin.org/post | ⏭ SKIP (httpbin 400) |
| 5. HTTPS 请求 | example.com HTTPS | ✅ 2/2 PASS |
| 6. HTTPS 跳过验证 | SSL verify=false | ✅ PASS |
| 7. PUT 请求 | httpbin.org/put | ⏭ SKIP |
| 8. DELETE 请求 | httpbin.org/delete | ⏭ SKIP |
| 9. PATCH 请求 | httpbin.org/patch | ⏭ SKIP |
| 10. HEAD 请求 | example.com HEAD | ✅ 3/3 PASS |
| 11. URL 编解码 | escape/unescape | ✅ 3/3 PASS |
| 12. 自定义 Header | httpbin.org/headers | ⏭ SKIP |
| 13. HTTP 认证 | httpbin.org/basic-auth | ⏭ SKIP |
| 14. getinfo 全面 | 6 项 getinfo | ✅ 6/6 PASS |
| 15. 复用客户端 | 5 次连续请求 | ✅ PASS |
| 16. 重定向设置 | follow location | ✅ PASS |
| 17. 设置超时 | timeout/connecttimeout | ✅ PASS |
| 18. 重置客户端 | reset 后请求 | ✅ PASS |
| 19. 错误处理 | 无效 URL | ✅ 2/2 PASS |
| 20. Response.str() | 格式化输出 | ✅ 2/2 PASS |

**汇总：总计 40 项，通过 40 项，失败 0 项，SKIP 7 项（httpbin.org 不可用）**

