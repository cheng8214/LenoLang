#include "include/lenolang.h"
#include "include/native.h"
#include "include/platform.h"    // utf16_to_utf8（Win）/ platform_self_exe_path（Linux/macOS）
#include "include/platform_thread.h"  // PlatformMutex（_spawn/_kill 的槽位表要加锁）
#include "include/leno_types.h"  // MAX_PATH_LEN
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifdef _WIN32
    #include <windows.h>
    #include <process.h>
#else
    #include <unistd.h>
    #include <pwd.h>
    #include <signal.h>     // kill / SIGKILL（_kill 收整个进程组）
    #include <errno.h>      // ESRCH（"整组都没了"也算收干净）；EWOULDBLOCK（_single_instance 判"已被占"）
    #include <sys/wait.h>   // waitpid（reap 子进程，别积僵尸）
    #include <fcntl.h>      // open(O_CREAT|O_RDWR) —— _single_instance 的锁文件
    #include <sys/file.h>   // flock(LOCK_EX|LOCK_NB) —— 内核持有、进程一死自动释放 ✓
    #ifdef __linux__
        #include <sys/prctl.h>  // PR_SET_PDEATHSIG —— "父进程一死就杀我"（仅 Linux 有）
    #endif
#endif

// 外部声明：main.c 中定义的命令行参数
extern int g_argc;
extern char** g_argv;

// ==================== ExecResult 的类型规格（`_exec` 的返回，v3.2.8） ====================
// `_exec(cmd[, timeout_ms])` 此前返回 `[output, code]` **二元数组** —— 两个元素**类型不齐**
//   （output 是 string、code 是 int）⇒ 同质的 `Array[T]` 表达不了，只能注册成 `Array[any]`，
//   调用点只好用 `r[0]` / `r[1]` **位置取值**、拿到的是 any（不可读，且顺序一改就静默错位）。
// 改结构体后字段类型**编译期已知**、零收窄 —— 与 `DirEntry`（dirs.walk）/ `DirInfo`（dirs.stat）/
//   `RegexMatch`（regexs.find_all）同一套做法（编译期字段表 + 运行期 ObjStructDef **同源**）。
// 字段：`output`（**合并**文本：stdout 后接 stderr）、`out` / `err`（两流**分开**，2026-10-14 补）、
//   `code`（退出码；超时是 **124**，见 P6）。
// ⚠ 它属于**全局内置函数**（`_exec` 无模块前缀）⇒ 走的是内置返回规格通道
//   `native_register_meta_spec()`（v3.2.8 新开，此前内置通道只有 Kind 槽）。
static const NativeTypeSpec S_EXEC_STR = { NTYPE_STRING, NULL, NULL, NULL, 0, -1 };
static const NativeTypeSpec S_EXEC_INT = { NTYPE_INT,    NULL, NULL, NULL, 0, -1 };
static const NativeTypeSpec S_EXECRESULT_SPEC = { NTYPE_STRUCT, "ExecResult", NULL, NULL, 0, -1 };

// ⚠ 为什么保留 `output` 又加 `out`/`err`（而不是把 output 改成"仅 stdout"）：
//   全仓 800+ 处调用点读的是 `output`，其中相当一部分**正靠它拿 stderr**（编译器的诊断全走 stderr）
//   ⇒ 改语义等于当场打掉它们；而"两个流分别是哪个"是确定信息、行序本来就不确定
//   （共享一个文件时行序由 OS 写入时机决定、跨平台还不一样）⇒ 拆成两组字段最诚实 ✓
static const char* EXECRESULT_FIELD_NAMES[] = { "output", "out", "err", "code" };
static const NativeTypeSpec* EXECRESULT_FIELD_TYPES[] = { &S_EXEC_STR, &S_EXEC_STR, &S_EXEC_STR, &S_EXEC_INT };
static const NativeStructSpec EXECRESULT_STRUCT_SPEC = {
    "sys", "ExecResult", 4, EXECRESULT_FIELD_NAMES, EXECRESULT_FIELD_TYPES
};

// ==================== _env 的类型规格（双形态内置，2026-09-28） ====================
// `_env(name)` 读 ⇒ string（不存在返回 null —— 按语言口径 null 可隐式赋具体类型，
//   与 Dict.get 1 参取 V 同口径）；`_env(name, value)` 写 ⇒ bool。
//   双形态返回类型不同 ⇒ 单一静态类型必谎报其一，Kind 槽也表达不了 arity 分支 ⇒
//   NTYPE_BY_ARITY 规格声明：实参数 < 2 取读形态，否则写形态（语义侧在调用点定型）。
static const NativeTypeSpec S_ENV_STR  = { NTYPE_STRING, NULL, NULL, NULL, 0, -1 };
static const NativeTypeSpec S_ENV_BOOL = { NTYPE_BOOL,   NULL, NULL, NULL, 0, -1 };
static const NativeTypeSpec S_ENV_SPEC = { NTYPE_BY_ARITY, NULL, &S_ENV_STR, &S_ENV_BOOL, 2, -1 };

// _args() - 返回脚本命令行参数数组（不包含解释器路径和脚本路径）
static Value native_args(int argCount, Value* args) {
    (void)argCount;
    (void)args;

    // 找到脚本路径的位置（第一个非选项参数；'--' 之后的一切都按位置参数看待）
    // ⚠ 这条边界必须与 main.c 的选项解析**同规矩**（P2）：脚本路径之后的参数（含 '-' 开头的）
    //   本就是"脚本参数"，两边若不按同一条边界切分，`_args()` 就会与解释器的判断不一致。
    int script_idx = -1;
    int past_ddash = 0;
    for (int i = 1; i < g_argc; i++) {
        if (!past_ddash && strcmp(g_argv[i], "--") == 0) { past_ddash = 1; continue; }
        if (past_ddash || g_argv[i][0] != '-') { script_idx = i; break; }
    }
    if (script_idx < 0) script_idx = g_argc;   // 没找到脚本（REPL 等）⇒ 参数数组为空
    
    // 检查是否是打包版（第一个非选项参数是 .exe 本身）
    int is_packaged = 0;
    if (script_idx < g_argc) {
        const char* first_arg = g_argv[script_idx];
        int len = (int)strlen(first_arg);
        is_packaged = (len > 4 && strcmp(first_arg + len - 4, ".exe") == 0);
    }
    
    // 打包版：跳过 exe 自身即可；普通模式：跳过解释器+脚本
    int start_idx = is_packaged ? 1 : script_idx + 1;
    int real_argc = g_argc - start_idx;
    if (real_argc < 0) real_argc = 0;
    
    ObjArray* arr = arr_new(real_argc);
    for (int i = 0; i < real_argc; i++) {
        arr_write(arr, i, val_obj((Object*)str_copy(
            g_argv[start_idx + i], 
            (int)strlen(g_argv[start_idx + i]))));
    }
    arr->count = real_argc;
    return val_obj((Object*)arr);
}

// _script() - 返回当前脚本路径（第一个非选项参数；'--' 之后按位置参数看待）
static Value native_script(int argCount, Value* args) {
    (void)argCount;
    (void)args;

    int past_ddash = 0;
    for (int i = 1; i < g_argc; i++) {
        if (!past_ddash && strcmp(g_argv[i], "--") == 0) { past_ddash = 1; continue; }
        if (past_ddash || g_argv[i][0] != '-') {
            return val_obj((Object*)str_copy(g_argv[i], (int)strlen(g_argv[i])));
        }
    }
    return val_null();
}

// _executable() - 返回**当前进程自身**可执行文件的绝对路径（跨平台 ✓）
//   · 未打包（`leno.exe app.leno`）⇒ 宿主解释器自己的路径（**不是脚本**！脚本用 `_script()` ✓）
//   · 单文件打包 ⇒ 应用 exe 自己的路径（注册表自启 / 快捷方式要的就是它 ✓）
// ⚠ 不返回 g_argv[0]：那只是"启动方写在命令行的第一个 token"—— 实测（2026-09-26）用相对写法
//   `build\leno.exe x.leno` 起子进程时它就是相对路径（cmd / 批处理 / 快捷方式都可能这么写）
//   ⇒ 拿它写 Run 键，登录时按登录进程的 CWD 解析必然错 ✗。平台真值：
//     Windows      = GetModuleFileNameW（**W 版** + utf16_to_utf8 ⇒ 中文路径不乱码 ✓）
//     Linux/macOS  = platform_self_exe_path（/proc/self/exe、_NSGetExecutablePath ✓）
//   取不到给 null（调用方自行降级 ✓；旧行为是把不可信的 argv[0] 原样给出，更糟 ✗）
static Value native_executable(int argCount, Value* args) {
    (void)argCount;
    (void)args;

    char path[MAX_PATH_LEN];
    path[0] = '\0';

#ifdef _WIN32
    wchar_t wexe[4096];                             // 缓冲口径与 dirs.c 的 script_dir() 一致 ✓
    DWORD n = GetModuleFileNameW(NULL, wexe, 4096);
    if (n == 0 || n >= 4096) { return val_null(); }  // 取不到 / 被截断（截断的值不可信 ⇒ 当失败 ✓）
    char* u8 = utf16_to_utf8(wexe);
    if (!u8) { return val_null(); }
    snprintf(path, sizeof(path), "%s", u8);
    free(u8);
#else
    if (!platform_self_exe_path(path, sizeof(path))) { return val_null(); }
#endif

    if (path[0] == '\0') { return val_null(); }
    return val_obj((Object*)str_copy(path, (int)strlen(path)));
}

// _gc(enabled) - 控制GC开关
static Value native_gc_control(int argCount, Value* args) {
    if (argCount > 0) {
        int enabled = 0;
        if (val_is_bool(args[0])) {
            enabled = val_as_bool(args[0]);
        } else if (val_is_int(args[0])) {
            enabled = (val_as_num(args[0]) != 0);
        }
        gc_set_enabled(enabled);
    }
    return val_bool(gc_get_enabled());
}

// _os() - 返回操作系统名称
static Value native_os(int argCount, Value* args) {
    (void)argCount;
    (void)args;

    #ifdef _WIN32
        return val_obj((Object*)str_copy("windows", 7));
    #elif defined(__APPLE__)
        #include "TargetConditionals.h"
        #if TARGET_OS_MAC
            return val_obj((Object*)str_copy("macos", 5));
        #else
            return val_obj((Object*)str_copy("ios", 3));
        #endif
    #elif defined(__linux__)
        return val_obj((Object*)str_copy("linux", 5));
    #elif defined(__unix__)
        return val_obj((Object*)str_copy("unix", 4));
    #else
        return val_obj((Object*)str_copy("unknown", 7));
    #endif
}

