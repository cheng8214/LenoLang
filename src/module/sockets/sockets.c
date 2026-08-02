// ============================================================================
// LenoC Sockets 模块 - Socket 实例方法风格
//
// 使用方式:
//   import sockets
//   Socket server = sockets.listen(host, port)
//   Socket client = sockets.connect(host, port)
//   server.accept()  server.send()  server.recv()  server.close()
//   client.send()    client.recv()  client.close()
//
// UDP:
//   Socket sock = sockets.udp_bind(host, port)
//   sock.sendto(data, addr, port)
//   sock.recvfrom(max_bytes)
//   sock.set_nonblocking(true)
// ============================================================================

// 必须在任何其他头文件之前包含 winsock2.h，否则会导致 windows.h 被先包含的警告
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
#endif

#include "include/native.h"
#include "include/leno_value.h"
#include <string.h>
#include <stdlib.h>

// ============================================================================
// 跨平台支持
// ============================================================================

#ifdef _WIN32
    #ifdef _MSC_VER
        #pragma comment(lib, "ws2_32.lib")
    #endif
    #define SOCKET_ERROR_VAL SOCKET_ERROR
    #define CLOSE_SOCKET(s) closesocket(s)
    #define GET_ERROR() WSAGetLastError()
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <netdb.h>
    #define SOCKET_ERROR_VAL (-1)
    #define CLOSE_SOCKET(s) close(s)
    #define GET_ERROR() errno
#endif

// ============================================================================
// 前向声明（定义在 object/object_socket.c 中）
// ============================================================================

extern void socket_register_method_with_params(const char* name, ObjNative* method, int arity,
                                               int min_arity, int max_arity,
                                               TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);
extern void socket_init_methods(void);
extern ObjNative* make_native(NativeFn fn, int arity, const char* name);

// ============================================================================
// Async I/O 内部实现 - 利用 select 多路复用
// ============================================================================

// 前向声明（定义在本文件后续位置）
static ObjSocket* create_socket_obj(socket_fd_t fd, int type);
static inline int socket_is_valid(ObjSocket* sock);

// 最大并发异步 I/O 数
#define MAX_ASYNC_IO 64

// 异步操作类型
typedef enum { ASYNC_OP_RECV = 1, ASYNC_OP_ACCEPT } AsyncOpType;

// 异步 I/O 请求
typedef struct {
    ObjSocket*    sock;        // 关联的 socket
    ObjFuture*    future;      // 等待者 Future
    AsyncOpType   op;          // 操作类型
    char*         buffer;      // recv 缓冲区
    int           buf_size;    // 缓冲区大小
    int           active;      // 是否仍在等待
} AsyncIORequest;

static AsyncIORequest g_async_requests[MAX_ASYNC_IO];
static int g_async_count = 0;

// 注册异步操作，返回 0 成功
static int async_add(ObjSocket* sock, ObjFuture* future, AsyncOpType op, char* buf, int buf_size) {
    if (g_async_count >= MAX_ASYNC_IO) return -1;
    AsyncIORequest* r = &g_async_requests[g_async_count++];
    r->sock = sock;
    r->future = future;
    r->op = op;
    r->buffer = buf;
    r->buf_size = buf_size;
    r->active = 1;
    return 0;
}

// 供 event_loop_run 调用: 用 select 等待就绪的 socket，执行真正的 I/O，完成 Future
int sockets_poll_async_io(uint64_t timeout_ms) {
    if (g_async_count == 0) return 0;

    fd_set readfds, writefds;
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    socket_fd_t max_fd = 0;
    int has_pending = 0;

    for (int i = 0; i < g_async_count; i++) {
        AsyncIORequest* r = &g_async_requests[i];
        if (!r->active || !socket_is_valid(r->sock)) continue;
        FD_SET(r->sock->fd, &readfds);
        if (r->sock->fd > max_fd) max_fd = r->sock->fd;
        has_pending = 1;
    }
    if (!has_pending) return 0;

    struct timeval tv;
    tv.tv_sec  = (long)(timeout_ms / 1000);
    tv.tv_usec = (long)((timeout_ms % 1000) * 1000);

    int ret = select((int)(max_fd + 1), &readfds, &writefds, NULL, &tv);
    if (ret <= 0) return 0;

    int done = 0;
    for (int i = 0; i < g_async_count; i++) {
        AsyncIORequest* r = &g_async_requests[i];
        if (!r->active) continue;
        if (!FD_ISSET(r->sock->fd, &readfds)) continue;

        if (r->op == ASYNC_OP_RECV) {
            int n = recv(r->sock->fd, r->buffer, r->buf_size, 0);
            r->active = 0;
            if (n > 0) {
                ObjString* s = str_new(r->buffer, n);
                future_complete(r->future, val_obj((Object*)s));
            } else {
                // 出错或关闭
                future_complete(r->future, val_null());
            }
            done++;
        } else if (r->op == ASYNC_OP_ACCEPT) {
            struct sockaddr_in addr;
            socklen_t len = sizeof(addr);
            socket_fd_t cfd = accept(r->sock->fd, (struct sockaddr*)&addr, &len);
            r->active = 0;
            if (cfd != INVALID_SOCKET_FD) {
                ObjSocket* cs = create_socket_obj(cfd, SOCKET_TYPE_TCP);
                if (cs) {
                    cs->is_connected = 1;
                    future_complete(r->future, val_obj((Object*)cs));
                } else {
                    CLOSE_SOCKET(cfd);
                    future_complete(r->future, val_null());
                }
            } else {
                future_complete(r->future, val_null());
            }
            done++;
        }
    }

    // 压缩数组：移除 inactive 项
    int w = 0;
    for (int i = 0; i < g_async_count; i++) {
        if (g_async_requests[i].active) {
            g_async_requests[w++] = g_async_requests[i];
        } else {
            if (g_async_requests[i].buffer) free(g_async_requests[i].buffer);
        }
    }
    g_async_count = w;
    return done;
}

