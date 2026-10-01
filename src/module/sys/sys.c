#include "include/lenolang.h"
#include "include/native.h"
#include "include/platform.h"    // utf16_to_utf8（Win）/ platform_self_exe_path（Linux/macOS）
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
// 字段：`output`（stdout+stderr 合并文本）、`code`（退出码；超时是 **124**，见 P6）。
// ⚠ 它属于**全局内置函数**（`_exec` 无模块前缀）⇒ 走的是内置返回规格通道
//   `native_register_meta_spec()`（v3.2.8 新开，此前内置通道只有 Kind 槽）。
static const NativeTypeSpec S_EXEC_STR = { NTYPE_STRING, NULL, NULL, NULL, 0, -1 };
static const NativeTypeSpec S_EXEC_INT = { NTYPE_INT,    NULL, NULL, NULL, 0, -1 };
static const NativeTypeSpec S_EXECRESULT_SPEC = { NTYPE_STRUCT, "ExecResult", NULL, NULL, 0, -1 };

static const char* EXECRESULT_FIELD_NAMES[] = { "output", "code" };
static const NativeTypeSpec* EXECRESULT_FIELD_TYPES[] = { &S_EXEC_STR, &S_EXEC_INT };
static const NativeStructSpec EXECRESULT_STRUCT_SPEC = {
    "sys", "ExecResult", 2, EXECRESULT_FIELD_NAMES, EXECRESULT_FIELD_TYPES
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

// _env(name) - 获取环境变量
// _env(name, value) - 设置环境变量，返回是否成功
static Value native_env(int argCount, Value* args) {
    // 个数（>= 1）由**编译期**把关（实测：`_env()` ⇒「参数过少: 最少 1 个, 实际 0」）⇒ 这里只守类型
    if (!val_is_string(args[0])) {
        return val_null();
    }

    ObjString* name = (ObjString*)val_as_obj(args[0]);

    if (argCount >= 2 && val_is_string(args[1])) {
        // 设置环境变量
        ObjString* value = (ObjString*)val_as_obj(args[1]);
        #ifdef _WIN32
            int result = _putenv_s(name->chars, value->chars);
        #else
            int result = setenv(name->chars, value->chars, 1);
        #endif
        return val_bool(result == 0);
    }

    // 获取环境变量
    const char* val = getenv(name->chars);
    if (val == NULL) {
        return val_null();
    }
    return val_obj((Object*)str_copy(val, (int)strlen(val)));
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

    const char* val = getenv(((ObjString*)val_as_obj(args[0]))->chars);
    if (val == NULL || val[0] == '\0') {
        return def ? val_obj((Object*)def) : val_obj((Object*)str_copy("", 0));
    }
    return val_obj((Object*)str_copy(val, (int)strlen(val)));
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

// _exec(cmd[, timeout_ms]) - 执行系统命令并返回 [stdout, exit_code]
//   timeout_ms > 0 ⇒ 超时**杀掉**子进程，退出码返回 **124**（GNU `timeout` 的惯例）；
//   不传 / 传 0 = 不限制（旧行为）。Windows 走 CreateProcessW + WaitForSingleObject +
//   TerminateProcess；POSIX 用 coreutils 的 `timeout` 包一层（没有该命令时会以 127 **响亮**失败，
//   不是静默）。加这个参数的原因（P6）：此前子进程一挂住，父进程乃至整套断言一起挂 ✗。
// 使用 + 临时文件避免 _popen 在大量并发调用时不稳定的问题
// Windows 版使用 UTF-16 转换以支持中文/Unicode 路径
static Value native_exec(int argCount, Value* args) {
    // 个数（>= 1）由编译期把关（实测：`_exec()` ⇒「参数过少: 最少 1 个」）⇒ 这里只守类型
    if (!val_is_string(args[0])) {
        return val_null();
    }
    ObjString* cmd = (ObjString*)val_as_obj(args[0]);

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
    swprintf(wtmp_path + tlen, MAX_PATH - tlen, L"leno_exec_%lu_%lu.txt",
             GetCurrentProcessId(), ++s_exec_seq);

    // 构建宽字符命令: cmd /c "command" > tmp 2>&1
    size_t full_len = wcslen(wcmd) + wcslen(wtmp_path) + 64;
    wchar_t* wfull = malloc(full_len * sizeof(wchar_t));
    if (!wfull) { free(wcmd); return val_null(); }
    swprintf(wfull, full_len, L"cmd /c %s > \"%s\" 2>&1", wcmd, wtmp_path);

    // ★ P6（2026-09-19）：带超时执行 —— 此前是 _wsystem（阻塞、无超时）⇒ 子进程挂住就把
    //   父进程（乃至整套断言）一起拖死 ✗。现在等不到就收掉，退出码返回 124。
    //   ⚠ 超时必须杀**整棵树**：`TerminateProcess` 只杀 `cmd` 自己，它的孙子（如 `ping`）会活下来
    //   继续握着临时文件 ✗ ⇒ 用 Job Object（`KILL_ON_JOB_CLOSE`）+ **CREATE_SUSPENDED**
    //   （先入 job 再放行，避免"还没入 job 就 fork 出孙子"这个竞态）✓。
    int timeout_ms = 0;
    if (argCount >= 2) {
        if (val_is_int(args[1])) timeout_ms = (int)val_as_int(args[1]);
        else if (val_is_num(args[1])) timeout_ms = (int)val_as_num(args[1]);
    }
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
            DeleteFileW(wtmp_path);
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

    // 读取临时文件 (宽字符路径)
    FILE* f = _wfopen(wtmp_path, L"rb");
    if (!f) { DeleteFileW(wtmp_path); return val_null(); }
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* output = malloc(flen + 1);
    if (!output) { fclose(f); DeleteFileW(wtmp_path); return val_null(); }
    fread(output, 1, flen, f);
    output[flen] = '\0';
    fclose(f);
    DeleteFileW(wtmp_path);

    // 返回 `ExecResult{ output, code }`（v3.2.8；见文件顶部的规格说明）
    ObjStruct* result = native_struct_new("ExecResult");
    if (!result) { free(output); return val_null(); }
    Value result_val = val_obj((Object*)result);
    gc_push_root(&result_val);   // 填字段期间它还没交出去 ⇒ 自己护住（str_copy 会分配）
    native_struct_set(result, "output", val_obj((Object*)str_copy(output, (int)flen)));
    native_struct_set(result, "code", val_int(rc));
    gc_pop_root();
    free(output);
    return result_val;
#else
    // ★ P6：POSIX 侧用 coreutils 的 `timeout` 包一层（超时同样得到退出码 124）。
    //   若系统没有 timeout（如部分 macOS 默认不带），命令会以 127 失败 —— 那是**响亮**的失败 ✓。
    int timeout_ms = 0;
    if (argCount >= 2) {
        if (val_is_int(args[1])) timeout_ms = (int)val_as_int(args[1]);
        else if (val_is_num(args[1])) timeout_ms = (int)val_as_num(args[1]);
    }
    // ★ 追加 `2>&1`：popen 只收 stdout，而编译器等子进程的诊断全走 stderr ⇒
    //   不合并的话 output 恒为空，与 Windows 分支（`cmd /c <cmd> > tmp 2>&1`）契约不一致 ✗
    //   （实测：assert/helpers/compiler.leno 断言「输出=空」、run_tests 的失败摘要也抓不到文案）
    size_t need = strlen(cmd->chars) + 64;
    char* run_buf = (char*)malloc(need);
    if (!run_buf) return val_null();
    if (timeout_ms > 0) {
        snprintf(run_buf, need, "timeout %d %s 2>&1", (timeout_ms + 999) / 1000, cmd->chars);
    } else {
        snprintf(run_buf, need, "%s 2>&1", cmd->chars);
    }
    char* run_cmd = run_buf;
    FILE* fp = popen(run_cmd, "r");
    if (!fp) { free(run_buf); return val_null(); }

    char buffer[256];
    size_t capacity = 1024, len = 0;
    char* output = malloc(capacity);
    if (!output) { pclose(fp); free(run_buf); return val_null(); }
    output[0] = '\0';

    while (fgets(buffer, sizeof(buffer), fp)) {
        size_t bl = strlen(buffer);
        if (len + bl + 1 > capacity) {
            capacity = (len + bl + 1) * 2;
            char* newer = realloc(output, capacity);
            if (!newer) { free(output); pclose(fp); free(run_buf); return val_null(); }
            output = newer;
        }
        memcpy(output + len, buffer, bl);
        len += bl;
        output[len] = '\0';
    }
    int status = pclose(fp);
    int rc = WIFEXITED(status) ? WEXITSTATUS(status) : 1;

    // 返回 `ExecResult{ output, code }`（v3.2.8；见文件顶部的规格说明）
    ObjStruct* result = native_struct_new("ExecResult");
    if (!result) { free(output); free(run_buf); return val_null(); }
    Value result_val = val_obj((Object*)result);
    gc_push_root(&result_val);
    native_struct_set(result, "output", val_obj((Object*)str_copy(output, (int)len)));
    native_struct_set(result, "code", val_int(rc));
    gc_pop_root();
    free(output);
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

// ==================== 初始化 ====================

void sys_init_globals(void) {
    // 注册全局 _args 函数（返回 Array[string]，0 个参数）
    vm_register_native("_args", native_args, 0, -1, -1, TYPE_ARRAY, TYPE_STRING, NULL);

    // 注册全局 _script 函数（返回 string，0 个参数）
    vm_register_native("_script", native_script, 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, NULL);

    // 注册全局 _executable 函数（返回 string，0 个参数）
    vm_register_native("_executable", native_executable, 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, NULL);

    // 注册全局 _gc 函数（返回 bool，0 或 1 个参数）
    vm_register_native("_gc", native_gc_control, -1, 0, 1, TYPE_BOOL, TYPE_UNKNOWN, NULL);

    // 注册全局 _os 函数（返回 string，0 个参数）
    vm_register_native("_os", native_os, 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, NULL);

    // 注册全局 _clear 函数（清屏，0 个参数）
    vm_register_native("_clear", native_clear, 0, -1, -1, TYPE_NULL, TYPE_UNKNOWN, NULL);

    // 注册全局 _console 函数（控制台显示控制，0 或 1 个参数）
    vm_register_native("_console", native_console, -1, 0, 1, TYPE_BOOL, TYPE_UNKNOWN, NULL);

    // 注册全局 _env 函数（环境变量：读 1 个参数 / 写 2 个参数；见 native_env 的三种返回）
    vm_register_native("_env", native_env, -1, 1, 2, TYPE_ANY, TYPE_UNKNOWN, NULL);
    // `_env` 的真实返回按形态分流：1 参读 ⇒ string、2 参写 ⇒ bool（2026-09-28）——
    //   内置通道的规格版声明（必须在 vm_register_native **之后**，同 `_exec`）
    native_register_meta_spec("_env", &S_ENV_SPEC);

    // 注册全局 _env_or(name, default)：总是 string 的读入口（取不到/空串 ⇒ default ✓）
    TypeKind env_or_params[] = {TYPE_STRING, TYPE_STRING};
    vm_register_native("_env_or", native_env_or, 2, -1, -1, TYPE_STRING, TYPE_UNKNOWN, env_or_params);

    // 注册全局 _exit 函数（退出程序，0 或 1 个参数）
    vm_register_native("_exit", native_exit, -1, 0, 1, TYPE_NULL, TYPE_UNKNOWN, NULL);

    // 注册全局 _pid 函数（进程ID，0 个参数）
    vm_register_native("_pid", native_pid, 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, NULL);

    // 注册全局 _arch 函数（CPU架构，0 个参数）
    vm_register_native("_arch", native_arch, 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, NULL);

    // 注册全局 _exec 函数（返回 [stdout_string, exit_code] 数组，混合类型用 TYPE_ANY）
    // ⚠ 参数个数是**可变 1..2**（`_exec(cmd[, timeout_ms])`，P6）—— 语义侧读的就是这里的
    //   arity/min/max：arity 写死成 1 时，`_exec(cmd, ms)` 会被判「参数数量不匹配: 期望 1」✗
    //   （实测踩过：新用例与 run_tests.leno 一起编译不过）。对照 `_gc` 的 (-1, 0, 1) 写法。
    vm_register_native("_exec", native_exec, -1, 1, 2, TYPE_ARRAY, TYPE_ANY, NULL);
    // `_exec` 的真实返回是 `ExecResult{ string output, int code }`（v3.2.8）——
    //   内置通道的规格版声明（必须在 vm_register_native **之后**：那是"查找并更新"）
    native_register_struct_spec(&EXECRESULT_STRUCT_SPEC);
    native_register_meta_spec("_exec", &S_EXECRESULT_SPEC);

    // 注册全局 _username 函数（用户名，0 个参数）
    vm_register_native("_username", native_username, 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, NULL);

    // 注册全局 _homedir 函数（主目录，0 个参数）
    vm_register_native("_homedir", native_homedir, 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, NULL);

    // 注册全局 _tmpdir 函数（临时目录，0 个参数）
    vm_register_native("_tmpdir", native_tmpdir, 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, NULL);

    // 注册全局 _sep 函数（路径分隔符，0 个参数）
    vm_register_native("_sep", native_sep, 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, NULL);
}
