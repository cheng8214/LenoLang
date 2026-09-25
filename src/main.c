#include "include/lenolang.h"
#include "include/leno_ast.h"
#include "include/leno_lexer.h"
#include "include/leno_parser.h"
#include "include/leno_semantic.h"
#include "include/leno_codegen.h"
#include "include/leno_optimize.h"
#include "include/leno_serialize.h"
#include "include/leno_dce.h"
#include "include/native.h"
#include "include/module_compiler.h"
#include "include/module_loader.h"
#include "include/module_symbol_table.h"
#include "include/leno_package.h"
#include "include/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <sys/wait.h>   // WIFEXITED/WEXITSTATUS（pack_vm_check_lenb 解 system() 的返回值）
#include <dirent.h>
#endif

// 版本信息
#define LENO_VERSION "1.0.0"

// 全局标志
int debugMode = 0;
static int pauseMode = 0;
static int compileMode = 0;
static int packMode = 0;
static int initMode = 0;
static int installMode = 0;
static char* debugOutFile = NULL;  // --debug-out 指定的输出文件路径
static char* packOutDir = NULL;    // -o/--pack-dir 指定的打包输出目录（NULL ⇒ <源码目录>/dist）
static int onefileMode = 0;        // --onefile：把原生库与 resource.toml 声明的资源一起内嵌进 exe
int g_use_gui_vm = 0;  // 语义分析阶段检测到 _console(false) 时置为 1

// -p 打包时选哪个 VM 基底（PE 子系统由 prepend 进去的 vm 数据决定，打包后改不了）。
// 优先级：显式开关 > 脚本里的 _console(false) 自动检测 > 默认控制台。
// 只有 Windows 有两个变体（leno_vm.exe / leno_vm_gui.exe），其它平台只有 leno_vm。
#define PACK_CONSOLE_AUTO  0   // 未指定：沿用 g_use_gui_vm（即 _console(false) 自动检测）
#define PACK_CONSOLE_FORCE 1   // --console：强制控制台版
#define PACK_CONSOLE_NONE  2   // --no-console：强制无控制台版（-mwindows）
static int packConsoleMode = PACK_CONSOLE_AUTO;

// 字节码输出重定向辅助（Windows 用 _dup/_dup2 保存/恢复 stdout 句柄）
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

static FILE* debug_redirect_stdout(const char* filename) {
    if (!filename) return NULL;
    fflush(stdout);
#ifdef _WIN32
    int backup_fd = _dup(_fileno(stdout));
    FILE* backup = _fdopen(backup_fd, "w");
    // UTF-8 路径支持：转宽字符后用 _wfreopen
    wchar_t* wpath = utf8_to_utf16(filename);
    if (wpath) {
        _wfreopen(wpath, L"w", stdout);
        free(wpath);
    } else {
        freopen(filename, "w", stdout);
    }
    return backup;
#else
    FILE* backup = stdout;
    FILE* _r = freopen(filename, "w", stdout); (void)_r;
    return backup;
#endif
}

static void debug_restore_stdout(FILE* backup) {
    if (!backup) return;
    fflush(stdout);
#ifdef _WIN32
    _dup2(_fileno(backup), _fileno(stdout));
    fclose(backup);
    // 恢复 stdout 的行缓冲模式
    setvbuf(stdout, NULL, _IOLBF, 0);
#else
    { FILE* _r = freopen("/dev/tty", "w", stdout); (void)_r; }
    fclose(backup);
#endif
}

// 统一输出全部字节码：主程序 + 所有已加载模块
// 在编译全部完成后调用，确保缓存命中/未命中的模块都能输出
static void debug_dump_all_bytecode(Chunk* main_chunk) {
    FILE* backup = debug_redirect_stdout(debugOutFile);

    // 1. 输出主程序字节码
    if (main_chunk) {
        disassembleChunk(main_chunk, "主程序");
    }

    // 2. 遍历所有已加载模块，输出每个模块的 init_chunk
    int mod_count = loaded_modules_get_count();
    for (int i = 0; i < mod_count; i++) {
        ObjModule* mod = loaded_modules_get(i);
        if (!mod) continue;
        if (mod->init_chunk && mod->init_chunk->len > 0) {
            char label[BUFFER_MEDIUM];
            snprintf(label, sizeof(label), "模块: %s (%s)",
                     mod->name ? mod->name : "<unnamed>",
                     mod->source_path ? mod->source_path : "?");
            disassembleChunk(mod->init_chunk, label);
        }
    }

    debug_restore_stdout(backup);
}

// 命令行参数（供 _args() 全局函数使用）
int g_argc = 0;
char** g_argv = NULL;

// 设置控制台UTF-8编码（Windows）
static void setupConsole(void) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
}

// 打印版本信息
static void printVersion(void) {
    printf("LenoLang Compiler %s\n", LENO_VERSION);
    printf("Copyright (c) 2025 LenoLang Team\n");
}

// 打印帮助信息
static void printHelp(const char* program) {
    printf("LenoLang Compiler %s\n\n", LENO_VERSION);
    printf("用法: %s [选项] <文件>\n\n", program);
    printf("选项:\n");
    printf("  -h, --help        显示帮助信息\n");
    printf("  -v, --version     显示版本信息\n");
    printf("  --pause           执行完毕后暂停\n");
    printf("  --debug           启用调试模式（输出字节码）\n");
    printf("  --debug-out <file> 字节码输出到指定文件（自动启用 --debug）\n");
    printf("  -c, --compile     编译为二进制文件（.lenb），不执行\n");
    printf("  -p, --pack        编译并打包为独立可执行文件（嵌入 leno_vm）\n");
    printf("  -o, --pack-dir <目录>  指定打包输出目录（默认 <源码目录>/dist）\n");
    printf("                    输出的 exe 与依赖的原生库（leno.toml 的 [native-libs]）\n");
    printf("                    会被复制到同一目录，可直接整体分发\n");
    printf("  --onefile         单文件打包：把原生库与 resource.toml [pack] resources 声明的\n");
    printf("                    资源内嵌进 exe，只需分发一个文件\n");
    printf("                    （首次运行自动解包到用户缓存目录，按内容哈希分目录）\n");
    printf("  --console         打包时强制用控制台版 leno_vm（默认；覆盖脚本里的\n");
    printf("                    _console(false) 自动检测）\n");
    printf("  --no-console      打包时强制用无控制台版 leno_vm_gui（Windows；双击不弹黑框）\n");
    printf("                    （二者互斥；非 Windows 平台无此变体，会提示并忽略）\n");
    printf("  --init [路径]     在当前目录创建新 Leno 包项目\n");
    printf("  --install         安装包或依赖到全局缓存\n");
    printf("  --                终止解释器选项解析：其后的参数都按位置参数处理\n");
    printf("\n");
    printf("说明: <文件> 之后的参数**原样传给脚本**（含 '-' 开头的，脚本内用 _args() 取）\n");
    printf("\n");
    printf("示例:\n");
    printf("  %s script.leno       运行脚本\n", program);
    printf("  %s script.leno --list  脚本参数：--list 原样传给脚本（_args()）\n", program);
    printf("  %s -- script.lenb    用 -- 终止解释器选项解析\n", program);
    printf("  %s script.lenb       运行编译后的二进制\n", program);
    printf("  %s -c test.leno      编译为二进制\n", program);
    printf("  %s -p test.leno      打包为独立可执行文件\n", program);
    printf("  %s -p test.leno -o release  打包到 release/（exe + 依赖库）\n", program);
    printf("  %s --debug test.leno 调试模式运行\n", program);
    printf("  %s --init my-package 创建新包\n", program);
    printf("  %s --install         安装当前项目依赖\n", program);
    printf("  %s --install <路径>  安装指定包目录\n", program);
    printf("  %s --install <git源> 从 Git 远程安装包\n", program);
    printf("                       如: gitee:user/repo/pkg-a\n");
}

// 主执行流程

// 入口文件缓存路径（由 lenolang_run_file 设置，lenolang_run 在序列化后写入缓存）
static char g_entry_cache_path[MAX_PATH_LEN] = {0};
// 入口缓存的依赖清单路径（<entry 缓存>.deps，与 .lenb 同目录、同批次落盘）
static char g_entry_deps_path[MAX_PATH_LEN + 8] = {0};
static int g_entry_cache_enabled = 0;

// ============================================================================
// 入口缓存依赖清单（entry_<hash>.lenb.deps）—— 修复「改被引用模块不失效」
//
// 背景：entry_<hash>.lenb 是**整个程序**的序列化快照——所有 import 进来的模块
// 都被内联在同一个文件里（serialize.c 的 OBJ_MODULE 分支只有在「模块缓存序列化」
// 时才把已加载模块预标记成 CONST_TAG_MODULE_REF，入口序列化走的是全量写出），
// 而文件名只由【入口文件自身】的内容哈希决定（见 lenolang_run_file）。
// 于是只修改被引用的 lib 模块（入口不变）时：缓存键不变、内容却是旧的 ⇒ 直接
// vm_load 改前的模块字节码；模块级 .lenomc 那套「自身 hash + 逐依赖 hash」校验
// 根本没机会执行（整个程序都没走 load_module_file）。
//
// 修法：写入口缓存时把本次编译涉及的**全部模块**做成快照落盘，加载入口缓存前逐条
// 比对，任一不符即视为未命中（并删掉过期的 .lenb）。
// 快照记录的是「这些字节码当初由哪一版源码编出来」——直接取自各模块自己的
// .lenomc header（本轮刚编译，或本轮从缓存载入时已校验过），而不是当场 stat 磁盘：
// 后者在「编译期间源码又被改动」时会记下新哈希却内联着旧字节码，反而制造假命中。
// 清单格式（文本、制表符分隔、末行 END）：
//     LENODEPS2
//     BIN\t<运行中 exe 的指纹：size+mtime+内容 FNV-1a(64)，十六进制>
//     <count>
//     <st_size>\t<fnv1a 十六进制>\t<模块源文件路径>
//     END
// 校验失败 / 清单缺失 / 任一模块拿不到 .lenomc header ⇒ 返回失效，回退源码编译。
// （注：覆盖范围是参与编译的 .leno 模块；被 cfunc/extern 绑定的原生库如 SDL3.dll
//   是运行时加载的，不在缓存失效范围内。）
//
// §8.112（2026-09-17）：第二行 BIN 是**新增**的失效输入，因此格式标识从 LENODEPS1
// 升为 LENODEPS2（旧清单必然被拒 ⇒ 自动重编译一次，这正是我们要的方向）。
// 为什么必须加：字节码里烙着编译期决策，其中一部分来自**原生模块注册表**
// （native 方法签名 / 模块常量 / 实例方法表）—— 它们**没有源文件**，改 C 代码不会让任何
// .leno 源快照变化 ⇒ 三个 source snapshot 都"成立" ⇒ 缓存被判有效却已过期 ✗。
// 实测事故：`times.ms()` 的注册由 TYPE_INT 改成 TYPE_FLOAT 后，基准脚本仍按"int 签名"
// 编译 ⇒ `t2 - t1` 整数相减 ⇒ 打印 10^9 量级的"毫秒"（静默错误代码）。
// 取 exe 自身当输入 ⇒ ABI / 注册表 / 编译器语义任何改动都要重新构建 ⇒ 指纹必变 ⇒
// 一次覆盖整类问题（不必逐表枚举，也就不会漏表）。
// 失败方向：指纹取不到（0）一律判失效（fail-closed）—— 宁可重编译，不要跑旧码。
//
// 本行格式标识已登记在 docs/待办_单一事实来源与重复实现收敛.md 第七节；
// **不升 `LENO_BIN_VERSION`** 的理由：`.lenb` 自身的字节格式与序列化语义一字未改，
// 变的只是"什么时候认这份缓存"的外部判定条件 ⇒ 升它会让所有模块级缓存（.lenomc /
// .lenosymc）跟着无谓失效，而它们各自有独立的失效判定。
// ============================================================================
#define ENTRY_DEPS_MAGIC "LENODEPS2"