// _clear() - 清屏（跨平台）
static Value native_clear(int argCount, Value* args) {
    (void)argCount;
    (void)args;

    #ifdef _WIN32
        { int _r = system("cls"); (void)_r; }
    #else
        { int _r = system("clear"); (void)_r; }
    #endif
    return val_null();
}

// _console(show) - 显示/隐藏控制台窗口（Windows）
// show=true:  显示控制台（无控制台 exe 会先 AllocConsole 分配）
// show=false: 隐藏控制台
// 无参数:     返回当前可见状态
static Value native_console(int argCount, Value* args) {
    #ifdef _WIN32
        if (argCount > 0) {
            int show = 1;
            if (val_is_bool(args[0])) {
                show = val_as_bool(args[0]);
            } else if (val_is_int(args[0])) {
                show = (val_as_num(args[0]) != 0);
            }
            HWND hwnd = GetConsoleWindow();
            if (show) {
                // 显示控制台：无控制台版 exe（-mwindows）需要先 AllocConsole
                if (hwnd == NULL) {
                    if (AllocConsole()) {
                        // 重定向标准流到新控制台
                        freopen("CONIN$", "r", stdin);
                        freopen("CONOUT$", "w", stdout);
                        freopen("CONOUT$", "w", stderr);
                        // 设置 UTF-8 编码
                        SetConsoleOutputCP(CP_UTF8);
                        SetConsoleCP(CP_UTF8);
                        hwnd = GetConsoleWindow();
                    }
                }
                if (hwnd != NULL) {
                    ShowWindow(hwnd, SW_SHOW);
                }
            } else {
                // 隐藏控制台
                if (hwnd != NULL) {
                    ShowWindow(hwnd, SW_HIDE);
                }
            }
        }
        // 返回当前控制台窗口是否可见
        HWND hwnd = GetConsoleWindow();
        if (hwnd != NULL) {
            return val_bool(IsWindowVisible(hwnd));
        }
        return val_bool(false);
    #else
        // 非 Windows 平台，控制台隐藏功能不可用
        (void)argCount;
        (void)args;
        return val_bool(true);
    #endif
}

// ==================== 环境变量的 UTF-8 边界（2026-10-14） ====================
// 事故：`_env("X", "中文")` **写失败**（返回 false），子进程里该变量 UNDEF；而纯 ASCII 值、
//   含换行的值、8000 字节的长值全都好好的。定位过程见 docs/module_sys.md 的"已知坑"一节。
// 根因：老实现走 `_putenv_s`（**窄口**）。Windows 上它要把 UTF-8 字节按 **ANSI 代码页**
//   （本机 CP936）转成宽字符再设进环境块 —— UTF-8 的"中文"在 GBK 下不是合法序列 ⇒ 转换失败、
//   变量从没进 Win32 环境块 ⇒ 子进程什么都看不到 ✗。更坏的是它**同时**更新了 CRT 自己的窄副本
//   ⇒ 表现为"本进程能读回、子进程读不到"，很有欺骗性。
// 修法：Windows 侧把**环境块当权威 UTF-8 通道**：
//   · 写：非 ASCII 值直接 `SetEnvironmentVariableW`（UTF-8 → UTF-16，全程不经代码页）✓
//   · 读：先 `GetEnvironmentVariableW`（UTF-16 → UTF-8），拿不到再退窄口 `getenv` ✓
//   ⚠ 纯 ASCII 值**继续走原来的 `_putenv_s`**（一个字节都不改）：那条路会同步更新 CRT 的**窄副本**，
//     而进程内的窄口消费者（libcurl 等直接用 `getenv` 的 C 库）只认那个副本 —— 动它才是回归。
//     非 ASCII 值本来就没法用窄副本表达（这正是不走窄口的原因）⇒ 只设宽副本，窄侧任其保持旧值。
#ifdef _WIN32
static int env_ascii_only(const char* s) {
    for (; *s; s++) {
        if ((unsigned char)*s >= 0x80) return 0;
    }
    return 1;
}
#endif

// 读环境变量：宽口优先（权威、UTF-8 无损），拿不到再退窄口。
// 返回 malloc 的 UTF-8 串（调用方 free）；**变量不存在 ⇒ NULL**（与 `getenv` 同口径）
static char* env_read_utf8(const char* name) {
#ifdef _WIN32
    wchar_t* wname = utf8_to_utf16(name);
    if (wname) {
        DWORD need = GetEnvironmentVariableW(wname, NULL, 0);   // 含结尾 NUL 的宽字符数
        if (need > 0) {
            wchar_t* wbuf = (wchar_t*)malloc((size_t)need * sizeof(wchar_t));
            if (wbuf) {
                DWORD got = GetEnvironmentVariableW(wname, wbuf, need);
                free(wname);
                char* out = (got > 0 && got < need) ? utf16_to_utf8(wbuf) : NULL;
                free(wbuf);
                return out;   // 宽口命中 ⇒ 不再看窄口（权威副本已经作答）
            }
        }
        free(wname);
    }
#endif
    const char* val = getenv(name);
    if (val == NULL) return NULL;
    size_t n = strlen(val);
    char* out = (char*)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, val, n + 1);
    return out;
}

// 写环境变量：ASCII ⇒ 窄口（同时喂饱 CRT 窄副本），非 ASCII ⇒ 宽口（不经代码页）
static int env_write_utf8(const char* name, const char* value) {
#ifdef _WIN32
    if (env_ascii_only(name) && env_ascii_only(value)) {
        return _putenv_s(name, value) == 0;
    }
    wchar_t* wname = utf8_to_utf16(name);
    wchar_t* wvalue = utf8_to_utf16(value);
    if (!wname || !wvalue) {
        free(wname);
        free(wvalue);
        return 0;
    }
    int ok = SetEnvironmentVariableW(wname, wvalue) ? 1 : 0;
    free(wname);
    free(wvalue);
    return ok;
#else
    return setenv(name, value, 1) == 0;
#endif
}

// _env(name) - 获取环境变量
// _env(name, value) - 设置环境变量，返回是否成功
static Value native_env(int argCount, Value* args) {
    // 个数（>= 1）由**编译期**把关（实测：`_env()` ⇒「参数过少: 最少 1 个, 实际 0」）⇒ 这里只守类型
    if (!val_is_string(args[0])) {
        return val_null();
    }

    ObjString* name = (ObjString*)val_as_obj(args[0]);

    if (argCount >= 2 && val_is_string(args[1])) {
        // 设置环境变量（UTF-8 边界见 env_write_utf8）
        ObjString* value = (ObjString*)val_as_obj(args[1]);
        return val_bool(env_write_utf8(name->chars, value->chars) != 0);
    }

    // 获取环境变量
    char* val = env_read_utf8(name->chars);
    if (val == NULL) {
        return val_null();
    }
    Value out = val_obj((Object*)str_copy(val, (int)strlen(val)));
    free(val);
    return out;
}

// _env_or(name, default) - 取环境变量；**取不到或为空串** ⇒ `default`（返回 string）
// 为什么需要它：`_env` 一次函数有**三种**返回 —— 参数不对 ⇒ null、2 参形态是"设置"⇒ bool、
// 取不到 ⇒ null、取到 ⇒ string ⇒ 想安全地读一个字符串变量，必须写 `if x == null` 再 `_str(x)`；
// 而 `_str(null)` 得到的是字符串 `"null"`（不是空串 ✗，TraeSign 里实测踩过）。
// 这里给一个"**总是 string**"的入口，省掉那两步 ✓（与 `_env` 并存，不是替换：设置/判存在仍用 `_env`）。
// 空串也回 default：Windows 的 `cmd` 里 `set VAR=` 本身就是**删除**该变量 ⇒ 平台上基本不可达 ✓；
// 想"特意设成空"请继续用 `_env` ✓。
static Value native_env_or(int argCount, Value* args) {
    ObjString* def = NULL;
    if (argCount >= 2 && val_is_string(args[1])) {
        def = (ObjString*)val_as_obj(args[1]);
    }
    // 个数由编译期把关（`_env_or` 是**定长 2 参**：实测 0 参 ⇒「参数数量不匹配: 期望 2」）⇒ 这里只守类型
    if (!val_is_string(args[0])) {
        return def ? val_obj((Object*)def) : val_obj((Object*)str_copy("", 0));
    }

    char* val = env_read_utf8(((ObjString*)val_as_obj(args[0]))->chars);
    if (val == NULL || val[0] == '\0') {
        free(val);
        return def ? val_obj((Object*)def) : val_obj((Object*)str_copy("", 0));
    }
    Value out = val_obj((Object*)str_copy(val, (int)strlen(val)));
    free(val);
    return out;
}

// _exit(code) - 以指定退出码终止程序
static Value native_exit(int argCount, Value* args) {
    int code = 0;
    if (argCount > 0) {
        if (val_is_int(args[0])) {
            code = val_as_int(args[0]);
        } else if (val_is_float(args[0])) {
            code = (int)val_as_double(args[0]);
        }
    }
    exit(code);
    return val_null(); // 不会执行
}

// _pid() - 返回当前进程ID
static Value native_pid(int argCount, Value* args) {
    (void)argCount;
    (void)args;

    #ifdef _WIN32
        return val_int((int)GetCurrentProcessId());
    #else
        return val_int((int)getpid());
    #endif
}

// _arch() - 返回CPU架构
static Value native_arch(int argCount, Value* args) {
    (void)argCount;
    (void)args;

    #if defined(_M_X64) || defined(__x86_64__) || defined(__amd64__)
        return val_obj((Object*)str_copy("x64", 3));
    #elif defined(_M_IX86) || defined(__i386__) || defined(__i486__) || defined(__i586__) || defined(__i686__)
        return val_obj((Object*)str_copy("x86", 3));
    #elif defined(_M_ARM64) || defined(__aarch64__) || defined(__arm64__)
        return val_obj((Object*)str_copy("arm64", 5));
    #elif defined(_M_ARM) || defined(__arm__)
        return val_obj((Object*)str_copy("arm", 3));
    #elif defined(__riscv) && __riscv_xlen == 64
        return val_obj((Object*)str_copy("riscv64", 7));
    #elif defined(__riscv) && __riscv_xlen == 32
        return val_obj((Object*)str_copy("riscv32", 7));
    #else
        return val_obj((Object*)str_copy("unknown", 7));
    #endif
}

