// 必须在任何其他头文件之前包含 winsock2.h，否则会导致 windows.h 被先包含的警告
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
#endif

#include "include/native.h"
#include "include/leno_vm.h"
#include <string.h>
#include <stdlib.h>

// ============================================================================
// 跨平台支持
// ============================================================================

#ifdef _WIN32
    #ifdef _MSC_VER
        #pragma comment(lib, "ws2_32.lib")
    #endif
    #define SOCKET_TYPE SOCKET
    #define INVALID_SOCKET_VAL INVALID_SOCKET
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
    #define SOCKET_TYPE int
    #define INVALID_SOCKET_VAL (-1)
    #define SOCKET_ERROR_VAL (-1)
    #define CLOSE_SOCKET(s) close(s)
    #define GET_ERROR() errno
#endif

// Socket 对象类型
#define SOCKET_TYPE_TCP 1
#define SOCKET_TYPE_UDP 2

// Socket 对象结构
typedef struct {
    Object header;
    SOCKET_TYPE fd;
    int type;           // SOCKET_TYPE_TCP 或 SOCKET_TYPE_UDP
    int is_connected;   // TCP 是否已连接
    int is_listening;   // TCP 是否在监听
    int is_nonblocking; // 是否非阻塞模式
    struct sockaddr_in udp_peer;  // UDP 对端地址（用于已连接的 UDP）
} ObjSocket;

// 模块初始化状态
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
static ObjSocket* create_socket_obj(SOCKET_TYPE fd, int type) {
    ObjSocket* sock = (ObjSocket*)gc_alloc(sizeof(ObjSocket), OBJ_NATIVE);
    if (!sock) return NULL;
    
    sock->fd = fd;
    sock->type = type;
    sock->is_connected = 0;
    sock->is_listening = 0;
    sock->is_nonblocking = 0;
    memset(&sock->udp_peer, 0, sizeof(sock->udp_peer));
    
    return sock;
}

// ============================================================================
// TCP 客户端功能
// ============================================================================

