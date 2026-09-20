/**
 * LSP 协议实现
 * 处理 JSON-RPC 消息和 LSP 协议核心方法
 */

#include "leno_lsp.h"
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>   // stat：判断"候选目录/leno_module 是否存在"（内置包定位）
#include "../src/include/module_loader.h"
#include "../src/include/leno_package.h"
#include "../src/include/leno_types.h"
#ifdef _WIN32
#include <windows.h>    // GetModuleFileNameA：拿 LSP 自身目录（相对定位内置包）
#else
#include <unistd.h>     // readlink("/proc/self/exe")
#endif

// 创建 JSON 对象辅助函数
JsonValue* json_object_new(void) {
    JsonValue* obj = (JsonValue*)malloc(sizeof(JsonValue));
    if (!obj) return NULL;
    obj->type = JSON_OBJECT;
    obj->data.object_val.members = NULL;
    obj->data.object_val.count = 0;
    return obj;
}

JsonValue* json_string_new(const char* str) {
    JsonValue* val = (JsonValue*)malloc(sizeof(JsonValue));
    if (!val) return NULL;
    val->type = JSON_STRING;
    val->data.string_val = strdup(str);
    return val;
}

JsonValue* json_int_new(int n) {
    JsonValue* val = (JsonValue*)malloc(sizeof(JsonValue));
    if (!val) return NULL;
    val->type = JSON_NUMBER;
    val->data.number_val = n;
    return val;
}

JsonValue* json_bool_new(bool b) {
    JsonValue* val = (JsonValue*)malloc(sizeof(JsonValue));
    if (!val) return NULL;
    val->type = JSON_BOOL;
    val->data.bool_val = b;
    return val;
}

JsonValue* json_array_new(void) {
    JsonValue* arr = (JsonValue*)malloc(sizeof(JsonValue));
    if (!arr) return NULL;
    arr->type = JSON_ARRAY;
    arr->data.array_val.items = NULL;
    arr->data.array_val.count = 0;
    return arr;
}

void json_object_set(JsonValue* obj, const char* key, JsonValue* val) {
    if (!obj || obj->type != JSON_OBJECT) return;
    
    // 检查是否已存在
    for (int i = 0; i < obj->data.object_val.count; i++) {
        if (strcmp(obj->data.object_val.members[i].key, key) == 0) {
            json_free(obj->data.object_val.members[i].value);
            obj->data.object_val.members[i].value = val;
            return;
        }
    }
    
    // 添加新成员
    int count = obj->data.object_val.count;
    obj->data.object_val.members = (JsonMember*)realloc(obj->data.object_val.members,
                                                         sizeof(JsonMember) * (count + 1));
    obj->data.object_val.members[count].key = strdup(key);
    obj->data.object_val.members[count].value = val;
    obj->data.object_val.count++;
}

void json_array_add(JsonValue* arr, JsonValue* val) {
    if (!arr || arr->type != JSON_ARRAY) return;
    
    int count = arr->data.array_val.count;
    arr->data.array_val.items = (JsonValue**)realloc(arr->data.array_val.items,
                                                      sizeof(JsonValue*) * (count + 1));
    arr->data.array_val.items[count] = val;
    arr->data.array_val.count++;
}

// 读取 LSP 消息（带 Content-Length 头）
char* lsp_read_message(FILE* stream) {
    char line[1024];
    int content_length = -1;
    
    // 读取头部
    while (fgets(line, sizeof(line), stream)) {
        // 移除行尾的换行符以便比较
        size_t len = strlen(line);
        
        // 空行表示头部结束（可能是 \r\n 或 \n）
        if (len == 0) continue;
        if ((len == 1 && line[0] == '\n') ||
            (len == 2 && line[0] == '\r' && line[1] == '\n')) {
            break;
        }
        
        // 解析 Content-Length
        if (strncmp(line, "Content-Length: ", 16) == 0) {
            content_length = atoi(line + 16);
        }
    }
    
    if (content_length < 0 || content_length > 1024 * 1024) {
        // 无效的长度或超过 1MB
        return NULL;
    }
    
    // 读取内容
    char* content = (char*)malloc(content_length + 1);
    if (!content) return NULL;
    
    size_t total_read = 0;
    while (total_read < (size_t)content_length) {
        size_t read = fread(content + total_read, 1, content_length - total_read, stream);
        if (read == 0) {
            if (feof(stream)) break;
            free(content);
            return NULL;
        }
        total_read += read;
    }
    
    if (total_read != (size_t)content_length) {
        free(content);
        return NULL;
    }
    
    content[content_length] = '\0';
    return content;
}