// ==================== _exec 的临时文件（两平台各一份） ====================
// 为什么两个流各用一个临时文件（而不是 `2>&1` 合成一个）：`out` / `err` 要**分开**交给脚本层。
//   代价说清楚：合并文本的行序不再是"真实交错"（共享一个文件时行序由 OS 的写入时机决定、
//   跨平台还不一样），但"这一行属于哪个流"是确定信息 ⇒ 宁可给确定的那一半。
//   `output` 字段仍然保留（= out 后接 err）⇒ 800+ 处老调用点零改动 ✓。

#ifdef _WIN32
// 读整个文件（宽路径）。成功返回 malloc 缓冲（调用方 free），失败 ⇒ NULL 且 *out_len = 0
static char* exec_read_file_wide(const wchar_t* path, long* out_len) {
    *out_len = 0;
    FILE* f = _wfopen(path, L"rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) n = 0;
    char* buf = (char*)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    *out_len = (long)got;
    fclose(f);
    return buf;
}

// out / err / in 三个临时路径（`win` 传 NULL ⇒ 不算 stdin 那份）
//   ⚠ GetTempPathW 返回的路径**自带结尾反斜杠** ⇒ `%ls` 之后直接接文件名 ✓
static void exec_temp_paths(const wchar_t* wtmp, unsigned long seq,
                            wchar_t* wout, wchar_t* werr, wchar_t* win) {
    unsigned long pid = (unsigned long)GetCurrentProcessId();
    swprintf(wout, MAX_PATH, L"%lsleno_exec_%lu_%lu.out", wtmp, pid, seq);
    swprintf(werr, MAX_PATH, L"%lsleno_exec_%lu_%lu.err", wtmp, pid, seq);
    if (win) swprintf(win, MAX_PATH, L"%lsleno_exec_%lu_%lu.in", wtmp, pid, seq);
}
#else
// POSIX 侧的同款（窄路径：临时目录来自 TMPDIR / /tmp）
static char* exec_read_file(const char* path, long* out_len) {
    *out_len = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) n = 0;
    char* buf = (char*)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    *out_len = (long)got;
    fclose(f);
    return buf;
}
#endif