// 跨平台 fopen / remove：Windows 上走宽字符，避免中文路径（如 文件管理器）失败
// （原先这里还有一份自己的 stat / 哈希 / 读 .lenomc header 的实现，2026-09-16 收敛到
//   serialize.c 的 module_source_snapshot_* —— 见 docs/待办_单一事实来源与重复实现收敛.md 的 Phase 3）
static FILE* entry_deps_fopen(const char* path, const char* mode) {
#ifdef _WIN32
    wchar_t* wp = utf8_to_utf16(path);
    if (!wp) return NULL;
    wchar_t* wm = utf8_to_utf16(mode);
    if (!wm) { free(wp); return NULL; }
    FILE* f = _wfopen(wp, wm);
    free(wp);
    free(wm);
    return f;
#else
    return fopen(path, mode);
#endif
}

static void entry_deps_remove(const char* path) {
    if (!path || !path[0]) return;
#ifdef _WIN32
    wchar_t* wp = utf8_to_utf16(path);
    if (!wp) return;
    _wremove(wp);
    free(wp);
#else
    remove(path);
#endif
}

// ============================================================================
// 旧入口缓存的回收（entry_<hash>.lenb 以前只增不减）
// ----------------------------------------------------------------------------
// 入口缓存的文件名由**入口源内容哈希**决定 ⇒ 改一次入口文件就换一个键，旧文件（典型 2~3MB）
// 永不回收、同目录里悄悄堆积（LenoSDL3 那种 2.7MB 的产物改几次就几十 MB）。
// 这里在"成功写入新缓存"之后，把同目录下的 entry_*.lenb 与 entry_*.lenb.deps 按 mtime 排序，
// 保留最新 ENTRY_CACHE_KEEP_FILES 个文件（≈ 一半数量的键；每个键 = .lenb + .deps 两个文件），
// 其余删除。
//
// 为什么是"保留最新 K 个"而不是"只留当前键"：`.lenocache` 是该目录**所有入口文件共享**的
// （缓存目录 = <entry 所在目录>/.lenocache），只留当前键会把同目录其它入口文件的缓存一起清掉
// —— 不至于错，但会莫名重编译。保留最新 K 个既让总量有界，又不误伤常用的邻居。
// 只认 entry_ 前缀：.lenomc / .lenosymc 是模块级缓存、各有自己的失效判定，不在这里管。
// ============================================================================
#define ENTRY_CACHE_KEEP_FILES 8

typedef struct { char* name; long long mtime; } EntryCacheEntry;

static int entry_cache_prune_cmp(const void* a, const void* b) {
    long long ma = ((const EntryCacheEntry*)a)->mtime;
    long long mb = ((const EntryCacheEntry*)b)->mtime;
    if (ma < mb) return 1;    // 新的排前面
    if (ma > mb) return -1;
    return 0;
}

// 是入口缓存文件吗：entry_<hash>.lenb 或 entry_<hash>.lenb.deps
static int entry_cache_name_is_target(const char* name) {
    if (!name || strncmp(name, "entry_", 6) != 0) return 0;
    size_t n = strlen(name);
    if (n > 5 && strcmp(name + n - 5, ".lenb") == 0) return 1;
    if (n > 10 && strcmp(name + n - 10, ".lenb.deps") == 0) return 1;
    return 0;
}

static void entry_cache_list_add(EntryCacheEntry** list, int* count, int* cap,
                                 const char* name, long long mtime) {
    if (*count == *cap) {
        int nc = *cap ? *cap * 2 : 8;
        EntryCacheEntry* nl = (EntryCacheEntry*)realloc(*list, sizeof(EntryCacheEntry) * (size_t)nc);
        if (!nl) return;   // 内存不足就放弃收集：只是不回收旧文件，不影响正确性
        *list = nl;
        *cap = nc;
    }
    char* dup = strdup(name);
    if (!dup) return;
    (*list)[*count].name = dup;
    (*list)[*count].mtime = mtime;
    (*count)++;
}

// 给定 "…/entry_x.lenb"，回收同目录里过旧的入口缓存
static void entry_cache_prune_stale(const char* current_cache_path) {
    if (!current_cache_path || !current_cache_path[0]) return;

    // 取目录部分（含结尾分隔符）
    char dir[MAX_PATH_LEN];
    strncpy(dir, current_cache_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char* sep = strrchr(dir, '\\');
    char* sep2 = strrchr(dir, '/');
    if (!sep || (sep2 && sep2 > sep)) sep = sep2;
    if (!sep) return;
    *(sep + 1) = '\0';

    EntryCacheEntry* list = NULL;
    int count = 0, cap = 0;

#ifdef _WIN32
    char pattern[MAX_PATH_LEN + 8];
    snprintf(pattern, sizeof(pattern), "%sentry_*", dir);
    wchar_t* wpat = utf8_to_utf16(pattern);
    if (!wpat) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpat, &fd);
    free(wpat);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        int wlen = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, NULL, 0, NULL, NULL);
        if (wlen <= 0) continue;
        char* name = (char*)malloc((size_t)wlen);
        if (!name) continue;
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, wlen, NULL, NULL);
        if (entry_cache_name_is_target(name)) {
            long long mt = ((long long)fd.ftLastWriteTime.dwHighDateTime << 32) |
                           (long long)fd.ftLastWriteTime.dwLowDateTime;
            entry_cache_list_add(&list, &count, &cap, name, mt);
        }
        free(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (!entry_cache_name_is_target(ent->d_name)) continue;
        char full[MAX_PATH_LEN];
        snprintf(full, sizeof(full), "%s%s", dir, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        entry_cache_list_add(&list, &count, &cap, ent->d_name, (long long)st.st_mtime);
    }
    closedir(d);
#endif

    if (count > ENTRY_CACHE_KEEP_FILES) {
        qsort(list, (size_t)count, sizeof(EntryCacheEntry), entry_cache_prune_cmp);
        // 保留最新的 K 个文件；本次刚写的那两个必然在最前面，绝不会被自己删掉
        for (int i = ENTRY_CACHE_KEEP_FILES; i < count; i++) {
            char full[MAX_PATH_LEN];
            snprintf(full, sizeof(full), "%s%s", dir, list[i].name);
            entry_deps_remove(full);
        }
    }
    for (int i = 0; i < count; i++) free(list[i].name);
    free(list);
}

// 写依赖清单。必须在入口 .lenb 写成功之后调用。
// 返回 0=成功（清单可信），-1=失败（调用方须把 .lenb 一并删掉，让下次运行重编译）
static int entry_deps_write(const char* deps_path) {
    if (!deps_path || !deps_path[0]) return -1;

    const char* cache_dir = module_loader_get_cache_dir();
    int total = loaded_modules_get_count();
    char** paths = NULL;
    uint64_t* sizes = NULL;
    uint64_t* hashes = NULL;
    int count = 0;
    int failed = 0;
    if (total > 0) {
        paths = (char**)malloc(sizeof(char*) * (size_t)total);
        sizes = (uint64_t*)malloc(sizeof(uint64_t) * (size_t)total);
        hashes = (uint64_t*)malloc(sizeof(uint64_t) * (size_t)total);
        if (!paths || !sizes || !hashes) {
            free(paths); free(sizes); free(hashes);
            return -1;
        }
    }

    for (int i = 0; i < total; i++) {
        ObjModule* m = loaded_modules_get(i);
        if (!m || !m->source_path || !m->source_path[0]) continue;
        // 同一路径只记一次（loaded_modules 按理已去重，这里防御）
        int dup = 0;
        for (int j = 0; j < count; j++) {
            if (strcmp(paths[j], m->source_path) == 0) { dup = 1; break; }
        }
        if (dup) continue;
        uint64_t size = 0, hash = 0;
        if (module_cache_read_source_snapshot(cache_dir, m->source_path, &size, &hash) != 0) {
            // 拿不到该模块「已编译版本」的快照 ⇒ 清单不完整 ⇒ 整体作废
            failed = 1;
            break;
        }
        paths[count] = m->source_path;
        sizes[count] = size;
        hashes[count] = hash;
        count++;
    }

    int rc = 0;
    // §8.112：运行中 exe 的指纹（缓存失效的第四类输入）。取不到就**不写缓存** ——
    // 写一份无从校验的清单等于留一颗定时炸弹（调用方会据此删掉 .lenb）。
    uint64_t bin_fp = cache_runtime_binary_fingerprint();
    if (!failed && bin_fp == 0) rc = -1;
    if (!failed && bin_fp != 0) {
        FILE* f = entry_deps_fopen(deps_path, "w");
        if (!f) {
            rc = -1;
        } else {
            fprintf(f, ENTRY_DEPS_MAGIC "\n");
            fprintf(f, "BIN\t%llx\n", (unsigned long long)bin_fp);
            fprintf(f, "%d\n", count);
            for (int i = 0; i < count; i++) {
                fprintf(f, "%llu\t%llx\t%s\n",
                        (unsigned long long)sizes[i],
                        (unsigned long long)hashes[i],
                        paths[i]);
            }
            fprintf(f, "END\n");
            if (ferror(f)) rc = -1;
            fclose(f);
        }
    } else {
        rc = -1;
    }
    if (rc != 0) entry_deps_remove(deps_path);

    free(paths);
    free(sizes);
    free(hashes);
    return rc;
}

// 校验依赖清单：0=有效（入口缓存的字节码与当前源码一致），-1=失效
// 判定与模块缓存一致：先比磁盘大小（O(1) stat），再比内容哈希
static int entry_deps_valid(const char* deps_path) {
    if (!deps_path || !deps_path[0]) return -1;
    FILE* f = entry_deps_fopen(deps_path, "r");
    if (!f) return -1;

    char line[MAX_PATH_LEN + 128];
    int ok = 0;
    if (fgets(line, sizeof(line), f) &&
        strncmp(line, ENTRY_DEPS_MAGIC, strlen(ENTRY_DEPS_MAGIC)) == 0 &&
        fgets(line, sizeof(line), f)) {
        // 第二行：运行中 exe 的指纹（见 leno_serialize.h ④ / §8.112）。
        // 取不到当前指纹（返回 0）一律判失效：宁可不编，不要猜。
        uint64_t want_bin = 0;
        int bin_ok = 0;
        if (strncmp(line, "BIN\t", 4) == 0) {
            char* endp = NULL;
            want_bin = (uint64_t)strtoull(line + 4, &endp, 16);
            bin_ok = (endp && *endp != '\0' && (*endp == '\n' || *endp == '\r'));
        }
        uint64_t cur_bin = cache_runtime_binary_fingerprint();
        if (bin_ok && cur_bin != 0 && want_bin == cur_bin &&
            fgets(line, sizeof(line), f)) {
            int count = atoi(line);
            if (count >= 0) {
                ok = 1;
                for (int i = 0; i < count && ok; i++) {
                    if (!fgets(line, sizeof(line), f)) { ok = 0; break; }
                    char* tab1 = strchr(line, '\t');
                    if (!tab1) { ok = 0; break; }
                    *tab1 = '\0';
                    char* rest = tab1 + 1;
                    char* tab2 = strchr(rest, '\t');
                    if (!tab2) { ok = 0; break; }
                    *tab2 = '\0';
                    char* path = tab2 + 1;
                    size_t plen = strlen(path);
                    while (plen > 0 && (path[plen - 1] == '\n' || path[plen - 1] == '\r')) {
                        path[--plen] = '\0';
                    }
                    if (plen == 0) { ok = 0; break; }
                    uint64_t want_size = strtoull(line, NULL, 10);
                    uint64_t want_hash = strtoull(rest, NULL, 16);
                    // 判定走统一实现（统计口径与失败方向都在 serialize.c 里，见 Phase 3）
                    if (!module_source_snapshot_matches(path, want_size, want_hash, 1)) {
                        ok = 0; break;
                    }
                }
                // 收尾标记：清单被截断时这里也会失败（不依赖原子替换）
                if (ok) {
                    if (!fgets(line, sizeof(line), f) || strncmp(line, "END", 3) != 0) ok = 0;
                }
            }
        }
    }
    fclose(f);
    return ok ? 0 : -1;
}