// 写入 LSP 消息
void lsp_write_message(FILE* stream, const char* message) {
    if (!message) return;
    
    int len = strlen(message);
    fprintf(stream, "Content-Length: %d\r\n\r\n%s", len, message);
    fflush(stream);
}

// 创建响应消息
char* lsp_create_response(int id, JsonValue* result) {
	JsonValue* response = json_object_new();
	if (!response) return NULL;
	
	json_object_set(response, "jsonrpc", json_string_new("2.0"));
	json_object_set(response, "id", json_int_new(id));
	if (result) {
		// 使用深拷贝，这样调用者可以安全地释放 result
		json_object_set(response, "result", json_deep_copy(result));
	} else {
		json_object_set(response, "result", json_object_new());
	}
	
	char* result_str = (char*)json_stringify(response);
	json_free(response);
	
	return result_str;
}

// 创建错误响应
char* lsp_create_error(int id, int code, const char* message) {
	JsonValue* response = json_object_new();
	JsonValue* error = json_object_new();
	
	json_object_set(response, "jsonrpc", json_string_new("2.0"));
	json_object_set(response, "id", json_int_new(id));
	
	json_object_set(error, "code", json_int_new(code));
	json_object_set(error, "message", json_string_new(message));
	json_object_set(response, "error", error);
	
	char* result_str = (char*)json_stringify(response);
	json_free(response);
	
	return result_str;
}

// 创建通知消息
char* lsp_create_notification(const char* method, JsonValue* params) {
	JsonValue* notification = json_object_new();
	if (!notification) return NULL;
	
	json_object_set(notification, "jsonrpc", json_string_new("2.0"));
	json_object_set(notification, "method", json_string_new(method));
	if (params) {
		// 使用深拷贝，这样调用者可以安全地释放 params
		json_object_set(notification, "params", json_deep_copy(params));
	}
	
	char* result_str = (char*)json_stringify(notification);
	json_free(notification);
	
	return result_str;
}

// ============================================================================
// 内置模块目录（leno_module/）定位
// ----------------------------------------------------------------------------
// 背景（2026-09-19 修「LSP 找不到 SDL3 这个模块在哪儿」）：
//   `import "SDL3" as SDL3` 这类**包名写法**由 `package_resolve_module_file()` 在各搜索
//   路径下找 `SDL3.leno`；而 SDL3 是**内置包**，只可能出现在
//   `<编译器目录>/leno_module/<包名>/lib/` 下（本仓库：`build/leno_module/LenoSDL3/lib/`）。
//   LSP 进程与编译器**不在同一目录**（LSP 在 `leno_lsp/build/`）⇒ 必须显式找到编译器目录，
//   否则内置包一律解析不到（症状：SDL3 找不到位置、`SDL3.xxx` 报未定义/无提示）。
//
// 此前只有两条路：客户端传的 `lenoExecutablePath`、或 `PATH` 里的 `leno.exe`；
// 而"用 .\build\leno.exe 直接跑编译器"的开发机**两条都不满足** ⇒ 静默失效 ✗。
//
// 现在：**把所有候选按优先级收集起来，挑第一个真含 `leno_module/` 的**；一个都不含时退回
// 首个候选（保持旧行为不变）。候选：
//   ① initializationOptions.lenoExecutablePath   ② 环境变量 LENO_EXE（逃生口）
//   ③ PATH 里的 leno.exe                          ④ 从工作区根逐级向上（含 <dir>/build/）
//   ⑤ LSP 自身相对（<lspdir>/{../, ../../, ../build/, ../../build/}）
// 每个候选都打日志（`[LSP-PATH] candidate ...`）⇒ 下次这类问题几秒可诊。
//
// 注：分隔符用本文件自己的 `LSP_SEP` —— 编译器侧的 `PATH_SEP` 定义在 src/package 的
//     内部头里，LSP 这边取不到（实测 gcc -fsyntax-only 报 undeclared ✗）。
#ifdef _WIN32
#define LSP_SEP '\\'
#else
#define LSP_SEP '/'
#endif
#define LSP_MAX_EXE_CANDIDATES 16

// 目录存在性判断
static int lsp_is_dir(const char* path) {
    struct stat st;
    return (path && path[0] && stat(path, &st) == 0 && (st.st_mode & S_IFDIR)) ? 1 : 0;
}