// _exec(cmd[, timeout_ms])            - 执行系统命令并返回 ExecResult
// _exec(cmd, stdin_text)              - 同上，并把 `stdin_text` 喂给子进程的 **stdin**
// _exec(cmd, timeout_ms, stdin_text)  - 两者兼有
//   第 2 个实参**按类型**分流（int ⇒ 超时、string ⇒ stdin）—— 与 `_env` 按实参形态分流同一取向
//   （规格侧仍是 1..3 个实参，类型留给运行时判）。
//   timeout_ms > 0 ⇒ 超时**杀掉**子进程，退出码返回 **124**（GNU `timeout` 的惯例）；
//   不传 / 传 0 = 不限制（旧行为）。Windows 走 CreateProcessW + WaitForSingleObject +
//   TerminateProcess；POSIX 用 coreutils 的 `timeout` 包一层（没有该命令时会以 127 **响亮**失败，
//   不是静默）。加这个参数的原因（P6）：此前子进程一挂住，父进程乃至整套断言一起挂 ✗。
// 返回 `ExecResult{ output（合并）, out（仅 stdout）, err（仅 stderr）, code }`。
// 使用 + 临时文件避免 _popen 在大量并发调用时不稳定的问题
// Windows 版使用 UTF-16 转换以支持中文/Unicode 路径
static Value native_exec(int argCount, Value* args) {
    // 个数（1..3）由编译期把关（实测：`_exec()` ⇒「参数过少: 最少 1 个」）⇒ 这里只守类型
    if (!val_is_string(args[0])) {
        return val_null();
    }
    ObjString* cmd = (ObjString*)val_as_obj(args[0]);

    // 第 2 参按类型分流、第 3 参固定是 stdin（见函数头注释）
    int timeout_ms = 0;
    ObjString* stdin_text = NULL;
    if (argCount >= 2) {
        if (val_is_string(args[1])) stdin_text = (ObjString*)val_as_obj(args[1]);
        else if (val_is_int(args[1])) timeout_ms = (int)val_as_int(args[1]);
        else if (val_is_num(args[1])) timeout_ms = (int)val_as_num(args[1]);
    }
    if (argCount >= 3 && val_is_string(args[2])) stdin_text = (ObjString*)val_as_obj(args[2]);

#ifdef _WIN32
    // 将 UTF-8 cmd 转为 UTF-16 宽字符
    int wlen = MultiByteToWideChar(CP_UTF8, 0, cmd->chars, -1, NULL, 0);
    if (wlen <= 0) return val_null();
    wchar_t* wcmd = malloc(wlen * sizeof(wchar_t));
    if (!wcmd) return val_null();
    MultiByteToWideChar(CP_UTF8, 0, cmd->chars, -1, wcmd, wlen);

    // 获取临时目录 (宽字符)
    wchar_t wtmp_path[MAX_PATH];
    DWORD tlen = GetTempPathW(MAX_PATH, wtmp_path);
    if (tlen == 0 || tlen >= MAX_PATH - 64) { free(wcmd); return val_null(); }
    // ⚠ P6：临时文件名必须**每次调用唯一**。此前是 `leno_exec_<pid>.txt`（同一进程反复调用时
    //   复用同名）⇒ 只要上一次留下的子进程还握着这个文件，下一次 `cmd /c … > file` 就会败在
    //   「The process cannot access the file because it is being used by another process」✗
    //   实测：一次"被杀的子进程"之后，**后面所有** `_exec` 全坏，套件 241/330 挂 ✗。
    static unsigned long s_exec_seq = 0;
    wchar_t wout_path[MAX_PATH], werr_path[MAX_PATH], win_path[MAX_PATH];
    exec_temp_paths(wtmp_path, ++s_exec_seq, wout_path, werr_path, stdin_text ? win_path : NULL);

    // stdin ⇒ 先落盘（`< file` 是 cmd 认的写法；为什么不用管道见函数头）
    // ⚠ 用 `->len`（UTF-8 **字节**数）而不是 `strlen`：stdin 允许二进制内容
    if (stdin_text) {
        FILE* fin = _wfopen(win_path, L"wb");
        if (!fin) { free(wcmd); return val_null(); }
        fwrite(stdin_text->chars, 1, (size_t)stdin_text->len, fin);
        fclose(fin);
    }

    // 构建宽字符命令: cmd /c ( <cmd> ) > out 2> err [< in]
    // ⚠ 外面那对括号是**必需**的，不是装饰：`cmd /c A & B > f` 里重定向只绑在 `&` 的**后半句**上
    //   ⇒ A 的输出漏到父进程控制台、而 f 里只有 B 的输出（实测踩到：`echo OUT & echo ERR 1>&2`
    //   的结果是 out 里出现 ERR、err 空）✗。用 `( )` 把用户命令行整个包起来，重定向才作用于整组 ✓
    size_t full_len = wcslen(wcmd) + 2 * MAX_PATH + 64;
    wchar_t* wfull = malloc(full_len * sizeof(wchar_t));
    if (!wfull) { free(wcmd); if (stdin_text) DeleteFileW(win_path); return val_null(); }
    // ⚠ `%ls` 不是笔误：宽版 printf 的 `%s` 吃 wchar_t* 只是 MSVC/UCRT 的非标准扩展语义，
    //   换 msvcrt 版 MinGW / 开 __USE_MINGW_ANSI_STDIO 后 `%s` 是"窄串" ⇒ 宽路径只剩首字符（详见 vm_main.c）
    if (stdin_text) {
        swprintf(wfull, full_len, L"cmd /c ( %ls ) > \"%ls\" 2> \"%ls\" < \"%ls\"",
                 wcmd, wout_path, werr_path, win_path);
    } else {
        swprintf(wfull, full_len, L"cmd /c ( %ls ) > \"%ls\" 2> \"%ls\"", wcmd, wout_path, werr_path);
    }

    // ★ P6（2026-09-19）：带超时执行 —— 此前是 _wsystem（阻塞、无超时）⇒ 子进程挂住就把
    //   父进程（乃至整套断言）一起拖死 ✗。现在等不到就收掉，退出码返回 124。
    //   ⚠ 超时必须杀**整棵树**：`TerminateProcess` 只杀 `cmd` 自己，它的孙子（如 `ping`）会活下来
    //   继续握着临时文件 ✗ ⇒ 用 Job Object（`KILL_ON_JOB_CLOSE`）+ **CREATE_SUSPENDED**
    //   （先入 job 再放行，避免"还没入 job 就 fork 出孙子"这个竞态）✓。
    // （timeout / stdin 的解析已挪到函数开头 —— 第 2 参按类型分流）
    int rc = 0;
    {
        // Job Object：**只**为了超时时把 cmd + 它的孙子一起收掉（下面 TerminateJobObject）✓
        // ⚠ 绝不能再设 JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE：该标志在**句柄关闭**时杀掉 job 内
        //   所有还活着的进程 —— 而 `cmd /c start "" "文件"` 这类**异步**拉起是正当用法：
        //   cmd 立刻退出、被拉起的应用还在跑，句柄一关就把它**当场杀掉** ✗
        //   实测（2026-09-27）：`_exec("cmd /c start …ping…")` 返回后 tasklist 里已看不到 ping.exe；
        //   文件管理器"双击/右键打开文件"因此表现为**没反应**（应用起来又立刻被杀）✗
        //   超时路径不受影响：TerminateJobObject 本身就是收整棵树的手段 ✓
        HANDLE job = CreateJobObjectW(NULL, NULL);
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof(si));
        memset(&pi, 0, sizeof(pi));
        si.cb = sizeof(si);
        if (!CreateProcessW(NULL, wfull, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
            if (job) CloseHandle(job);
            free(wfull);
            free(wcmd);
            DeleteFileW(wout_path);
            DeleteFileW(werr_path);
            if (stdin_text) DeleteFileW(win_path);
            return val_null();
        }
        if (job) AssignProcessToJobObject(job, pi.hProcess);
        ResumeThread(pi.hThread);

        DWORD waited = WaitForSingleObject(pi.hProcess, timeout_ms > 0 ? (DWORD)timeout_ms : INFINITE);
        if (waited == WAIT_TIMEOUT) {
            if (job) TerminateJobObject(job, 124);     // 整棵树一起收 ✓
            else TerminateProcess(pi.hProcess, 124);
            WaitForSingleObject(pi.hProcess, 5000);    // 等它真的退出，别留下孤儿
            rc = 124;                                  // GNU timeout 的惯例
        } else {
            DWORD code = 0;
            if (!GetExitCodeProcess(pi.hProcess, &code)) code = 1;
            rc = (int)code;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (job) CloseHandle(job);                     // 只关句柄、**不杀**：命令自己拉起的后台进程该活着 ✓
    }
    free(wfull);
    free(wcmd);

    // 分别读取 out / err（异步拉起的孙子可能还握着文件 ⇒ 删不掉也无所谓，下次会盖写新名字）
    long olen = 0, elen = 0;
    char* out_buf = exec_read_file_wide(wout_path, &olen);
    char* err_buf = exec_read_file_wide(werr_path, &elen);
    DeleteFileW(wout_path);
    DeleteFileW(werr_path);
    if (stdin_text) DeleteFileW(win_path);
    // 读不到（文件没生成 / 打不开）⇒ 按**空串**处理：退出码仍然是真实信息，
    //   比整个返回 null 更有用（"命令跑了但没输出"与"根本没跑起来"由 code 区分）
    if (!out_buf) { out_buf = (char*)malloc(1); if (out_buf) out_buf[0] = '\0'; olen = 0; }
    if (!err_buf) { err_buf = (char*)malloc(1); if (err_buf) err_buf[0] = '\0'; elen = 0; }
    if (!out_buf || !err_buf) { free(out_buf); free(err_buf); return val_null(); }

    long total = olen + elen;
    char* merged = (char*)malloc((size_t)total + 1);
    if (!merged) { free(out_buf); free(err_buf); return val_null(); }
    memcpy(merged, out_buf, (size_t)olen);
    memcpy(merged + olen, err_buf, (size_t)elen);
    merged[total] = '\0';

    // 返回 `ExecResult{ output, out, err, code }`（v3.2.8 / 流分流与 stdin 于 2026-10-14 补）
    ObjStruct* result = native_struct_new("ExecResult");
    if (!result) { free(merged); free(out_buf); free(err_buf); return val_null(); }
    Value result_val = val_obj((Object*)result);
    gc_push_root(&result_val);   // 填字段期间它还没交出去 ⇒ 自己护住（str_copy 会分配）
    native_struct_set(result, "output", val_obj((Object*)str_copy(merged, (int)total)));
    native_struct_set(result, "out", val_obj((Object*)str_copy(out_buf, (int)olen)));
    native_struct_set(result, "err", val_obj((Object*)str_copy(err_buf, (int)elen)));
    native_struct_set(result, "code", val_int(rc));
    gc_pop_root();
    free(merged);
    free(out_buf);
    free(err_buf);
    return result_val;
#else
    // ★ P6：POSIX 侧用 coreutils 的 `timeout` 包一层（超时同样得到退出码 124）。
    //   若系统没有 timeout（如部分 macOS 默认不带），命令会以 127 失败 —— 那是**响亮**的失败 ✓。
    //   （timeout / stdin 的解析已在函数开头完成）
    // ★ 两个流**分开**重定向到临时文件 —— 与 Windows 分支同契约：
    //   `output` = out 后接 err；`out` / `err` 各自独立；stdin 走 `'< in'`
    //   ⚠ 从 popen 换成 system()：popen 只能收一个流，分流就做不到（sh 同一个入口，退出码同口径 ✓）
    const char* tmpdir = getenv("TMPDIR");
    if (!tmpdir || !*tmpdir) tmpdir = "/tmp";
    static unsigned long s_exec_seq = 0;
    unsigned long seq = ++s_exec_seq;
    char out_path[512], err_path[512], in_path[512];
    // 重定向尾巴 `" > 'out' 2> 'err' < 'in'"`：三个路径各占一份 + 引号/空格/空终止的余量
    char tail[sizeof(out_path) + sizeof(err_path) + sizeof(in_path) + 64];
    snprintf(out_path, sizeof(out_path), "%s/leno_exec_%d_%lu.out", tmpdir, (int)getpid(), seq);
    snprintf(err_path, sizeof(err_path), "%s/leno_exec_%d_%lu.err", tmpdir, (int)getpid(), seq);
    snprintf(in_path,  sizeof(in_path),  "%s/leno_exec_%d_%lu.in",  tmpdir, (int)getpid(), seq);

    // stdin ⇒ 先落盘（用 `->len` UTF-8 字节数，允许二进制内容）
    if (stdin_text) {
        FILE* fin = fopen(in_path, "wb");
        if (!fin) return val_null();
        fwrite(stdin_text->chars, 1, (size_t)stdin_text->len, fin);
        fclose(fin);
    }

    size_t need = strlen(cmd->chars) + sizeof(out_path) + sizeof(err_path) + sizeof(in_path) + 64;
    char* run_buf = (char*)malloc(need);
    if (!run_buf) { if (stdin_text) unlink(in_path); return val_null(); }
    // ⚠ 与 Windows 分支同款：用户命令行必须**整组**包进 `( )`，否则 `A & B > f` 的重定向只绑后半句 ✗
    //   `timeout` 放在分组**里面**（`timeout N ( cmd )` 是 sh 的语法错）
    if (timeout_ms > 0) {
        snprintf(run_buf, need, "( timeout %d %s )", (timeout_ms + 999) / 1000, cmd->chars);
    } else {
        snprintf(run_buf, need, "( %s )", cmd->chars);
    }
    if (stdin_text) {
        snprintf(tail, sizeof(tail), " > '%s' 2> '%s' < '%s'", out_path, err_path, in_path);
    } else {
        snprintf(tail, sizeof(tail), " > '%s' 2> '%s'", out_path, err_path);
    }
    size_t rlen = strlen(run_buf);
    if (rlen + strlen(tail) + 1 > need) {
        // 分组括号/超时前缀都可能把命令撑长 ⇒ 按实际长度再取一次缓冲（不猜）
        char* grown = (char*)realloc(run_buf, rlen + strlen(tail) + 1);
        if (!grown) { free(run_buf); if (stdin_text) unlink(in_path); return val_null(); }
        run_buf = grown;
        need = rlen + strlen(tail) + 1;
    }
    strncat(run_buf, tail, need - rlen - 1);

    int status = system(run_buf);
    int rc = (status == -1) ? 1 : (WIFEXITED(status) ? WEXITSTATUS(status) : 1);

    long olen = 0, elen = 0;
    char* out_buf = exec_read_file(out_path, &olen);
    char* err_buf = exec_read_file(err_path, &elen);
    unlink(out_path);
    unlink(err_path);
    if (stdin_text) unlink(in_path);
    if (!out_buf) { out_buf = (char*)malloc(1); if (out_buf) out_buf[0] = '\0'; olen = 0; }
    if (!err_buf) { err_buf = (char*)malloc(1); if (err_buf) err_buf[0] = '\0'; elen = 0; }
    if (!out_buf || !err_buf) { free(out_buf); free(err_buf); free(run_buf); return val_null(); }

    long total = olen + elen;
    char* merged = (char*)malloc((size_t)total + 1);
    if (!merged) { free(out_buf); free(err_buf); free(run_buf); return val_null(); }
    memcpy(merged, out_buf, (size_t)olen);
    memcpy(merged + olen, err_buf, (size_t)elen);
    merged[total] = '\0';

    // 返回 `ExecResult{ output, out, err, code }`（v3.2.8 / 流分流与 stdin 于 2026-10-14 补）
    ObjStruct* result = native_struct_new("ExecResult");
    if (!result) { free(merged); free(out_buf); free(err_buf); free(run_buf); return val_null(); }
    Value result_val = val_obj((Object*)result);
    gc_push_root(&result_val);
    native_struct_set(result, "output", val_obj((Object*)str_copy(merged, (int)total)));
    native_struct_set(result, "out", val_obj((Object*)str_copy(out_buf, (int)olen)));
    native_struct_set(result, "err", val_obj((Object*)str_copy(err_buf, (int)elen)));
    native_struct_set(result, "code", val_int(rc));
    gc_pop_root();
    free(merged);
    free(out_buf);
    free(err_buf);
    free(run_buf);
    return result_val;
#endif
}

// _username() - 返回当前登录用户名
static Value native_username(int argCount, Value* args) {
    (void)argCount;
    (void)args;

    #ifdef _WIN32
        char username[256];
        DWORD size = sizeof(username);
        if (GetUserNameA(username, &size)) {
            return val_obj((Object*)str_copy(username, (int)strlen(username)));
        }
    #else
        struct passwd* pw = getpwuid(getuid());
        if (pw != NULL) {
            return val_obj((Object*)str_copy(pw->pw_name, (int)strlen(pw->pw_name)));
        }
        // 回退：使用环境变量
        const char* user = getenv("USER");
        if (user != NULL) {
            return val_obj((Object*)str_copy(user, (int)strlen(user)));
        }
    #endif
    return val_null();
}

// _homedir() - 返回用户主目录路径
static Value native_homedir(int argCount, Value* args) {
    (void)argCount;
    (void)args;

    #ifdef _WIN32
        // 优先使用 USERPROFILE
        const char* home = getenv("USERPROFILE");
        if (home != NULL) {
            return val_obj((Object*)str_copy(home, (int)strlen(home)));
        }
        // 回退：组合 HOMEDRIVE 和 HOMEPATH
        const char* drive = getenv("HOMEDRIVE");
        const char* path = getenv("HOMEPATH");
        if (drive != NULL && path != NULL) {
            int total_len = (int)(strlen(drive) + strlen(path));
            char* full = (char*)malloc(total_len + 1);
            if (full != NULL) {
                sprintf(full, "%s%s", drive, path);
                ObjString* result = str_copy(full, total_len);
                free(full);
                return val_obj((Object*)result);
            }
        }
    #else
        const char* home = getenv("HOME");
        if (home != NULL) {
            return val_obj((Object*)str_copy(home, (int)strlen(home)));
        }
        struct passwd* pw = getpwuid(getuid());
        if (pw != NULL) {
            return val_obj((Object*)str_copy(pw->pw_dir, (int)strlen(pw->pw_dir)));
        }
    #endif
    return val_null();
}

// _tmpdir() - 返回系统临时目录路径
static Value native_tmpdir(int argCount, Value* args) {
    (void)argCount;
    (void)args;

    #ifdef _WIN32
        char buf[MAX_PATH];
        UINT len = GetTempPathA(MAX_PATH, buf);
        if (len > 0) {
            return val_obj((Object*)str_copy(buf, (int)len));
        }
    #else
        const char* tmp = getenv("TMPDIR");
        if (tmp != NULL) {
            return val_obj((Object*)str_copy(tmp, (int)strlen(tmp)));
        }
        return val_obj((Object*)str_copy("/tmp", 4));
    #endif
    return val_null();
}

// _sep() - 返回路径分隔符
static Value native_sep(int argCount, Value* args) {
    (void)argCount;
    (void)args;

    #ifdef _WIN32
        return val_obj((Object*)str_copy("\\", 1));
    #else
        return val_obj((Object*)str_copy("/", 1));
    #endif
}

// ==================== 进程句柄：_spawn / _kill（2026-10-07） ====================
// 由来（真实事故）：2026-10-07 本机两次蓝屏 `0xEF CRITICAL_PROCESS_DIED`，两次都发生在"批量拉起
//   无头浏览器"的那一瞬间（`%TEMP%` 里 `_exec` 临时文件的断点、WER 的 `Kernel_ef_*` 报告、
//   以及 exec 临时文件的内容都对得上）。其中一个**结构性**原因：异步拉起长驻进程时**拿不到句柄** ——
//   此前只有 `_exec("cmd /c start …")`：`start` 让子进程脱离父子关系，父进程手上什么都没有 ⇒
//   事后收尾只能"回系统里重新找回来"（从 `netstat` 文本里猜 PID、或按命令行扫全场）。
//   实测后果：**一次用例漏下 45 个 msedge 进程**，反复跑一路堆上去。
//
// 语义：
//   `_spawn(exe, args)` —— **异步**拉起 `exe`（`args` 原样拼在其后），返回**整数句柄**（> 0；失败 -1）
//   `_kill(handle)`     —— 收掉该句柄对应的**整棵进程树**；true = 确实收了一个有效句柄
//
// ★ 核心保证（本次改动的目的）：Windows 侧把子进程放进 **Job Object** 并设
//   `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` ⇒ **Leno 进程一退出（正常退出 / 崩溃 / 被 taskkill），
//   句柄随之关闭，内核替我们把 job 里所有进程收掉** —— 孤儿进程在结构上不可能出现 ✓
//   ⚠ 与 `_exec` 的 job 正好相反：那边**绝不能**设这个标志（`cmd /c start` 拉起的应用要活下来，
//     见 `assert/test_exec_timeout.leno` ④ 那条守卫）。两个 job 各自为自己的语义负责，互不影响。
//   POSIX 侧：`setsid()` 让子进程自成进程组 ⇒ `_kill` 用 `kill(-pgid)` 收整棵树；
//     "父死子死"只有 Linux 有（`PR_SET_PDEATHSIG`，作用于直接子进程），macOS 得靠显式 `_kill`
//     —— 这正是 Playwright 改走"CDP 管道"的原因（管道一断浏览器自己退）。
//
// ⚠ 句柄是**槽位下标 + 1**，不是裸指针：
//   · 0 / 负数一律无效 ⇒ Leno 侧"结构体字段默认 0"不会误伤某个槽位；
//   · Windows 的 HANDLE 不暴露到脚本层（脚本层只看见一个不透明整数）。
// ⚠ 槽位表加锁：Leno 有 `threads` 模块，多线程可能同时 spawn/kill。
// ⚠ 槽位满（256）时**宁可失败并把刚拉起的那个收掉**，也绝不留没人管的进程。
#define LENO_SPAWN_MAX 256

typedef struct {
    int used;
    int pid;          // 直接子进程 pid（查表/调试用）
#ifdef _WIN32
    HANDLE job;       // 可空：建 job/入 job 失败时退化成"只有显式 _kill 才收"
    HANDLE proc;      // 必须：关它就等于交还句柄
#else
    int pgid;         // 子进程组（setsid 后 = 子进程 pid）
#endif
} LenoSpawnSlot;

static LenoSpawnSlot s_spawn_slots[LENO_SPAWN_MAX];
static PlatformMutex s_spawn_mutex;
static int s_spawn_mutex_inited = 0;

static void spawn_init_once(void) {
    if (!s_spawn_mutex_inited) {
        platform_mutex_init(&s_spawn_mutex);
        s_spawn_mutex_inited = 1;
    }
}

static void spawn_lock(void) {
    spawn_init_once();
    platform_mutex_lock(&s_spawn_mutex);
}

static void spawn_unlock(void) {
    platform_mutex_unlock(&s_spawn_mutex);
}

// 槽位里的进程是否已退出（非阻塞探测）。调用方持锁。
static int spawn_slot_exited_locked(int i) {
#ifdef _WIN32
    if (!s_spawn_slots[i].proc) return 1;
    return WaitForSingleObject(s_spawn_slots[i].proc, 0) == WAIT_OBJECT_0;
#else
    int st = 0;
    return waitpid((pid_t)s_spawn_slots[i].pid, &st, WNOHANG) == (pid_t)s_spawn_slots[i].pid;
#endif
}

// 清空槽位。Windows 侧**关 job 句柄本身就是收尾**（KILL_ON_JOB_CLOSE 会顺手收掉残留子孙）。
// 调用方持锁。
static void spawn_slot_clear_locked(int i) {
#ifdef _WIN32
    if (s_spawn_slots[i].proc) CloseHandle(s_spawn_slots[i].proc);
    if (s_spawn_slots[i].job)  CloseHandle(s_spawn_slots[i].job);
#else
    int st = 0;
    waitpid((pid_t)s_spawn_slots[i].pid, &st, WNOHANG);   // 已退出的顺手 reap，别积僵尸
#endif
    memset(&s_spawn_slots[i], 0, sizeof(s_spawn_slots[i]));
}

// 取一个空槽位；顺便回收**已退出**的（否则长跑的爬虫会把 256 个槽位用光）。调用方持锁。
static int spawn_alloc_slot_locked(void) {
    for (int i = 0; i < LENO_SPAWN_MAX; i++) {
        if (s_spawn_slots[i].used && spawn_slot_exited_locked(i)) {
            spawn_slot_clear_locked(i);
        }
    }
    for (int i = 0; i < LENO_SPAWN_MAX; i++) {
        if (!s_spawn_slots[i].used) return i;
    }
    return -1;
}

#ifndef _WIN32
// exe 是否**真的能执行**（带路径则直接判；不带路径按 PATH 找，同 execvp）。
//   ⚠ 为什么需要它：POSIX 侧一律经 `/bin/sh -c "exe" args` 拉起 ⇒ 程序不存在时 sh 只退出
//     127，而 `_spawn` 早已"成功"返回句柄了 ✗ —— 与 Windows 侧 CreateProcessW 的语义
//     （程序不存在 ⇒ 直接失败）不一致。契约是「拉不起来 ⇒ -1」⇒ 这里对齐 ✓
static int spawn_exe_runnable(const char* exe) {
    if (!exe || !exe[0]) return 0;
    if (strchr(exe, '/')) return access(exe, X_OK) == 0;
    const char* path = getenv("PATH");
    if (!path) return 0;
    const char* p = path;
    for (;;) {
        const char* colon = strchr(p, ':');
        const char* dir = p;
        size_t dlen = colon ? (size_t)(colon - p) : strlen(p);
        if (dlen == 0) { dir = "."; dlen = 1; }   // PATH 里的空项 = 当前目录（同 execvp）
        char buf[MAX_PATH_LEN];
        if (dlen + strlen(exe) + 2 <= sizeof(buf)) {
            memcpy(buf, dir, dlen);
            buf[dlen] = '/';
            strcpy(buf + dlen + 1, exe);
            if (access(buf, X_OK) == 0) return 1;
        }
        if (!colon) break;
        p = colon + 1;
    }
    return 0;
}
#endif

// _spawn(exe, args) —— 异步拉起，返回句柄（> 0）；失败 -1
// ⚠ 两个参数分开（而不是收一条命令行）是为了让"哪个是可执行文件"没有歧义：
//   Job / 进程组都要认准那**一个**进程，再从命令行字符串里解析一遍就多一次出错的机会。
static Value native_spawn(int argCount, Value* args) {
    if (argCount < 1 || !val_is_string(args[0])) return val_int(-1);
    const char* exe = ((ObjString*)val_as_obj(args[0]))->chars;
    const char* extra = "";
    if (argCount >= 2 && val_is_string(args[1])) extra = ((ObjString*)val_as_obj(args[1]))->chars;
    if (exe[0] == '\0') return val_int(-1);

    size_t need = strlen(exe) + strlen(extra) + 8;
    char* cmd = (char*)malloc(need);
    if (!cmd) return val_int(-1);
    snprintf(cmd, need, "\"%s\" %s", exe, extra);

#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, cmd, -1, NULL, 0);
    if (wlen <= 0) { free(cmd); return val_int(-1); }
    wchar_t* wcmd = (wchar_t*)malloc((size_t)wlen * sizeof(wchar_t));
    if (!wcmd) { free(cmd); return val_int(-1); }
    MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, wlen);
    free(cmd);

    // Job Object + KILL_ON_JOB_CLOSE —— "随父进程死"的内核保证就在这里
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
        memset(&jeli, 0, sizeof(jeli));
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
            CloseHandle(job);
            job = NULL;    // 拿不到保证也照常跑（少了"随父死"而已），不因此让调用方失败
        }
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    // CREATE_SUSPENDED：**先入 job 再放行**，堵住"还没入 job 就 fork 出孙子"这个竞态（同 `_exec`）
    // CREATE_NO_WINDOW：控制台程序不弹黑窗（批量拉起时很要紧 —— 爬虫不该在用户桌面上闪窗）
    if (!CreateProcessW(NULL, wcmd, NULL, NULL, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        if (job) CloseHandle(job);
        free(wcmd);
        return val_int(-1);
    }
    free(wcmd);
    if (job && !AssignProcessToJobObject(job, pi.hProcess)) {
        CloseHandle(job);
        job = NULL;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    spawn_lock();
    int idx = spawn_alloc_slot_locked();
    if (idx < 0) {
        spawn_unlock();
        // 槽位满：把刚拉起的收掉 —— 绝不留没人管的进程（那正是本函数要消灭的东西）
        if (job) TerminateJobObject(job, 137);
        else TerminateProcess(pi.hProcess, 137);
        CloseHandle(pi.hProcess);
        if (job) CloseHandle(job);
        return val_int(-1);
    }
    s_spawn_slots[idx].used = 1;
    s_spawn_slots[idx].pid  = (int)pi.dwProcessId;
    s_spawn_slots[idx].job  = job;
    s_spawn_slots[idx].proc = pi.hProcess;
    spawn_unlock();
    return val_int(idx + 1);
#else
    // 程序不存在/不可执行 ⇒ 直接失败（与 Windows 的 CreateProcessW 同口径；见上面的 helper 说明）
    if (!spawn_exe_runnable(exe)) { free(cmd); return val_int(-1); }
    pid_t parent_pid = getpid();   // fork 前记下"我"，供子进程堵 PDEATHSIG 的竞态（见下）
    pid_t pid = fork();
    if (pid < 0) { free(cmd); return val_int(-1); }
    if (pid == 0) {
        setsid();                          // 自成会话/进程组 ⇒ 父侧 kill(-pid) 收整棵树
#ifdef __linux__
        prctl(PR_SET_PDEATHSIG, SIGKILL);  // Linux 独有：父进程一死就杀我
#endif
        // ⚠ 竞态堵漏：若父进程在 fork 之后、prctl 之前就退出了，PDEATHSIG 会被"绑到"当时的
        //   收尸进程（subreaper，实测 1528）上 ⇒「父死子死」永不触发 ✗（实测 flaky：3 次漏 2 次，
        //   漏掉的探针还握着父进程的管道 ⇒ 整套 assert 卡死）。比对 getppid() 即可发现"父已不在"
        //   ⇒ 当场退，绝不让孤儿 exec 出来 ✓
        if (getppid() != parent_pid) _exit(127);
        // fork 之后到 exec 之间只能用**异步信号安全**的函数（setsid/prctl/getppid/execl/_exit 都是）✓
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(127);                        // exec 失败（连 /bin/sh 都没有）—— 响亮地失败
    }
    free(cmd);

    spawn_lock();
    int idx = spawn_alloc_slot_locked();
    if (idx < 0) {
        spawn_unlock();
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
        return val_int(-1);
    }
    s_spawn_slots[idx].used = 1;
    s_spawn_slots[idx].pid  = (int)pid;
    s_spawn_slots[idx].pgid = (int)pid;
    spawn_unlock();
    return val_int(idx + 1);
#endif
}

// _kill(handle) —— 收掉句柄对应的整棵进程树；true = 有效句柄
// ⚠ 先把槽位腾空**再**动手：里面要等最多 3 s，持着锁等会把别的线程一起挡住。
//   槽位一腾空，重复调用自然返回 false（幂等），也不会出现二次关句柄。
static Value native_kill(int argCount, Value* args) {
    if (argCount < 1) return val_bool(0);
    long h = 0;
    if (val_is_int(args[0])) h = (long)val_as_int(args[0]);
    else if (val_is_num(args[0])) h = (long)val_as_num(args[0]);
    if (h <= 0 || h > LENO_SPAWN_MAX) return val_bool(0);

    int idx = (int)h - 1;
    spawn_lock();
    if (!s_spawn_slots[idx].used) { spawn_unlock(); return val_bool(0); }
#ifdef _WIN32
    HANDLE job  = s_spawn_slots[idx].job;
    HANDLE proc = s_spawn_slots[idx].proc;
    memset(&s_spawn_slots[idx], 0, sizeof(s_spawn_slots[idx]));
    spawn_unlock();

    int ok = 0;
    if (job) ok = TerminateJobObject(job, 137) != 0;     // 整棵树一起收
    else if (proc) ok = TerminateProcess(proc, 137) != 0;
    if (proc) WaitForSingleObject(proc, 3000);           // 有界等待：别把调用方拖住
    if (proc) CloseHandle(proc);
    if (job) CloseHandle(job);                           // 关 job ⇒ 万一还有子孙，一并收掉
    return val_bool(ok);
#else
    int pid  = s_spawn_slots[idx].pid;
    int pgid = s_spawn_slots[idx].pgid;
    memset(&s_spawn_slots[idx], 0, sizeof(s_spawn_slots[idx]));
    spawn_unlock();

    int ok = 0;
    int r1 = kill(-pgid, SIGKILL);                       // 整个进程组
    int e1 = errno;
    if (r1 == 0) ok = 1;
    else if (e1 == ESRCH) ok = 1;                        // 整组都没了 ⇒ 句柄有效、树已收干净
    else if (kill(pid, SIGKILL) == 0) ok = 1;
    for (int i = 0; i < 200; i++) {                      // 有界 reap（最多 ~2 s）
        int st = 0;
        if (waitpid(pid, &st, WNOHANG) == pid) break;
        platform_sleep_ms(10);
    }
    return val_bool(ok);
#endif
}

// ==================== 单实例互斥：_single_instance([name])（2026-10-15） ====================
//
// 由来（用户实测）：`leno -p` 打出的**单文件 exe**，双击两次就会**起两个程序** —— 托盘应用 /
//   清理工具这类当场打架：两边同写一份配置与日志、托盘里两个图标、两边抢同一台设备/同一批文件。
//   任何系统都**不会**替我们判重：macOS 只对 **.app bundle** 由 Finder 保证单实例（双击第二次
//   只是激活已运行的那个），而我们产出的是**裸二进制** ⇒ 一样开两个；Linux 双击 .desktop 同理。
//
// 语义：`_single_instance([name])`
//   · 返回 **true**  = 本进程拿到了锁（它是第一个实例，可以继续跑）
//   · 返回 **false** = 已经有实例在跑（调用方该干的是：把已有窗口顶到前台，然后自己退出）
//   · `name` 省略 / 空串 / 全空白 ⇒ 用**本进程 exe 的绝对路径**当名字
//     ⇒ "同一个程序只许一个实例"，**零配置**（打包 exe 双击场景要的就是这个 ✓）
//   · `name` 给了 ⇒ 用它（同一个 exe 想分多个身份时用，如 "LenoDumate" / "LenoDumate-portable"）
//   ⚠ 同一进程内重复调**同一个名字**也返回 false —— 因为事实就是"已经有一个实例在跑了"，
//     不管那个实例是不是自己（要"重入"请自己开个名字 ✓）
//
// ★ 两端都用**内核持有**的锁 ⇒ 进程正常退出 / 崩溃 / 被 taskkill，内核自动释放
//   ⇒ **不存在"上次崩了留下死锁、以后永远起不来"** —— 这正是选内核锁、而不是"锁文件存不存在"
//   那种做法的唯一理由（后者崩溃后必须靠人删文件，且"谁先创建"没有原子性 ✗）
//     · Windows：命名互斥体 `CreateMutexW` + `GetLastError() == ERROR_ALREADY_EXISTS`
//     · POSIX  ：`open(..., O_CREAT|O_RDWR, 0600)` + `flock(fd, LOCK_EX|LOCK_NB)`
//       （`flock` 的锁挂在**打开文件描述**上 ⇒ 同进程再 open 一次同一个文件也会被拒 ✓
//         与 Windows 那边"重复调用返回 false"行为一致 ✓）
//   ⚠ 锁的寿命 = 句柄/fd 的寿命 ⇒ 句柄**一直留着**（存进下表，进程退出时才由内核交还）✓
//
// ⚠ 失败（极少见：句柄/fd/文件建不出来）⇒ **放行**（返回 true）并往 stderr 打一行警告：
//   宁可"可能多起一个"，也不要"用户双击了、程序却起不来"（fail-open ✓ —— 单实例是锦上添花，
//   不是运行的前提）。
// ⚠ 名字要**净化**：Windows 的互斥体名**不许含反斜杠**（那被当成 `Global\` / `Local\` 命名空间
//   分隔符）；POSIX 的文件名不许含 `/`。⇒ 非法字符一律换成 `_`。
// ⚠ 过长（> 120）⇒ 截断并**接一段 FNV-1a 哈希后缀**：不接哈希的话，两个长名字截断后会**撞成同
//   一把锁** ⇒ 两个不同的应用互相顶掉（比"没做单实例"更难查 ✗）。
// ⚠ Windows 侧**只对"默认名"（exe 路径）折叠大小写**：NTFS 路径大小写不敏感 ⇒ 同一个 exe
//   写成 `D:\App\a.exe` / `d:\app\A.EXE` 必须落到同一把锁 ✓。
//   **显式给的名字一律原样用**（两端都不折）—— 否则 `_single_instance("MyApp")` 与
//   `_single_instance("myapp")` 在 Windows 上是同一把锁、在 Linux 上却是两把 ⇒ 同一份代码
//   两端行为不一致 ✗（这种"平台间悄悄不同"最难查）。
#define LENO_INSTANCE_MAX 16
#define LENO_INSTANCE_NAME_MAX 160

#ifdef _WIN32
static HANDLE s_instance_handles[LENO_INSTANCE_MAX];
#else
static int s_instance_fds[LENO_INSTANCE_MAX];
#endif
static int s_instance_count = 0;

// 名字净化（见上面两条 ⚠）；fold_lower 只在 Windows 侧为 1
static void instance_sanitize(const char* in, char* out, size_t outsz, int fold_lower) {
    unsigned long hash = 2166136261u;   // FNV-1a 32：只在需要截断时用来补后缀 ✓
    const char* p;
    for (p = in; *p; p++) {
        hash = (hash ^ (unsigned char)*p) * 16777619u;
    }

    size_t limit = outsz - 10;          // 给 "_%08lx" 留位（多余位一律不用）
    int truncated = 0;
    if (strlen(in) > 120) { limit = 120; truncated = 1; }

    size_t j = 0;
    for (p = in; *p && j < limit; p++) {
        unsigned char c = (unsigned char)*p;
        if (fold_lower && c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        // ⚠ 保留集**必须同时含大小写**：fold_lower=0 时大写字母不会被折成小写，
        //   只写 `a-z` 的话它们会被当成非法字符换成 `_` ✗（2026-10-15 实测：显式名字
        //   "LenoTestInstMainX" 被净成 "...X→_"，与另一个名字撞成同一把锁 ⇒ 用例当场红 ✓）
        int keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                   || c == '_' || c == '-' || c == '.';
        out[j++] = keep ? (char)c : '_';
    }
    if (truncated) {
        snprintf(out + j, outsz - j, "_%08lx", hash);
        j += 9;
    }
    out[j] = '\0';
    if (j == 0) snprintf(out, outsz, "default");
}

// 解析出"名字"与"记 pid 的文件路径"（两个内置函数共用；失败返回 0）
//   · 名字：`given` 为空 ⇒ 本进程 exe 的绝对路径（Windows 侧才折大小写，见上）
//   · pid 文件：Windows 用 `%TEMP%\leno-inst-<名字>.pid`（TEMP 本来就是"每个用户一份" ✓）；
//     POSIX 用**锁文件本身**（`_single_instance` 拿到锁后就把 pid 写进去 ✓）
static int instance_resolve(const char* given, char* nm, size_t nmsz,
                            char* pid_path, size_t ppsz) {
    char base[MAX_PATH_LEN];
    base[0] = '\0';
    if (given) {
        snprintf(base, sizeof(base), "%s", given);
    } else {
#ifdef _WIN32
        wchar_t wexe[4096];
        DWORD n = GetModuleFileNameW(NULL, wexe, 4096);
        if (n > 0 && n < 4096) {
            char* u8 = utf16_to_utf8(wexe);
            if (u8) { snprintf(base, sizeof(base), "%s", u8); free(u8); }
        }
#else
        if (!platform_self_exe_path(base, sizeof(base))) base[0] = '\0';
#endif
    }
    // ⚠ 取不到自身路径 ⇒ 让调用方**放行**，绝不退回某个"公共名字"
    //   （那会让**不相干的应用**互相顶掉 ✗✗）
    if (base[0] == '\0') return 0;

#ifdef _WIN32
    instance_sanitize(base, nm, nmsz, given ? 0 : 1);   // 只对默认名（路径）折大小写 ✓
    wchar_t wtmp[MAX_PATH_LEN];
    if (GetTempPathW(MAX_PATH_LEN, wtmp) == 0) return 0;
    char* tmp = utf16_to_utf8(wtmp);                    // 走 UTF-8：用户名带中文也不能乱码 ✓
    if (!tmp) return 0;
    int ok = snprintf(pid_path, ppsz, "%sleno-inst-%s.pid", tmp, nm) < (int)ppsz;
    free(tmp);
    return ok;
#else
    instance_sanitize(base, nm, nmsz, 0);
    // 放"每个用户一份"的地方：XDG_RUNTIME_DIR（systemd 会给，登录时清空 ✓）优先，否则 TMPDIR / /tmp
    const char* dir = getenv("XDG_RUNTIME_DIR");
    if (!dir || dir[0] == '\0') dir = getenv("TMPDIR");
    if (!dir || dir[0] == '\0') dir = "/tmp";
    // ⚠ 文件名里带 uid ⇒ 多用户机器上互不影响（"全局单实例"对桌面应用没意义 ✗）
    return snprintf(pid_path, ppsz, "%s/leno-%s-%u.lock", dir, nm, (unsigned)getuid()) < (int)ppsz;
#endif
}

// 把 pid 写进 pid 文件 —— **只服务于 `_instance_activate`（找持锁进程的窗口）** ✓
//   ⚠ 判重**不看**这个文件（全靠锁本身）⇒ 内容陈旧/写失败都不影响单实例语义 ✓
static void instance_write_pid(const char* path, int pid) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d\n", pid);
    if (len <= 0) return;
#ifdef _WIN32
    wchar_t* wp = utf8_to_utf16(path);
    if (!wp) return;
    FILE* f = _wfopen(wp, L"w");
    free(wp);
#else
    FILE* f = fopen(path, "w");
#endif
    if (!f) return;
    fwrite(buf, 1, (size_t)len, f);
    fclose(f);
}

// 读 pid 文件（读不到 / 内容不是数字 ⇒ 0 ⇒ 调用方"如实说找不到" ✓）
static int instance_read_pid(const char* path) {
#ifdef _WIN32
    wchar_t* wp = utf8_to_utf16(path);
    if (!wp) return 0;
    FILE* f = _wfopen(wp, L"r");
    free(wp);
#else
    FILE* f = fopen(path, "r");
#endif
    if (!f) return 0;
    int pid = 0;
    if (fscanf(f, "%d", &pid) != 1) pid = 0;
    fclose(f);
    return pid;
}

static Value native_single_instance(int argCount, Value* args) {
    const char* given = NULL;
    if (argCount >= 1 && val_is_string(args[0])) {
        const char* s = ((ObjString*)val_as_obj(args[0]))->chars;
        const char* q = s;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '\0') given = s;      // 空 / 全空白 ⇒ 当"没给"（用默认名）
    }

    char nm[LENO_INSTANCE_NAME_MAX];
    char path[MAX_PATH_LEN * 2];
    if (!instance_resolve(given, nm, sizeof(nm), path, sizeof(path))) {
        fprintf(stderr, "[_single_instance] 解析不出名字/路径 ⇒ 本次不做单实例检查（放行）\n");
        return val_bool(1);
    }
    if (s_instance_count >= LENO_INSTANCE_MAX) {
        fprintf(stderr, "[_single_instance] 实例锁已达上限 %d ⇒ 本次跳过（放行）\n", LENO_INSTANCE_MAX);
        return val_bool(1);
    }

#ifdef _WIN32
    char full[LENO_INSTANCE_NAME_MAX + 8];
    // `Local\` = 本登录会话内唯一（桌面应用要的正是"每个登录用户一份" ✓）——
    //   用 `Global\` 会跨会话/跨用户互斥（服务与多用户机器上语义不对 ✗）
    snprintf(full, sizeof(full), "Local\\%s", nm);
    wchar_t* wname = utf8_to_utf16(full);
    if (!wname) {
        fprintf(stderr, "[_single_instance] 名字转宽字符失败 ⇒ 本次跳过（放行）\n");
        return val_bool(1);
    }
    // bInitialOwner = FALSE：只问"这个名字被占住没有"，不要求立刻持有（要的是判重，不是递归锁 ✓）
    HANDLE h = CreateMutexW(NULL, FALSE, wname);
    DWORD err = GetLastError();
    free(wname);
    if (!h) {
        fprintf(stderr, "[_single_instance] CreateMutexW 失败（err=%lu）⇒ 本次跳过（放行）\n",
                (unsigned long)err);
        return val_bool(1);
    }
    if (err == ERROR_ALREADY_EXISTS) {
        CloseHandle(h);                 // 别人的 ⇒ 关掉我们自己拿到的这个引用，别让计数虚涨
        return val_bool(0);
    }
    s_instance_handles[s_instance_count++] = h;   // ★ 留着不放：它就是锁本身 ✓
    instance_write_pid(path, (int)GetCurrentProcessId());   // 供 _instance_activate 找窗口 ✓
    return val_bool(1);
#else
    int fd = open(path, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        fprintf(stderr, "[_single_instance] 打不开锁文件 %s（errno=%d）⇒ 本次跳过（放行）\n", path, errno);
        return val_bool(1);
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int e = errno;
        close(fd);
        if (e == EWOULDBLOCK) return val_bool(0);   // 别人拿着 ⇒ 这就是"已有实例在跑" ✓
        fprintf(stderr, "[_single_instance] flock 失败（errno=%d）⇒ 本次跳过（放行）\n", e);
        return val_bool(1);
    }
    // 顺手把 pid 写进**锁文件本身**（`_instance_activate` 会来读它找窗口 ✓；`cat` 一下也知道谁占着 ✓）
    //   ⚠ 判重不看内容（全靠 flock）⇒ 写不进去也不影响锁的语义 ✓
    if (ftruncate(fd, 0) == 0) {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
        if (len > 0) {
            ssize_t w = write(fd, buf, (size_t)len);
            (void)w;
        }
    }
    s_instance_fds[s_instance_count++] = fd;        // ★ 留着不关：关掉就等于放锁 ✓
    return val_bool(1);
#endif
}

