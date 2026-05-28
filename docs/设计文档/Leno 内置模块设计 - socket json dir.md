# Leno 内置模块设计：sockets、jsons、dirs

## 1. 总体设计

三个模块全部用 C 实现，作为内置模块注册到 VM。

```c
// 在 native.c 中注册
void register_builtin_modules() {
    socket_init_module();   // socket 网络模块
    json_init_module();     // JSON 解析模块  
    dir_init_module();      // 目录操作模块
}
```

***

## 2. socket 模块

### 2.1 API 设计

```leno
import sockets
import asyncs
import io

// TCP 客户端
main() {
    var conn = sockets.connect("127.0.0.1", 8080)
    if conn == null {
        io.print("连接失败")
        return
    }
    conn.send("Hello")
    var data = conn.recv(1024)      // 最多接收 1024 字节
    io.print(data)
    conn.close()
}

// TCP 服务器
async func tcp_server() {
    var server = sockets.listen("0.0.0.0", 8080)
    if server == null {
        io.print("监听失败")
        return
    }
    
    while true {
        var conn = await server.accept()   // async 等待连接
        async func handle(var c) {
            var data = await c.recv_async(1024)
            c.send($"Echo: {data}")
            c.close()
        }
        handle(conn)
    }
}

main() {
    async func start() {
        tcp_server()
    }
    start()
    asyncs.run()
}

// UDP
main() {
    var udp = sockets.udp_bind("0.0.0.0", 0)
    udp.sendto("Hello", "127.0.0.1", 9999)
    
    // recvfrom 返回数组 [data, addr, port]
    var result = udp.recvfrom(1024)
    var data = result[0]
    var addr = result[1]
    var port = result[2]
}
```

### 2.2 与 async 集成

```c
// socket 模块提供 async 版本 API
// socket.accept() -> 返回 Future，await 等待连接
// socket.recv_async() -> 返回 Future，await 等待数据

// 实现：内部用 select/poll/epoll 监控 fd
// 有数据时唤醒对应的协程
// 统一使用 asyncs.run() 处理所有异步事件
```

### 2.3 C 实现要点

```c
// 跨平台头文件
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
#endif

// ObjSocket 对象
typedef struct {
    Object header;
    int fd;                 // socket fd
    int type;               // TCP/UDP (1=TCP, 2=UDP)
    int listening;          // 是否监听中
} ObjSocket;

// 原生函数
Value socket_connect(int argc, Value* args);    // connect(host, port) -> ObjSocket|null
Value socket_listen(int argc, Value* args);     // listen(host, port) -> ObjSocket|null
Value socket_accept(int argc, Value* args);     // accept() -> Future
Value socket_send(int argc, Value* args);       // send(data) -> bool
Value socket_recv(int argc, Value* args);       // recv(max_bytes) -> string
Value socket_recv_async(int argc, Value* args); // recv_async(max_bytes) -> Future
Value socket_close(int argc, Value* args);      // close() -> null
Value socket_udp_bind(int argc, Value* args);   // udp_bind(host, port) -> ObjSocket|null
Value socket_sendto(int argc, Value* args);     // sendto(data, addr, port) -> bool
Value socket_recvfrom(int argc, Value* args);   // recvfrom(max_bytes) -> Array[data, addr, port]
```

### 2.4 错误处理

```leno
import sockets
import io

main() {
    // 方式1：检查返回值
    var conn = sockets.connect("127.0.0.1", 8080)
    if conn == null {
        io.print("连接失败")
        return
    }
    
    // 方式2：try-catch
    try {
        conn.send("Hello")
        var data = conn.recv(1024)
    } catch e {
        io.print($"错误: {e}")
    } finally {
        conn.close()
    }
}
```

***

## 3. json 模块

### 3.1 API 设计

```leno
import jsons
import io

main() {
    // 编码
    var data = {
        name: "张三",
        age: 25,
        scores: [85, 90, 78]
    }
    var str = jsons.encode(data)
    io.print(str)
    // {"name":"张三","age":25,"scores":[85,90,78]}
    
    // 解码
    var text = '{"x": 10, "y": 20}'
    var obj = jsons.decode(text)
    io.print(obj.x)    // 10
    
    // 美化输出
    var pretty = jsons.encode_pretty(data)
    io.print(pretty)
    
    // 文件操作
    jsons.write_file("data.json", data)
    var loaded = jsons.read_file("data.json")
    io.print(loaded.name)
}
```

### 3.2 Leno 类型映射

| JSON       | Leno               |
| ---------- | ------------------ |
| object     | Dict\[string, any] |
| array      | Array\[any]        |
| string     | string             |
| number     | int/float          |
| true/false | bool               |
| null       | null               |

### 3.3 C 实现要点