// 查询是否有待处理的异步 I/O 请求
int sockets_has_async_io(void) {
    return g_async_count;
}

// 清理某个协程关联的所有异步请求（协程结束时调用）
void sockets_cancel_async(ObjCoroutine* co) {
    for (int i = 0; i < g_async_count; i++) {
        if (g_async_requests[i].future && g_async_requests[i].future->waiter == co) {
            g_async_requests[i].active = 0;
        }
    }
}

// ============================================================================
// Socket 实例方法编译期元信息注册（在 native_register_all_instance_method_metas 中调用）
// ============================================================================
void sockets_init_instance_methods(void) {
    TypeKind no_params[] = {};

    /* sock.send(data) -> bool */
    TypeKind send_params[] = {TYPE_STRING};
    native_register_instance_method_meta_with_params("Socket", "send", 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, send_params);

    /* sock.recv(max_bytes) -> string|null */
    TypeKind recv_params[] = {TYPE_INT};
    native_register_instance_method_meta_with_params("Socket", "recv", 1, -1, -1, TYPE_STRING, TYPE_UNKNOWN, recv_params);

    /* sock.close() -> null */
    native_register_instance_method_meta_with_params("Socket", "close", 0, -1, -1, TYPE_NULL, TYPE_UNKNOWN, no_params);

    /* sock.accept() -> Socket|null */
    native_register_instance_method_meta_with_params("Socket", "accept", 0, -1, -1, TYPE_SOCKET, TYPE_UNKNOWN, no_params);

    /* sock.sendto(data, addr, port) -> bool */
    TypeKind sendto_params[] = {TYPE_STRING, TYPE_STRING, TYPE_INT};
    native_register_instance_method_meta_with_params("Socket", "sendto", 3, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, sendto_params);

    /* sock.recvfrom(max_bytes) -> string|null */
    TypeKind recvfrom_params[] = {TYPE_INT};
    native_register_instance_method_meta_with_params("Socket", "recvfrom", 1, -1, -1, TYPE_STRING, TYPE_UNKNOWN, recvfrom_params);

    /* sock.set_nonblocking(nonblocking) -> bool */
    TypeKind set_nonblocking_params[] = {TYPE_BOOL};
    native_register_instance_method_meta_with_params("Socket", "set_nonblocking", 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, set_nonblocking_params);

    /* sock.peer_addr() -> string|null */
    native_register_instance_method_meta_with_params("Socket", "peer_addr", 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, no_params);

    /* sock.peer_port() -> int|null */
    native_register_instance_method_meta_with_params("Socket", "peer_port", 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, no_params);

    /* sock.local_addr() -> string|null */
    native_register_instance_method_meta_with_params("Socket", "local_addr", 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, no_params);

    /* sock.local_port() -> int|null */
    native_register_instance_method_meta_with_params("Socket", "local_port", 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, no_params);

    /* sock.error() -> int */
    native_register_instance_method_meta_with_params("Socket", "error", 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, no_params);

    /* sock.shutdown(how) -> bool */
    TypeKind shutdown_params[] = {TYPE_INT};
    native_register_instance_method_meta_with_params("Socket", "shutdown", 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, shutdown_params);

    /* sock.set_timeout(ms) -> bool */
    TypeKind set_timeout_params[] = {TYPE_INT};
    native_register_instance_method_meta_with_params("Socket", "set_timeout", 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, set_timeout_params);

    /* sock.arecv(max_bytes) -> Future (异步接收) */
    TypeKind arecv_params[] = {TYPE_INT};
    native_register_instance_method_meta_with_params("Socket", "arecv", 1, -1, -1, TYPE_FUTURE, TYPE_STRING, arecv_params);

    /* sock.aaccept() -> Future (异步接受连接) */
    native_register_instance_method_meta_with_params("Socket", "aaccept", 0, -1, -1, TYPE_FUTURE, TYPE_SOCKET, no_params);
}

// ============================================================================
// 模块初始化状态
// ============================================================================

static int sockets_initialized = 0;

// 初始化 Winsock（Windows 需要）
static int init_sockets(void) {
    if (sockets_initialized) return 1;

#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return 0;
    }