// ==================== 唤起已有实例的窗口：_instance_activate([name])（2026-10-15） ====================
//
// 为什么要它（**没有它，单实例反而更差**）：加了 `_single_instance` 之后，"双击快捷方式"会变成
//   "闪一下就没"—— 用户看到的现象从"窗口到最前面了"退化成"点了没反应" ✗。所以两者是**一对**：
//     if not _single_instance() { _instance_activate(); _exit(0) }
//   （就是 .NET 里 `Mutex` + `SetForegroundWindow` 那套的等价位 ✓）
//
// 语义：找"**当前持锁那个进程**"的**第一个可见顶层窗口**，显示（SW_RESTORE）并请求顶到前台；
//   找到并请求过 ⇒ true；找不到（没窗口 / 拿不到 pid / 平台不支持）⇒ false —— **绝不假装** ✗
//   · Windows：持锁进程的 pid 从 pid 文件读（`_single_instance` 写的 ✓）⇒ `EnumWindows` 按 **pid**
//     找窗口（**不**按窗口标题/类名找：标题会变、类名会撞，两者都比 pid 不可靠 ✗）
//   · POSIX  ：`wmctrl -lp` 按 pid 找窗口、`wmctrl -i -a <id>` 激活；**wmctrl 不在 ⇒ false**
//     （X11 要连 X 服务器才能 XRaiseWindow，Wayland 更是没有跨 WM 的做法 ⇒ 不装工具就只能如实说做不到 ✓）
//   ⚠ **自己持有锁**时调用它没有意义（不会有人需要"激活自己"）⇒ 直接 false ✓
//   ⚠ Windows 的前台切换受"前台窗口锁"限制 ⇒ 我们只能**请求**（ShowWindow + SetForegroundWindow
//     + BringWindowToTop）：返回 true 的含义是"**找到了窗口并且请求过了**"，
//     **不代表**它一定成了前台 ✓（要更强的话得走 AllowSetForegroundWindow / 附加输入队列那一套 ✗）
#ifdef _WIN32
typedef struct {
    DWORD pid;
    HWND found;
} LenoInstFind;