// ============================================================================
// 空源文件告警
// ----------------------------------------------------------------------------
// 空文件（或只有空白）会"编译成功"：exit 0、什么都不打印，还会写一份缓存
// （键 = FNV-1a 空串基值 cbf29ce484222325）。这种"假成功"与"真的跑了但没输出"无法区分 ——
// 2026-09-16 就被它骗过一次（探针文件写入落空 ⇒ 编译了个空程序，却毫无提示）。
// 只告警不报错：空程序在语法上合法，"什么都不做"也可能是刻意的。
// 覆盖范围是 CLI 入口（run / -c / -p）；被 import 的模块为空不算 —— 一个不导出任何东西的
// 模块是合法形态，不该报警。
// ============================================================================
static void warn_if_source_empty(const char* source) {
    if (!source) return;
    for (const unsigned char* p = (const unsigned char*)source; *p; p++) {
        if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '\f' && *p != '\v') {
            return;   // 有实义字符
        }
    }
    warning_add_at(WARN_EMPTY_SOURCE, 1, 1,
                   "源文件为空（或只有空白）：编译出来的程序不会做任何事");
}

int lenolang_run(const char* source) {
      if (debugMode) {
         printf("debug模式:进入主执行流程\n");
     }
    // 0. 清空错误、扫描栈与符号表记忆化（记忆化是进程级缓存，跨编译必须清）
    error_clear();
    warning_clear();
    module_symbol_table_reset_scan_stack();
    module_symbol_table_reset_memo();
    warn_if_source_empty(source);
     if (debugMode) {
         printf("debug模式:进入语法分析阶段\n");
     }
    // 1. 词法分析 + 语法分析
    Parser parser;
    parser_init(&parser, source);
    if (parser_parse(&parser) < 0) {
        // 语法分析失败，只释放 AST
        error_print_all();
        warning_print_all();
        ast_free(parser.root);
        return -1;
    }

    // 2. 语义分析（单遍）
    Semantic sem;
    semantic_init(&sem, parser.root);
    // 提前初始化 CodeGen，确保 fail 路径上 codegen_cleanup 安全
    Chunk chunk;
    chunk_init(&chunk);
    CodeGen gen;
    codegen_init(&gen, &chunk, &sem);
    semantic_analyze(&sem, parser.root);
    if (error_has_any()) goto fail;

    // 2.5 常量折叠优化
    optimize_constant_fold(parser.root);

    // 2.6 死代码消除
    optimize_dead_code_elimination(parser.root);

    // 3. 生成字节码
    codegen(&gen, parser.root);
    if (error_has_any()) goto fail;

    // 调试模式：统一输出全部字节码（主程序 + 所有已加载模块）
    // 此时所有模块已编译完成（含缓存命中和未命中），统一输出
    if (debugMode) {
        debug_dump_all_bytecode(&chunk);
    }

    // 4. 内存态序列化运行：编译 → 序列化 → 释放编译器 → 反序列化 → 运行
    //    让 .leno 直跑与 .lenb 路径一致，消除编译器驻留导致的 CPU 缓存污染
    {
        uint8_t* lenb_buf = NULL;
        size_t lenb_size = 0;
        SerializeResult sr = chunk_serialize_to_memory(&chunk, sem.root_scope, &lenb_buf, &lenb_size);
        if (sr == SERIALIZE_OK && lenb_buf) {
            // 入口文件缓存写入：将序列化结果落盘，下次运行可直接加载跳过编译
            if (g_entry_cache_enabled && g_entry_cache_path[0]) {
                int entry_written = 0;
#ifdef _WIN32
                int wlen2 = MultiByteToWideChar(CP_UTF8, 0, g_entry_cache_path, -1, NULL, 0);
                if (wlen2 > 0) {
                    wchar_t* wpath2 = (wchar_t*)malloc(wlen2 * sizeof(wchar_t));
                    if (wpath2) {
                        MultiByteToWideChar(CP_UTF8, 0, g_entry_cache_path, -1, wpath2, wlen2);
                        FILE* cf = _wfopen(wpath2, L"wb");
                        if (cf) {
                            size_t wrote = fwrite(lenb_buf, 1, lenb_size, cf);
                            fclose(cf);
                            entry_written = (wrote == lenb_size);
                        }
                        free(wpath2);
                    }
                }
#else
                FILE* cf = fopen(g_entry_cache_path, "wb");
                if (cf) {
                    size_t wrote = fwrite(lenb_buf, 1, lenb_size, cf);
                    fclose(cf);
                    entry_written = (wrote == lenb_size);
                }
#endif
                // 依赖清单必须与 .lenb 一同成立：清单写不出来（或 .lenb 没写完整）就把两者
                // 都删掉——宁可直接重编译，也不能留下一份无从校验的旧字节码。
                if (!entry_written || entry_deps_write(g_entry_deps_path) != 0) {
                    entry_deps_remove(g_entry_deps_path);
                    entry_deps_remove(g_entry_cache_path);
                } else {
                    // 写成功：顺手回收同目录里过旧的入口缓存（只增不减会堆到几十 MB）
                    entry_cache_prune_stale(g_entry_cache_path);
                }
            }

            // 立刻释放编译器资源（核心！清零「内存税」）
            codegen_cleanup(&gen);
            ast_free(parser.root);
            semantic_cleanup(&sem);
            chunk_free(&chunk);

            // 从内存反序列化出全新 chunk + scope
            Chunk run_chunk;
            chunk_init(&run_chunk);
            Scope* run_scope = NULL;
            sr = chunk_deserialize_from_memory(lenb_buf, lenb_size, &run_chunk, &run_scope);
            free(lenb_buf);  // 缓冲已用完，立即释放

            if (sr == SERIALIZE_OK) {
                // 与 lenb 路径一致的运行前准备（补 native 函数指针）
                register_defs_from_chunk(&run_chunk);
                gc_init();
                fix_module_function_ptrs(&run_chunk);
                vm_init_with_scope(run_scope);
                vm_load(&run_chunk);
                int ret = vm_run();

                chunk_free(&run_chunk);
                // run_scope 已被 vm_init_with_scope 设为 vm.global_scope，由 gc_free_all 释放
                gc_free_all();

                if (ret != 0 || error_has_any()) {
                    error_print_all();
                    warning_print_all();
                    return -1;
                }
                warning_print_all();
                // main 的返回值作为进程退出码
                return vm_get_exit_code();
            }
            // 反序列化失败，回退到原路径（需重新编译，因为编译器资源已释放）
            // 这种情况理论上不会发生，但做兜底保护
            fprintf(stderr, "[警告] 内存反序列化失败(%d)，回退到直接运行\n", (int)sr);
            return -1;
        }
        // 序列化失败，回退到原 vm_run 直跑路径
        if (lenb_buf) free(lenb_buf);
    }

    // 兜底：原 vm_run 直跑路径（序列化失败时走这里）
    gc_init();
    vm_init_with_scope(sem.root_scope);  // 使用语义分析的 scope，确保索引一致
    vm_load(&chunk);
    int ret = vm_run();

    // 5. 释放
    codegen_cleanup(&gen);
    ast_free(parser.root);
    // 释放语义分析中的资源（函数名列表）
    semantic_cleanup(&sem);
    // 释放字节码块
    chunk_free(&chunk);
    // gc_free_all 会释放 VM 的 global_scope
    gc_free_all();
    
    // 如果有运行时错误，打印错误信息
    if (ret != 0 || error_has_any()) {
        error_print_all();
        warning_print_all();
        return -1;
    }

    warning_print_all();
    // main 的返回值作为进程退出码
    return vm_get_exit_code();

fail:
    if (debugMode) {
        printf("debug模式:编译失败\n");
    }
    error_print_all();
    warning_print_all();
    codegen_cleanup(&gen);
    ast_free(parser.root);
    // 编译失败时释放 root_scope（VM 未初始化）
    if (sem.root_scope) {
        scope_free(sem.root_scope);
        sem.root_scope = NULL;
    }
    // 释放语义分析中的其他资源
    semantic_cleanup(&sem);
    return -1;
}


// fix_module_function_ptrs 等函数已移至 module.c

int lenolang_run_binary(const char* path) {
    Chunk chunk;
    Scope* scope = NULL;

    SerializeResult result = chunk_deserialize(path, &chunk, &scope);
    if (result != SERIALIZE_OK) {
        const char* err_msg = "未知错误";
        switch (result) {
            case SERIALIZE_ERR_FILE:    err_msg = "无法打开文件"; break;
            case SERIALIZE_ERR_MAGIC:   err_msg = "不是有效的 .lenb 文件"; break;
            case SERIALIZE_ERR_VERSION: err_msg = "二进制文件版本不兼容"; break;
            case SERIALIZE_ERR_FORMAT:  err_msg = "文件格式错误"; break;
            case SERIALIZE_ERR_MEMORY:  err_msg = "内存不足"; break;
            case SERIALIZE_ERR_READ:    err_msg = "读取文件失败"; break;
            default: break;
        }
        fprintf(stderr, "加载二进制文件失败: %s (%s)\n", path, err_msg);
        return -1;
    }

    if (debugMode) {
        printf("debug模式:从二进制文件加载成功\n");
        debug_dump_all_bytecode(&chunk);
    }

    gc_init();
    vm_init_with_scope(scope);
    fix_module_function_ptrs(&chunk);
    vm_load(&chunk);
    int ret = vm_run();

    chunk_free(&chunk);
    gc_free_all();

    if (ret != 0 || error_has_any()) {
        error_print_all();
        warning_print_all();
        return -1;
    }

    warning_print_all();
    // main 的返回值作为进程退出码
    return vm_get_exit_code();
}

// 编译源代码到二进制文件
int lenolang_compile(const char* source, const char* output_path) {
    clock_t compile_t0 = clock();
    error_clear();
    warning_clear();
    module_symbol_table_reset_scan_stack();
    module_symbol_table_reset_memo();
    warn_if_source_empty(source);   // 空源文件别"假成功"（-c / -p 也走这里）

    gc_init();
    vm_init();

    clock_t t_parse0 = clock();
    Parser parser;
    parser_init(&parser, source);
    if (parser_parse(&parser) < 0) {
        error_print_all();
        warning_print_all();
        ast_free(parser.root);
        gc_free_all();
        return -1;
    }
    clock_t t_parse1 = clock();
    fprintf(stderr, "[TIME] parse: %.1f ms\n", (double)(t_parse1 - t_parse0) / CLOCKS_PER_SEC * 1000.0);

    Semantic sem;
    semantic_init(&sem, parser.root);
    Chunk chunk;
    chunk_init(&chunk);
    CodeGen gen;
    codegen_init(&gen, &chunk, &sem);
    clock_t t_sem0 = clock();
    semantic_analyze(&sem, parser.root);
    clock_t t_sem1 = clock();
    fprintf(stderr, "[TIME] semantic: %.1f ms\n", (double)(t_sem1 - t_sem0) / CLOCKS_PER_SEC * 1000.0);
    if (error_has_any()) goto compile_fail;

    clock_t t_opt0 = clock();
    optimize_constant_fold(parser.root);
    optimize_dead_code_elimination(parser.root);
    clock_t t_opt1 = clock();
    fprintf(stderr, "[TIME] optimize: %.1f ms\n", (double)(t_opt1 - t_opt0) / CLOCKS_PER_SEC * 1000.0);

    clock_t t_cg0 = clock();
    codegen(&gen, parser.root);
    clock_t t_cg1 = clock();
    fprintf(stderr, "[TIME] codegen: %.1f ms\n", (double)(t_cg1 - t_cg0) / CLOCKS_PER_SEC * 1000.0);
    if (error_has_any()) goto compile_fail;

    if (debugMode) {
        debug_dump_all_bytecode(&chunk);
    }

    clock_t t_ser0 = clock();
    SerializeResult result = chunk_serialize(output_path, &chunk, sem.root_scope);
    clock_t t_ser1 = clock();
    fprintf(stderr, "[TIME] serialize: %.1f ms\n", (double)(t_ser1 - t_ser0) / CLOCKS_PER_SEC * 1000.0);
    if (result != SERIALIZE_OK) {
        fprintf(stderr, "写入二进制文件失败: %s (错误码: %d)\n", output_path, result);
        codegen_cleanup(&gen);
        ast_free(parser.root);
        semantic_cleanup(&sem);
        chunk_free(&chunk);
        gc_free_all();
        return -1;
    }

    printf("编译成功: %s -> %s\n", chunk.filename ? chunk.filename : "stdin", output_path);
    {
        clock_t compile_t1 = clock();
        double compile_ms = (double)(compile_t1 - compile_t0) / CLOCKS_PER_SEC * 1000.0;
        printf("编译耗时: %.1f ms\n", compile_ms);
    }

    codegen_cleanup(&gen);
    ast_free(parser.root);
    semantic_cleanup(&sem);
    chunk_free(&chunk);
    gc_free_all();
    warning_print_all();
    return 0;

compile_fail:
    error_print_all();
    warning_print_all();
    codegen_cleanup(&gen);
    ast_free(parser.root);
    if (sem.root_scope) {
        scope_free(sem.root_scope);
        sem.root_scope = NULL;
    }
    semantic_cleanup(&sem);
    gc_free_all();
    return -1;
}

