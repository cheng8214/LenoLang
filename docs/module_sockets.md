# LenoC 网络模块 (sockets)

本文档详细说明 `sockets` 模块提供的 TCP/UDP 网络编程功能。

## 目录

- [使用方式](#使用方式)
- [核心概念](#核心概念)
- [模块方法（工厂方法）](#模块方法工厂方法)
- [实例方法](#实例方法)
- [TCP 客户端](#tcp-客户端)
- [TCP 服务器](#tcp-服务器)
- [UDP 通信](#udp-通信)
- [非阻塞 IO](#非阻塞-io)
- [异步 IO (arecv / aaccept)](#异步-io-arecv--aaccept)
- [连接信息与错误处理](#连接信息与错误处理)
- [示例代码](#示例代码)
- [与 asyncs 模块配合使用](#与-asyncs-模块配合使用)
- [注意事项](#注意事项)
- [完整 API 速查表](#完整-api-速查表)
- [字节序转换](#字节序转换)

---

## 使用方式

```leno
import sockets
import io

main() {
    // TCP 客户端示例 - 面向对象风格
    Socket conn = sockets.connect("www.example.com", 80)
    if conn != null {
        conn.send("GET / HTTP/1.0\r\n\r\n")
        var response = conn.recv(1024)
        io.print(response)
        conn.close()
    }
}
```

---

## 核心概念

### Socket 对象

Socket 对象是网络连接的封装，通过模块工厂方法创建，通过实例方法操作：

```leno
// TCP 客户端 socket
Socket conn = sockets.connect("127.0.0.1", 8080)

// TCP 服务器 socket
Socket server = sockets.listen("0.0.0.0", 8080)

// UDP socket
Socket udp = sockets.udp_bind("0.0.0.0", 9999)
```

**设计模式**: `sockets` 模块采用**工厂方法 + 实例方法**的面向对象设计：
- **模块方法**（`sockets.xxx`）：创建 Socket 对象或执行模块级操作
- **实例方法**（`sock.xxx`）：操作具体的 Socket 连接

**注意**: Socket 对象需要通过 `sock.close()` 显式关闭。

---

## 模块方法（工厂方法）

模块方法通过 `sockets.xxx()` 调用，用于创建 Socket 对象或执行模块级操作。

### `sockets.connect(host, port)`

连接到指定的 TCP 服务器，返回 Socket 对象。

**参数**:
- `host` (string): 主机名或 IP 地址
- `port` (int): 端口号

**返回**: 
- 成功: Socket 对象
- 失败: `null`

```leno
Socket conn = sockets.connect("127.0.0.1", 8080)
Socket conn = sockets.connect("www.example.com", 443)  // 支持 DNS 解析
```

---

### `sockets.listen(host, port)`

创建 TCP 服务器并监听指定端口。

**参数**:
- `host` (string): 监听地址（"0.0.0.0" 表示所有接口）
- `port` (int): 端口号

**返回**: 
- 成功: 服务器 Socket 对象
- 失败: `null`

```leno
Socket server = sockets.listen("0.0.0.0", 8080)
if server != null {
    io.print("服务器已启动")
}
```

---

### `sockets.udp_bind(host, port)`

创建 UDP Socket 并绑定到指定地址。

**参数**:
- `host` (string): 绑定地址
- `port` (int): 端口号

**返回**: 
- 成功: UDP Socket 对象
- 失败: `null`

```leno
Socket udp = sockets.udp_bind("0.0.0.0", 9999)
```

---

### `sockets.select(sockets_array, timeout_ms)`

等待多个 socket 可读，返回可读的 socket 数组（模块级静态方法）。

**参数**:
- `sockets_array` (array): 要监听的 socket 对象数组
- `timeout_ms` (int): 超时时间（毫秒），-1 表示无限等待

**返回**: 数组 - 包含可读的 socket 对象

```leno
var ready = sockets.select(clients, 100)
var i = 0
while i < ready.len() {
    var data = ready[i].recv(1024)
    if data != null {
        io.print("收到: " + data)
    }
    i = i + 1
}
```

---

### `sockets.resolve(host)`

DNS 解析，将域名转换为 IP 地址。

**参数**:
- `host` (string): 域名

**返回**: 
- 成功: IP 地址字符串
- 失败: `null`

```leno
var ip = sockets.resolve("www.baidu.com")
io.print("baidu.com -> " + ip)  // 例如: 39.156.70.239
```

---

### 字节序转换

```leno
var port_be = sockets.htons(8080)   // 主机序 -> 网络序 (16位)
var addr_be = sockets.htonl(0x7f000001)  // 主机序 -> 网络序 (32位)
var port_h  = sockets.ntohs(port_be)     // 网络序 -> 主机序 (16位)
var addr_h  = sockets.ntohl(addr_be)     // 网络序 -> 主机序 (32位)
```

---

## 实例方法

实例方法通过 `sock.xxx()` 调用，操作具体的 Socket 连接。

### `sock.send(data)`

通过 TCP 连接发送数据。

**参数**:
- `data` (string): 要发送的数据

**返回**: `bool` - 是否发送成功

```leno
Socket conn = sockets.connect("127.0.0.1", 8080)
if conn != null {
    var sent = conn.send("Hello, Server!")
    io.print("发送成功: " + sent)
}
```

---

### `sock.recv(max_bytes)`

从 TCP 连接接收数据。

**参数**:
- `max_bytes` (int): 最大接收字节数（最大 65536）

**返回**: 
- 成功: 接收到的字符串数据
- 失败/连接关闭: `null`

```leno
var data = conn.recv(1024)
if data != null {
    io.print("收到: " + data)
}
```

---

### `sock.close()`

关闭 Socket 连接。

**返回**: `null`

```leno
conn.close()
```

---

### `sock.accept()`

接受客户端连接（阻塞模式，仅用于服务器 Socket）。

**返回**: 
- 成功: 客户端 Socket 对象
- 失败: `null`

```leno
Socket server = sockets.listen("0.0.0.0", 8080)
Socket client = server.accept()
if client != null {
    io.print("客户端来自: " + client.peer_addr() + ":" + client.peer_port())
}
```

---

### `sock.sendto(data, addr, port)`

向指定地址发送 UDP 数据包。

**参数**:
- `data` (string): 要发送的数据
- `addr` (string): 目标 IP 地址
- `port` (int): 目标端口

**返回**: `bool` - 是否发送成功

```leno
Socket udp = sockets.udp_bind("0.0.0.0", 0)
udp.sendto("Hello UDP", "127.0.0.1", 9999)
```

---

### `sock.recvfrom(max_bytes)`

接收 UDP 数据包。

**参数**:
- `max_bytes` (int): 最大接收字节数

**返回**: 
- 成功: 数组 `[data, addr, port]`
- 失败: `null`

```leno
var result = udp.recvfrom(1024)
if result != null {
    io.print("从 " + result[1] + ":" + result[2] + " 收到: " + result[0])
}
```

---

### `sock.set_nonblocking(flag)`

设置 socket 为非阻塞模式。

**参数**:
- `flag` (bool): `true` 开启非阻塞模式，`false` 恢复阻塞模式

**返回**: `bool` - 是否设置成功

```leno
server.set_nonblocking(true)
```

---

### `sock.peer_addr()`

获取对端（远程）IP 地址。

**返回**: 
- 成功: IP 地址字符串
- 失败: `null`

```leno
Socket client = server.accept()
io.print("客户端IP: " + client.peer_addr())  // 例如: "192.168.1.100"
```

---

### `sock.peer_port()`

获取对端（远程）端口号。

**返回**: 
- 成功: 端口号 (int)
- 失败: `null`

```leno
io.print("客户端端口: " + client.peer_port())  // 例如: 52341
```

---

### `sock.local_addr()`

获取本地 IP 地址。

**返回**: 
- 成功: IP 地址字符串
- 失败: `null`

```leno
io.print("本地IP: " + conn.local_addr())  // 例如: "192.168.10.13"
```

---

### `sock.local_port()`

获取本地端口号。

**返回**: 
- 成功: 端口号 (int)
- 失败: `null`

```leno
io.print("本地端口: " + conn.local_port())  // 例如: 13169
```

---

### `sock.error()`

获取最后一次操作的错误码。

**返回**: `int` - 错误码（0 表示无错误）

```leno
var data = conn.recv(1024)
if data == null {
    io.print("接收失败，错误码: " + conn.error())
}
```

**说明**: 每次 `send`、`recv`、`sendto`、`recvfrom`、`accept` 等操作失败时会自动记录错误码。

---

### `sock.shutdown(how)`

优雅关闭连接（不释放文件描述符）。

**参数**:
- `how` (int): 关闭方式
  - `0` - 关闭读取端（SHUT_RD）
  - `1` - 关闭写入端（SHUT_WR）
  - `2` - 关闭两端（SHUT_RDWR）

**返回**: `bool` - 是否成功

```leno
// 关闭写入端，发送 EOF 给对端，但仍可接收
conn.shutdown(1)
var last = conn.recv(1024)  // 仍可读取对端数据
conn.close()
```

---

### `sock.arecv(max_bytes)` (异步方法)

异步接收数据。必须在 `async` 函数中使用 `await` 获取结果。

**参数**:
- `max_bytes` (int): 最大接收字节数（最大 65536）

**返回**: `Future` - 在 async 函数中 `await sock.arecv(1024)` 的结果为 `string|null`

```leno
async func receive_data(Socket conn) {
    // 异步等待数据，释放事件循环给其他协程
    var data = await conn.arecv(1024)
    if data != null {
        io.print("收到: " + data)
    } else {
        io.print("连接已关闭")
    }
}
```

**与 `recv()` 的区别**:
- `recv()` - 阻塞等待，会挂起整个事件循环直到数据到达
- `arecv()` - 异步等待，让出执行权，其他协程可以继续运行

---

### `sock.aaccept()` (异步方法)

异步接受客户端连接。必须在 `async` 函数中使用 `await` 获取结果。

**返回**: `Future` - 在 async 函数中 `await server.aaccept()` 的结果为 `Socket|null`

```leno
async func echo_server(int port) {
    Socket server = sockets.listen("127.0.0.1", port)
    if server == null { return }

    while true {
        // 异步等待客户端连接，释放事件循环
        var client = await server.aaccept()
        if client == null { break }

        // 为每个客户端派生独立协程，不阻塞 accept 循环
        handle_client(client)
    }
}
```

**与 `accept()` 的区别**:
- `accept()` - 阻塞等待客户端连接
- `aaccept()` - 异步等待，让出执行权，可以同时处理多个协程

---

### `sock.set_timeout(ms)`

设置 socket 收发超时时间。

**参数**:
- `ms` (int): 超时时间（毫秒），0 表示无超时

**返回**: `bool` - 是否设置成功

```leno
Socket conn = sockets.connect("www.example.com", 80)
conn.set_timeout(5000)  // 设置5秒超时
var data = conn.recv(1024)  // 5秒内无数据将返回 null
```

**说明**: 同时设置接收超时（SO_RCVTIMEO）和发送超时（SO_SNDTIMEO）。

---

## TCP 客户端

完整的 TCP 客户端流程：

```leno
import sockets
import io

main() {
    // 1. 连接服务器
    Socket conn = sockets.connect("www.example.com", 80)
    if conn == null {
        io.print("连接失败")
        return
    }
    
    io.print("已连接到 " + conn.peer_addr() + ":" + conn.peer_port())
    io.print("本地地址: " + conn.local_addr() + ":" + conn.local_port())
    
    // 2. 设置超时
    conn.set_timeout(5000)
    
    // 3. 发送请求
    var request = "GET / HTTP/1.0\r\nHost: www.example.com\r\n\r\n"
    if !conn.send(request) {
        io.print("发送失败，错误码: " + conn.error())
        conn.close()
        return
    }
    
    // 4. 接收响应
    var response = ""
    while true {
        var chunk = conn.recv(4096)
        if chunk == null {
            break
        }
        response = response + chunk
    }
    
    io.print("响应长度: " + response.len())
    
    // 5. 优雅关闭
    conn.shutdown(2)
    conn.close()
}
```

---

## TCP 服务器

完整的 TCP 服务器流程：

```leno
import sockets
import io

main() {
    // 1. 创建服务器
    Socket server = sockets.listen("0.0.0.0", 8080)
    if server == null {
        io.print("启动服务器失败")
        return
    }
    
    io.print("服务器运行在 " + server.local_addr() + ":" + server.local_port())
    
    // 2. 接受连接
    Socket client = server.accept()
    if client != null {
        io.print("客户端来自: " + client.peer_addr() + ":" + client.peer_port())
        
        // 3. 通信
        var data = client.recv(1024)
        if data != null {
            client.send("Echo: " + data)
        } else {
            io.print("接收失败，错误码: " + client.error())
        }
        
        // 4. 关闭客户端
        client.close()
    }
    
    // 5. 关闭服务器
    server.close()
}
```

---

## UDP 通信

```leno
import sockets
import io

main() {
    // 创建 UDP socket
    Socket udp = sockets.udp_bind("0.0.0.0", 9999)
    if udp == null {
        io.print("绑定失败")
        return
    }
    
    io.print("UDP 本地端口: " + udp.local_port())
    
    // 发送
    udp.sendto("Hello!", "127.0.0.1", 9998)
    
    // 接收
    var result = udp.recvfrom(1024)
    if result != null {
        io.print("从 " + result[1] + ":" + result[2] + " 收到: " + result[0])
    }
    
    udp.close()
}
```

---

## 非阻塞 IO

### 基本用法

```leno
import sockets
import io

main() {
    Socket server = sockets.listen("127.0.0.1", 8888)
    server.set_nonblocking(true)
    
    var clients = []
    
    while true {
        // 非阻塞 accept
        Socket client = server.accept()
        if client != null {
            client.set_nonblocking(true)
            clients.add(client)
            io.print("新客户端: " + client.peer_addr())
        }
        
        // 使用 select 检测可读的 socket
        if clients.len() > 0 {
            var ready = sockets.select(clients, 100)
            
            var i = 0
            while i < ready.len() {
                var data = ready[i].recv(1024)
                if data == null {
                    io.print("客户端断开，错误码: " + ready[i].error())
                    ready[i].close()
                    // 从列表移除...
                } else {
                    io.print("收到: " + data)
                }
                i = i + 1
            }
        }
    }
}
```

---

## 异步 IO (arecv / aaccept)

`sockets` 模块提供了基于事件循环的异步 IO 方法，配合 `async`/`await` 可以在单个协程中实现非阻塞的网络通信，避免阻塞整个事件循环。

### 基本模式

异步 IO 需要结合 `asyncs` 模块的事件循环使用：

```leno
import sockets
import asyncs
import io

async func handle_client(Socket client, int id) {
    io.print($"[#{id}] 客户端已连接")

    while true {
        // 异步等待数据，让出执行权给其他协程
        var data = await client.arecv(1024)
        if data == null or data == "" {
            io.print($"[#{id}] 客户端断开")
            client.close()
            return
        }

        io.print($"[#{id}] 收到: {data}")
        client.send("Echo: " + data)

        if data == "DISCONNECT" {
            client.close()
            return
        }
    }
}

async func echo_server(int port) {
    var server = sockets.listen("127.0.0.1", port)
    if server == null { return }

    var client_id = 0
    while true {
        // 异步等待客户端连接
        var client = await server.aaccept()
        if client == null { break }

        client_id = client_id + 1
        // 派生独立协程处理，不阻塞 accept 循环
        handle_client(client, client_id)
    }
    server.close()
}

async func client(int id) {
    await asyncs.sleep(50)  // 等服务器就绪
    var conn = sockets.connect("127.0.0.1", 23456)
    if conn is Socket {
        conn.send($"Hello from client {id}")
        var reply = await conn.arecv(1024)
        io.print($"[Client#{id}] 收到: {reply}")
        conn.send("DISCONNECT")
        await asyncs.sleep(50)
        conn.close()
    }
}

main() {
    echo_server(23456)
    client(1)
    client(2)
    client(3)
    asyncs.run()
}
```

### 与阻塞 IO 的对比

| 特性 | 阻塞 IO (`recv`/`accept`) | 异步 IO (`arecv`/`aaccept`) |
|------|---------------------------|----------------------------|
| 执行方式 | 阻塞直到完成 | 让出执行权，事件循环驱动 |
| 并发能力 | 需要非阻塞+select/poll | 天然支持，每个连接独立协程 |
| 代码复杂度 | 需要手动管理事件循环 | 类似同步代码，易读易写 |
| 适用场景 | 简单脚本、单连接 | 多连接服务器、高并发场景 |

### 大数据分片接收

当数据超过单次 `arecv` 的缓冲区大小时，需要循环接收：

```leno
async func recv_all(Socket conn, int total_bytes):string {
    var received = ""
    while received.len() < total_bytes {
        var chunk = await conn.arecv(1024)
        if chunk == null or chunk == "" { break }
        received = received + chunk
    }
    return received
}
```

### 超时处理

结合 `asyncs.timeout()` 实现超时控制：

```leno
async func recv_with_timeout(Socket conn, int ms) {
    var future = conn.arecv(1024)
    var data = await asyncs.timeout(future, ms)
    if data == null {
        io.print("接收超时")
        return ""
    }
    return data
}
```

---

## 连接信息与错误处理

### 获取连接信息

```leno
Socket conn = sockets.connect("www.baidu.com", 80)
if conn != null {
    // 对端信息（服务器地址）
    io.print("对端: " + conn.peer_addr() + ":" + conn.peer_port())
    
    // 本地信息（客户端地址）
    io.print("本地: " + conn.local_addr() + ":" + conn.local_port())
    
    conn.close()
}
```

### 错误处理模式

```leno
Socket conn = sockets.connect("www.example.com", 80)
if conn == null {
    io.print("连接失败")
    return
}

conn.set_timeout(3000)  // 3秒超时

var data = conn.recv(1024)
if data == null {
    var err = conn.error()
    if err != 0 {
        io.print("接收出错，错误码: " + err)
    } else {
        io.print("连接已正常关闭")
    }
}

conn.close()
```

### 优雅关闭

```leno
// 半关闭模式：关闭写入端，通知对端数据已发完，但仍可接收剩余数据
conn.shutdown(1)   // SHUT_WR

// 继续接收对端数据
while true {
    var data = conn.recv(1024)
    if data == null {
        break
    }
    process(data)
}

// 完全关闭
conn.close()
```

---

## 示例代码

### 1. HTTP 客户端

```leno
import sockets
import io

func http_get(string host, string path) -> string {
    Socket conn = sockets.connect(host, 80)
    if conn == null {
        return ""
    }
    
    conn.set_timeout(5000)
    
    var request = "GET " + path + " HTTP/1.0\r\n"
    request = request + "Host: " + host + "\r\n"
    request = request + "Connection: close\r\n\r\n"
    
    if !conn.send(request) {
        io.print("发送失败: " + conn.error())
        conn.close()
        return ""
    }
    
    var response = ""
    while true {
        var chunk = conn.recv(4096)
        if chunk == null {
            break
        }
        response = response + chunk
    }
    
    conn.close()
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

main() {
    Socket server = sockets.listen("127.0.0.1", 8080)
    if server == null {
        io.print("启动服务器失败")
        return
    }
    
    io.print("Echo 服务器运行在 127.0.0.1:8080")
    
    Socket client = server.accept()
    if client != null {
        io.print("客户端来自: " + client.peer_addr() + ":" + client.peer_port())
        
        while true {
            var data = client.recv(1024)
            if data == null {
                break
            }
            client.send("Echo: " + data)
        }
        
        io.print("客户端已断开")
        client.close()
    }
    
    server.close()
}
```

---

### 3. UDP 聊天程序

```leno
import sockets
import io

main() {
    var port = 9999
    var peer_port = 9998
    
    Socket udp = sockets.udp_bind("127.0.0.1", port)
    if udp == null {
        io.print("绑定失败")
        return
    }
    
    io.print("UDP 聊天已启动 (端口 " + udp.local_port() + ")")
    
    udp.sendto("Hello!", "127.0.0.1", peer_port)
    
    var result = udp.recvfrom(1024)
    if result != null {
        io.print("对方说: " + result[0])
    }
    
    udp.close()
}
```

---

### 4. 非阻塞多客户端聊天服务器

```leno
import sockets
import io

var clients = []
var client_names = []
var client_counter = 0

func broadcast(string msg, var exclude) {
    var i = 0
    while i < clients.len() {
        if clients[i] != exclude {
            clients[i].send(msg)
        }
        i = i + 1
    }
}

main() {
    var port = 8888
    
    io.print("=== 非阻塞聊天服务器 ===")
    
    Socket server = sockets.listen("127.0.0.1", port)
    if server == null {
        io.print("启动失败")
        return
    }
    
    server.set_nonblocking(true)
    io.print("服务器运行在 127.0.0.1:" + port)
    
    while true {
        // 接受新连接
        Socket client = server.accept()
        if client != null {
            client_counter = client_counter + 1
            var name = "User" + client_counter
            
            io.print("新客户端: " + name + " (" + client.peer_addr() + ")")
            
            client.set_nonblocking(true)
            client.send("Welcome " + name + "!\n")
            
            clients.add(client)
            client_names.add(name)
            
            broadcast("[系统] " + name + " 加入\n", client)
        }
        
        // 处理现有客户端
        if clients.len() > 0 {
            var ready = sockets.select(clients, 100)
            
            var idx = 0
            while idx < ready.len() {
                var sock = ready[idx]
                
                var client_idx = 0
                while client_idx < clients.len() {
                    if clients[client_idx] == sock {
                        break
                    }
                    client_idx = client_idx + 1
                }
                
                var data = sock.recv(1024)
                
                if data == null {
                    io.print("[" + client_names[client_idx] + "] 断开 (错误码: " + sock.error() + ")")
                    sock.close()
                    
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
                    var msg = data.trim()
                    if msg.len() > 0 {
                        var n = client_names[client_idx]
                        io.print("[" + n + "]: " + msg)
                        broadcast("[" + n + "]: " + msg + "\n", sock)
                    }
                }
                
                idx = idx + 1
            }
        }
    }
}
```

---

### 5. DNS 解析

```leno
import sockets
import io

main() {
    var ip = sockets.resolve("www.baidu.com")
    if ip != null {
        io.print("baidu.com -> " + ip)
    } else {
        io.print("DNS 解析失败")
    }
}
```

---

## 与 asyncs 模块配合使用

### 并发 HTTP 请求

```leno
import sockets
import asyncs
import io

async func fetch_http(string host, string path):string {
    Socket conn = sockets.connect(host, 80)
    if conn == null {
        return ""
    }
    
    var request = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\n\r\n"
    conn.send(request)
    
    await asyncs.yield()
    
    var response = ""
    while true {
        var chunk = conn.recv(4096)
        if chunk == null {
            break
        }
        response = response + chunk
        await asyncs.yield()
    }
    
    conn.close()
    return response
}

async func fetch_multiple() {
    var f1 = fetch_http("www.example.com", "/")
    var f2 = fetch_http("www.baidu.com", "/")
    
    await f1
    await f2
    
    var results = asyncs.all([f1, f2])
    io.print("example.com: " + results[0].len() + " 字节")
    io.print("baidu.com: " + results[1].len() + " 字节")
}

main() {
    fetch_multiple()
    asyncs.run()
}
```

### 协程间协作的服务器

```leno
import sockets
import asyncs
import io

async func handle_client(Socket client, int id) {
    io.print("处理客户端 " + id + " (" + client.peer_addr() + ")")
    
    var data = client.recv(1024)
    if data != null {
        client.send("Echo: " + data)
    }
    client.close()
}

async func start_server(int port) {
    Socket server = sockets.listen("127.0.0.1", port)
    if server == null {
        return
    }
    
    var client_id = 0
    while true {
        Socket client = server.accept()
        if client != null {
            client_id = client_id + 1
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
   - `sockets.connect()` 是阻塞的，直到连接建立或失败
   - 默认情况下 `sock.accept()`、`sock.recv()` 和 `sock.recvfrom()` 是阻塞的
   - 使用 `sock.set_nonblocking(true)` 可以设置为非阻塞模式

2. **资源管理**
   - 每个创建的 socket 都应该通过 `sock.close()` 关闭
   - 服务器 socket 和客户端 socket 需要分别关闭
   - `sock.shutdown()` 不释放资源，仍需调用 `sock.close()`

3. **错误处理**
   - 所有操作都可能失败，应该检查返回值
   - `sockets.connect()` 返回 `null` 表示连接失败
   - `sock.recv()` 返回 `null` 表示连接已关闭或出错
   - 使用 `sock.error()` 获取具体错误码

4. **超时控制**
   - 使用 `sock.set_timeout(ms)` 设置收发超时
   - 超时后 `recv()` 返回 `null`，`sock.error()` 可查看错误码

5. **缓冲区限制**
   - `sock.recv()` 和 `sock.recvfrom()` 的 `max_bytes` 最大为 65536
   - 超过限制的数据会被截断

6. **字符编码**
   - 所有数据以字符串形式传输
   - 二进制数据需要自行编码/解码

7. **DNS 解析**
   - `sockets.connect()` 支持域名和 IP 地址
   - 使用 `sockets.resolve()` 单独进行 DNS 解析
   - 域名解析失败会返回 `null`

8. **端口绑定**
   - `sockets.listen()` 和 `sockets.udp_bind()` 需要指定端口
   - 端口 0 表示让系统自动分配

9. **地址格式**
   - IPv4 地址格式: "127.0.0.1"
   - "0.0.0.0" 表示监听所有网络接口
   - "127.0.0.1" 表示仅本地访问

10. **非阻塞模式注意事项**
    - 设置非阻塞后，`sock.recv()` 会立即返回，无数据时返回 `null`
    - `sock.accept()` 会立即返回，无连接时返回 `null`
    - 需要配合 `sockets.select()` 或循环轮询使用

---

## 完整 API 速查表

### 模块方法（`sockets.xxx`）

| 方法 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `sockets.connect(host, port)` | string, int | Socket\|null | TCP 连接 |
| `sockets.listen(host, port)` | string, int | Socket\|null | TCP 监听 |
| `sockets.udp_bind(host, port)` | string, int | Socket\|null | UDP 绑定 |
| `sockets.select(socks, timeout_ms)` | array, int | array | 等待多个 socket 可读 |
| `sockets.resolve(host)` | string | string\|null | DNS 解析 |
| `sockets.htons(host_short)` | int | int | 主机序 -> 网络序 (16位) |
| `sockets.htonl(host_long)` | int | int | 主机序 -> 网络序 (32位) |
| `sockets.ntohs(net_short)` | int | int | 网络序 -> 主机序 (16位) |
| `sockets.ntohl(net_long)` | int | int | 网络序 -> 主机序 (32位) |

### 实例方法 - 同步（`sock.xxx`）

| 方法 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `sock.send(data)` | string | bool | TCP 发送 |
| `sock.recv(max)` | int | string\|null | TCP 接收 |
| `sock.close()` | - | null | 关闭连接 |
| `sock.accept()` | - | Socket\|null | 接受连接（服务器，阻塞） |
| `sock.sendto(data, addr, port)` | string, string, int | bool | UDP 发送 |
| `sock.recvfrom(max)` | int | array\|null | UDP 接收 |
| `sock.set_nonblocking(flag)` | bool | bool | 设置非阻塞模式 |
| `sock.peer_addr()` | - | string\|null | 获取对端IP |
| `sock.peer_port()` | - | int\|null | 获取对端端口 |
| `sock.local_addr()` | - | string\|null | 获取本地IP |
| `sock.local_port()` | - | int\|null | 获取本地端口 |
| `sock.error()` | - | int | 获取最后错误码 |
| `sock.shutdown(how)` | int | bool | 优雅关闭 (0=读/1=写/2=双向) |
| `sock.set_timeout(ms)` | int | bool | 设置收发超时 |

### 异步实例方法（`sock.a*`）

> **重要**: 异步方法必须在 `async` 函数中使用 `await` 获取结果。这些方法基于事件循环 + select() 实现非阻塞 IO。

| 方法 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `sock.arecv(max_bytes)` | int | Future | 异步接收数据，await 返回 string\|null |
| `sock.aaccept()` | - | Future | 异步接受连接，await 返回 Socket\|null |

**注意**: 调用 `sock.arecv()` 或 `sock.aaccept()` 立即返回 Future 对象，需要 `await` 等待结果。在 `await` 期间，事件循环可以执行其他协程。

---

## 字节序转换

sockets 模块提供字节序转换函数，用于处理网络协议中的多字节数据。

### 为什么需要字节序转换？

不同计算机架构使用不同的字节序存储多字节数据：
- **小端序 (Little-Endian)**: 低位字节存储在低地址（x86/x64 架构）
- **大端序 (Big-Endian)**: 高位字节存储在低地址（网络协议标准）

网络协议统一使用大端序（网络字节序），因此需要进行转换。

### `sockets.htons(host_short)`

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

### `sockets.htonl(host_long)`

将 32 位整数从主机字节序转换为网络字节序。

**参数**:
- `host_long` (int): 主机字节序的 32 位整数

**返回**: 网络字节序的 32 位整数

```leno
var addr = 0x7f000001  // 127.0.0.1
var addr_be = sockets.htonl(addr)
```

---

### `sockets.ntohs(net_short)`

将 16 位整数从网络字节序转换为主机字节序。

**参数**:
- `net_short` (int): 网络字节序的 16 位整数

**返回**: 主机字节序的 16 位整数

---

### `sockets.ntohl(net_long)`

将 32 位整数从网络字节序转换为主机字节序。

**参数**:
- `net_long` (int): 网络字节序的 32 位整数

**返回**: 主机字节序的 32 位整数

---

*最后更新: 2026-08-01*