// 只要**可见的顶层**窗口（有 owner 的是对话框/工具窗 ⇒ 不是主窗口 ✓）
static BOOL CALLBACK leno_instance_enum_cb(HWND hwnd, LPARAM lp) {
    LenoInstFind* ctx = (LenoInstFind*)lp;
    DWORD wpid = 0;
    GetWindowThreadProcessId(hwnd, &wpid);
    if (wpid != ctx->pid) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != NULL) return TRUE;
    ctx->found = hwnd;
    return FALSE;                        // 找到第一个就停手 ✓
}
#endif

static Value native_instance_activate(int argCount, Value* args) {
    const char* given = NULL;
    if (argCount >= 1 && val_is_string(args[0])) {
        const char* s = ((ObjString*)val_as_obj(args[0]))->chars;
        const char* q = s;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '\0') given = s;
    }

    char nm[LENO_INSTANCE_NAME_MAX];
    char path[MAX_PATH_LEN * 2];
    if (!instance_resolve(given, nm, sizeof(nm), path, sizeof(path))) return val_bool(0);

#ifdef _WIN32
    int owner = instance_read_pid(path);
    if (owner <= 0) return val_bool(0);                          // 没有 pid ⇒ 找不到（不猜 ✓）
    if (owner == (int)GetCurrentProcessId()) return val_bool(0); // 自己拿着锁 ⇒ 无需"唤起自己"

    LenoInstFind ctx;
    ctx.pid = (DWORD)owner;
    ctx.found = NULL;
    EnumWindows(leno_instance_enum_cb, (LPARAM)&ctx);
    if (ctx.found == NULL) return val_bool(0);                   // 那个进程没有可见顶层窗口 ✓
    ShowWindow(ctx.found, SW_RESTORE);                           // 最小化的先还原 ✓
    SetForegroundWindow(ctx.found);
    BringWindowToTop(ctx.found);
    return val_bool(1);