// ============================================================================
// 单文件打包：组装内嵌资源段
// ----------------------------------------------------------------------------
// 格式 v2（**必须与 vm_main.c 的 extract_embedded_resources 严格同构**，改一处要改两处）：
//   "LENOPACK"(8) | version:u32(=2) | payload_hash:u64 | entry_count:u32
//   entry_count × [ rel_len:u16 | size:u64 | hash:u32 | rel_path(rel_len) ]
//   各文件内容按索引顺序紧随其后
// payload_hash = FNV-1a 64 over [entry_count .. 末尾]（本函数最后算）—— 运行期拿它当
//   **释放目录的键**（不同构建 ⇒ 不同目录，互不覆盖；同构建 ⇒ 同目录，可复用）。
// rel_path 用 '/' 分隔；原生库用纯文件名，资源保留目录结构。
// ============================================================================
#define PACK_BLOB_HEADER "LENOPACK"
#define PACK_BLOB_VERSION 2u
// v2 头长：8(魔数) + 4(version) + 8(payload_hash) + 4(count)
#define PACK_BLOB_HDR 24

static uint32_t pack_fnv1a32(const unsigned char* data, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

// FNV-1a 64 —— 必须与运行侧 vm_main.c 的 res_fnv1a64 逐位一致
// （算法不一致 ⇒ 每次启动都重新释放，且不同构建可能撞进同一目录）
static uint64_t pack_fnv1a64(const unsigned char* data, size_t len) {
    uint64_t h = 1469598103934665603ULL;   // FNV-1a 64 offset basis
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)data[i];
        h *= 1099511628211ULL;             // FNV-1a 64 prime
    }
    return h;
}

// 读文件全部内容（UTF-8 路径安全；返回 malloc 缓冲，调用方 free）
static unsigned char* pack_read_all(const char* path, size_t* out_size) {
#ifdef _WIN32
    wchar_t* wp = utf8_to_utf16(path);
    if (!wp) return NULL;
    FILE* f = _wfopen(wp, L"rb");
    free(wp);
#else
    FILE* f = fopen(path, "rb");
#endif
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return NULL; }
    unsigned char* buf = (unsigned char*)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (rd != (size_t)size) { free(buf); return NULL; }
    *out_size = (size_t)size;
    return buf;
}

// 把 libs（原生库）与 res（[pack] resources）打成内嵌资源段
static int pack_build_res_blob(PackLib* libs, int lib_n, PackRes* res, int res_n,
                               unsigned char** out_blob, size_t* out_size) {
    int total = lib_n + res_n;
    *out_blob = NULL;
    *out_size = 0;
    if (total <= 0) return 0;   /* 没东西要嵌：不写资源段 */

    /* 1) 索引区大小 + 内容区大小 */
    size_t idx_size = 0;
    size_t content_size = 0;
    for (int i = 0; i < lib_n; i++) {
        size_t rl = strlen(libs[i].file_name);
        idx_size += 14 + rl;
    }
    for (int i = 0; i < res_n; i++) {
        size_t rl = strlen(res[i].rel_path);
        idx_size += 14 + rl;
    }
    for (int i = 0; i < lib_n; i++) {
        size_t sz = 0;
        unsigned char* d = pack_read_all(libs[i].src_path, &sz);
        if (!d) {
            fprintf(stderr, "[pack] 错误: 无法读取原生库: %s\n", libs[i].src_path);
            return -1;
        }
        free(d);
        content_size += sz;
    }
    for (int i = 0; i < res_n; i++) {
        size_t sz = 0;
        unsigned char* d = pack_read_all(res[i].src_path, &sz);
        if (!d) {
            fprintf(stderr, "[pack] 错误: 无法读取资源: %s\n", res[i].src_path);
            return -1;
        }
        free(d);
        content_size += sz;
    }

    size_t blob_size = PACK_BLOB_HDR + idx_size + content_size;
    unsigned char* blob = (unsigned char*)malloc(blob_size);
    if (!blob) return -1;

    /* 2) 头 + 索引 */
    memcpy(blob, PACK_BLOB_HEADER, 8);
    uint32_t version = PACK_BLOB_VERSION;
    memcpy(blob + 8, &version, 4);
    uint64_t hash_placeholder = 0;          /* 内容区写完后回填（见末尾） */
    memcpy(blob + 12, &hash_placeholder, 8);
    uint32_t count = (uint32_t)total;
    memcpy(blob + 20, &count, 4);

    unsigned char* ip = blob + PACK_BLOB_HDR;
    for (int i = 0; i < lib_n; i++) {
        const char* rel = libs[i].file_name;
        uint16_t rl = (uint16_t)strlen(rel);
        size_t sz = 0;
        unsigned char* d = pack_read_all(libs[i].src_path, &sz);
        if (!d) { free(blob); return -1; }
        uint64_t fsz = (uint64_t)sz;
        uint32_t h = pack_fnv1a32(d, sz);
        free(d);
        memcpy(ip, &rl, 2);
        memcpy(ip + 2, &fsz, 8);
        memcpy(ip + 10, &h, 4);
        memcpy(ip + 14, rel, rl);
        ip += 14 + rl;
    }
    for (int i = 0; i < res_n; i++) {
        const char* rel = res[i].rel_path;
        uint16_t rl = (uint16_t)strlen(rel);
        size_t sz = 0;
        unsigned char* d = pack_read_all(res[i].src_path, &sz);
        if (!d) { free(blob); return -1; }
        uint64_t fsz = (uint64_t)sz;
        uint32_t h = pack_fnv1a32(d, sz);
        free(d);
        memcpy(ip, &rl, 2);
        memcpy(ip + 2, &fsz, 8);
        memcpy(ip + 10, &h, 4);
        memcpy(ip + 14, rel, rl);
        ip += 14 + rl;
    }

    /* 3) 内容区 */
    unsigned char* cp = blob + PACK_BLOB_HDR + idx_size;
    for (int i = 0; i < lib_n; i++) {
        size_t sz = 0;
        unsigned char* d = pack_read_all(libs[i].src_path, &sz);
        if (!d) { free(blob); return -1; }
        memcpy(cp, d, sz);
        cp += sz;
        free(d);
    }
    for (int i = 0; i < res_n; i++) {
        size_t sz = 0;
        unsigned char* d = pack_read_all(res[i].src_path, &sz);
        if (!d) { free(blob); return -1; }
        memcpy(cp, d, sz);
        cp += sz;
        free(d);
    }

    /* 4) 回填 payload_hash：FNV-1a 64 over [entry_count .. 末尾]。
     *    运行侧拿它当释放目录的键（vm_main.c 的 res_fnv1a64 必须同算法）。 */
    {
        uint64_t ph = pack_fnv1a64(blob + 20, blob_size - 20);
        memcpy(blob + 12, &ph, 8);
    }

    *out_blob = blob;
    *out_size = blob_size;
    return 0;
}

// 该路径是不是已存在的文件（UTF-8 路径安全）
static int pack_file_exists_utf8(const char* path) {
#ifdef _WIN32
    wchar_t* wp = utf8_to_utf16(path);
    if (!wp) return 0;
    DWORD attr = GetFileAttributesW(wp);
    free(wp);
    return (attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

// ============================================================================
// 打包：递归创建输出目录
// -o 可能给多级路径（如 build/release/v1），逐级创建；已存在视为成功。
// Windows 走 _wmkdir（UTF-8/中文路径安全）。
// ============================================================================
static int pack_ensure_dir(const char* dir) {
    if (!dir || !dir[0]) return -1;
    char tmp[MAX_PATH_LEN];
    strncpy(tmp, dir, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    size_t len = strlen(tmp);
    while (len > 1 && (tmp[len - 1] == '\\' || tmp[len - 1] == '/')) tmp[--len] = '\0';

#ifdef _WIN32
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '\\' || *p == '/') {
            char save = *p;
            *p = '\0';
            wchar_t* w = utf8_to_utf16(tmp);
            if (w) { _wmkdir(w); free(w); }
            *p = save;
        }
    }
    wchar_t* wfull = utf8_to_utf16(tmp);
    if (!wfull) return -1;
    int rc = (_wmkdir(wfull) == 0 || errno == EEXIST) ? 0 : -1;
    free(wfull);
    return rc;
#else
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            char save = *p;
            *p = '\0';
            mkdir(tmp, 0755);
            *p = save;
        }
    }
    return (mkdir(tmp, 0755) == 0 || errno == EEXIST) ? 0 : -1;
#endif
}