// sockets.connect(host, port) -> ObjSocket|null
static Value sockets_connect_func(int argc, Value* args) {
    (void)argc;
    
    if (!init_sockets()) {
        return val_null();
    }
    
    ObjString* host = (ObjString*)val_as_obj(args[0]);
    int port = val_as_int(args[1]);

    // 创建 socket
    SOCKET_TYPE fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET_VAL) {
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

// socket.send(data) -> bool
static Value socket_send_func(int argc, Value* args) {
    (void)argc;
    
    ObjSocket* sock = (ObjSocket*)val_as_obj(args[0]);
    ObjString* data = (ObjString*)val_as_obj(args[1]);

    if (sock->type != SOCKET_TYPE_TCP || !sock->is_connected) {
        return val_bool(false);
    }
    
    int sent = send(sock->fd, data->chars, (int)data->len, 0);
    return val_bool(sent != SOCKET_ERROR_VAL);
}

// socket.recv(max_bytes) -> string|null
static Value socket_recv_func(int argc, Value* args) {
    (void)argc;
    
    ObjSocket* sock = (ObjSocket*)val_as_obj(args[0]);
    int max_bytes = val_as_int(args[1]);

    if (sock->type != SOCKET_TYPE_TCP || !sock->is_connected) {
        return val_null();
    }
    
    // 限制最大缓冲区大小
    if (max_bytes <= 0) max_bytes = 1024;
    if (max_bytes > 65536) max_bytes = 65536;
    
    char* buffer = (char*)malloc(max_bytes + 1);
    if (!buffer) return val_null();
    
    int received = recv(sock->fd, buffer, max_bytes, 0);
    
    if (received <= 0) {
        // 在非阻塞模式下，EWOULDBLOCK/EAGAIN 表示没有数据可读，这是正常的
        if (received < 0 && sock->is_nonblocking) {
            int err = GET_ERROR();
#ifdef _WIN32
            if (err == WSAEWOULDBLOCK) {
#else
            if (err == EAGAIN || err == EWOULDBLOCK) {
#endif
                free(buffer);
                return val_null();  // 非阻塞模式下无数据，立即返回
            }
        }
        // 连接关闭或错误
        free(buffer);
        return val_null();
    }
    
    buffer[received] = '\0';
    ObjString* result = str_copy(buffer, received);
    free(buffer);
    
    return val_obj((Object*)result);
}

// socket.close() -> null
static Value socket_close_func(int argc, Value* args) {
    (void)argc;
    
    ObjSocket* sock = (ObjSocket*)val_as_obj(args[0]);

    if (sock->fd != INVALID_SOCKET_VAL) {
        CLOSE_SOCKET(sock->fd);
        sock->fd = INVALID_SOCKET_VAL;
    }
    
    sock->is_connected = 0;
    sock->is_listening = 0;
    
    return val_null();
}

// ============================================================================
// TCP 服务器功能
// ============================================================================

// sockets.listen(host, port) -> ObjSocket|null
static Value sockets_listen_func(int argc, Value* args) {
    (void)argc;
    
    if (!init_sockets()) {
        return val_null();
    }
    
    ObjString* host = (ObjString*)val_as_obj(args[0]);
    int port = val_as_int(args[1]);

    // 创建 socket
    SOCKET_TYPE fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET_VAL) {
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

// socket.accept() -> ObjSocket|null
static Value socket_accept_func(int argc, Value* args) {
    (void)argc;
    
    ObjSocket* server = (ObjSocket*)val_as_obj(args[0]);

    if (server->type != SOCKET_TYPE_TCP || !server->is_listening) {
        return val_null();
    }
    
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    
    SOCKET_TYPE client_fd = accept(server->fd, (struct sockaddr*)&client_addr, &addr_len);
    
    if (client_fd == INVALID_SOCKET_VAL) {
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

// ============================================================================
// UDP 功能
// ============================================================================

// sockets.udp_bind(host, port) -> ObjSocket|null
static Value sockets_udp_bind_func(int argc, Value* args) {
    (void)argc;
    
    if (!init_sockets()) {
        return val_null();
    }
    
    ObjString* host = (ObjString*)val_as_obj(args[0]);
    int port = val_as_int(args[1]);

    // 创建 UDP socket
    SOCKET_TYPE fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == INVALID_SOCKET_VAL) {
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

// socket.sendto(data, addr, port) -> bool
static Value socket_sendto_func(int argc, Value* args) {
    (void)argc;
    
    ObjSocket* sock = (ObjSocket*)val_as_obj(args[0]);
    ObjString* data = (ObjString*)val_as_obj(args[1]);
    ObjString* addr_str = (ObjString*)val_as_obj(args[2]);
    int port = val_as_int(args[3]);
    
    if (sock->type != SOCKET_TYPE_UDP) {
        return val_bool(false);
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((short)port);
    inet_pton(AF_INET, addr_str->chars, &addr.sin_addr);
    
    int sent = sendto(sock->fd, data->chars, (int)data->len, 0,
                      (struct sockaddr*)&addr, sizeof(addr));
    
    return val_bool(sent != SOCKET_ERROR_VAL);
}

// socket.recvfrom(max_bytes) -> array|null
static Value socket_recvfrom_func(int argc, Value* args) {
    (void)argc;
    
    ObjSocket* sock = (ObjSocket*)val_as_obj(args[0]);
    int max_bytes = val_as_int(args[1]);
    
    if (sock->type != SOCKET_TYPE_UDP) {
        return val_null();
    }
    
    if (max_bytes <= 0) max_bytes = 1024;
    if (max_bytes > 65536) max_bytes = 65536;
    
    char* buffer = (char*)malloc(max_bytes + 1);
    if (!buffer) return val_null();
    
    struct sockaddr_in from_addr;
    socklen_t addr_len = sizeof(from_addr);
    
    int received = recvfrom(sock->fd, buffer, max_bytes, 0,
                            (struct sockaddr*)&from_addr, &addr_len);
    
    if (received <= 0) {
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

// ============================================================================
// 非阻塞 IO 功能 (V2)
// ============================================================================

// socket.set_nonblocking(sock, nonblocking) -> bool
static Value socket_set_nonblocking_func(int argc, Value* args) {
    (void)argc;
    
    ObjSocket* sock = (ObjSocket*)val_as_obj(args[0]);

    int nonblocking = 0;
    if (val_is_bool(args[1])) {
        nonblocking = val_as_bool(args[1]);
    } else {
        nonblocking = val_as_int(args[1]);
    }
    
    if (sock->fd == INVALID_SOCKET_VAL) {
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

// ============================================================================
// 字节序转换函数
// ============================================================================

// htons(host_short) -> int - 主机字节序转网络字节序 (16位)
static Value sockets_htons_func(int argc, Value* args) {
    (void)argc;
    int host_short = val_as_int(args[0]);
    return val_int(htons((uint16_t)host_short));
}

// htonl(host_long) -> int - 主机字节序转网络字节序 (32位)
static Value sockets_htonl_func(int argc, Value* args) {
    (void)argc;
    int host_long = val_as_int(args[0]);
    return val_int((int)htonl((uint32_t)host_long));
}

// ntohs(net_short) -> int - 网络字节序转主机字节序 (16位)
static Value sockets_ntohs_func(int argc, Value* args) {
    (void)argc;
    int net_short = val_as_int(args[0]);
    return val_int(ntohs((uint16_t)net_short));
}

// ntohl(net_long) -> int - 网络字节序转主机字节序 (32位)
static Value sockets_ntohl_func(int argc, Value* args) {
    (void)argc;
    int net_long = val_as_int(args[0]);
    return val_int((int)ntohl((uint32_t)net_long));
}

// socket.select(sockets_array, timeout_ms) -> array
// 等待多个 socket 可读，返回可读的 socket 数组
static Value socket_select_func(int argc, Value* args) {
    (void)argc;
    
    ObjArray* socks_arr = (ObjArray*)val_as_obj(args[0]);
    int timeout_ms = val_as_int(args[1]);
    
    if (socks_arr->count == 0) {
        return val_obj((Object*)socks_arr);  // 返回空数组
    }
    
    fd_set readfds;
    FD_ZERO(&readfds);
    
    SOCKET_TYPE max_fd = 0;
    
    // 将所有 socket 加入集合
    for (int i = 0; i < socks_arr->count; i++) {
        ObjSocket* sock = (ObjSocket*)val_as_obj(socks_arr->elements[i]);
        if (sock->fd != INVALID_SOCKET_VAL) {
            FD_SET(sock->fd, &readfds);
#ifdef _WIN32
            // Windows 不需要 max_fd
#else
            if (sock->fd > max_fd) max_fd = sock->fd;
#endif
        }
    }
    
    // 设置超时
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    
    // 调用 select
    int result = select((int)(max_fd + 1), &readfds, NULL, NULL, &tv);
    
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
        if (sock->fd != INVALID_SOCKET_VAL && FD_ISSET(sock->fd, &readfds)) {
            result_arr->elements[idx++] = val_obj((Object*)sock);
            gc_write_barrier((Object*)result_arr, val_obj((Object*)sock));
        }
    }
    result_arr->count = idx;
    
    return val_obj((Object*)result_arr);
}

// ============================================================================
// 模块初始化
// ============================================================================

void sockets_init_module(void) {
    init_sockets();
    
    // 注册模块方法
    // TCP 客户端
    TypeKind connect_params[] = {TYPE_STRING, TYPE_INT};
    native_register_module_method("sockets", "connect", sockets_connect_func, 2, -1, -1, TYPE_ANY, connect_params);

    // TCP 服务器
    TypeKind listen_params[] = {TYPE_STRING, TYPE_INT};
    native_register_module_method("sockets", "listen", sockets_listen_func, 2, -1, -1, TYPE_ANY, listen_params);

    // UDP
    TypeKind udp_bind_params[] = {TYPE_STRING, TYPE_INT};
    native_register_module_method("sockets", "udp_bind", sockets_udp_bind_func, 2, -1, -1, TYPE_ANY, udp_bind_params);

    // Socket 实例方法（通过模块方式注册，第一个参数是 socket 对象）
    TypeKind send_params[] = {TYPE_ANY, TYPE_STRING};
    native_register_module_method("sockets", "send", socket_send_func, 2, -1, -1, TYPE_BOOL, send_params);

    TypeKind recv_params[] = {TYPE_ANY, TYPE_INT};
    native_register_module_method("sockets", "recv", socket_recv_func, 2, -1, -1, TYPE_ANY, recv_params);

    TypeKind close_params[] = {TYPE_ANY};
    native_register_module_method("sockets", "close", socket_close_func, 1, -1, -1, TYPE_NULL, close_params);

    TypeKind accept_params[] = {TYPE_ANY};
    native_register_module_method("sockets", "accept", socket_accept_func, 1, -1, -1, TYPE_ANY, accept_params);

    TypeKind sendto_params[] = {TYPE_ANY, TYPE_STRING, TYPE_STRING, TYPE_INT};
    native_register_module_method("sockets", "sendto", socket_sendto_func, 4, -1, -1, TYPE_BOOL, sendto_params);

    TypeKind recvfrom_params[] = {TYPE_ANY, TYPE_INT};
    native_register_module_method("sockets", "recvfrom", socket_recvfrom_func, 2, -1, -1, TYPE_ANY, recvfrom_params);

    // V2: 非阻塞 IO
    TypeKind set_nonblocking_params[] = {TYPE_ANY, TYPE_BOOL};
    native_register_module_method("sockets", "set_nonblocking", socket_set_nonblocking_func, 2, -1, -1, TYPE_BOOL, set_nonblocking_params);

    TypeKind select_params[] = {TYPE_ARRAY, TYPE_INT};
    native_register_module_method("sockets", "select", socket_select_func, 2, -1, -1, TYPE_ARRAY, select_params);

    // 字节序转换函数
    TypeKind htons_params[] = {TYPE_INT};
    native_register_module_method("sockets", "htons", sockets_htons_func, 1, -1, -1, TYPE_INT, htons_params);

    TypeKind htonl_params[] = {TYPE_INT};
    native_register_module_method("sockets", "htonl", sockets_htonl_func, 1, -1, -1, TYPE_INT, htonl_params);

    TypeKind ntohs_params[] = {TYPE_INT};
    native_register_module_method("sockets", "ntohs", sockets_ntohs_func, 1, -1, -1, TYPE_INT, ntohs_params);

    TypeKind ntohl_params[] = {TYPE_INT};
    native_register_module_method("sockets", "ntohl", sockets_ntohl_func, 1, -1, -1, TYPE_INT, ntohl_params);
}