#else
    int owner = instance_read_pid(path);
    if (owner <= 0) return val_bool(0);
    if (owner == (int)getpid()) return val_bool(0);

    // 探测 + 使用都交给 wmctrl（**真二进制**；不在就如实 false ✓ —— 与 LenoSys 的 openWith 同取向）
    if (system("command -v wmctrl >/dev/null 2>&1") != 0) return val_bool(0);
    FILE* p = popen("wmctrl -lp", "r");
    if (!p) return val_bool(0);
    char wid[32];
    wid[0] = '\0';
    char line[512];
    while (fgets(line, sizeof(line), p) != NULL) {
        unsigned long w = 0;
        long desk = 0, wpid = 0;
        // 形态：`0x03400007  0 12345  host  Window title`（窗口id 桌面  pid 主机 标题 ✓）
        if (sscanf(line, "%lx %ld %ld", &w, &desk, &wpid) >= 3 && (int)wpid == owner) {
            snprintf(wid, sizeof(wid), "0x%lx", w);
            break;
        }
    }
    pclose(p);
    if (wid[0] == '\0') return val_bool(0);

    char cmd[96];
    snprintf(cmd, sizeof(cmd), "wmctrl -i -a %s", wid);
    return val_bool(system(cmd) == 0);
#endif
}