```c
// 使用 cJSON 或 jansson 库
// 或者自研简单解析器（推荐，减少依赖）

Value jsons_encode(int argc, Value* args);       // encode(value): string
Value jsons_decode(int argc, Value* args);       // decode(string): any
Value jsons_encode_pretty(int argc, Value* args);// encode_pretty(value): string
Value jsons_write_file(int argc, Value* args);   // write_file(path, value): bool
Value jsons_read_file(int argc, Value* args);    // read_file(path): any|null
```

### 3.4 错误处理

```leno
import jsons
import io

main() {
    var text = "{invalid json}"
    var obj = jsons.decode(text)
    if obj == null {
        io.print("解析失败")
        return
    }
    
    // 或者使用 try-catch
    try {
        var data = jsons.read_file("config.json")
    } catch e {
        io.print($"读取失败: {e}")
    }
}
```

***

## 4. dir 模块

### 4.1 API 设计

```leno
import dirs
import io

main() {
    // 路径操作
    var cwd = dirs.cwd()                 // 当前目录: string
    var abs = dirs.abspath("./test.txt") // 转绝对路径: string
    
    // split 返回数组 [dir, filename]
    var parts = dirs.split("/a/b/c.txt") // Array["/a/b", "c.txt"]
    var dir_part = parts[0]
    var file_part = parts[1]
    
    var ext = dirs.extname("test.txt")   // ".txt"
    var name = dirs.basename("/a/b/c.txt") // "c.txt"
    
    // 路径拼接（自动处理分隔符）
    var path = dirs.join("a", "b", "c")  // "a/b/c" 或 "a\b\c"
    var sep = dirs.sep()                 // "/" 或 "\\"
    
    // 目录操作
    dirs.mkdir("new_folder")             // 创建目录: bool
    dirs.mkdir_p("a/b/c")                // 递归创建: bool
    dirs.rmdir("empty_folder")           // 删除空目录: bool
    dirs.rm_rf("folder")                 // 递归删除: bool
    
    // 文件操作
    dirs.rename("old.txt", "new.txt")    // 重命名: bool
    dirs.remove("file.txt")              // 删除文件: bool
    dirs.copy("src.txt", "dst.txt")      // 复制文件: bool
    dirs.move("src.txt", "dst.txt")      // 移动文件: bool
    dirs.exists("file.txt")              // 检查存在: bool
    
    // 遍历
    var files = dirs.listdir(".")        // Array[string]
    for files to f {
        io.print(f)
    }
    
    // 通配符匹配
    var leno_files = dirs.glob("*.leno") // Array[string]
    
    // 文件信息
    var info = dirs.stat("test.txt")
    // info.size (int), info.mtime (int), info.is_file (bool), info.is_dir (bool)
    
    // 遍历目录树
    // walk 返回嵌套数组 [[root, dirs, files], ...]
    var entries = dirs.walk(".")
    for entries to entry {
        var root = entry[0]   // string: 当前目录
        var dirs = entry[1]   // Array[string]: 子目录列表
        var files = entry[2]  // Array[string]: 文件列表
        io.print($"目录: {root}")
        for files to f {
            io.print($"  文件: {f}")
        }
    }
}
```

### 4.2 C 实现要点

```c
// 跨平台头文件
#ifdef _WIN32
    #include <windows.h>
    #include <direct.h>
    #include <io.h>
    #define mkdir(path, mode) _mkdir(path)
    #define rmdir(path) _rmdir(path)
    #define getcwd(buffer, size) _getcwd(buffer, size)
    #define chdir(path) _chdir(path)
    #define access(path, mode) _access(path, mode)
#else
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <dirent.h>
    #include <unistd.h>
#endif

Value dirs_cwd(int argc, Value* args);           // cwd(): string
Value dirs_abspath(int argc, Value* args);       // abspath(path): string
Value dirs_split(int argc, Value* args);         // split(path): Array[dir, filename]
Value dirs_basename(int argc, Value* args);      // basename(path): string
Value dirs_extname(int argc, Value* args);       // extname(path): string
Value dirs_join(int argc, Value* args);          // join(part1, part2, ...): string
Value dirs_sep(int argc, Value* args);           // sep(): string
Value dirs_mkdir(int argc, Value* args);         // mkdir(path): bool
Value dirs_mkdir_p(int argc, Value* args);       // mkdir_p(path): bool
Value dir_rmdir(int argc, Value* args);         // rmdir(path): bool
Value dirs_rm_rf(int argc, Value* args);         // rm_rf(path): bool
Value dirs_rename(int argc, Value* args);        // rename(old, new): bool
Value dirs_remove(int argc, Value* args);        // remove(path): bool
Value dirs_copy(int argc, Value* args);          // copy(src, dst): bool
Value dirs_move(int argc, Value* args);          // move(src, dst): bool
Value dirs_exists(int argc, Value* args);        // exists(path): bool
Value dirs_listdir(int argc, Value* args);       // listdir(path): Array[string]
Value dirs_glob(int argc, Value* args);          // glob(pattern): Array[string]
Value dirs_stat(int argc, Value* args);          // stat(path): Dict
Value dirs_walk(int argc, Value* args);          // walk(path): Array[[root, dirs, files], ...]
```