// 把"文件路径或目录路径"统一成**以分隔符结尾的目录**：
//   · 已是目录（带尾随分隔符，或 stat 判定为目录）⇒ 补上尾随分隔符；
//   · 看起来是文件（如 .../leno.exe）⇒ 截到最后一个分隔符之后。
// ⚠ 必须带尾随分隔符：`package_builtin_add_to_search_paths_from()` 内部取"最后一个分隔符
//   之前"当目录 ⇒ 传 `.../build`（无尾随）会被它当成 `.../` ✗（内置包就永远找不到）。
static int lsp_to_dir_with_sep(const char* path, char* out, int out_len) {
    if (!path || !path[0] || !out || out_len <= 1) return -1;
    snprintf(out, out_len, "%s", path);
    size_t len = strlen(out);
    if (len == 0 || len + 2 > (size_t)out_len) return -1;
    char last = out[len - 1];
    if (last == '/' || last == '\\') return 0;
    if (lsp_is_dir(out)) {
        out[len] = LSP_SEP;
        out[len + 1] = '\0';
        return 0;
    }
    char* s1 = strrchr(out, '\\');
    char* s2 = strrchr(out, '/');
    char* sep = (s2 && (!s1 || s2 > s1)) ? s2 : s1;
    if (!sep) return -1;
    *(sep + 1) = '\0';
    return 0;
}

// 取"上一级目录"（输入/输出都是带尾随分隔符的目录）；到根或失败返回 -1
static int lsp_parent_dir(const char* dir, char* out, int out_len) {
    if (!dir || !dir[0] || !out || out_len <= 1) return -1;
    snprintf(out, out_len, "%s", dir);
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '/' || out[len - 1] == '\\')) {
        out[--len] = '\0';                          // 去掉尾随分隔符 ⇒ "a/b/c"
    }
    if (len == 0) return -1;
    char* s1 = strrchr(out, '\\');
    char* s2 = strrchr(out, '/');
    char* sep = (s2 && (!s1 || s2 > s1)) ? s2 : s1;
    if (!sep) return -1;                            // 已到根（如 "d:"）
    *(sep + 1) = '\0';                              // ⇒ "a/b/"
    if (strcmp(out, dir) == 0) return -1;           // 防自环
    return 0;
}

// LSP 自身 exe 所在目录（带尾随分隔符）
static int lsp_get_self_dir(char* out, int out_len) {
#ifdef _WIN32
    char buf[MAX_PATH_LEN];
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)sizeof(buf));
    if (n == 0 || n >= (DWORD)sizeof(buf)) return -1;
    buf[n] = '\0';
    return lsp_to_dir_with_sep(buf, out, out_len);
#else
    char buf[MAX_PATH_LEN];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return -1;
    buf[n] = '\0';
    return lsp_to_dir_with_sep(buf, out, out_len);
#endif
}

