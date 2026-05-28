# LenoC 网络模块 (sockets)

本文档详细说明 `sockets` 模块提供的 TCP/UDP 网络编程功能。

## 目录

- [使用方式](#使用方式)
- [核心概念](#核心概念)
- [TCP 客户端](#tcp-客户端)
- [TCP 服务器](#tcp-服务器)
- [UDP 通信](#udp-通信)
- [非阻塞 IO](#非阻塞-io)
- [示例代码](#示例代码)
- [与 asyncs 模块配合使用](#与-asyncs-模块配合使用)
- [注意事项](#注意事项)

---

## 使用方式

```leno
import sockets
import io

main() {
    // TCP 客户端示例
    var conn = sockets.connect("www.example.com", 80)
    if conn != null {
        sockets.send(conn, "GET / HTTP/1.0\r\n\r\n")
        var response = sockets.recv(conn, 1024)
        io.print(response)
        sockets.close(conn)
    }
}
```

---

## 核心概念

### Socket 对象

Socket 对象是网络连接的内部表示，通过 `sockets.connect()`、`sockets.listen()` 或 `sockets.udp_bind()` 创建：

```leno
// TCP 客户端 socket
var tcp_conn = sockets.connect("127.0.0.1", 8080)

// TCP 服务器 socket
var server = sockets.listen("0.0.0.0", 8080)

// UDP socket
var udp = sockets.udp_bind("0.0.0.0", 9999)
```

**注意**: Socket 对象需要通过 `sockets.close()` 显式关闭。

---

## TCP 客户端

### `connect(host, port)`

连接到指定的 TCP 服务器。

**参数**:
- `host` (string): 主机名或 IP 地址
- `port` (int): 端口号

**返回**: 
- 成功: Socket 对象
- 失败: `null`

```leno
// 连接到本地服务器
var conn = sockets.connect("127.0.0.1", 8080)

// 连接到远程服务器（支持 DNS 解析）
var conn = sockets.connect("www.example.com", 443)
```

---

### `send(socket, data)`

通过 TCP 连接发送数据。

**参数**:
- `socket`: Socket 对象
- `data` (string): 要发送的数据

**返回**: `bool` - 是否发送成功

```leno
var conn = sockets.connect("127.0.0.1", 8080)
if conn != null {
    var sent = sockets.send(conn, "Hello, Server!")
    io.print("发送成功: " + sent)
}
```

---

### `recv(socket, max_bytes)`

从 TCP 连接接收数据。

**参数**:
- `socket`: Socket 对象
- `max_bytes` (int): 最大接收字节数（最大 65536）

**返回**: 
- 成功: 接收到的字符串数据
- 失败/连接关闭: `null`

```leno
var conn = sockets.connect("127.0.0.1", 8080)
if conn != null {
    sockets.send(conn, "Hello")
    var data = sockets.recv(conn, 1024)
    if data != null {
        io.print("收到: " + data)
    }
    sockets.close(conn)
}
```

---

### `close(socket)`

关闭 Socket 连接。

**参数**:
- `socket`: Socket 对象

**返回**: `null`

```leno
var conn = sockets.connect("127.0.0.1", 8080)
// ... 使用连接 ...
sockets.close(conn)  // 显式关闭
```

---

## TCP 服务器

### `listen(host, port)`

创建 TCP 服务器并监听指定端口。

**参数**:
- `host` (string): 监听地址（"0.0.0.0" 表示所有接口）
- `port` (int): 端口号

**返回**: 
- 成功: 服务器 Socket 对象
- 失败: `null`

```leno
// 创建监听 8080 端口的服务器
var server = sockets.listen("0.0.0.0", 8080)
if server != null {
    io.print("服务器已启动")
}
```

---

### `accept(server_socket)`

接受客户端连接（阻塞模式）。

**参数**:
- `server_socket`: 服务器 Socket 对象

**返回**: 
- 成功: 客户端 Socket 对象
- 失败: `null`

```leno
var server = sockets.listen("0.0.0.0", 8080)
if server != null {
    io.print("等待连接...")
    var client = sockets.accept(server)
    if client != null {
        io.print("客户端已连接")
        var data = sockets.recv(client, 1024)
        sockets.send(client, "Hello, Client!")
        sockets.close(client)
    }
}
```

---

## UDP 通信

### `udp_bind(host, port)`

创建 UDP Socket 并绑定到指定地址。

**参数**:
- `host` (string): 绑定地址
- `port` (int): 端口号

**返回**: 
- 成功: UDP Socket 对象
- 失败: `null`

```leno
// 创建 UDP socket
var udp = sockets.udp_bind("0.0.0.0", 9999)
if udp != null {
    io.print("UDP socket 已创建")
}
```

---

### `sendto(socket, data, addr, port)`

向指定地址发送 UDP 数据包。

**参数**:
- `socket`: UDP Socket 对象
- `data` (string): 要发送的数据
- `addr` (string): 目标 IP 地址
- `port` (int): 目标端口

**返回**: `bool` - 是否发送成功

```leno
var udp = sockets.udp_bind("0.0.0.0", 0)  // 绑定到随机端口
if udp != null {
    var sent = sockets.sendto(udp, "Hello UDP", "127.0.0.1", 9999)
    io.print("发送成功: " + sent)
}
```

---

### `recvfrom(socket, max_bytes)`

接收 UDP 数据包。

**参数**:
- `socket`: UDP Socket 对象
- `max_bytes` (int): 最大接收字节数

**返回**: 
- 成功: 数组 `[data, addr, port]`
- 失败: `null`

```leno
var udp = sockets.udp_bind("0.0.0.0", 9999)
if udp != null {
    var result = sockets.recvfrom(udp, 1024)
    if result != null {
        var data = result[0]   // 接收到的数据
        var addr = result[1]   // 发送方地址
        var port = result[2]   // 发送方端口
        io.print("从 " + addr + ":" + port + " 收到: " + data)
    }
}
```

---

## 非阻塞 IO

从 V2 版本开始，sockets 模块支持非阻塞 IO 操作，可以实现单线程多客户端并发处理。

### `set_nonblocking(socket, nonblocking)`

设置 socket 为非阻塞模式。

**参数**:
- `socket`: Socket 对象
- `nonblocking` (bool): `true` 开启非阻塞模式，`false` 恢复阻塞模式

**返回**: `bool` - 是否设置成功

```leno
var server = sockets.listen("127.0.0.1", 8888)
if server != null {
    // 设置为非阻塞模式
    var result = sockets.set_nonblocking(server, true)
    io.print("非阻塞模式: " + result)
}
```

**说明**:
- 非阻塞模式下，`accept()` 和 `recv()` 会立即返回
- 如果没有数据可读，返回 `null`
- 需要配合 `select()` 使用来检测可读 socket

---

### `select(sockets_array, timeout_ms)`

等待多个 socket 可读，返回可读的 socket 数组。

**参数**:
- `sockets_array` (array): 要监听的 socket 对象数组
- `timeout_ms` (int): 超时时间（毫秒），-1 表示无限等待

**返回**: 数组 - 包含可读的 socket 对象

```leno
var clients = []  // 存储多个客户端 socket

// 等待有数据的 socket，超时 100ms
var ready = sockets.select(clients, 100)

var i = 0
while i < ready.len() {
    var sock = ready[i]
    var data = sockets.recv(sock, 1024)
    if data != null {
        io.print("收到: " + data)
    }
    i = i + 1
}
```

**说明**:
- `select` 可以同时监视多个 socket 的可读状态
- 在非阻塞模式下非常有用，避免频繁轮询
- 返回的数组只包含当前有数据可读的 socket

---

## 示例代码

### 1. HTTP 客户端

```leno
import sockets
import io

// 简单的 HTTP GET 请求
func http_get(string host, string path) -> string {
    var conn = sockets.connect(host, 80)
    if conn == null {
        return ""
    }
    
    // 构造 HTTP 请求
    var request = "GET " + path + " HTTP/1.0\r\n"
    request = request + "Host: " + host + "\r\n"
    request = request + "Connection: close\r\n\r\n"
    
    // 发送请求
    sockets.send(conn, request)
    
    // 接收响应
    var response = ""
    while true {
        var chunk = sockets.recv(conn, 4096)
        if chunk == null {
            break
        }
        response = response + chunk
    }
    
    sockets.close(conn)
    return response
}

main() {
    var html = http_get("www.example.com", "/")
    io.print("响应长度: " + html.len())
    io.print("前100字符:")
    io.print(html.sub_str(0, 100))
}
```

---

### 2. Echo TCP 服务器

```leno
import sockets
import io

// 简单的 Echo 服务器
main() {
    var server = sockets.listen("127.0.0.1", 8080)
    if server == null {
        io.print("启动服务器失败")
        return
    }
    
    io.print("Echo 服务器运行在 127.0.0.1:8080")
    io.print("按 Ctrl+C 停止")
    
    // 只处理一个连接（简化版）
    var client = sockets.accept(server)
    if client != null {
        io.print("客户端已连接")
        
        // 读取数据并回显
        while true {
            var data = sockets.recv(client, 1024)
            if data == null {
                break
            }
            io.print("收到: " + data)
            sockets.send(client, "Echo: " + data)
        }
        
        io.print("客户端已断开")
        sockets.close(client)
    }
    
    sockets.close(server)
}
```

---

### 3. UDP 聊天程序

```leno
import sockets
import io

// 简单的 UDP 聊天
main() {
    var port = 9999
    var peer_port = 9998
    
    // 创建 UDP socket
    var udp = sockets.udp_bind("127.0.0.1", port)
    if udp == null {
        io.print("绑定失败")
        return
    }
    
    io.print("UDP 聊天已启动 (端口 " + port + ")")
    io.print("输入消息，对方在端口 " + peer_port)
    
    // 发送消息
    var msg = "Hello!"
    sockets.sendto(udp, msg, "127.0.0.1", peer_port)
    
    // 接收消息
    var result = sockets.recvfrom(udp, 1024)
    if result != null {
        io.print("对方说: " + result[0])
    }
    
    sockets.close(udp)
}
```

---

### 4. 网络爬虫（基础版）

```leno
import sockets
import io

// 下载网页内容
func fetch_url(string url) -> string {
    // 简单解析 URL（假设格式: host/path）
    var parts = url.split("/")
    var host = parts[0]
    var path = "/"
    
    if parts.len() > 1 {
        path = "/" + parts[1]
    }
    
    // 连接并获取
    var conn = sockets.connect(host, 80)
    if conn == null {
        return ""
    }
    
    var request = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\n\r\n"
    sockets.send(conn, request)
    
    var response = ""
    while true {
        var chunk = sockets.recv(conn, 4096)
        if chunk == null {
            break
        }
        response = response + chunk
    }
    
    sockets.close(conn)
    return response
}

main() {
    var content = fetch_url("www.baidu.com/")
    io.print("获取到 " + content.len() + " 字节")
    
    // 提取标题（简化版）
    var title_start = content.find("<title>")
    var title_end = content.find("</title>")
    if title_start != null && title_end != null {
        var title = content.slice(title_start + 6, title_end)
        io.print("页面标题: " + title)
    }
}
```

---

### 5. 非阻塞多客户端聊天服务器

```leno
import sockets
import io

// V2: 非阻塞多客户端聊天服务器
var clients = []
var client_names = []
var client_counter = 0

// 广播消息给所有客户端（排除发送者）
func broadcast(string msg, var exclude) {
    var i = 0
    while i < clients.len() {
        if clients[i] != exclude {
            sockets.send(clients[i], msg)
        }
        i = i + 1
    }
}

main() {
    var port = 8888
    
    io.print("=== 非阻塞聊天服务器 ===")
    
    var server = sockets.listen("127.0.0.1", port)
    if server == null {
        io.print("启动失败")
        return
    }
    
    // 设置为非阻塞模式
    sockets.set_nonblocking(server, true)
    io.print("服务器运行在 127.0.0.1:" + port)
    io.print("")
    
    while true {
        // 接受新连接
        var client = sockets.accept(server)
        if client != null {
            client_counter = client_counter + 1
            var name = "User" + client_counter
            
            io.print("新客户端: " + name)
            
            // 设置客户端为非阻塞模式
            sockets.set_nonblocking(client, true)
            sockets.send(client, "Welcome " + name + "!\n")
            
            // 添加到客户端列表
            clients.add(client)
            client_names.add(name)
            
            // 广播新用户加入
            broadcast("[系统] " + name + " 加入\n", client)
            io.print("当前客户端: " + clients.len())
        }
        
        // 处理现有客户端的消息
        if clients.len() > 0 {
            // 等待有消息的客户端
            var ready = sockets.select(clients, 100)
            
            var idx = 0
            while idx < ready.len() {
                var sock = ready[idx]
                
                // 找到对应的客户端索引
                var client_idx = 0
                while client_idx < clients.len() {
                    if clients[client_idx] == sock {
                        break
                    }
                    client_idx = client_idx + 1
                }
                
                // 接收消息
                var data = sockets.recv(sock, 1024)
                
                if data == null {
                    // 客户端断开
                    io.print("[" + client_names[client_idx] + "] 断开")
                    sockets.close(sock)
                    
                    // 从列表中移除
                    var new_clients = []
                    var new_names = []
                    var j = 0
                    while j < clients.len() {
                        if j != client_idx {
                            new_clients.add(clients[j])
                            new_names.add(client_names[j])
                        }
                        j = j + 1
                    }
                    clients = new_clients
                    client_names = new_names
                } else {
                    // 处理消息
                    var msg = data.trim()
                    if msg.len() > 0 {
                        var n = client_names[client_idx]
                        io.print("[" + n + "]: " + msg)
                        
                        // 广播给其他客户端
                        broadcast("[" + n + "]: " + msg + "\n", sock)
                    }
                }
                
                idx = idx + 1
            }
        }
    }
    
    sockets.close(server)
}
```

**说明**:
- 使用 `set_nonblocking()` 开启非阻塞模式
- 使用 `select()` 同时监视多个客户端
- 单线程即可处理多个并发连接

---

## 与 asyncs 模块配合使用

sockets 模块可以与 asyncs 异步模块配合使用，实现并发网络操作。

### 并发 HTTP 请求示例

```leno
import sockets
import asyncs
import io

// 异步 HTTP 请求
async func fetch_http(string host, string path):string {
    var conn = sockets.connect(host, 80)
    if conn == null {
        return ""
    }
    
    var request = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\n\r\n"
    sockets.send(conn, request)
    
    // 让出执行权，允许其他协程运行
    await asyncs.yield()
    
    var response = ""
    while true {
        var chunk = sockets.recv(conn, 4096)
        if chunk == null {
            break
        }
        response = response + chunk
        await asyncs.yield()  // 每次接收后让出
    }
    
    sockets.close(conn)
    return response
}

// 并发请求多个网站
async func fetch_multiple() {
    // 同时启动三个请求
    var f1 = fetch_http("www.example.com", "/")
    var f2 = fetch_http("www.baidu.com", "/")
    var f3 = fetch_http("www.taobao.com", "/")
    
    // 等待所有请求完成
    await f1
    await f2
    await f3
    
    // 收集结果
    var results = asyncs.all([f1, f2, f3])
    
    io.print("example.com: " + results[0].len() + " 字节")
    io.print("baidu.com: " + results[1].len() + " 字节")
    io.print("taobao.com: " + results[2].len() + " 字节")
}

main() {
    fetch_multiple()
    asyncs.run()  // 启动事件循环
}
```

### 带超时的网络请求

```leno
import sockets
import asyncs
import io

async func fetch_with_timeout(string host, int timeout_ms) {
    var task = fetch_http(host, "/")
    var result = await asyncs.timeout(task, timeout_ms)
    
    if result != null {
        io.print("请求成功，收到 " + result.len() + " 字节")
    } else {
        io.print("请求超时!")
    }
}
```

### 协程间协作的服务器

```leno
import sockets
import asyncs
import io

// 异步处理客户端连接
async func handle_client(var client, int id) {
    var data = sockets.recv(client, 1024)
    if data != null {
        sockets.send(client, "Echo: " + data)
    }
    sockets.close(client)
    io.print("客户端 " + id + " 已处理")
}

// 启动服务器
async func start_server(int port) {
    var server = sockets.listen("127.0.0.1", port)
    if server == null {
        return
    }
    
    var client_id = 0
    while true {
        var client = sockets.accept(server)
        if client != null {
            client_id = client_id + 1
            // 异步处理每个客户端
            handle_client(client, client_id)
            await asyncs.yield()
        }
    }
}

main() {
    start_server(8080)
    asyncs.run()
}
```

---

## 注意事项

1. **阻塞操作**
   - `connect()` 是阻塞的，直到连接建立或失败
   - 默认情况下 `accept()`、`recv()` 和 `recvfrom()` 是阻塞的
   - 使用 `set_nonblocking()` 可以设置为非阻塞模式

2. **资源管理**
   - 每个创建的 socket 都应该通过 `close()` 关闭
   - 服务器 socket 和客户端 socket 需要分别关闭

3. **错误处理**
   - 所有操作都可能失败，应该检查返回值
   - `connect()` 返回 `null` 表示连接失败
   - `recv()` 返回 `null` 表示连接已关闭或出错（非阻塞模式下也表示无数据）

4. **缓冲区限制**
   - `recv()` 和 `recvfrom()` 的 `max_bytes` 最大为 65536
   - 超过限制的数据会被截断

5. **字符编码**
   - 所有数据以字符串形式传输
   - 二进制数据需要自行编码/解码

6. **DNS 解析**
   - `connect()` 支持域名和 IP 地址
   - 域名解析失败会返回 `null`

7. **端口绑定**
   - `listen()` 和 `udp_bind()` 需要指定端口
   - 端口 0 表示让系统自动分配

8. **地址格式**
   - IPv4 地址格式: "127.0.0.1"
   - "0.0.0.0" 表示监听所有网络接口
   - "127.0.0.1" 表示仅本地访问

9. **非阻塞模式注意事项**
   - 设置非阻塞后，`recv()` 会立即返回，无数据时返回 `null`
   - `accept()` 会立即返回，无连接时返回 `null`
   - 需要配合 `select()` 或循环轮询使用
   - 单线程可以实现多客户端并发，但客户端 `input()` 仍会阻塞

---

## 完整 API 速查表

| 函数 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `connect(host, port)` | string, int | Socket\|null | TCP 连接 |
| `send(sock, data)` | Socket, string | bool | TCP 发送 |
| `recv(sock, max)` | Socket, int | string\|null | TCP 接收 |
| `close(sock)` | Socket | null | 关闭连接 |
| `listen(host, port)` | string, int | Socket\|null | TCP 监听 |
| `accept(server)` | Socket | Socket\|null | 接受连接 |
| `udp_bind(host, port)` | string, int | Socket\|null | UDP 绑定 |
| `sendto(sock, data, addr, port)` | Socket, string, string, int | bool | UDP 发送 |
| `recvfrom(sock, max)` | Socket, int | array\|null | UDP 接收 |
| `set_nonblocking(sock, flag)` | Socket, bool | bool | 设置非阻塞模式 |
| `select(socks, timeout_ms)` | array, int | array | 等待多个 socket 可读 |
| `htons(host_short)` | int | int | 主机字节序转网络字节序 (16位) |
| `htonl(host_long)` | int | int | 主机字节序转网络字节序 (32位) |
| `ntohs(net_short)` | int | int | 网络字节序转主机字节序 (16位) |
| `ntohl(net_long)` | int | int | 网络字节序转主机字节序 (32位) |

---

## 字节序转换

sockets 模块提供字节序转换函数，用于处理网络协议中的多字节数据。

### 为什么需要字节序转换？

不同计算机架构使用不同的字节序存储多字节数据：
- **小端序 (Little-Endian)**: 低位字节存储在低地址（x86/x64 架构）
- **大端序 (Big-Endian)**: 高位字节存储在低地址（网络协议标准）

网络协议统一使用大端序（网络字节序），因此需要进行转换。

### `htons(host_short)`

将 16 位整数从主机字节序转换为网络字节序。

**参数**:
- `host_short` (int): 主机字节序的 16 位整数

**返回**: 网络字节序的 16 位整数

```leno
var port = 8080
var port_be = sockets.htons(port)
io.print("端口 " + port + " 的网络字节序: " + port_be)  // 36895
```

---

### `htonl(host_long)`

将 32 位整数从主机字节序转换为网络字节序。

**参数**:
- `host_long` (int): 主机字节序的 32 位整数

**返回**: 网络字节序的 32 位整数

```leno
var addr = 0x7F000001  // 127.0.0.1
var addr_be = sockets.htonl(addr)
io.print("地址的网络字节序: 0x" + addr_be)  // 0x16777343
```

---

### `ntohs(net_short)`

将 16 位整数从网络字节序转换为主机字节序。

**参数**:
- `net_short` (int): 网络字节序的 16 位整数

**返回**: 主机字节序的 16 位整数

```leno
var port_be = 36895  // 网络字节序
var port = sockets.ntohs(port_be)
io.print("主机字节序端口: " + port)  // 8080
```

---

### `ntohl(net_long)`

将 32 位整数从网络字节序转换为主机字节序。

**参数**:
- `net_long` (int): 网络字节序的 32 位整数

**返回**: 主机字节序的 32 位整数

```leno
var addr_be = 0x16777343  // 网络字节序
var addr = sockets.ntohl(addr_be)
io.print("主机字节序地址: 0x" + addr)  // 0x7F000001 (127.0.0.1)
```

---

### 使用场景

#### 1. 构造自定义网络协议

```leno
import sockets
import io

// 构造一个简单的协议头
func create_header(int cmd, int len) -> string {
    // 使用 cstruct 定义协议头
    cstruct ProtocolHeader {
        u16 magic      // 魔数
        u16 command    // 命令码
        u32 length     // 数据长度
    }
    
    var header = ProtocolHeader.malloc()
    header.magic = sockets.htons(0x1234)    // 网络字节序
    header.command = sockets.htons(cmd)      // 网络字节序
    header.length = sockets.htonl(len)       // 网络字节序
    
    // 获取字节数据
    var ptr = header.to_ptr()
    // ... 发送数据 ...
    
    header.free()
    return "header_bytes"
}
```

#### 2. 解析二进制协议数据

```leno
// 解析收到的协议头
func parse_header(Ptr data) -> map {
    // 读取网络字节序的数据
    var magic_be = ffi.read_int(data, 0)      // 假设使用 ffi
    var cmd_be = ffi.read_int(data, 2)
    var len_be = ffi.read_int(data, 4)
    
    // 转换为主机字节序
    return {
        magic: sockets.ntohs(magic_be),
        command: sockets.ntohs(cmd_be),
        length: sockets.ntohl(len_be)
    }
}
```

#### 3. 处理原始 IP 地址和端口

```leno
// 将 IP 地址和端口打包为网络字节序
func pack_addr(string ip, int port) -> map {
    var parts = ip.split(".")
    var addr = 0
    addr = addr + parts[0].to_int() * 256 * 256 * 256
    addr = addr + parts[1].to_int() * 256 * 256
    addr = addr + parts[2].to_int() * 256
    addr = addr + parts[3].to_int()
    
    return {
        addr: sockets.htonl(addr),    // 网络字节序 IP
        port: sockets.htons(port)     // 网络字节序端口
    }
}
```

---

## 注意事项

1. **字节序转换只影响多字节数据**
   - 8 位数据（单字节）不需要转换
   - 16 位数据（short）使用 `htons`/`ntohs`
   - 32 位数据（int/long）使用 `htonl`/`ntohl`

2. **往返转换**
   - `ntohs(htons(x)) == x`
   - `ntohl(htonl(x)) == x`

3. **与 cstruct 配合使用**
   - cstruct 字段可以直接赋值网络字节序值
   - 适合构造和解析二进制协议

---

*文档版本: 2.1*  
*最后更新: 2026-05-16*