#endif

    sockets_initialized = 1;
    return 1;
}

// 创建新的 Socket 对象
static ObjSocket* create_socket_obj(socket_fd_t fd, int type) {
    ObjSocket* sock = (ObjSocket*)gc_alloc(sizeof(ObjSocket), OBJ_SOCKET);
    if (!sock) return NULL;

    sock->fd = fd;
    sock->type = type;
    sock->is_connected = 0;
    sock->is_listening = 0;
    sock->is_nonblocking = 0;
    sock->last_error = 0;

    return sock;
}

// 检查 socket 是否有效
static inline int socket_is_valid(ObjSocket* sock) {
    return sock && sock->fd != INVALID_SOCKET_FD;
}

// ============================================================================
// 实例方法实现（self 是第一个参数）
// ============================================================================

/* sock.arecv(max_bytes) -> Future (异步接收，await 后返回 string|null) */
static Value socket_arecv_func(int argc, Value* args) {
    (void)argc;
    ObjSocket* sock = (ObjSocket*)val_as_obj(args[0]);
    int max_bytes = val_as_int(args[1]);

    if (!socket_is_valid(sock)) return val_null();
    if (sock->type == SOCKET_TYPE_TCP && !sock->is_connected) return val_null();
    if (max_bytes <= 0) max_bytes = 1024;
    if (max_bytes > 65536) max_bytes = 65536;

    ObjCoroutine* co = vm_current_coroutine();
    if (!co) {
        native_throw_error("arecv 只能在 async 函数中使用");
        return val_null();
    }

    // 设置非阻塞
    if (!sock->is_nonblocking) {
        u_long mode = 1;
        #ifdef _WIN32
        ioctlsocket(sock->fd, FIONBIO, &mode);
        #else
        int flags = fcntl(sock->fd, F_GETFL, 0);
        if (flags != -1) fcntl(sock->fd, F_SETFL, flags | O_NONBLOCK);
        #endif
        sock->is_nonblocking = 1;
    }

    char* buffer = (char*)malloc(max_bytes);
    if (!buffer) return val_null();

    ObjFuture* future = future_new();
    future->waiter = co;

    if (async_add(sock, future, ASYNC_OP_RECV, buffer, max_bytes) != 0) {
        free(buffer);
        native_throw_error("注册异步 I/O 失败（可能已达上限）");
        return val_null();
    }

    co->waiting_for = future;
    return val_obj((Object*)future);
}

/* sock.aaccept() -> Future (异步接受连接，await 后返回 Socket|null) */
static Value socket_aaccept_func(int argc, Value* args) {
    (void)argc;
    ObjSocket* sock = (ObjSocket*)val_as_obj(args[0]);

    if (!socket_is_valid(sock) || !sock->is_listening) return val_null();

    ObjCoroutine* co = vm_current_coroutine();
    if (!co) {
        native_throw_error("aaccept 只能在 async 函数中使用");
        return val_null();
    }

    // 设置非阻塞
    if (!sock->is_nonblocking) {
        u_long mode = 1;
        #ifdef _WIN32
        ioctlsocket(sock->fd, FIONBIO, &mode);
        #else
        int flags = fcntl(sock->fd, F_GETFL, 0);
        if (flags != -1) fcntl(sock->fd, F_SETFL, flags | O_NONBLOCK);
        #endif
        sock->is_nonblocking = 1;
    }

    ObjFuture* future = future_new();
    future->waiter = co;

    if (async_add(sock, future, ASYNC_OP_ACCEPT, NULL, 0) != 0) {
        native_throw_error("注册异步 I/O 失败（可能已达上限）");
        return val_null();
    }

    co->waiting_for = future;
    return val_obj((Object*)future);
}

/* sock.send(data) -> bool
 * 注意：发送空字符串("")是合法的无操作（返回 true），
 * 但不会导致对端 arecv() 被唤醒。TCP 层面不发送零长度段。
 * 如果需要确保对端感知到事件，请发送至少 1 字节数据。
 */
static Value socket_send_func(int argc, Value* args) {
    (void)argc;
    ObjSocket* sock = (ObjSocket*)val_as_obj(args[0]);
    ObjString* data = (ObjString*)val_as_obj(args[1]);

    if (!socket_is_valid(sock)) return val_bool(false);

    if (sock->type == SOCKET_TYPE_TCP && !sock->is_connected) {
        return val_bool(false);
    }

    /* 空字符串优化：跳过系统调用 */
    if (data->len == 0) {
        return val_bool(true);
    }

    int sent = send(sock->fd, data->chars, (int)data->len, 0);
    if (sent == SOCKET_ERROR_VAL) {
        sock->last_error = GET_ERROR();
        return val_bool(false);
    }
    return val_bool(true);
}