### 4.3 错误处理

```leno
import dirs
import io

main() {
    // 检查返回值
    if !dirs.mkdir("test") {
        io.print("创建目录失败")
    }
    
    // 检查文件是否存在
    if dirs.exists("config.json") {
        var info = dirs.stat("config.json")
        io.print($"文件大小: {info.size}")
    }
    
    // try-catch
    try {
        dirs.copy("src.txt", "dst.txt")
    } catch e {
        io.print($"复制失败: {e}")
    }
}
```

***

## 5. 实现优先级

### P0 - dir 模块（2 天）

最简单，纯 POSIX API，无依赖。

| 任务   | 时间    |
| ---- | ----- |
| 路径操作 | 0.5 天 |
| 目录操作 | 0.5 天 |
| 文件操作 | 0.5 天 |
| 遍历功能 | 0.5 天 |

### P1 - json 模块（2 天）

需要解析器，但逻辑清晰。

| 任务       | 时间    |
| -------- | ----- |
| JSON 解析器 | 1 天   |
| JSON 生成器 | 0.5 天 |
| 文件操作     | 0.5 天 |

### P2 - socket 模块（3 天）

最复杂，需要与 async 集成。

| 任务         | 时间    |
| ---------- | ----- |
| TCP 基础 API | 1 天   |
| UDP API    | 0.5 天 |
| async 集成   | 1 天   |
| 错误处理       | 0.5 天 |

### 总计：7 天

***

## 6. 使用示例

### 完整 HTTP 客户端

```leno
import sockets
import jsons
import io
import asyncs

async func http_get(string url) {
    // 简单解析 URL（实际应该解析 host, port, path）
    var host = "api.example.com"
    var port = 80
    var path = "/data"
    
    var conn = sockets.connect(host, port)
    if conn == null {
        return null
    }
    
    conn.send($"GET {path} HTTP/1.1\r\n")
    conn.send($"Host: {host}\r\n")
    conn.send("Connection: close\r\n\r\n")
    
    var response = await conn.recv_async(4096)
    conn.close()
    
    // 解析 JSON（简化版，实际应该提取 body）
    return json.decode(response)
}

main() {
    async func fetch() {
        var data = await http_get("api.example.com/data")
        if data != null {
            io.print(data.name)
        }
    }
    fetch()
    asyncs.run()
}
```

### 配置文件管理

```leno
import json
import dir
import io

// 获取配置目录（跨平台）
func config_dir() -> string {
    var home = dir.cwd()  // 简化版，实际应该获取用户主目录
    return dir.join(home, ".leno")
}

func load_config(string name) -> var {
    var path = dir.join(config_dir(), $"{name}.json")
    if dir.exists(path) {
        return json.read_file(path)
    }
    return {}
}

func save_config(string name, var data) {
    var dir_path = config_dir()
    dir.mkdir_p(dir_path)
    var path = dir.join(dir_path, $"{name}.json")
    json.write_file(path, data)
}

main() {
    save_config("settings", {theme: "dark", font_size: 14})
    var cfg = load_config("settings")
    io.print(cfg.theme)
}
```

### 批量处理文件

```leno
import dir
import io

main() {
    // 遍历所有 .leno 文件
    var files = dir.glob("*.leno")
    for files to f {
        var info = dir.stat(f)
        io.print($"{f}: {info.size} bytes")
    }
    
    // 递归遍历目录
    var entries = dir.walk(".")
    var total_size = 0
    for entries to entry {
        var files = entry[2]
        for files to f {
            var full_path = dir.join(entry[0], f)
            var info = dir.stat(full_path)
            total_size = total_size + info.size
        }
    }
    io.print($"总大小: {total_size} bytes")
}
```

***

## 7. 依赖库选择

| 模块     | 方案        | 理由                               |
| ------ | --------- | -------------------------------- |
| socket | 自研        | BSD socket API 简单，跨平台封装后不需要依赖    |
| json   | 自研或 cJSON | cJSON 单文件，MIT 许可；自研更轻量           |
| dir    | 自研        | POSIX API 足够，Windows 用 \_wstat 等 |

**推荐全部自研**，减少外部依赖，保持 Leno 的独立性。

***

## 8. 跨平台注意事项

### 8.1 socket 模块

- Windows 需要 `WSAStartup` / `WSACleanup`
- Windows 的 socket fd 是 `SOCKET` 类型（实际上是 `UINT_PTR`），不是 `int`
- Windows 关闭 socket 用 `closesocket()`，不是 `close()`
- Windows 错误码用 `WSAGetLastError()`，不是 `errno`