// 在 PATH 里找 leno.exe（返回 malloc 的完整路径；找不到 NULL）
static char* lsp_find_exe_in_path(void) {
    const char* path_env = getenv("PATH");
    if (!path_env) return NULL;
#ifdef _WIN32
    const char* exe_suffix = ".exe";
    const char* seps = ";";
#else
    const char* exe_suffix = "";
    const char* seps = ":";
#endif
    char path_copy[MAX_PATH_LEN * 4];               // PATH 可能很长
    strncpy(path_copy, path_env, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

    char* dir = strtok(path_copy, seps);
    while (dir) {
        char candidate[MAX_PATH_LEN];
        snprintf(candidate, sizeof(candidate), "%s%cleno%s", dir, LSP_SEP, exe_suffix);
        FILE* fp = fopen(candidate, "r");
        if (fp) {
            fclose(fp);
            return strdup(candidate);
        }
        dir = strtok(NULL, seps);
    }
    return NULL;
}

// 候选目录表（去重）
typedef struct {
    char dirs[LSP_MAX_EXE_CANDIDATES][MAX_PATH_LEN];
    int count;
} LspDirCands;

static void lsp_cand_add(LspDirCands* c, const char* path) {
    if (!c) return;
    char dir[MAX_PATH_LEN];
    if (lsp_to_dir_with_sep(path, dir, sizeof(dir)) != 0) return;
    for (int i = 0; i < c->count; i++) {
        if (strcmp(c->dirs[i], dir) == 0) return;   // 去重
    }
    if (c->count < LSP_MAX_EXE_CANDIDATES) {
        snprintf(c->dirs[c->count], MAX_PATH_LEN, "%s", dir);
        c->count++;
    }
}

// 从 dir 起逐级向上，把 <dir>/ 与 <dir>/build/ 加入候选
// （覆盖"把子目录当工作区打开"的情况：工作区里没有 leno_module 也能向上找到）
static void lsp_cand_add_walk_up(LspDirCands* c, const char* dir, int max_levels) {
    char cur[MAX_PATH_LEN];
    if (lsp_to_dir_with_sep(dir, cur, sizeof(cur)) != 0) return;
    for (int i = 0; i < max_levels; i++) {
        char probe[MAX_PATH_LEN];
        size_t clen = strlen(cur);
        if (clen + 8 < sizeof(probe)) {
            snprintf(probe, sizeof(probe), "%sbuild%c", cur, LSP_SEP);
            lsp_cand_add(c, probe);
        }
        lsp_cand_add(c, cur);
        char up[MAX_PATH_LEN];
        if (lsp_parent_dir(cur, up, sizeof(up)) != 0) break;
        snprintf(cur, sizeof(cur), "%s", up);
    }
}

// 挑第一个"真含 leno_module/"的候选；都没有则退回首个候选（保持旧行为）
static const char* lsp_pick_builtin_parent(LspDirCands* c) {
    if (!c || c->count <= 0) return NULL;
    for (int i = 0; i < c->count; i++) {
        char probe[MAX_PATH_LEN];
        snprintf(probe, sizeof(probe), "%sleno_module%c", c->dirs[i], LSP_SEP);
        if (lsp_is_dir(probe)) return c->dirs[i];
    }
    return c->dirs[0];
}

// 处理 initialize 请求
char* lsp_handle_initialize(LspServer* server, int id, JsonValue* params) {
    if (server->state != LSP_STATE_UNINITIALIZED) {
        return lsp_create_error(id, LSP_ERROR_INVALID_REQUEST, 
                                "Server already initialized");
    }
    
    server->state = LSP_STATE_INITIALIZING;
    
    // 解析客户端能力（可选）
    JsonValue* client_info = json_object_get(params, "clientInfo");
    if (client_info) {
        JsonValue* name = json_object_get(client_info, "name");
        if (name) {
            lsp_log(server, LSP_LOG_INFO, "Client: %s", json_string_value(name));
        }
    }
    
    // 获取根目录
    JsonValue* root_uri = json_object_get(params, "rootUri");
    if (root_uri && root_uri->type == JSON_STRING) {
        server->root_path = lsp_uri_to_path(json_string_value(root_uri));
    }
    
    // 定位"编译器目录"（用于注册内置包 <编译器目录>/leno_module/<包>/lib/）
    // 候选制：全部收集 → 挑第一个**真含 leno_module/** 的（详见上方辅助函数的说明）。
    // 旧实现只看两条路（客户端传的路径 / PATH），本仓库开发者两条都不满足 ⇒ 内置包解析不到 ✗。
    char* leno_exe_path = NULL;
    {
        LspDirCands cands;
        memset(&cands, 0, sizeof(cands));

        // ① 客户端（VS Code 扩展）传来的 lenoExecutablePath
        JsonValue* init_options = json_object_get(params, "initializationOptions");
        if (init_options && init_options->type == JSON_OBJECT) {
            JsonValue* exe_path_val = json_object_get(init_options, "lenoExecutablePath");
            if (exe_path_val && exe_path_val->type == JSON_STRING) {
                const char* exe_str = json_string_value(exe_path_val);
                if (exe_str && exe_str[0]) {
                    fprintf(stderr, "[LSP-INIT] client lenoExecutablePath: %s\n", exe_str);
                    fflush(stderr);
                    lsp_cand_add(&cands, exe_str);
                }
            }
        }

        // ② 环境变量 LENO_EXE（逃生口：手工指定编译器位置）
        {
            const char* env_exe = getenv("LENO_EXE");
            if (env_exe && env_exe[0]) {
                fprintf(stderr, "[LSP-INIT] LENO_EXE env: %s\n", env_exe);
                fflush(stderr);
                lsp_cand_add(&cands, env_exe);
            }
        }

        // ③ PATH 里的 leno.exe
        {
            char* path_exe = lsp_find_exe_in_path();
            if (path_exe) {
                fprintf(stderr, "[LSP-INIT] leno found in PATH: %s\n", path_exe);
                fflush(stderr);
                lsp_cand_add(&cands, path_exe);
                free(path_exe);
            }
        }

        // ④ 从工作区根逐级向上（含每一级的 build/ 子目录）
        if (server->root_path) {
            lsp_cand_add_walk_up(&cands, server->root_path, 6);
        }

        // ⑤ LSP 自身相对（本仓库布局：<repo>/leno_lsp/build/leno_lsp.exe ⇒ <repo>/build/leno.exe）
        {
            char self_dir[MAX_PATH_LEN];
            if (lsp_get_self_dir(self_dir, sizeof(self_dir)) == 0) {
                lsp_cand_add(&cands, self_dir);
                char build1[MAX_PATH_LEN];
                snprintf(build1, sizeof(build1), "%sbuild%c", self_dir, LSP_SEP);
                lsp_cand_add(&cands, build1);
                char up1[MAX_PATH_LEN];
                if (lsp_parent_dir(self_dir, up1, sizeof(up1)) == 0) {
                    lsp_cand_add(&cands, up1);
                    char build2[MAX_PATH_LEN];
                    snprintf(build2, sizeof(build2), "%sbuild%c", up1, LSP_SEP);
                    lsp_cand_add(&cands, build2);
                    char up2[MAX_PATH_LEN];
                    if (lsp_parent_dir(up1, up2, sizeof(up2)) == 0) {
                        lsp_cand_add(&cands, up2);
                        char build3[MAX_PATH_LEN];
                        snprintf(build3, sizeof(build3), "%sbuild%c", up2, LSP_SEP);
                        lsp_cand_add(&cands, build3);
                    }
                }
            }
        }

        // 打印候选（含"该目录下有没有 leno_module"）⇒ 下次这类问题几秒可诊
        for (int ci = 0; ci < cands.count; ci++) {
            char probe[MAX_PATH_LEN];
            snprintf(probe, sizeof(probe), "%sleno_module%c", cands.dirs[ci], LSP_SEP);
            fprintf(stderr, "[LSP-PATH] candidate #%d: %s (leno_module: %s)\n",
                    ci, cands.dirs[ci], lsp_is_dir(probe) ? "yes" : "no");
        }

        const char* chosen = lsp_pick_builtin_parent(&cands);
        if (chosen) {
            leno_exe_path = strdup(chosen);
            fprintf(stderr, "[LSP-PATH] builtin parent chosen: %s\n", leno_exe_path);
        } else {
            fprintf(stderr, "[LSP-PATH] no candidate at all; fall back to LSP's own directory\n");
        }
        fflush(stderr);
    }
    
    // 设置模块符号表缓存目录（与编译器共享 .lenocache/ 目录）
    // 这是 LSP 性能的关键：没有缓存目录，每次分析都要重新扫描所有导入的模块源文件
    if (server->root_path) {
        char cache_dir[1024];
#ifdef _WIN32
        snprintf(cache_dir, sizeof(cache_dir), "%s\\.lenocache\\", server->root_path);
#else
        snprintf(cache_dir, sizeof(cache_dir), "%s/.lenocache/", server->root_path);
#endif
        module_loader_set_cache_enabled(1);
        module_loader_set_cache_dir(cache_dir);
        fprintf(stderr, "[LSP-CACHE] cache_dir=%s\n", cache_dir);
        fflush(stderr);
    } else {
        fprintf(stderr, "[LSP-CACHE] WARNING: root_path is NULL, module cache disabled\n");
        fflush(stderr);
    }

    // 设置模块搜索路径（与编译器 src/main.c 保持一致）
    // 这是 LSP 正确解析 import 的关键：没有搜索路径，package_resolve_module_file
    // 无法解析不带 .leno 后缀的模块名（如 import "SDL3" / import "LenoMusic"）
    {
        package_search_path_clear();

        // 1. 项目根 lib/ 目录 + leno.toml 依赖
        if (server->root_path) {
            // 添加 <项目根>/lib/ 到搜索路径
            char lib_path[MAX_PATH_LEN];
            snprintf(lib_path, sizeof(lib_path), "%slib%c", server->root_path,
#ifdef _WIN32
                '\\'
#else
                '/'
#endif
            );
            package_search_path_add(lib_path);

            // 从 leno.toml 读取依赖，添加依赖包的 lib/ 到搜索路径
            char toml_path[MAX_PATH_LEN];
            snprintf(toml_path, sizeof(toml_path), "%sleno.toml", server->root_path);
            PackageConfig* pkg_cfg = package_config_parse(toml_path);
            if (pkg_cfg) {
                for (int di = 0; di < pkg_cfg->dep_count; di++) {
                    const char* dn = pkg_cfg->dependencies[di].name;
                    if (!dn) continue;
                    const char* cache = package_cache_dir();
                    char dep_lib[MAX_PATH_LEN];
                    snprintf(dep_lib, sizeof(dep_lib), "%s%s%clib%c",
                             cache, dn,
#ifdef _WIN32
                             '\\',
#else
                             '/',
#endif
#ifdef _WIN32
                             '\\'
#else
                             '/'
#endif
                    );
                    package_search_path_add(dep_lib);
                }
                package_config_free(pkg_cfg);
            }
        }

        // 2. 添加内置模块搜索路径（<编译器目录>/leno_module/<包名>/lib/）
        //    内置模块优先于全局缓存，确保随 exe 分发的模块版本不被缓存覆盖
        //    `leno_exe_path` 由上面那段"候选制"选出：**以分隔符结尾的目录** ✓
        if (leno_exe_path) {
            package_builtin_add_to_search_paths_from(leno_exe_path);
            fprintf(stderr, "[LSP-PATH] builtin modules registered from: %s\n", leno_exe_path);
        } else {
            // fallback：尝试用当前进程路径（LSP 自身），大多数情况无效但不影响其他搜索路径
            package_builtin_add_to_search_paths();
            fprintf(stderr, "[LSP-PATH] builtin modules registered from LSP's own directory "
                            "(no better candidate found)\n");
        }
        fflush(stderr);

        // 3. 添加全局缓存中所有已安装包的 lib/ 到搜索路径
        //    这样 import "包名" 就能直接在缓存中查找（外部安装包）
        package_cache_add_to_search_paths();

        fprintf(stderr, "[LSP-PATH] search paths initialized: %d paths\n",
                package_search_path_count());
        fflush(stderr);
    }
    
    if (leno_exe_path) free(leno_exe_path);
    
    // 构建服务器能力
    JsonValue* capabilities = json_object_new();
    
    // 文本同步 - 增量更新
    JsonValue* text_sync = json_object_new();
    json_object_set(text_sync, "openClose", json_bool_new(true));
    json_object_set(text_sync, "change", json_int_new(1)); // Full
    json_object_set(capabilities, "textDocumentSync", text_sync);
    
    // 补全提供者
    if (server->completion_provider) {
        JsonValue* completion = json_object_new();
        json_object_set(completion, "resolveProvider", json_bool_new(false));
        
        JsonValue* trigger_chars = json_array_new();
        json_array_add(trigger_chars, json_string_new("."));
        json_array_add(trigger_chars, json_string_new(":"));
        json_object_set(completion, "triggerCharacters", trigger_chars);
        
        json_object_set(capabilities, "completionProvider", completion);
    }
    
    // 悬停提供者
    if (server->hover_provider) {
        json_object_set(capabilities, "hoverProvider", json_bool_new(true));
    }
    
    // 定义跳转提供者
    if (server->definition_provider) {
        json_object_set(capabilities, "definitionProvider", json_bool_new(true));
    }
    
    // 诊断提供者
    if (server->diagnostic_provider) {
        JsonValue* diagnostic = json_object_new();
        json_object_set(diagnostic, "interFileDependencies", json_bool_new(true));
        json_object_set(diagnostic, "workspaceDiagnostics", json_bool_new(false));
        json_object_set(capabilities, "diagnosticProvider", diagnostic);
    }
    
    // 文档符号提供者 (大纲、面包屑导航、Ctrl+Shift+O)
    if (server->document_symbol_provider) {
        json_object_set(capabilities, "documentSymbolProvider", json_bool_new(true));
    }
    
    // Signature Help 提供者 (函数参数提示)
    {
        JsonValue* sig_help = json_object_new();
        JsonValue* trigger_chars = json_array_new();
        json_array_add(trigger_chars, json_string_new("("));
        json_array_add(trigger_chars, json_string_new(","));
        json_object_set(sig_help, "triggerCharacters", trigger_chars);
        json_object_set(capabilities, "signatureHelpProvider", sig_help);
    }
    
    // References 提供者 (查找引用)
    json_object_set(capabilities, "referencesProvider", json_bool_new(true));
    
    // Rename 提供者 (重命名)
    json_object_set(capabilities, "renameProvider", json_bool_new(true));
    
    // Folding Range 提供者 (代码折叠)
    json_object_set(capabilities, "foldingRangeProvider", json_bool_new(true));
    
    // 构建响应
    JsonValue* result = json_object_new();
    json_object_set(result, "capabilities", capabilities);
    
    JsonValue* server_info = json_object_new();
    json_object_set(server_info, "name", json_string_new("LenoLSP"));
    json_object_set(server_info, "version", json_string_new(LSP_VERSION));
    json_object_set(result, "serverInfo", server_info);
    
    char* response = lsp_create_response(id, result);
	
	// 释放 JSON 对象（result 会递归释放所有子对象）
	json_free(result);
	
	server->state = LSP_STATE_INITIALIZED;
	
	lsp_log(server, LSP_LOG_INFO, "Server initialized");
	
	return response;
}

// 处理 shutdown 请求
char* lsp_handle_shutdown(LspServer* server, int id) {
    if (server->state != LSP_STATE_INITIALIZED) {
        return lsp_create_error(id, LSP_ERROR_INVALID_REQUEST,
                                "Server not initialized");
    }
    
    server->state = LSP_STATE_SHUTDOWN;
    
    lsp_log(server, LSP_LOG_INFO, "Server shutting down");
    
    return lsp_create_response(id, NULL);
}

// 处理 exit 通知
char* lsp_handle_exit(LspServer* server) {
    lsp_log(server, LSP_LOG_INFO, "Server exiting");
    return NULL; // 返回 NULL 表示退出
}

// 处理文档打开通知
char* lsp_handle_did_open(LspServer* server, JsonValue* params) {
    if (!server || !params) return NULL;
    
    JsonValue* text_doc = json_object_get(params, "textDocument");
    if (!text_doc || text_doc->type != JSON_OBJECT) return NULL;
    
    JsonValue* uri = json_object_get(text_doc, "uri");
    JsonValue* text = json_object_get(text_doc, "text");
    JsonValue* version = json_object_get(text_doc, "version");
    
    if (!uri || uri->type != JSON_STRING) return NULL;
    if (!text || text->type != JSON_STRING) return NULL;
    
    const char* uri_str = json_string_value(uri);
    const char* text_str = json_string_value(text);
    int ver = json_int_value(version);
    
    if (!uri_str || !text_str) return NULL;
    
    // 打开文档
    lsp_document_open(server, uri_str, text_str, ver);
    
    lsp_log(server, LSP_LOG_INFO, "Document opened: %s", uri_str);
    
    // 发布诊断
    if (server->diagnostic_provider) {
        lsp_publish_diagnostics(server, uri_str);
    }
    
    return NULL;
}

// 处理文档修改通知
char* lsp_handle_did_change(LspServer* server, JsonValue* params) {
    JsonValue* text_doc = json_object_get(params, "textDocument");
    JsonValue* content_changes = json_object_get(params, "contentChanges");
    
    if (!text_doc || !content_changes) return NULL;
    
    JsonValue* uri = json_object_get(text_doc, "uri");
    JsonValue* version = json_object_get(text_doc, "version");
    
    if (!uri) return NULL;
    
    fprintf(stderr, "[DID-CHANGE] uri=%s version=%d\n", json_string_value(uri), 
            version ? (int)json_int_value(version) : -1);
    fflush(stderr);
    
    // 获取文档
    LspTextDocument* doc = lsp_document_get(server, json_string_value(uri));
    if (!doc) return NULL;
    
    // 应用内容变更（使用最后一个变更的 text 进行全量替换）
    if (content_changes->type == JSON_ARRAY && 
        content_changes->data.array_val.count > 0) {
        // 获取最后一个变更（最新的内容）
        int last_idx = content_changes->data.array_val.count - 1;
        JsonValue* change = content_changes->data.array_val.items[last_idx];
        
        if (change && change->type == JSON_OBJECT) {
            JsonValue* new_text = json_object_get(change, "text");
            
            if (new_text && new_text->type == JSON_STRING) {
                const char* text = json_string_value(new_text);
                if (text) {
                    lsp_document_update(server, json_string_value(uri),
                                       text,
                                       json_int_value(version));
                }
            }
        }
    }
    
    // 发布诊断
    if (server->diagnostic_provider) {
        lsp_publish_diagnostics(server, json_string_value(uri));
    }
    
    return NULL;
}

// 处理文档关闭通知
char* lsp_handle_did_close(LspServer* server, JsonValue* params) {
    JsonValue* text_doc = json_object_get(params, "textDocument");
    if (!text_doc) return NULL;
    
    JsonValue* uri = json_object_get(text_doc, "uri");
    if (!uri) return NULL;
    
    lsp_document_close(server, json_string_value(uri));
    
    return NULL;
}

// 主消息处理函数
char* lsp_handle_message(LspServer* server, const char* message) {
    JsonValue* req = json_parse(message);
    if (!req) {
        return lsp_create_error(0, LSP_ERROR_PARSE_ERROR, "Invalid JSON");
    }
    
    // 检查 jsonrpc 版本
    JsonValue* jsonrpc = json_object_get(req, "jsonrpc");
    if (!jsonrpc || strcmp(json_string_value(jsonrpc), "2.0") != 0) {
        json_free(req);
        return lsp_create_error(0, LSP_ERROR_INVALID_REQUEST, 
                                "Unsupported JSON-RPC version");
    }
    
    // 获取方法名
    JsonValue* method = json_object_get(req, "method");
    if (!method || method->type != JSON_STRING) {
        json_free(req);
        return lsp_create_error(0, LSP_ERROR_INVALID_REQUEST, 
                                "Missing or invalid method");
    }
    
    const char* method_name = json_string_value(method);
    JsonValue* params = json_object_get(req, "params");
    JsonValue* id_val = json_object_get(req, "id");
    int id = id_val ? json_int_value(id_val) : -1;
    
    char* response = NULL;
    
    // 分发处理
    if (strcmp(method_name, "initialize") == 0) {
        response = lsp_handle_initialize(server, id, params);
    }
    else if (strcmp(method_name, "shutdown") == 0) {
        response = lsp_handle_shutdown(server, id);
    }
    else if (strcmp(method_name, "exit") == 0) {
        response = lsp_handle_exit(server);
    }
    else if (strcmp(method_name, "textDocument/didOpen") == 0) {
        response = lsp_handle_did_open(server, params);
    }
    else if (strcmp(method_name, "textDocument/didChange") == 0) {
        response = lsp_handle_did_change(server, params);
    }
    else if (strcmp(method_name, "textDocument/didClose") == 0) {
        response = lsp_handle_did_close(server, params);
    }
    else if (strcmp(method_name, "textDocument/completion") == 0) {
        response = lsp_handle_completion(server, id, params);
    }
    else if (strcmp(method_name, "textDocument/hover") == 0) {
        response = lsp_handle_hover(server, id, params);
    }
    else if (strcmp(method_name, "textDocument/definition") == 0) {
		response = lsp_handle_definition(server, id, params);
	}
	else if (strcmp(method_name, "textDocument/diagnostic") == 0) {
		response = lsp_handle_document_diagnostic(server, id, params);
	}
	else if (strcmp(method_name, "textDocument/documentSymbol") == 0) {
		response = lsp_handle_document_symbol(server, id, params);
	}
	else if (strcmp(method_name, "textDocument/signatureHelp") == 0) {
		response = lsp_handle_signature_help(server, id, params);
	}
	else if (strcmp(method_name, "textDocument/references") == 0) {
		response = lsp_handle_references(server, id, params);
	}
	else if (strcmp(method_name, "textDocument/rename") == 0) {
		response = lsp_handle_rename(server, id, params);
	}
	else if (strcmp(method_name, "textDocument/foldingRange") == 0) {
		response = lsp_handle_folding_range(server, id, params);
	}
	else {
        // 未知方法
        if (id >= 0) {
            response = lsp_create_error(id, LSP_ERROR_METHOD_NOT_FOUND,
                                        "Method not found");
        }
    }
    
    json_free(req);
    return response;
}

// URI 转换工具
char* lsp_uri_to_path(const char* uri) {
    if (!uri) return NULL;
    
    const char* path_start = uri;
    
    // file:// 协议
    if (strncmp(uri, "file://", 7) == 0) {
        // Windows: file:///C:/path -> C:/path  或 file:///d%3A/path -> d:/path
        // Unix: file:///path -> /path
        if (uri[7] == '/' && strlen(uri) > 9 &&
            (uri[9] == ':' || (uri[9] == '%' && uri[10] == '3' && (uri[11] == 'A' || uri[11] == 'a')))) {
            // Windows 绝对路径
            path_start = uri + 8;
        } else {
            path_start = uri + 7;
        }
    }
    
    // URL 解码（处理 %3A -> : 等编码）
    const char* src = path_start;
    int len = strlen(src);
    char* result = (char*)malloc(len + 1);
    if (!result) return NULL;
    
    int dst = 0;
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            // 解析两位十六进制
            char hex[3] = {src[1], src[2], 0};
            char* endptr;
            long val = strtol(hex, &endptr, 16);
            if (endptr == hex + 2) {
                result[dst++] = (char)val;
                src += 3;
                continue;
            }
        }
        result[dst++] = *src++;
    }
    result[dst] = '\0';
    
    return result;
}

char* lsp_path_to_uri(const char* path) {
    if (!path) return NULL;
    
    // 检查是否已经是 URI
    if (strncmp(path, "file://", 7) == 0) {
        return strdup(path);
    }
    
    // 构建 file:// URI
    char* uri = (char*)malloc(strlen(path) + 8);
    if (!uri) return NULL;
    
    #ifdef _WIN32
    sprintf(uri, "file:///%s", path);
    #else
    sprintf(uri, "file://%s", path);
    #endif
    
    return uri;
}