/* sock.recv(max_bytes) -> string|null */
static Value socket_recv_func(int argc, Value* args) {
    (void)argc;
    ObjSocket* sock = (ObjSocket*)val_as_obj(args[0]);
    int max_bytes = val_as_int(args[1]);

    if (!socket_is_valid(sock)) return val_null();

    if (sock->type == SOCKET_TYPE_TCP && !sock->is_connected) {
        return val_null();
    }

    // 限制最大缓冲区大小
    if (max_bytes <= 0) max_bytes = 1024;
    if (max_bytes > 65536) max_bytes = 65536;

    char* buffer = (char*)malloc(max_bytes + 1);
    if (!buffer) return val_null();

    int received = recv(sock->fd, buffer, max_bytes, 0);

    if (received <= 0) {
        // 在非阻塞模式下，EWOULDBLOCK/EAGAIN 表示没有数据可读
        if (received < 0) {
            int err = GET_ERROR();
            sock->last_error = err;
#ifdef _WIN32
            if (err == WSAEWOULDBLOCK) {
#else
            if (err == EAGAIN || err == EWOULDBLOCK) {
#endif
                free(buffer);
                return val_null();  // 非阻塞模式下无数据
            }
        }
        free(buffer);
        return val_null();
    }

    buffer[received] = '\0';
    ObjString* result = str_copy(buffer, received);
    free(buffer);

    return val_obj((Object*)result);
}

/* sock.close() -> null */
static Value socket_close_func(int argc, Value* args) {
    (void)argc;
    ObjSocket* sock = (ObjSocket*)val_as_obj(args[0]);

    if (sock->fd != INVALID_SOCKET_FD) {
        CLOSE_SOCKET(sock->fd);
        sock->fd = INVALID_SOCKET_FD;
    }

    sock->is_connected = 0;
    sock->is_listening = 0;

    return val_null();
}

/* sock.accept() -> Socket|null */
static Value socket_accept_func(int argc, Value* args) {
    (void)argc;
    ObjSocket* server = (ObjSocket*)val_as_obj(args[0]);

    if (!socket_is_valid(server)) return val_null();
    if (server->type != SOCKET_TYPE_TCP || !server->is_listening) {
        return val_null();
    }

    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    socket_fd_t client_fd = accept(server->fd, (struct sockaddr*)&client_addr, &addr_len);

    if (client_fd == INVALID_SOCKET_FD) {
        return val_null();
    }

    // 创建客户端 socket 对象
    ObjSocket* client = create_socket_obj(client_fd, SOCKET_TYPE_TCP);
    if (!client) {
        CLOSE_SOCKET(client_fd);
        return val_null();
    }

    client->is_connected = 1;
    return val_obj((Object*)client);
}

/* sock.sendto(data, addr, port) -> bool
 * 注意：UDP 发送空字符串会发送 0 字节数据报，对端 recvfrom 会返回空字符串。
 * TCP 模式下 sendto 不是有效操作。
 */
static Value socket_sendto_func(int argc, Value* args) {
    (void)argc;
    ObjSocket* sock = (ObjSocket*)val_as_obj(args[0]);
    ObjString* data = (ObjString*)val_as_obj(args[1]);
    ObjString* addr_str = (ObjString*)val_as_obj(args[2]);
    int port = val_as_int(args[3]);

    if (!socket_is_valid(sock)) return val_bool(false);

    /* 空数据优化：UDP 空数据报 */
    if (data->len == 0) {
        /* UDP 允许发送空数据报 */
        if (sock->type != SOCKET_TYPE_UDP) {
            return val_bool(false);
        }
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((short)port);
    inet_pton(AF_INET, addr_str->chars, &addr.sin_addr);

    int sent = sendto(sock->fd, data->chars, (int)data->len, 0,
                      (struct sockaddr*)&addr, sizeof(addr));

    if (sent == SOCKET_ERROR_VAL) {
        sock->last_error = GET_ERROR();
        return val_bool(false);
    }
    return val_bool(true);
}

/* sock.recvfrom(max_bytes) -> [data, addr, port]|null */
static Value socket_recvfrom_func(int argc, Value* args) {
    (void)argc;
    ObjSocket* sock = (ObjSocket*)val_as_obj(args[0]);
    int max_bytes = val_as_int(args[1]);

    if (!socket_is_valid(sock)) return val_null();

    if (max_bytes <= 0) max_bytes = 1024;
    if (max_bytes > 65536) max_bytes = 65536;

    char* buffer = (char*)malloc(max_bytes + 1);
    if (!buffer) return val_null();

    struct sockaddr_in from_addr;
    socklen_t addr_len = sizeof(from_addr);

    int received = recvfrom(sock->fd, buffer, max_bytes, 0,
                            (struct sockaddr*)&from_addr, &addr_len);

    if (received <= 0) {
        // 处理非阻塞模式
        if (received < 0) {
            int err = GET_ERROR();
            sock->last_error = err;
#ifdef _WIN32
            if (err == WSAEWOULDBLOCK) {
#else
            if (err == EAGAIN || err == EWOULDBLOCK) {
#endif
                free(buffer);
                return val_null();
            }
        }
        free(buffer);
        return val_null();
    }

    buffer[received] = '\0';

    // 创建返回数组 [data, addr, port]
    ObjArray* arr = (ObjArray*)gc_alloc(sizeof(ObjArray), OBJ_ARRAY);
    if (!arr) {
        free(buffer);
        return val_null();
    }

    arr->count = 3;
    arr->capacity = 3;
    arr->elements = (Value*)malloc(3 * sizeof(Value));

    if (!arr->elements) {
        free(buffer);
        return val_null();
    }

    // 数据
    ObjString* data = str_copy(buffer, received);
    arr->elements[0] = val_obj((Object*)data);
    gc_write_barrier((Object*)arr, arr->elements[0]);

    // 地址
    char addr_buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &from_addr.sin_addr, addr_buf, sizeof(addr_buf));
    ObjString* addr = str_copy(addr_buf, strlen(addr_buf));
    arr->elements[1] = val_obj((Object*)addr);
    gc_write_barrier((Object*)arr, arr->elements[1]);

    // 端口
    arr->elements[2] = val_int(ntohs(from_addr.sin_port));

    free(buffer);
    return val_obj((Object*)arr);
}

/* sock.set_nonblocking(nonblocking) -> bool */
static Value socket_set_nonblocking_func(int argc, Value* args) {
    (void)argc;
    ObjSocket* sock = (ObjSocket*)val_as_obj(args[0]);

    int nonblocking = 0;
    if (val_is_bool(args[1])) {
        nonblocking = val_as_bool(args[1]);
    } else {
        nonblocking = val_as_int(args[1]);
    }

    if (!socket_is_valid(sock)) {
        return val_bool(false);
    }

#ifdef _WIN32
    u_long mode = nonblocking ? 1 : 0;
    int result = ioctlsocket(sock->fd, FIONBIO, &mode);
#else
    int flags = fcntl(sock->fd, F_GETFL, 0);
    if (flags < 0) return val_bool(false);

    if (nonblocking) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    int result = fcntl(sock->fd, F_SETFL, flags);
#endif

    if (result == 0) {
        sock->is_nonblocking = nonblocking ? 1 : 0;
    }

    return val_bool(result == 0);
}

/* sock.peer_addr() -> string|null  获取对端IP地址 */
static Value socket_peer_addr_func(int argc, Value* args) {
    (void)argc;
    ObjSocket* sock = (ObjSocket*)val_as_obj(args[0]);

    if (!socket_is_valid(sock)) return val_null();

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    if (getpeername(sock->fd, (struct sockaddr*)&addr, &addr_len) != 0) {
        sock->last_error = GET_ERROR();
        return val_null();
    }

    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
    return val_obj((Object*)str_copy(buf, (int)strlen(buf)));
}

/* sock.peer_port() -> int|null  获取对端端口 */
static Value socket_peer_port_func(int argc, Value* args) {
    (void)argc;
    ObjSocket* sock = (ObjSocket*)val_as_obj(args[0]);

    if (!socket_is_valid(sock)) return val_null();

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    if (getpeername(sock->fd, (struct sockaddr*)&addr, &addr_len) != 0) {
        sock->last_error = GET_ERROR();
        return val_null();
    }

    return val_int(ntohs(addr.sin_port));
}

/* sock.local_addr() -> string|null  获取本地IP地址 */
static Value socket_local_addr_func(int argc, Value* args) {
    (void)argc;
    ObjSocket* sock = (ObjSocket*)val_as_obj(args[0]);

    if (!socket_is_valid(sock)) return val_null();

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    if (getsockname(sock->fd, (struct sockaddr*)&addr, &addr_len) != 0) {
        sock->last_error = GET_ERROR();
        return val_null();
    }

    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
    return val_obj((Object*)str_copy(buf, (int)strlen(buf)));
}

/* sock.local_port() -> int|null  获取本地端口 */
static Value socket_local_port_func(int argc, Value* args) {
    (void)argc;
    ObjSocket* sock = (ObjSocket*)val_as_obj(args[0]);

    if (!socket_is_valid(sock)) return val_null();

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    if (getsockname(sock->fd, (struct sockaddr*)&addr, &addr_len) != 0) {
        sock->last_error = GET_ERROR();
        return val_null();
    }

    return val_int(ntohs(addr.sin_port));
}

/* sock.error() -> int  获取最后一次错误码 */
static Value socket_error_func(int argc, Value* args) {
    (void)argc;
    ObjSocket* sock = (ObjSocket*)val_as_obj(args[0]);
    return val_int(sock->last_error);
}

/* sock.shutdown(how) -> bool  优雅关闭连接
   how: 0=关闭读取端, 1=关闭写入端, 2=关闭两端 */
static Value socket_shutdown_func(int argc, Value* args) {
    (void)argc;
    ObjSocket* sock = (ObjSocket*)val_as_obj(args[0]);
    int how = val_as_int(args[1]);

    if (!socket_is_valid(sock)) return val_bool(false);

    // 限制 how 值范围
    if (how < 0) how = 0;
    if (how > 2) how = 2;

    int result = shutdown(sock->fd, how);
    if (result != 0) {
        sock->last_error = GET_ERROR();
        return val_bool(false);
    }

    return val_bool(true);
}

/* sock.set_timeout(ms) -> bool  设置收发超时（毫秒） */
static Value socket_set_timeout_func(int argc, Value* args) {
    (void)argc;
    ObjSocket* sock = (ObjSocket*)val_as_obj(args[0]);
    int timeout_ms = val_as_int(args[1]);

    if (!socket_is_valid(sock)) return val_bool(false);

#ifdef _WIN32
    DWORD tv = (DWORD)timeout_ms;
#else
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
#endif

    int rcv_result = setsockopt(sock->fd, SOL_SOCKET, SO_RCVTIMEO,
                                (const char*)&tv, sizeof(tv));
    int snd_result = setsockopt(sock->fd, SOL_SOCKET, SO_SNDTIMEO,
                                (const char*)&tv, sizeof(tv));

    if (rcv_result != 0 || snd_result != 0) {
        sock->last_error = GET_ERROR();
        return val_bool(false);
    }

    return val_bool(true);
}

/* sockets.resolve(host) -> string|null  DNS解析，返回IP地址 */
static Value sockets_resolve_func(int argc, Value* args) {
    (void)argc;

    if (!init_sockets()) return val_null();

    ObjString* host = (ObjString*)val_as_obj(args[0]);

    struct hostent* he = gethostbyname(host->chars);
    if (!he || !he->h_addr_list[0]) {
        return val_null();
    }

    struct in_addr addr;
    memcpy(&addr, he->h_addr_list[0], sizeof(addr));

    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr, buf, sizeof(buf));
    return val_obj((Object*)str_copy(buf, (int)strlen(buf)));
}

/* sockets.select(sockets_array, timeout_ms) -> array (模块级静态方法) */
static Value sockets_select_func(int argc, Value* args) {
    (void)argc;
    ObjArray* socks_arr = (ObjArray*)val_as_obj(args[0]);
    int timeout_ms = val_as_int(args[1]);

    if (socks_arr->count == 0) {
        return val_obj((Object*)socks_arr);
    }

    fd_set readfds;
    FD_ZERO(&readfds);

#ifndef _WIN32
    socket_fd_t max_fd = 0;
#endif

    // 将所有 socket 加入集合
    for (int i = 0; i < socks_arr->count; i++) {
        ObjSocket* sock = (ObjSocket*)val_as_obj(socks_arr->elements[i]);
        if (sock->fd != INVALID_SOCKET_FD) {
            FD_SET(sock->fd, &readfds);
#ifndef _WIN32
            if (sock->fd > max_fd) max_fd = sock->fd;
#endif
        }
    }

    // 设置超时
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    // 调用 select
#ifdef _WIN32
    int result = select(0, &readfds, NULL, NULL, &tv);
#else
    int result = select((int)(max_fd + 1), &readfds, NULL, NULL, &tv);
#endif

    if (result <= 0) {
        // 超时或无可用 socket，返回空数组
        ObjArray* empty = (ObjArray*)gc_alloc(sizeof(ObjArray), OBJ_ARRAY);
        if (empty) {
            empty->count = 0;
            empty->capacity = 0;
            empty->elements = NULL;
        }
        return val_obj((Object*)empty);
    }

    // 创建结果数组（只包含可读的 socket）
    ObjArray* result_arr = (ObjArray*)gc_alloc(sizeof(ObjArray), OBJ_ARRAY);
    if (!result_arr) return val_null();

    result_arr->count = 0;
    result_arr->capacity = result;
    result_arr->elements = (Value*)malloc(result * sizeof(Value));

    if (!result_arr->elements) return val_null();

    int idx = 0;
    for (int i = 0; i < socks_arr->count; i++) {
        ObjSocket* sock = (ObjSocket*)val_as_obj(socks_arr->elements[i]);
        if (sock->fd != INVALID_SOCKET_FD && FD_ISSET(sock->fd, &readfds)) {
            result_arr->elements[idx++] = val_obj((Object*)sock);
            gc_write_barrier((Object*)result_arr, val_obj((Object*)sock));
        }
    }
    result_arr->count = idx;

    return val_obj((Object*)result_arr);
}

// ============================================================================
// 模块工厂函数
// ============================================================================

/* sockets.connect(host, port) -> Socket|null */
static Value sockets_connect_func(int argc, Value* args) {
    (void)argc;

    if (!init_sockets()) {
        return val_null();
    }

    ObjString* host = (ObjString*)val_as_obj(args[0]);
    int port = val_as_int(args[1]);

    // 创建 socket
    socket_fd_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET_FD) {
        return val_null();
    }

    // 解析地址
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((short)port);

    // 尝试解析 IP 地址
    if (inet_pton(AF_INET, host->chars, &addr.sin_addr) <= 0) {
        // 不是 IP 地址，尝试 DNS 解析
        struct hostent* he = gethostbyname(host->chars);
        if (!he || !he->h_addr_list[0]) {
            CLOSE_SOCKET(fd);
            return val_null();
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    }

    // 连接
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR_VAL) {
        CLOSE_SOCKET(fd);
        return val_null();
    }

    // 创建对象
    ObjSocket* sock = create_socket_obj(fd, SOCKET_TYPE_TCP);
    if (!sock) {
        CLOSE_SOCKET(fd);
        return val_null();
    }

    sock->is_connected = 1;
    return val_obj((Object*)sock);
}