// ==================== 初始化 ====================

void sys_init_globals(void) {
    // 注册全局 _args 函数（返回 Array[string]，0 个参数）
    vm_register_native("_args", native_args, TYPE_ARRAY, TYPE_STRING, NATIVE_FIXED_NONE(0));

    // 注册全局 _script 函数（返回 string，0 个参数）
    vm_register_native("_script", native_script, TYPE_STRING, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));

    // 注册全局 _executable 函数（返回 string，0 个参数）
    vm_register_native("_executable", native_executable, TYPE_STRING, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));

    // 注册全局 _gc 函数（返回 bool，0 或 1 个参数）
    vm_register_native("_gc", native_gc_control, TYPE_BOOL, TYPE_UNKNOWN, NATIVE_VARARG(0, 1, 0, NULL, TYPE_ANY));

    // 注册全局 _os 函数（返回 string，0 个参数）
    vm_register_native("_os", native_os, TYPE_STRING, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));

    // 注册全局 _clear 函数（清屏，0 个参数）
    vm_register_native("_clear", native_clear, TYPE_NULL, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));

    // 注册全局 _console 函数（控制台显示控制，0 或 1 个参数）
    vm_register_native("_console", native_console, TYPE_BOOL, TYPE_UNKNOWN, NATIVE_VARARG(0, 1, 0, NULL, TYPE_ANY));

    // 注册全局 _env 函数（环境变量：读 1 个参数 / 写 2 个参数；见 native_env 的三种返回）
    vm_register_native("_env", native_env, TYPE_ANY, TYPE_UNKNOWN, NATIVE_VARARG(1, 2, 0, NULL, TYPE_ANY));
    // `_env` 的真实返回按形态分流：1 参读 ⇒ string、2 参写 ⇒ bool（2026-09-28）——
    //   内置通道的规格版声明（必须在 vm_register_native **之后**，同 `_exec`）
    native_register_meta_spec("_env", &S_ENV_SPEC);

    // 注册全局 _env_or(name, default)：总是 string 的读入口（取不到/空串 ⇒ default ✓）
    TypeKind env_or_params[] = {TYPE_STRING, TYPE_STRING};
    vm_register_native("_env_or", native_env_or, TYPE_STRING, TYPE_UNKNOWN, NATIVE_FIXED(env_or_params));

    // 注册全局 _exit 函数（退出程序，0 或 1 个参数）
    vm_register_native("_exit", native_exit, TYPE_NULL, TYPE_UNKNOWN, NATIVE_VARARG(0, 1, 0, NULL, TYPE_ANY));

    // 注册全局 _pid 函数（进程ID，0 个参数）
    vm_register_native("_pid", native_pid, TYPE_INT, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));

    // 注册全局 _arch 函数（CPU架构，0 个参数）
    vm_register_native("_arch", native_arch, TYPE_STRING, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));

    // 注册全局 _exec 函数（返回 ExecResult，见文件顶部的规格说明）
    // ⚠ 参数个数是**可变 1..3**（`_exec(cmd[, timeout_ms][, stdin_text])`，P6 / 2026-10-14）——
    //   语义侧读的就是这里的 arity/min/max：
    //     · arity 写死成 1 ⇒ `_exec(cmd, ms)` 会被判「参数数量不匹配: 期望 1」✗（实测踩过）
    //     · max 停在 2   ⇒ 带 stdin 的三参形态同样编译不过 ✗
    //   第 2 参按**类型**分流（int ⇒ 超时、string ⇒ stdin），规格层表达不了就留给运行时判 ✓
    vm_register_native("_exec", native_exec, TYPE_ARRAY, TYPE_ANY, NATIVE_VARARG(1, 3, 0, NULL, TYPE_ANY));
    // `_exec` 的真实返回是 `ExecResult{ string output, string out, string err, int code }`（v3.2.8）——
    //   内置通道的规格版声明（必须在 vm_register_native **之后**：那是"查找并更新"）
    native_register_struct_spec(&EXECRESULT_STRUCT_SPEC);
    native_register_meta_spec("_exec", &S_EXECRESULT_SPEC);

    // 注册全局 _username 函数（用户名，0 个参数）
    vm_register_native("_username", native_username, TYPE_STRING, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));

    // 注册全局 _homedir 函数（主目录，0 个参数）
    vm_register_native("_homedir", native_homedir, TYPE_STRING, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));

    // 注册全局 _tmpdir 函数（临时目录，0 个参数）
    vm_register_native("_tmpdir", native_tmpdir, TYPE_STRING, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));

    // 注册全局 _sep 函数（路径分隔符，0 个参数）
    vm_register_native("_sep", native_sep, TYPE_STRING, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));

    // 注册全局 _spawn(exe, args) / _kill(handle) —— **拿句柄的异步拉起**（2026-10-07）
    //   `_spawn` 返回整数句柄（> 0；失败 -1），`_kill` 收掉整棵进程树（true = 有效句柄）。
    //   为什么要它：`_exec("cmd /c start …")` 会**丢掉句柄**，事后只能猜 PID 去杀（实测漏 45 个进程）；
    //   `_spawn` 的 job 带 KILL_ON_JOB_CLOSE ⇒ 父进程一退出，内核替我们收掉整棵树（详见函数头注释）。
    //   ⚠ 参数是**两段**：`exe` 是可执行文件、`args` 是参数串（不写成单条命令行，避免"哪部分是程序"歧义）。
    TypeKind spawn_params[] = { TYPE_STRING, TYPE_STRING };
    vm_register_native("_spawn", native_spawn, TYPE_INT, TYPE_UNKNOWN, NATIVE_FIXED(spawn_params));
    TypeKind kill_params[] = { TYPE_INT };
    vm_register_native("_kill", native_kill, TYPE_BOOL, TYPE_UNKNOWN, NATIVE_FIXED(kill_params));
    spawn_init_once();

    // 注册全局 _single_instance([name]) —— **单实例互斥**（2026-10-15，见函数头注释）
    //   `_single_instance()`      ⇒ 名字 = 本 exe 路径（"同一个程序只许一个实例" ✓）
    //   `_single_instance("app")` ⇒ 显式名字（同一个 exe 分多个身份 ✓）
    //   返回 bool：true = 我拿到了（第一个实例）；false = 已有实例在跑
    //   ⚠ 参数个数 0..1；那 1 个按 string 查表（VARARG 的 min 可以小于声明数 ✓，同 strings.to_hex）
    TypeKind instance_params[] = { TYPE_STRING };
    vm_register_native("_single_instance", native_single_instance, TYPE_BOOL, TYPE_UNKNOWN,
                       NATIVE_VARARG(0, 1, 1, instance_params, TYPE_ANY));

    // 注册全局 _instance_activate([name]) —— 把**持锁那个进程**的窗口顶到前台（见函数头注释）
    //   ⚠ 与 `_single_instance` 是**一对**：只有它俩一起用，"双击第二次"才是"切到已有窗口"
    //     而不是"闪一下就没" ✓
    //   返回 bool：true = 找到窗口并请求过（**不代表**一定成了前台 ✓）；false = 没找到/平台不支持
    vm_register_native("_instance_activate", native_instance_activate, TYPE_BOOL, TYPE_UNKNOWN,
                       NATIVE_VARARG(0, 1, 1, instance_params, TYPE_ANY));
}