// ============================================================================
// 打包前握手：让**即将内嵌的那个 VM** 校验本次产物能不能被它读出来（只校验、不执行）
// ----------------------------------------------------------------------------
// 为什么必须有（2026-09-25 实测事故）：VM 二进制与编译器各自带着 LENO_BIN_VERSION
// 和各自的序列化读写实现。编译器侧一改（如"行号表 RLE"那次把版本升到 v3.0.2），
// `build.bat` 只重建编译器，打包仍会内嵌**旧的** leno_vm*.exe ⇒ 产物进 exe 后一启动
// 就「内存反序列化失败: 5」（5 = SERIALIZE_ERR_VERSION）闪退，而打包却报"成功" ✗。
// 现在：握手不过 ⇒ 打包**失败**（不写 exe 文件），并提示先跑 build_vm。
// 实现：Windows 用 CreateProcessW（UTF-16 ⇒ 中文路径可用；子进程继承本进程控制台
//   ⇒ VM 的报错原样打在这里，便于定位）；POSIX 走 system()（路径用单引号包住）。
// 子进程模式：`<vm> --check-bin <file>`（见 vm_main.c 的 check_lenb_only）。
// 返回：子进程退出码（0 = 可读）；进程起不来返回 -1。
// ============================================================================
static int pack_vm_check_lenb(const char* vm_exe, const char* lenb_path) {
#ifdef _WIN32
    wchar_t* wvm = utf8_to_utf16(vm_exe);
    wchar_t* wbin = utf8_to_utf16(lenb_path);
    if (!wvm || !wbin) { free(wvm); free(wbin); return -1; }
    size_t need = wcslen(wvm) + wcslen(wbin) + 32;
    wchar_t* cmdline = (wchar_t*)malloc(need * sizeof(wchar_t));
    if (!cmdline) { free(wvm); free(wbin); return -1; }
    swprintf(cmdline, need, L"\"%s\" --check-bin \"%s\"", wvm, wbin);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    int rc = -1;
    if (CreateProcessW(wvm, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        DWORD waited = WaitForSingleObject(pi.hProcess, 60000);
        DWORD code = 1;
        if (waited == WAIT_TIMEOUT) {
            TerminateProcess(pi.hProcess, 124);
            WaitForSingleObject(pi.hProcess, 5000);
            code = 124;
        } else if (!GetExitCodeProcess(pi.hProcess, &code)) {
            code = 1;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        rc = (int)code;
    }
    free(cmdline);
    free(wvm);
    free(wbin);
    return rc;
#else
    char cmd[MAX_PATH_LEN * 2 + 32];
    snprintf(cmd, sizeof(cmd), "'%s' --check-bin '%s'", vm_exe, lenb_path);
    int rc = system(cmd);
    if (rc == -1) return -1;
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    return 126;
#endif
}

// 从文件运行
int lenolang_run_file(const char* path) {
    // 检查是否是 .lenb 二进制文件
    if (serialize_is_binary_file(path)) {
        return lenolang_run_binary(path);
    }

    // 将路径转换为绝对路径，确保模块导入时相对路径能正确解析
    char abs_path[MAX_PATH_LEN];
#ifdef _WIN32
    // Windows: _fullpath 使用 ANSI 代码页，不支持 UTF-8 中文路径
    // 需要先将 UTF-8 转为宽字符，用 _wfullpath，再转回 UTF-8
    {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
        if (wlen > 0) {
            wchar_t* wpath = (wchar_t*)malloc(wlen * sizeof(wchar_t));
            if (wpath) {
                MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);
                wchar_t wabs[MAX_PATH_LEN];
                if (_wfullpath(wabs, wpath, MAX_PATH_LEN) != NULL) {
                    int abs_len = WideCharToMultiByte(CP_UTF8, 0, wabs, -1, abs_path, MAX_PATH_LEN, NULL, NULL);
                    if (abs_len > 0) {
                        error_set_filename(abs_path);
                    } else {
                        error_set_filename(path);
                    }
                } else {
                    error_set_filename(path);
                }
                free(wpath);
            } else {
                error_set_filename(path);
            }
        } else {
            error_set_filename(path);
        }
    }
#else
    if (realpath(path, abs_path) != NULL) {
        error_set_filename(abs_path);
    } else {
        error_set_filename(path);
    }
#endif
    
#ifdef _WIN32
    // Windows: 使用宽字符支持中文路径
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    wchar_t* widePath = (wchar_t*)malloc(wideLen * sizeof(wchar_t));
    if (widePath == NULL) {
        fprintf(stderr, "内存不足\n");
        return -1;
    }
    MultiByteToWideChar(CP_UTF8, 0, path, -1, widePath, wideLen);
    
    FILE* file = _wfopen(widePath, L"rb");
    free(widePath);
#else
    FILE* file = fopen(path, "rb");
#endif
    
    if (!file) {
        fprintf(stderr, "无法打开文件: %s\n", path);
        return -1;
    }
    
    // 获取文件大小
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    // 读取文件内容
    char* source = (char*)malloc(size + 1);
    if (!source) {
        fprintf(stderr, "内存分配失败\n");
        fclose(file);
        return -1;
    }
    
    size_t read = fread(source, 1, size, file);
    source[read] = '\0';
    fclose(file);

    // 设置模块搜索路径（根据项目根目录 + 全局缓存）
    // 必须在编译/打包/运行之前设置，确保 import 能解析到包
    {
        package_search_path_clear();
        const char* abs_file = error_get_filename();
        if (abs_file) {
            char* proj_root = package_find_project_root(abs_file);
            if (proj_root) {
                // 添加 <项目根>/lib/ 到搜索路径
                char lib_path[MAX_PATH_LEN];
                snprintf(lib_path, sizeof(lib_path), "%slib%c", proj_root, 
#ifdef _WIN32
                    '\\'
#else
                    '/'
#endif
                );
                package_search_path_add(lib_path);

                // 从 leno.toml 读取依赖，添加依赖包的 lib/ 到搜索路径
                char toml_path[MAX_PATH_LEN];
                snprintf(toml_path, sizeof(toml_path), "%sleno.toml", proj_root);
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
                free(proj_root);
            }
            // 始终添加源文件所在目录作为搜索路径
            char file_dir[MAX_PATH_LEN];
            strncpy(file_dir, abs_file, MAX_PATH_LEN - 1);
            file_dir[MAX_PATH_LEN - 1] = '\0';
            char* last_sep = strrchr(file_dir, 
#ifdef _WIN32
                '\\'
#else
                '/'
#endif
            );
            if (last_sep) *(last_sep + 1) = '\0';
            package_search_path_add(file_dir);
        }

        // 添加内置模块搜索路径（exe_dir/leno_module/<包名>/lib/）
        // 内置模块优先于全局缓存，确保随 exe 分发的模块版本不被缓存覆盖
        package_builtin_add_to_search_paths();

        // 添加全局缓存中所有已安装包的 lib/ 到搜索路径
        package_cache_add_to_search_paths();

        // 设置模块编译缓存目录：始终使用 entry 文件所在目录
        // 每个运行目录有独立的 .lenocache，避免不同项目/示例的缓存混在同一目录
        if (module_loader_is_cache_enabled()) {
            const char* abs_f = error_get_filename();
            if (abs_f) {
                char dir_buf[MAX_PATH_LEN];
                strncpy(dir_buf, abs_f, MAX_PATH_LEN - 1);
                dir_buf[MAX_PATH_LEN - 1] = '\0';
                char* ls = strrchr(dir_buf,
#ifdef _WIN32
                    '\\'
#else
                    '/'
#endif
                );
                if (ls) *(ls + 1) = '\0';

                char cache_dir[MAX_PATH_LEN + 16];
                snprintf(cache_dir, sizeof(cache_dir), "%s.lenocache%c", dir_buf,
#ifdef _WIN32
                    '\\'
#else
                    '/'
#endif
                );
                module_loader_set_cache_dir(cache_dir);
            }
        }
    }

    // 编译模式：编译为 .lenb 文件
    if (compileMode) {
        char* bin_path = serialize_get_bin_path(path);
        if (!bin_path) {
            fprintf(stderr, "无法生成输出路径\n");
            free(source);
            return -1;
        }
        // ★ DCE 需要**完整**的引用图：图是靠 codegen 边生成边收集的，而命中 .lenomc
        //   的模块压根不走 codegen ⇒ 图缺一半 ⇒ 该模块的函数会被误判成"不可达"剪掉
        //   （调用处静默返回 null）。所以在分发产物（-c / -p）里一律**不读模块缓存**。
        //   这是**有代价的**：读模块缓存本来快得多（hello_window `-c`，5 轮交替取 min：
        //   读缓存 168 ms vs 全量重编译 531 ms）⇒ 每次分发编译多付约 **360 ms**。
        //   仍然值得：换来的是产物体积 −50%（862048 → 432725 B）。
        //   附注：产物大小还会随缓存状态漂移（863795 vs 862048 B），DCE 会把这个差放大到
        //   25% ⇒ 分发产物必须只由源码决定，不能用"图不完整时少剪一点"来折中。
        //   LENO_NO_DCE=1 时不需要这个保证，照旧允许用缓存。
        int saved_cache_enabled = module_loader_is_cache_enabled();
        if (dce_enabled()) module_loader_set_cache_enabled(0);

        // 引用图开一轮新的（同一次进程里可能编译多次：-p 打包 = 编译 + 内嵌校验）
        dce_reset();

        int result = lenolang_compile(source, bin_path);

        module_loader_set_cache_enabled(saved_cache_enabled);
        free(source);
        free(bin_path);

        if (pauseMode) {
            printf("\n按任意键继续...");
            getchar();
        }
        return result;
    }

    // 打包模式：编译为 .lenb 并嵌入 leno_vm.exe 尾部
    if (packMode) {
        char* bin_path = serialize_get_bin_path(path);
        if (!bin_path) {
            fprintf(stderr, "无法生成输出路径\n");
            free(source);
            return -1;
        }
        clock_t pack_t0 = clock();

        // ★ 与 -c 同理：打包产物也是分发的 .lenb ⇒ DCE 需要完整引用图 ⇒ 不读模块缓存，
        //   并为本轮编译开一份新的引用图（见上面 compileMode 分支的说明）。
        int saved_cache_enabled = module_loader_is_cache_enabled();
        if (dce_enabled()) module_loader_set_cache_enabled(0);
        dce_reset();

        // 先编译：语义分析阶段会自动检测 _console(false) 并设置 g_use_gui_vm
        int result = lenolang_compile(source, bin_path);

        module_loader_set_cache_enabled(saved_cache_enabled);
        free(source);
        if (result != 0) {
            free(bin_path);
            return result;
        }
        clock_t pack_compile_end = clock();

        // 编译完成后 g_use_gui_vm 已确定，选择对应的 VM 运行时。
        // 优先级：显式开关（--console / --no-console）> 脚本里的 _console(false) 自动检测
        //         > 默认控制台。
        // 为什么要有开关：自动检测是"用运行期调用去猜链接期需求"的静态启发式 ——
        //   · 只认字面量 false/0（`var v = false; _console(v)` 检测不到）；
        //   · 写在永远不执行的分支里也会误判；
        //   · 而 PE 子系统由 prepend 进去的 vm 数据决定，**打包后改不了** ⇒ 猜错就没救。
#ifdef _WIN32
        int use_gui_vm = g_use_gui_vm;
        switch (packConsoleMode) {
            case PACK_CONSOLE_FORCE:
                use_gui_vm = 0;
                printf("[pack] --console: 使用控制台版 leno_vm.exe（已忽略 _console 自动检测）\n");
                break;
            case PACK_CONSOLE_NONE:
                use_gui_vm = 1;
                printf("[pack] --no-console: 使用无控制台版 leno_vm_gui.exe\n");
                break;
            default:
                if (use_gui_vm) {
                    printf("[pack] 检测到 _console(false)，使用无控制台版 leno_vm_gui.exe\n");
                }
                break;
        }
#else
        // 非 Windows 只构建了 leno_vm 一个变体（build_vm.sh 的 -mwindows 版是 Windows 专属），
        // 开关在这里没有对应物 ⇒ 提示一句并忽略，不让跨平台构建脚本因为多了个开关就失败。
        if (packConsoleMode != PACK_CONSOLE_AUTO) {
            printf("[pack] 提示: 本平台只有 leno_vm 一个变体，%s 已忽略\n",
                   packConsoleMode == PACK_CONSOLE_NONE ? "--no-console" : "--console");
        }
#endif

        // 收集要随 exe 分发的原生库（各实际 import 包 [native-libs] 的当前平台条目）。
        // 必须在编译**之后**：import 是编译期解析的，此刻模块清单才完整；
        // 取路径用 loaded_modules_get_path（不是 GC 对象，gc_free_all 之后仍可读）。
        PackLib* pack_libs = NULL;
        int pack_lib_count = 0;
        {
            const char* entry_abs = error_get_filename();
            if (package_collect_pack_libs(entry_abs ? entry_abs : path,
                                          &pack_libs, &pack_lib_count) != 0) {
                fprintf(stderr, "[pack] 原生库收集失败，已中止打包\n");
#ifdef _WIN32
                { wchar_t wp[MAX_PATH_LEN]; MultiByteToWideChar(CP_UTF8, 0, bin_path, -1, wp, MAX_PATH_LEN); _wremove(wp); }
#else
                remove(bin_path);
#endif
                free(bin_path);
                return -1;
            }
        }

        #ifdef _WIN32
        const char* PSEP = "\\";
#else
        const char* PSEP = "/";
#endif

        // 是否单文件模式：--onefile 或 resource.toml [pack] onefile = true
        // 包根找不到 leno.toml 时退回入口文件所在目录 —— resource.toml 独立存在，
        // 纯应用目录（无 leno.toml）也能用它声明单文件打包
        int onefile = onefileMode;
        PackRes* pack_res = NULL;
        int pack_res_count = 0;
        unsigned char* res_blob = NULL;
        size_t res_blob_size = 0;
        // resource.toml [pack] icon 解析出来的**绝对路径**（空串 = 没声明）。
        // 两种模式都用它：图标是 prepend 进产物的 VM 基底上的，与是否单文件无关。
        char pack_icon[MAX_PATH_LEN] = {0};
        {
            const char* entry_abs2 = error_get_filename();
            char* proj = entry_abs2 ? package_find_project_root(entry_abs2) : NULL;
            char root[MAX_PATH_LEN];
            if (proj) {
                strncpy(root, proj, sizeof(root) - 1);
                root[sizeof(root) - 1] = '\0';
                free(proj);
            } else if (entry_abs2) {
                strncpy(root, entry_abs2, sizeof(root) - 1);
                root[sizeof(root) - 1] = '\0';
                char* a = strrchr(root, '\\');
                char* b = strrchr(root, '/');
                char* sep = a;
                if (b && (!a || b > a)) sep = b;
                if (sep) *(sep + 1) = '\0';
                else root[0] = '\0';
            } else {
                root[0] = '\0';
            }
            if (root[0]) {
                char toml_path[MAX_PATH_LEN + 16];   /* root 最多 MAX_PATH_LEN，再拼 "resource.toml" */
                snprintf(toml_path, sizeof(toml_path), "%sresource.toml", root);
                PackConfig* res_cfg = package_pack_config_parse(toml_path);
                if (res_cfg) {
                    if (res_cfg->onefile) onefile = 1;
                    // 目录模式下声明了资源却没开单文件：给个明确提示（否则会以为没生效）
                    if (!onefile && res_cfg->resource_count > 0) {
                        printf("[pack] 提示: resource.toml 声明了 %d 个资源模式，但当前不是单文件模式，\n"
                               "       dist/ 不会带上这些资源（需要单文件请加 --onefile 或 [pack] onefile = true）\n",
                               res_cfg->resource_count);
                    }
                    // [pack] icon：相对包根（root 已带结尾分隔符）；写绝对路径的也认
                    if (res_cfg->icon && res_cfg->icon[0]) {
                        const char* ic = res_cfg->icon;
                        int abs_icon = (ic[0] == '/' || ic[0] == '\\' ||
                                        (((ic[0] >= 'A' && ic[0] <= 'Z') ||
                                          (ic[0] >= 'a' && ic[0] <= 'z')) && ic[1] == ':'));
                        if (abs_icon) snprintf(pack_icon, sizeof(pack_icon), "%s", ic);
                        else snprintf(pack_icon, sizeof(pack_icon), "%s%s", root, ic);
                    }
                    package_pack_config_free(res_cfg);
                }
            }
        }

        if (onefile) {
            const char* entry_abs2 = error_get_filename();
            package_collect_pack_resources(entry_abs2 ? entry_abs2 : path,
                                           &pack_res, &pack_res_count);
            if (pack_build_res_blob(pack_libs, pack_lib_count, pack_res, pack_res_count,
                                    &res_blob, &res_blob_size) != 0) {
                fprintf(stderr, "[pack] 内嵌资源段组装失败，已中止打包\n");
                package_pack_res_free(pack_res, pack_res_count);
                package_pack_libs_free(pack_libs, pack_lib_count);
                free(bin_path);
                return -1;
            }
            printf("[pack] 单文件模式: 内嵌 %d 个原生库 + %d 个资源文件（%.1f MB）\n",
                   pack_lib_count, pack_res_count, res_blob_size / 1048576.0);
        }

        // 生成输出目录：-o 指定则原样用（相对当前工作目录），否则 <源码目录>/dist
        char out_dir[MAX_PATH_LEN];
        if (packOutDir) {
            strncpy(out_dir, packOutDir, MAX_PATH_LEN - 1);
            out_dir[MAX_PATH_LEN - 1] = '\0';
        } else {
            strncpy(out_dir, path, MAX_PATH_LEN - 1);
            out_dir[MAX_PATH_LEN - 1] = '\0';
            char* s1 = strrchr(out_dir, '\\');
            char* s2 = strrchr(out_dir, '/');
            if (s2 && (!s1 || s2 > s1)) s1 = s2;
            if (s1) *(s1 + 1) = '\0';   // 保留源码目录（含分隔符）
            else out_dir[0] = '\0';
            strncat(out_dir, "dist", sizeof(out_dir) - strlen(out_dir) - 1);
        }
        if (pack_ensure_dir(out_dir) != 0) {
            fprintf(stderr, "[pack] 错误: 无法创建输出目录: %s\n", out_dir);
            package_pack_libs_free(pack_libs, pack_lib_count);
            free(bin_path);
            return -1;
        }

        // 产物路径 = <输出目录>/<源码名>.exe
        // 缓冲区放大到 4×MAX_PATH_LEN：下面是把两个 MAX_PATH_LEN 量级的串拼一起
        char out_exe[MAX_PATH_LEN * 4];
        {
            const char* base = path;
            const char* b1 = strrchr(path, '\\');
            const char* b2 = strrchr(path, '/');
            if (b1 || b2) {
                const char* bsep = b1;
                if (b2 && (!b1 || b2 > b1)) bsep = b2;
                base = bsep + 1;
            }
            char stem[MAX_PATH_LEN];
            strncpy(stem, base, sizeof(stem) - 1);
            stem[sizeof(stem) - 1] = '\0';
            char* stem_dot = strrchr(stem, '.');
            if (stem_dot) *stem_dot = '\0';
            size_t dlen = strlen(out_dir);
            const char* dsep = (dlen > 0 && out_dir[dlen - 1] != '\\' &&
                                out_dir[dlen - 1] != '/') ? PSEP : "";
            snprintf(out_exe, sizeof(out_exe), "%s%s%s%s", out_dir, dsep, stem,
#ifdef _WIN32
                     ".exe"
#else
                     ""
#endif
            );
        }
        printf("[pack] 输出目录: %s\n", out_dir);

        // 依赖的原生库直接复制到 exe 旁（不能放子目录：Windows 解析 DLL 自身依赖时
        // 只搜主 exe 目录/系统目录/PATH，不看 DLL 自己所在的目录 ⇒ SDL3_image→SDL3 会断）
        if (onefile) {
            printf("[pack] 单文件: dist 内只产出 exe；库与资源在首次运行时解包到用户缓存目录\n");
            // 切换过模式的话，dist 里可能还留着上次目录模式拷进去的同名文件 —— 只提示不删
            // （输出目录是用户的，误删代价远大于留个提示）
            int stale = 0;
            for (int i = 0; i < pack_lib_count; i++) {
                size_t dlen = strlen(out_dir);
                const char* dsep = (dlen > 0 && out_dir[dlen - 1] != '\\' &&
                                    out_dir[dlen - 1] != '/') ? PSEP : "";
                char p[MAX_PATH_LEN * 4];
                snprintf(p, sizeof(p), "%s%s%s", out_dir, dsep, pack_libs[i].file_name);
                if (!pack_file_exists_utf8(p)) continue;
                if (stale == 0) {
                    printf("[pack] 注意: dist 内仍有上次遗留的同名文件（单文件分发只需 exe，可自行删除）:\n");
                }
                printf("        %s\n", pack_libs[i].file_name);
                stale++;
            }
        } else if (pack_lib_count > 0) {
            printf("[pack] 复制原生库 %d 个:\n", pack_lib_count);
            for (int i = 0; i < pack_lib_count; i++) {
                size_t dlen = strlen(out_dir);
                const char* dsep = (dlen > 0 && out_dir[dlen - 1] != '\\' &&
                                    out_dir[dlen - 1] != '/') ? PSEP : "";
                char dst[MAX_PATH_LEN * 4];
                snprintf(dst, sizeof(dst), "%s%s%s", out_dir, dsep, pack_libs[i].file_name);
                if (package_copy_file(pack_libs[i].src_path, dst) != 0) {
                    fprintf(stderr, "[pack] 错误: 复制原生库失败: %s -> %s\n",
                            pack_libs[i].src_path, dst);
                    package_pack_libs_free(pack_libs, pack_lib_count);
                    package_pack_res_free(pack_res, pack_res_count);
                    free(res_blob);
                    free(bin_path);
                    return -1;
                }
                printf("        %s  ← %s\n", pack_libs[i].file_name, pack_libs[i].from_pkg);
            }
        } else {
            printf("[pack] 无需复制原生库（依赖里没有原生库声明）\n");
        }
        package_pack_libs_free(pack_libs, pack_lib_count);
        // 资源清单的内容已拷进 res_blob，这里即可释放（blob 到写盘后再释放）
        package_pack_res_free(pack_res, pack_res_count);

        // 查找 leno_vm：先在与 leno 同目录下找
        // use_gui_vm=1 时用无控制台版 leno_vm_gui.exe
        // （来自 --no-console 开关，或脚本调用了 _console(false) 的自动检测；见上面选择逻辑）
        char vm_exe[MAX_PATH_LEN];
#ifdef _WIN32
        // 获取当前 exe 所在目录
        char exe_dir[MAX_PATH_LEN];
        GetModuleFileNameA(NULL, exe_dir, MAX_PATH_LEN);
        exe_dir[MAX_PATH_LEN - 1] = '\0';
        char* last_sep = strrchr(exe_dir, '\\');
        const char* vm_name = use_gui_vm ? "leno_vm_gui.exe" : "leno_vm.exe";
        if (last_sep) {
            *(last_sep + 1) = '\0';
            size_t dir_len = strlen(exe_dir);
            size_t vm_name_len = strlen(vm_name);
            if (dir_len + vm_name_len < MAX_PATH_LEN) {
                memcpy(vm_exe, exe_dir, dir_len);
                memcpy(vm_exe + dir_len, vm_name, vm_name_len + 1);
            } else {
                strcpy(vm_exe, vm_name);
            }
        } else {
            strcpy(vm_exe, vm_name);
        }
#else
        // Linux/macOS：获取当前可执行文件所在目录
        // （自身路径统一走 platform_self_exe_path；实现见 src/platform/platform_path.c）
        char exe_dir[MAX_PATH_LEN];
        if (platform_self_exe_path(exe_dir, sizeof(exe_dir))) {
            char* last_sep = strrchr(exe_dir, '/');
            if (last_sep) {
                *(last_sep + 1) = '\0';
                const char* vm_name = "leno_vm";
                size_t dir_len = strlen(exe_dir);
                size_t vm_name_len = strlen(vm_name);
                if (dir_len + vm_name_len < MAX_PATH_LEN) {
                    memcpy(vm_exe, exe_dir, dir_len);
                    memcpy(vm_exe + dir_len, vm_name, vm_name_len + 1);
                } else {
                    strcpy(vm_exe, "leno_vm");
                }
            } else {
                strcpy(vm_exe, "leno_vm");
            }
        } else {
            strcpy(vm_exe, "leno_vm");
        }
#endif

        // 读取 leno_vm 文件
#ifdef _WIN32
        wchar_t wvm_exe[MAX_PATH_LEN];
        MultiByteToWideChar(CP_UTF8, 0, vm_exe, -1, wvm_exe, MAX_PATH_LEN);
        FILE* vm_fp = _wfopen(wvm_exe, L"rb");
#else
        FILE* vm_fp = fopen(vm_exe, "rb");
#endif
        if (!vm_fp) {
            fprintf(stderr, "[错误] 找不到 leno_vm: %s\n", vm_exe);
            fprintf(stderr, "请先运行 build_vm.sh 构建 VM 运行时\n");
#ifdef _WIN32
            { wchar_t wp[MAX_PATH_LEN]; MultiByteToWideChar(CP_UTF8, 0, bin_path, -1, wp, MAX_PATH_LEN); _wremove(wp); }
#else
            remove(bin_path);
#endif
            free(bin_path);
            return -1;
        }
        fseek(vm_fp, 0, SEEK_END);
        long vm_size = ftell(vm_fp);
        fseek(vm_fp, 0, SEEK_SET);
        unsigned char* vm_data = (unsigned char*)malloc(vm_size);
        if (!vm_data) {
            fclose(vm_fp);
            free(bin_path);
            return -1;
        }
        if (fread(vm_data, 1, vm_size, vm_fp) != (size_t)vm_size) {
            fclose(vm_fp);
            free(vm_data);
            free(bin_path);
            return -1;
        }
        fclose(vm_fp);

        // ---- 打包前握手（2026-09-25）----
        // 让即将内嵌的这个 VM 校验本次产物：读不出来 ⇒ 打包失败，**不产出坏 exe**。
        // （以前会"打包成功"、双击时才以「内存反序列化失败: 5」闪退；见上面
        //   pack_vm_check_lenb 的说明。）
        {
            int check_rc = pack_vm_check_lenb(vm_exe, bin_path);
            if (check_rc != 0) {
                fprintf(stderr, "[pack] 错误: 即将内嵌的 VM 读不了本次编译产物（VM 退出码 %d）\n",
                        check_rc);
                fprintf(stderr, "       VM: %s\n", vm_exe);
                fprintf(stderr, "       常见原因: 编译器侧改过（序列化格式 / LENO_BIN_VERSION）"
                                "而 VM 还是旧的\n");
                fprintf(stderr, "       处理: 先运行 build_vm.bat 重建 leno_vm / leno_vm_gui，"
                                "再重新打包\n");
                fprintf(stderr, "       （本次未产出 exe；编译出的 .lenb 已保留: %s）\n", bin_path);
                free(vm_data);
                free(bin_path);
                return -1;
            }
        }

        // resource.toml [pack] icon：把 **VM 副本**的应用图标换掉。
        // ⚠ 必须在这一步做（prepend 之前）：图标在 PE 资源段里，也就是文件最开头那段；
        //   而 EndUpdateResource 会按 PE 结构重写整个文件 —— 对**最终 exe**（尾部还挂着
        //   资源段 + lenb）调用会把它们一起丢掉。详见 src/package/package_icon.c。
        if (pack_icon[0]) {
#ifdef _WIN32
            unsigned char* vm_patched = NULL;
            size_t vm_patched_size = 0;
            char icon_err[256];
            int icon_rc = package_icon_replace(vm_data, (size_t)vm_size, pack_icon,
                                               &vm_patched, &vm_patched_size,
                                               icon_err, sizeof(icon_err));
            if (icon_rc == 0) {
                free(vm_data);
                vm_data = vm_patched;
                vm_size = (long)vm_patched_size;
                printf("[pack] 图标: %s\n", pack_icon);
            } else {
                // 用户明确指定了图标 ⇒ 失败就响亮中止，别静默给一个没图标的产物
                fprintf(stderr, "[pack] 错误: 替换图标失败: %s\n", icon_err);
                free(vm_data);
                free(bin_path);
                return -1;
            }
#else
            printf("[pack] 提示: 本平台不支持替换 PE 图标，[pack] icon 已忽略\n");
#endif
        }

        // 读取编译好的 .lenb 文件
#ifdef _WIN32
        wchar_t wlenb_path[MAX_PATH_LEN];
        MultiByteToWideChar(CP_UTF8, 0, bin_path, -1, wlenb_path, MAX_PATH_LEN);
        FILE* lenb_fp = _wfopen(wlenb_path, L"rb");
#else
        FILE* lenb_fp = fopen(bin_path, "rb");
#endif
        if (!lenb_fp) {
            fprintf(stderr, "[错误] 无法读取编译产物: %s\n", bin_path);
            free(vm_data);
#ifdef _WIN32
            { wchar_t wp[MAX_PATH_LEN]; MultiByteToWideChar(CP_UTF8, 0, bin_path, -1, wp, MAX_PATH_LEN); _wremove(wp); }
#else
            remove(bin_path);
#endif
            free(bin_path);
            return -1;
        }
        fseek(lenb_fp, 0, SEEK_END);
        long lenb_size = ftell(lenb_fp);
        fseek(lenb_fp, 0, SEEK_SET);
        unsigned char* lenb_data = (unsigned char*)malloc(lenb_size);
        if (!lenb_data) {
            fclose(lenb_fp);
            free(vm_data);
#ifdef _WIN32
            { wchar_t wp[MAX_PATH_LEN]; MultiByteToWideChar(CP_UTF8, 0, bin_path, -1, wp, MAX_PATH_LEN); _wremove(wp); }
#else
            remove(bin_path);
#endif
            free(bin_path);
            return -1;
        }
        if (fread(lenb_data, 1, lenb_size, lenb_fp) != (size_t)lenb_size) {
            fclose(lenb_fp);
            free(lenb_data);
            free(vm_data);
            free(bin_path);
            return -1;
        }
        fclose(lenb_fp);

        // 删除临时 .lenb 文件（已读入内存）
#ifdef _WIN32
        { wchar_t wp[MAX_PATH_LEN]; MultiByteToWideChar(CP_UTF8, 0, bin_path, -1, wp, MAX_PATH_LEN); _wremove(wp); }
#else
        remove(bin_path);
#endif

        // 写入输出文件。
        //   目录模式:  [vm 数据] [lenb 数据] [4B lenb_size] [4B LENB_MAGIC]
        //   单文件模式: [vm 数据] [资源段] [4B 资源段大小] [4B RES_MAGIC]
        //                       [lenb 数据] [4B lenb_size] [4B LENB_MAGIC]
        //   资源段放在 lenb **之前** ⇒ 末尾 8 字节仍是 LENB_MAGIC，旧 VM 读新 exe 不受影响。
#ifdef _WIN32
        wchar_t wout_exe[MAX_PATH_LEN];
        MultiByteToWideChar(CP_UTF8, 0, out_exe, -1, wout_exe, MAX_PATH_LEN);
        FILE* out_fp = _wfopen(wout_exe, L"wb");
#else
        FILE* out_fp = fopen(out_exe, "wb");
#endif
        if (!out_fp) {
            fprintf(stderr, "[错误] 无法创建输出文件: %s\n", out_exe);
            free(res_blob);
            free(lenb_data);
            free(vm_data);
            free(bin_path);
            return -1;
        }
        fwrite(vm_data, 1, vm_size, out_fp);
        if (onefile) {
            fwrite(res_blob, 1, res_blob_size, out_fp);
            uint32_t res_size_le = (uint32_t)res_blob_size;
            fwrite(&res_size_le, 4, 1, out_fp);
            uint32_t res_magic = 0x524E454C; // "LENR"
            fwrite(&res_magic, 4, 1, out_fp);
        }
        fwrite(lenb_data, 1, lenb_size, out_fp);
        uint32_t lenb_size_le = (uint32_t)lenb_size;
        fwrite(&lenb_size_le, 4, 1, out_fp);
        uint32_t magic = 0x424E454C; // "LENB"
        fwrite(&magic, 4, 1, out_fp);
        fclose(out_fp);

#ifndef _WIN32
        // Linux/macOS：添加可执行权限
        chmod(out_exe, 0755);
#endif

        free(vm_data);
        free(lenb_data);
        free(res_blob);
        free(bin_path);

        {
            clock_t pack_end = clock();
            double embed_ms = (double)(pack_end - pack_compile_end) / CLOCKS_PER_SEC * 1000.0;
            double total_ms = (double)(pack_end - pack_t0) / CLOCKS_PER_SEC * 1000.0;
            printf("打包成功: %s -> %s (%.1f KB)%s\n", path, out_exe,
                   (vm_size + lenb_size + 8 + (onefile ? (double)res_blob_size + 8 : 0)) / 1024.0,
                   onefile ? " [单文件]" : "");
            if (onefile) {
                printf("单文件分发：只需拷贝这一个 exe；首次运行时依赖会自动解包到用户缓存目录\n");
            }
            printf("打包嵌入耗时: %.1f ms\n", embed_ms);
            printf("总耗时: %.1f ms\n", total_ms);
        }

        if (pauseMode) {
            printf("\n按任意键继续...");
            getchar();
        }
        return 0;
    }

    // 输出源代码
   if (!debugMode) 
    {
        // printf("===== 源代码 =====\n");
        //  printf("%s", source);
        //  printf("\n===== 执行结果 =====\n\n");
    }
    
    // ===== 入口文件缓存 =====
    // 检查入口文件的 .lenb 缓存，如果源码哈希不变则直接加载跳过编译
    if (module_loader_is_cache_enabled() && !compileMode && !packMode && !debugMode) {
        const char* abs_f = error_get_filename();
        const char* cache_dir = module_loader_get_cache_dir();
        if (abs_f && cache_dir && cache_dir[0]) {
            // 计算源码哈希
            uint64_t src_hash = serialize_source_hash(source, strlen(source));
            // 生成缓存路径：<cache_dir>entry_<hash>.lenb
            char cache_dir_norm[MAX_PATH_LEN];
            strncpy(cache_dir_norm, cache_dir, MAX_PATH_LEN - 1);
            cache_dir_norm[MAX_PATH_LEN - 1] = '\0';
            size_t dlen = strlen(cache_dir_norm);
            // 确保目录后有分隔符
    #ifdef _WIN32
            if (dlen > 0 && cache_dir_norm[dlen-1] != '\\' && cache_dir_norm[dlen-1] != '/') {
                cache_dir_norm[dlen] = '\\'; cache_dir_norm[dlen+1] = '\0';
            }
    #else
            if (dlen > 0 && cache_dir_norm[dlen-1] != '/') {
                cache_dir_norm[dlen] = '/'; cache_dir_norm[dlen+1] = '\0';
            }
    #endif
            snprintf(g_entry_cache_path, sizeof(g_entry_cache_path),
                     "%sentry_%llx.lenb", cache_dir_norm, (unsigned long long)src_hash);
            snprintf(g_entry_deps_path, sizeof(g_entry_deps_path),
                     "%s.deps", g_entry_cache_path);
            g_entry_cache_enabled = 1;

            // 依赖清单校验：.lenb 里内联了全部 import 模块的字节码，而它的键只含入口
            // 文件——只改被引用模块（入口不变）时键不变却已过期。清单缺失/不符即视为
            // 未命中，并把过期的入口缓存删掉（避免遗留与误用）。
            if (entry_deps_valid(g_entry_deps_path) != 0) {
                entry_deps_remove(g_entry_cache_path);
                entry_deps_remove(g_entry_deps_path);
            } else {
                // 尝试加载缓存
                Chunk entry_chunk;
                Scope* entry_scope = NULL;
                SerializeResult cache_sr = chunk_deserialize(g_entry_cache_path, &entry_chunk, &entry_scope);
                if (cache_sr == SERIALIZE_OK) {
                    register_defs_from_chunk(&entry_chunk);
                    gc_init();
                    fix_module_function_ptrs(&entry_chunk);
                    vm_init_with_scope(entry_scope);
                    vm_load(&entry_chunk);
                    int ret = vm_run();
                    chunk_free(&entry_chunk);
                    gc_free_all();
                    free(source);
                    if (ret != 0 || error_has_any()) {
                        error_print_all();
                        warning_print_all();
                        return -1;
                    }
                    warning_print_all();
                    return vm_get_exit_code();
                }
            }
        }
    }

    int result = lenolang_run(source);
    free(source);
    
    // 暂停模式
    if (pauseMode) {
        printf("\n按任意键继续...");
        getchar();
    }
    
    return result;
}


// 主函数逻辑
static int main_logic(int argc, char** argv) {
    setupConsole();

    // 注册模块编译器函数指针（解耦 module_loader 和编译器）
    set_module_compile_func(compile_module_new);

    // 环境变量禁用缓存（LENO_NO_CACHE=1）
    if (getenv("LENO_NO_CACHE") != NULL) {
        module_loader_set_cache_enabled(0);
    }

    // 保存命令行参数，供 _args() 全局函数使用
    g_argc = argc;
    g_argv = argv;

    const char* filePath = NULL;
    char* joined_path = NULL;  // 动态分配，用于拼接含空格的路径（需在返回前释放）
    
    // 解析参数
    // Leno 内置选项（--pause, --debug 等）在任何位置都生效
    // 第一个非选项参数作为脚本路径，之后的非选项参数传给脚本
    // 支持含空格的路径：如果首个非选项参数不是有效文件，尝试拼接后续参数
    int file_arg_start = -1;  // 第一个非选项参数的索引
    int options_terminated = 0;  // 是否已遇到 '--'（终止解释器自己的选项解析；P2）
    for (int i = 1; i < argc; i++) {
        // 先检查是否是 Leno 内置选项（在任何位置都处理）
        if (strcmp(argv[i], "--pause") == 0) {
            pauseMode = 1;
            continue;
        } else if (strcmp(argv[i], "--debug") == 0) {
            debugMode = 1;
            continue;
        } else if (strcmp(argv[i], "--debug-out") == 0) {
            debugMode = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                debugOutFile = argv[i + 1];
                i++;  // 消费文件名参数
            } else {
                fprintf(stderr, "错误: --debug-out 需要指定输出文件路径\n");
                return 1;
            }
            continue;
        } else if (strcmp(argv[i], "--compile") == 0 || strcmp(argv[i], "-c") == 0) {
            compileMode = 1;
            continue;
        } else if (strcmp(argv[i], "--pack") == 0 || strcmp(argv[i], "-p") == 0) {
            packMode = 1;
            continue;
        } else if (strcmp(argv[i], "--pack-dir") == 0 || strcmp(argv[i], "-o") == 0) {
            // 打包输出目录（相对当前工作目录；默认 <源码目录>/dist）
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                packOutDir = argv[i + 1];
                i++;  // 消费目录参数
            } else {
                fprintf(stderr, "错误: -o/--pack-dir 需要指定输出目录\n");
                return 1;
            }
            continue;
        } else if (strcmp(argv[i], "--onefile") == 0) {
            // 单文件打包：把 [native-libs] 的库与 [pack] resources 内嵌进 exe，
            // 首次运行解包到**用户缓存目录**（按内容哈希分目录；详见 vm_main.c 的
            // extract_embedded_resources）。**不是** exe 旁 —— 那样会污染桌面/下载目录。
            onefileMode = 1;
            continue;
        } else if (strcmp(argv[i], "--console") == 0) {
            // 打包时强制用控制台版 leno_vm.exe（覆盖脚本里的 _console(false) 自动检测）
            if (packConsoleMode == PACK_CONSOLE_NONE) {
                fprintf(stderr, "错误: --console 与 --no-console 互斥\n");
                return 1;
            }
            packConsoleMode = PACK_CONSOLE_FORCE;
            continue;
        } else if (strcmp(argv[i], "--no-console") == 0) {
            // 打包时强制用无控制台版 leno_vm_gui.exe（-mwindows；Windows 才有此变体）
            if (packConsoleMode == PACK_CONSOLE_FORCE) {
                fprintf(stderr, "错误: --console 与 --no-console 互斥\n");
                return 1;
            }
            packConsoleMode = PACK_CONSOLE_NONE;
            continue;
        } else if (strcmp(argv[i], "--no-cache") == 0) {
            module_loader_set_cache_enabled(0);
            continue;
        } else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printVersion();
            return 0;
        } else if (strcmp(argv[i], "--init") == 0) {
            initMode = 1;
            continue;
        } else if (strcmp(argv[i], "--install") == 0) {
            installMode = 1;
            continue;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printHelp(argv[0]);
            return 0;
        }

        // 非选项参数
        if (argv[i][0] != '-') {
            if (file_arg_start < 0) {
                file_arg_start = i;
            }
            // 后续非选项参数保留给脚本（由 _args() 获取）
            continue;
        }

        // '--'：显式终止**解释器自己**的选项解析。只在脚本路径之前有意义：
        // 脚本路径之后的 '--' 与其它参数一样原样交给脚本（与 _args() 的取法保持一致）。
        if (file_arg_start < 0 && strcmp(argv[i], "--") == 0) {
            options_terminated = 1;
            continue;
        }

        // ★ P2 修复（2026-09-18）：脚本路径**之后**的 '-' 开头参数，一律属于脚本 / 位置参数。
        //   此前它走下面的"未知选项"分支 ⇒ `leno.exe trae_sign.leno --list` 会打印解释器帮助、
        //   **脚本根本不跑** ✗（移植 TraeSign 时几乎静默地卡住；参考件的 CLI 全是 --status/--json/--list，
        //   照抄必踩）。这条与 `_args()` 的取法一致：它把"脚本路径之后"的参数**原样**返回。
        if (options_terminated || file_arg_start >= 0) {
            if (file_arg_start < 0) {
                // '--' 之后的第一个参数就是脚本路径，哪怕它以 '-' 开头
                file_arg_start = i;
            }
            continue;
        }

        // 未知的 '-' 开头选项（只会在脚本路径之前、且未遇到 '--' 时到达这里）
        fprintf(stderr, "未知选项: %s\n", argv[i]);
        printHelp(argv[0]);
        return 64;
    }

    // 尝试拼接非选项参数以支持含空格的文件路径
    // 从 file_arg_start 开始，逐步拼接更多参数，直到找到存在的文件
    if (file_arg_start >= 0) {
        char buf[MAX_PATH_LEN];
        buf[0] = '\0';
        filePath = NULL;
        for (int i = file_arg_start; i < argc; i++) {
            // 跳过已被识别为选项的参数
            if (strcmp(argv[i], "--pause") == 0 || strcmp(argv[i], "--debug") == 0 ||
                strcmp(argv[i], "--compile") == 0 || strcmp(argv[i], "-c") == 0 ||
                strcmp(argv[i], "--pack") == 0 || strcmp(argv[i], "-p") == 0 ||
                strcmp(argv[i], "--pack-dir") == 0 || strcmp(argv[i], "-o") == 0 ||
                strcmp(argv[i], "--onefile") == 0 ||
                strcmp(argv[i], "--console") == 0 || strcmp(argv[i], "--no-console") == 0 ||
                strcmp(argv[i], "--no-cache") == 0 ||
                strcmp(argv[i], "--init") == 0 || strcmp(argv[i], "--install") == 0 ||
                strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0 ||
                strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                continue;
            }
            // 跳过选项参数（但 '--' 之后不再跳过：那之后的 '-' 开头参数是脚本路径/参数，P2）
            if (argv[i][0] == '-' && !options_terminated) continue;

            if (buf[0] != '\0') {
                size_t cur_len = strlen(buf);
                if (cur_len + 1 < MAX_PATH_LEN) {
                    buf[cur_len] = ' ';
                    buf[cur_len + 1] = '\0';
                }
            }
            size_t remain = MAX_PATH_LEN - strlen(buf) - 1;
            strncat(buf, argv[i], remain);

            // 检查拼接后的路径是否存在（文件或目录）
#ifdef _WIN32
            int wlen = MultiByteToWideChar(CP_UTF8, 0, buf, -1, NULL, 0);
            if (wlen > 0) {
                wchar_t* wcheck = (wchar_t*)malloc(wlen * sizeof(wchar_t));
                if (wcheck) {
                    MultiByteToWideChar(CP_UTF8, 0, buf, -1, wcheck, wlen);
                    DWORD attrs = GetFileAttributesW(wcheck);
                    free(wcheck);
                    if (attrs != INVALID_FILE_ATTRIBUTES) {
                        joined_path = strdup(buf);
                        filePath = joined_path;
                        break;
                    }
                }
            }
#else
            struct stat st;
            if (stat(buf, &st) == 0) {
                joined_path = strdup(buf);
                filePath = joined_path;
                break;
            }
#endif
        }
        // 如果拼接后仍未找到有效路径，使用第一个非选项参数
        if (!filePath) {
            for (int i = file_arg_start; i < argc; i++) {
                if (strcmp(argv[i], "--pause") == 0 || strcmp(argv[i], "--debug") == 0 ||
                    strcmp(argv[i], "--compile") == 0 || strcmp(argv[i], "-c") == 0 ||
                    strcmp(argv[i], "--pack") == 0 || strcmp(argv[i], "-p") == 0 ||
                    strcmp(argv[i], "--pack-dir") == 0 || strcmp(argv[i], "-o") == 0 ||
                    strcmp(argv[i], "--onefile") == 0 ||
                    strcmp(argv[i], "--console") == 0 || strcmp(argv[i], "--no-console") == 0 ||
                    strcmp(argv[i], "--no-cache") == 0 ||
                    strcmp(argv[i], "--init") == 0 || strcmp(argv[i], "--install") == 0 ||
                    strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0 ||
                    strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                    continue;
                }
                if (argv[i][0] == '-') continue;
                filePath = argv[i];
                break;
            }
        }
    }
    
    if (initMode) {
        int result;
        if (filePath) {
            /* 如果参数包含路径分隔符，当作目录路径；否则在当前目录下创建子目录 */
            int is_path = strchr(filePath, '/') || strchr(filePath, '\\');
            if (is_path) {
                result = package_init(filePath, NULL);
            } else {
                result = package_init(filePath, filePath);
            }
        } else {
            result = package_init(".", NULL);
        }
        free(joined_path);
        return result;
    } else if (installMode) {
        int result;
        package_cache_ensure();
        if (filePath) {
            /* leno --install <git-url> 或 <本地目录路径> */
            if (strncmp(filePath, "gitee:", 6) == 0 ||
                strncmp(filePath, "github:", 7) == 0 ||
                strncmp(filePath, "gitlab:", 7) == 0 ||
                strncmp(filePath, "git:", 4) == 0 ||
                strncmp(filePath, "https://", 8) == 0 ||
                strncmp(filePath, "http://", 7) == 0 ||
                strstr(filePath, "git@")) {
                /* git 源 → 远程安装 */
                result = package_install_from_git(filePath);
            } else {
                /* 本地目录路径 → 本地安装 */
                result = package_install_from_dir(filePath);
            }
        } else {
            /* leno --install - 从当前目录的 leno.toml 安装所有依赖 */
            char toml_path[MAX_PATH_LEN];
            snprintf(toml_path, sizeof(toml_path), "leno.toml");
            result = package_install_deps(toml_path);
        }
        free(joined_path);
        return result;
    } else if (filePath == NULL) {
        // 没有文件参数，显示帮助信息
        printHelp(argv[0]);
        // 暂停以便用户查看帮助信息
        printf("\n按任意键继续...");
        getchar();
        free(joined_path);
        return 0;
    } else {
        // 文件模式
        int result = lenolang_run_file(filePath);
        free(joined_path);
        return result;
    }
    
    free(joined_path);
}

#ifdef _WIN32
// Windows: 使用 wmain 支持 Unicode 命令行
int wmain(int argc, wchar_t* wargv[]) {
    // 注册模块编译器函数指针（解耦 module_loader 和编译器）
    set_module_compile_func(compile_module_new);

    // 将宽字符参数转换为 UTF-8
    char** argv = (char**)malloc((argc + 1) * sizeof(char*));
    if (!argv) {
        fprintf(stderr, "内存分配失败\n");
        return 1;
    }
    
    for (int i = 0; i < argc; i++) {
        int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        argv[i] = (char*)malloc(len);
        if (!argv[i]) {
            fprintf(stderr, "内存分配失败\n");
            return 1;
        }
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, argv[i], len, NULL, NULL);
    }
    argv[argc] = NULL;
    
    int result = main_logic(argc, argv);
    
    // 释放内存
    for (int i = 0; i < argc; i++) {
        free(argv[i]);
    }
    free(argv);
    
    return result;
}
#else
// Linux/macOS: 使用标准 main
int main(int argc, char* argv[]) {
    return main_logic(argc, argv);
}
#endif