/* sockets.listen(host, port) -> Socket|null */
static Value sockets_listen_func(int argc, Value* args) {
    (void)argc;

    if (!init_sockets()) {
        return val_null();
    }

    ObjString* host = (ObjString*)val_as_obj(args[0]);
    int port = val_as_int(args[1]);

    // 创建 socket
    socket_fd_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET_FD) {
        return val_null();
    }

    // 设置地址重用
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    // 绑定地址
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((short)port);

    if (strcmp(host->chars, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, host->chars, &addr.sin_addr);
    }

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR_VAL) {
        CLOSE_SOCKET(fd);
        return val_null();
    }

    // 开始监听
    if (listen(fd, SOMAXCONN) == SOCKET_ERROR_VAL) {
        CLOSE_SOCKET(fd);
        return val_null();
    }

    // 创建对象
    ObjSocket* sock = create_socket_obj(fd, SOCKET_TYPE_TCP);
    if (!sock) {
        CLOSE_SOCKET(fd);
        return val_null();
    }

    sock->is_listening = 1;
    return val_obj((Object*)sock);
}

/* sockets.udp_bind(host, port) -> Socket|null */
static Value sockets_udp_bind_func(int argc, Value* args) {
    (void)argc;

    if (!init_sockets()) {
        return val_null();
    }

    ObjString* host = (ObjString*)val_as_obj(args[0]);
    int port = val_as_int(args[1]);

    // 创建 UDP socket
    socket_fd_t fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == INVALID_SOCKET_FD) {
        return val_null();
    }

    // 绑定地址
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((short)port);

    if (strcmp(host->chars, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, host->chars, &addr.sin_addr);
    }

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR_VAL) {
        CLOSE_SOCKET(fd);
        return val_null();
    }

    // 创建对象
    ObjSocket* sock = create_socket_obj(fd, SOCKET_TYPE_UDP);
    if (!sock) {
        CLOSE_SOCKET(fd);
        return val_null();
    }

    return val_obj((Object*)sock);
}