### 8.2 dir 模块

- Windows 路径分隔符是 `\`，Unix 是 `/`
- Windows 使用宽字符 API (`_wstat`, `_wmkdir` 等) 支持中文路径
- Windows 没有 `S_ISDIR` 宏，需要手动实现

### 8.3 json 模块

- 纯算法，无平台差异
- 注意字符编码（UTF-8）

***

## 9. 未来规划：FFI 模块（P3）

### 9.1 设计目标

FFI（Foreign Function Interface）模块允许 Leno 直接调用 C 动态库（.dll/.so/.dylib），打通与 C 生态的桥梁。

### 9.2 使用场景

```leno
import ffi
import io

main() {
    // 加载 C 标准库
    var libc = ffi.open("msvcrt.dll")      // Windows
    // var libc = ffi.open("libc.so.6")      // Linux
    
    // 定义函数：返回类型, [参数类型...]
    var printf = libc.func("printf", "int", ["string", "int"])
    printf("Hello %d\n", 42)
    
    // 加载第三方库
    var curl = ffi.open("libcurl.dll")
    var curl_init = curl.func("curl_easy_init", "pointer", [])
    var handle = curl_init()
    
    // 调用 Windows API
    var user32 = ffi.open("user32.dll")
    var msgbox = user32.func("MessageBoxA", "int", ["pointer", "string", "string", "int"])
    msgbox(null, "Hello from Leno!", "FFI", 0)
}
```

### 9.3 API 设计

```leno
import ffi

main() {
    // 1. 打开动态库
    var lib = ffi.open("libname")              // 自动加平台后缀
    var lib = ffi.open_path("/usr/lib/lib.so") // 完整路径
    
    // 2. 定义函数
    // 基础类型: void, int, uint, long, ulong, float, double, 
    //          bool, char, string, pointer
    var func = lib.func("func_name", "return_type", ["arg1_type", "arg2_type"])
    
    // 3. 调用
    var result = func(arg1, arg2)
    
    // 4. 结构体（高级功能）
    var Point = ffi.struct("Point", [
        ["x", "int"],
        ["y", "int"]
    ])
    var p = Point.new()
    p.x = 10
    p.y = 20
    
    // 5. 回调函数（高级功能）
    var callback = ffi.callback("int", ["int", "int"], func(a, b) {
        return a + b
    })
    
    // 6. 关闭
    lib.close()
}
```

### 9.4 实现方案对比

| 方案              | 体积增加    | 复杂度 | 推荐度   |
| --------------- | ------- | --- | ----- |
| **简化版 FFI（自研）** | \~50KB  | 中   | ⭐⭐⭐⭐⭐ |
| libffi 精简版      | \~200KB | 低   | ⭐⭐⭐⭐  |
| 完整 libffi       | \~500KB | 低   | ⭐⭐⭐   |

### 9.5 简化版 FFI 设计

不依赖 libffi，只支持：

- 基本类型：void, int, float, double, pointer, string
- 固定参数（最多 8 个）
- x86/x64 平台

核心代码约 200 行，直接内联汇编实现调用约定：

```c
// Windows x64: RCX, RDX, R8, R9 + 栈
// Linux/macOS x64: RDI, RSI, RDX, RCX, R8, R9 + 栈
```

### 9.6 生态威力

有了 FFI，Leno 可以调用：

| 领域   | 库                     | 用途             |
| ---- | --------------------- | -------------- |
| GUI  | GTK、Qt、SDL            | 桌面应用           |
| 数据库  | libmysql、libpq        | 数据库操作          |
| 图像   | OpenCV、ImageMagick    | 图像处理           |
| 音频   | OpenAL、PortAudio      | 音频处理           |
| 网络   | libcurl、libwebsockets | HTTP/WebSocket |
| 科学计算 | BLAS、LAPACK           | 数值计算           |
| 游戏   | Raylib、BGFX           | 游戏开发           |
| AI   | TensorFlow C API      | 机器学习           |

### 9.7 实现优先级

**P3 优先级**（在 dir、json、socket 之后）

| 任务          | 时间  | 说明        |
| ----------- | --- | --------- |
| 基础 FFI（简化版） | 2 天 | 基本类型、固定参数 |
| 结构体支持       | 1 天 | 内存布局对齐    |
| 回调函数        | 2 天 | 需要跳板代码    |
| 测试完善        | 1 天 | 多平台测试     |

**总计：6 天**

### 9.8 体积预估

```
当前 Leno:           ~400KB
+ 简化版 FFI:        ~50KB
---------------------------
最终体积:            ~450KB

对比：
- Lua 5.4:           ~250KB
- Python 3:          ~15MB
- Node.js:           ~30MB
```

即使加上 FFI，Leno 仍然非常轻量！