// ============================================================================
// 字节序转换函数
// ============================================================================

/* sockets.htons(host_short) -> int */
static Value sockets_htons_func(int argc, Value* args) {
    (void)argc;
    int host_short = val_as_int(args[0]);
    return val_int(htons((uint16_t)host_short));
}

/* sockets.htonl(host_long) -> int */
static Value sockets_htonl_func(int argc, Value* args) {
    (void)argc;
    int host_long = val_as_int(args[0]);
    return val_int((int)htonl((uint32_t)host_long));
}

/* sockets.ntohs(net_short) -> int */
static Value sockets_ntohs_func(int argc, Value* args) {
    (void)argc;
    int net_short = val_as_int(args[0]);
    return val_int(ntohs((uint16_t)net_short));
}

/* sockets.ntohl(net_long) -> int */
static Value sockets_ntohl_func(int argc, Value* args) {
    (void)argc;
    int net_long = val_as_int(args[0]);
    return val_int((int)ntohl((uint32_t)net_long));
}

// ============================================================================
// 模块初始化
// ============================================================================

void sockets_init_module(void) {
    init_sockets();

    // 初始化实例方法表
    socket_init_methods();

    // =====================================================
    // 注册实例方法（Socket 对象上的方法）
    // 注意：方法函数的 arity 是包括 self 的实际参数个数
    // 注册时的 arity（第三个参数）是不包括 self 的参数个数
    // =====================================================

    TypeKind no_params[] = {};

    /* sock.send(data) -> bool */
    TypeKind send_params[] = {TYPE_STRING};
    socket_register_method_with_params("send", make_native(socket_send_func, 2, "send"), 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, send_params);

    /* sock.recv(max_bytes) -> string|null */
    TypeKind recv_params[] = {TYPE_INT};
    socket_register_method_with_params("recv", make_native(socket_recv_func, 2, "recv"), 1, -1, -1, TYPE_STRING, TYPE_UNKNOWN, recv_params);

    /* sock.close() -> null */
    socket_register_method_with_params("close", make_native(socket_close_func, 1, "close"), 0, -1, -1, TYPE_NULL, TYPE_UNKNOWN, no_params);

    /* sock.accept() -> Socket|null */
    socket_register_method_with_params("accept", make_native(socket_accept_func, 1, "accept"), 0, -1, -1, TYPE_SOCKET, TYPE_UNKNOWN, no_params);

    /* sock.sendto(data, addr, port) -> bool */
    TypeKind sendto_params[] = {TYPE_STRING, TYPE_STRING, TYPE_INT};
    socket_register_method_with_params("sendto", make_native(socket_sendto_func, 4, "sendto"), 3, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, sendto_params);

    /* sock.recvfrom(max_bytes) -> [data, addr, port]|null */
    TypeKind recvfrom_params[] = {TYPE_INT};
    socket_register_method_with_params("recvfrom", make_native(socket_recvfrom_func, 2, "recvfrom"), 1, -1, -1, TYPE_STRING, TYPE_UNKNOWN, recvfrom_params);

    /* sock.set_nonblocking(nonblocking) -> bool */
    TypeKind set_nonblocking_params[] = {TYPE_BOOL};
    socket_register_method_with_params("set_nonblocking", make_native(socket_set_nonblocking_func, 2, "set_nonblocking"), 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, set_nonblocking_params);

    /* sock.peer_addr() -> string|null */
    socket_register_method_with_params("peer_addr", make_native(socket_peer_addr_func, 1, "peer_addr"), 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, no_params);

    /* sock.peer_port() -> int|null */
    socket_register_method_with_params("peer_port", make_native(socket_peer_port_func, 1, "peer_port"), 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, no_params);

    /* sock.local_addr() -> string|null */
    socket_register_method_with_params("local_addr", make_native(socket_local_addr_func, 1, "local_addr"), 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, no_params);

    /* sock.local_port() -> int|null */
    socket_register_method_with_params("local_port", make_native(socket_local_port_func, 1, "local_port"), 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, no_params);

    /* sock.error() -> int */
    socket_register_method_with_params("error", make_native(socket_error_func, 1, "error"), 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, no_params);

    /* sock.shutdown(how) -> bool */
    TypeKind shutdown_params[] = {TYPE_INT};
    socket_register_method_with_params("shutdown", make_native(socket_shutdown_func, 2, "shutdown"), 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, shutdown_params);

    /* sock.set_timeout(ms) -> bool */
    TypeKind set_timeout_params[] = {TYPE_INT};
    socket_register_method_with_params("set_timeout", make_native(socket_set_timeout_func, 2, "set_timeout"), 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, set_timeout_params);

    /* sock.arecv(max_bytes) -> Future (异步接收，在 async 函数中 await 得到 string|null) */
    TypeKind arecv_params[] = {TYPE_INT};
    socket_register_method_with_params("arecv", make_native(socket_arecv_func, 2, "arecv"), 1, -1, -1, TYPE_FUTURE, TYPE_UNKNOWN, arecv_params);

    /* sock.aaccept() -> Future (异步接受连接，在 async 函数中 await 得到 Socket|null) */
    socket_register_method_with_params("aaccept", make_native(socket_aaccept_func, 1, "aaccept"), 0, -1, -1, TYPE_FUTURE, TYPE_UNKNOWN, no_params);

    // =====================================================
    // 注册模块工厂方法（sockets.xxx）
    // =====================================================

    // TCP 客户端
    TypeKind connect_params[] = {TYPE_STRING, TYPE_INT};
    native_register_module_method("sockets", "connect", sockets_connect_func, 2, -1, -1, TYPE_SOCKET, TYPE_UNKNOWN, connect_params);

    // TCP 服务器
    TypeKind listen_params[] = {TYPE_STRING, TYPE_INT};
    native_register_module_method("sockets", "listen", sockets_listen_func, 2, -1, -1, TYPE_SOCKET, TYPE_UNKNOWN, listen_params);

    // UDP
    TypeKind udp_bind_params[] = {TYPE_STRING, TYPE_INT};
    native_register_module_method("sockets", "udp_bind", sockets_udp_bind_func, 2, -1, -1, TYPE_SOCKET, TYPE_UNKNOWN, udp_bind_params);

    // 字节序转换函数
    TypeKind htons_params[] = {TYPE_INT};
    native_register_module_method("sockets", "htons", sockets_htons_func, 1, -1, -1, TYPE_INT, TYPE_UNKNOWN, htons_params);

    TypeKind htonl_params[] = {TYPE_INT};
    native_register_module_method("sockets", "htonl", sockets_htonl_func, 1, -1, -1, TYPE_INT, TYPE_UNKNOWN, htonl_params);

    TypeKind ntohs_params[] = {TYPE_INT};
    native_register_module_method("sockets", "ntohs", sockets_ntohs_func, 1, -1, -1, TYPE_INT, TYPE_UNKNOWN, ntohs_params);

    TypeKind ntohl_params[] = {TYPE_INT};
    native_register_module_method("sockets", "ntohl", sockets_ntohl_func, 1, -1, -1, TYPE_INT, TYPE_UNKNOWN, ntohl_params);

    // DNS 解析
    TypeKind resolve_params[] = {TYPE_STRING};
    native_register_module_method("sockets", "resolve", sockets_resolve_func, 1, -1, -1, TYPE_STRING, TYPE_UNKNOWN, resolve_params);

    // select（模块级静态方法，不属于某个 socket 实例）
    TypeKind select_params[] = {TYPE_ARRAY, TYPE_INT};
    native_register_module_method("sockets", "select", sockets_select_func, 2, -1, -1, TYPE_ARRAY, TYPE_UNKNOWN, select_params);
}
