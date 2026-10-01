#include "include/leno_vm_runtime.h"
#include "include/module_dispatch.h"
// v33（收敛 S10）：导出名的唯一来源是模块符号表扫描器 —— 本文件此前自带一份复刻的文本
// 扫描器 extract_exports()（已删除），现改为只读 module_symbol_table_export_names()/has_export()
#include "include/module_symbol_table.h"
#include "include/leno_serialize.h"
#include "include/leno_dce.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>  // _wmkdir
#endif

#define MAX_MODULE_NAME 128
#define MAX_EXPORT_NAME 128
#define MAX_EXPORTS 512
#define MAX_LOADED_MODULES 128

typedef struct {
    char names[MAX_EXPORTS][MAX_EXPORT_NAME];
    int count;
} ExportList;

typedef struct {
    char paths[MAX_LOADED_MODULES][MAX_PATH_LEN];
    ObjModule* modules[MAX_LOADED_MODULES];
    int count;
} LoadedModules;

static LoadedModules loaded_modules = {0};

// 模块编译缓存配置
static char* g_cache_dir = NULL;   // 缓存目录（NULL 表示未配置/禁用）
static int g_cache_enabled = 1;    // 缓存开关

// 检查模块是否已加载，如果已加载返回模块对象
static void add_loaded_module(const char* path, ObjModule* module);

void add_loaded_module_public(const char* path, ObjModule* module) {
    add_loaded_module(path, module);
}

ObjModule* find_loaded_module(const char* path) {
    for (int i = 0; i < loaded_modules.count; i++) {
        if (strcmp(loaded_modules.paths[i], path) == 0) {
            return loaded_modules.modules[i];
        }
    }
    return NULL;
}

// 获取已加载模块数量（供序列化遍历依赖）
int loaded_modules_get_count(void) {
    return loaded_modules.count;
}

// 获取指定索引的已加载模块
ObjModule* loaded_modules_get(int index) {
    if (index < 0 || index >= loaded_modules.count) return NULL;
    return loaded_modules.modules[index];
}

// 获取指定索引的模块源文件路径（GC 释放后仍可读，见头文件说明）
const char* loaded_modules_get_path(int index) {
    if (index < 0 || index >= loaded_modules.count) return NULL;
    return loaded_modules.paths[index];
}

// 启用/禁用模块编译缓存
void module_loader_set_cache_enabled(int enabled) {
    g_cache_enabled = enabled;
}

// 查询缓存是否启用（供 main.c 在设置缓存目录前判断）
int module_loader_is_cache_enabled(void) {
    return g_cache_enabled;
}

// 获取缓存目录路径（供符号表缓存计算缓存文件路径）
const char* module_loader_get_cache_dir(void) {
    return g_cache_dir;
}

// 递归创建目录（用于缓存目录）
static void ensure_cache_dir(const char* dir) {
    if (!dir || !*dir) return;
    char tmp[MAX_PATH_LEN];
    strncpy(tmp, dir, MAX_PATH_LEN - 1);
    tmp[MAX_PATH_LEN - 1] = '\0';
    size_t len = strlen(tmp);
    // 去掉末尾分隔符
    if (len > 0) {
        char last = tmp[len - 1];
        if (last == '/' || last == '\\') {
            tmp[len - 1] = '\0';
        }
    }
    // 逐级创建
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = '\0';
#ifdef _WIN32
            {
                int wl = MultiByteToWideChar(CP_UTF8, 0, tmp, -1, NULL, 0);
                wchar_t* wp = (wchar_t*)malloc(wl * sizeof(wchar_t));
                if (wp) {
                    MultiByteToWideChar(CP_UTF8, 0, tmp, -1, wp, wl);
                    _wmkdir(wp);
                    free(wp);
                }
            }
#else
            mkdir(tmp, 0755);
#endif
            *p = c;
        }
    }
    // 最后一级
#ifdef _WIN32
    {
        int wl = MultiByteToWideChar(CP_UTF8, 0, tmp, -1, NULL, 0);
        wchar_t* wp = (wchar_t*)malloc(wl * sizeof(wchar_t));
        if (wp) {
            MultiByteToWideChar(CP_UTF8, 0, tmp, -1, wp, wl);
            _wmkdir(wp);
            free(wp);
        }
    }
#else
    mkdir(tmp, 0755);
#endif
}

// 设置模块缓存目录
void module_loader_set_cache_dir(const char* dir) {
    if (g_cache_dir) {
        free(g_cache_dir);
        g_cache_dir = NULL;
    }
    if (dir && *dir) {
        g_cache_dir = strdup(dir);
        ensure_cache_dir(g_cache_dir);
    }
}

// 添加已加载模块
static void add_loaded_module(const char* path, ObjModule* module) {
    if (loaded_modules.count >= MAX_LOADED_MODULES) {
        fprintf(stderr, "[错误] 已加载模块数量超过上限 %d，'%s' 被忽略\n",
                MAX_LOADED_MODULES, path);
        return;
    }
    size_t path_len = strlen(path);
    if (path_len >= MAX_PATH_LEN) path_len = MAX_PATH_LEN - 1;
    memcpy(loaded_modules.paths[loaded_modules.count], path, path_len);
    loaded_modules.paths[loaded_modules.count][path_len] = '\0';
    loaded_modules.modules[loaded_modules.count] = module;
    loaded_modules.count++;
}

// 更新已加载模块（用于循环依赖场景）
static void update_loaded_module(const char* path, ObjModule* module) {
    for (int i = 0; i < loaded_modules.count; i++) {
        if (strcmp(loaded_modules.paths[i], path) == 0) {
            loaded_modules.modules[i] = module;
            return;
        }
    }
}

// 提取模块名称（从文件路径）
static void extract_module_name(const char* file_path, char* out_name, int max_len) {
    const char* base = strrchr(file_path, '/');
    if (!base) base = strrchr(file_path, '\\');
    if (!base) base = file_path;
    else base++;

    const char* dot = strrchr(base, '.');
    if (dot) {
        int len = (int)(dot - base);
        if (len >= max_len) len = max_len - 1;
        strncpy(out_name, base, len);
        out_name[len] = '\0';
    } else {
        strncpy(out_name, base, max_len - 1);
        out_name[max_len - 1] = '\0';
    }
}

// 提取导出项
// ============================================================================
// v33（收敛 S10）：这里**曾经**是一个独立的手写文本扫描器
//   static void extract_exports(const char* source, ExportList* list)
// 它重读源码文本判"什么算 export"（`strncmp(p,"export",6)` + 自己跳注释/字符串/反引号 +
// 循环跳 `const` 后的类型关键字 + "类型在前"兜底分支），与 module_symbol_table 的扫描链
// 各判一遍同一件语义 —— 语言每加一条声明语法就得改两处，漏一处就是"导出名对不上"类静默错
// （典型症状：跨模块 `m.foo()` 报「模块 'm' 中没有方法 'foo'」，而源码里明明 export 了）。
// 现已删除：导出名的唯一实现者 = 扫描链（module_symbol_table/inc/scan/*.inc 在顶层
// `export` 声明处调 module_symbol_table_add_export_name），消费者只读
// module_symbol_table_export_names() / module_symbol_table_has_export()。
// 详见 docs/待办_单一事实来源与重复实现收敛.md 第二节 S10。
// ============================================================================
// 规范化路径（统一使用平台特定的分隔符，处理 . 和 ..）
int normalize_path(char* path, int max_len) {
    char result[MAX_PATH_LEN];
    int result_len = 0;

#ifdef _WIN32
    // Windows: 统一转换为反斜杠
    for (int i = 0; path[i] && i < max_len; i++) {
        if (path[i] == '/') path[i] = '\\';
    }
    const char sep = '\\';
#else
    // Linux/macOS: 统一转换为正斜杠
    for (int i = 0; path[i] && i < max_len; i++) {
        if (path[i] == '\\') path[i] = '/';
    }
    const char sep = '/';
#endif

    const char* p = path;
    while (*p && result_len < MAX_PATH_LEN - 1) {
        if (*p == sep && *(p+1) == '.') {
            if (*(p+2) == sep || *(p+2) == '\0') {
                // ./ 跳过
                p += 2;
                continue;
            } else if (*(p+2) == '.' && (*(p+3) == sep || *(p+3) == '\0')) {
                // ../ 返回上一级
                p += 3;
                while (result_len > 0 && result[result_len-1] != sep) {
                    result_len--;
                }
                if (result_len > 0) result_len--;
                continue;
            }
        }
        result[result_len++] = *p++;
    }
    result[result_len] = '\0';

    strncpy(path, result, max_len - 1);
    path[max_len - 1] = '\0';
    return 1;
}

// 读取文件内容
static char* read_file(const char* file_path) {
#ifdef _WIN32
    // Windows 下使用宽字符版本以支持中文路径
    int wlen = MultiByteToWideChar(CP_UTF8, 0, file_path, -1, NULL, 0);
    if (wlen <= 0) {
        return NULL;
    }
    wchar_t* wpath = (wchar_t*)malloc(wlen * sizeof(wchar_t));
    if (!wpath) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, file_path, -1, wpath, wlen);
    
    FILE* file = _wfopen(wpath, L"r");
    free(wpath);
#else
    FILE* file = fopen(file_path, "r");
#endif
    if (!file) {
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char* content = (char*)malloc(size + 1);
    if (!content) {
        fclose(file);
        return NULL;
    }

    size_t read = fread(content, 1, size, file);
    content[read] = '\0';
    fclose(file);

    return content;
}

// 解析模块文件的完整（规范化）路径
// 成功返回 1 并写入 full_path，失败返回 0。
// 与 read_module_file / load_module_file 原来的内联逻辑完全一致，抽出来是为了让
// 调用方（如导出项扫描缓存）能用同一个「完整路径」做缓存键 / stat。
static int module_resolve_path(char* full_path, const char* file_path, const char* current_file) {
    char normalized_current[MAX_PATH_LEN];

    if (current_file != NULL) {
        strncpy(normalized_current, current_file, MAX_PATH_LEN - 1);
        normalized_current[MAX_PATH_LEN - 1] = '\0';
        // 统一使用平台特定的分隔符
#ifdef _WIN32
        for (int i = 0; normalized_current[i]; i++) {
            if (normalized_current[i] == '/') normalized_current[i] = '\\';
        }
#else
        for (int i = 0; normalized_current[i]; i++) {
            if (normalized_current[i] == '\\') normalized_current[i] = '/';
        }
#endif
    }

    if (current_file != NULL && file_path[0] != '/' && file_path[0] != '\\' &&
        !(file_path[1] == ':' && (file_path[2] == '/' || file_path[2] == '\\'))) {
#ifdef _WIN32
        const char* last_slash = strrchr(normalized_current, '\\');
#else
        const char* last_slash = strrchr(normalized_current, '/');
#endif

        if (last_slash != NULL) {
            size_t dir_len = last_slash - normalized_current + 1;
            if (dir_len >= MAX_PATH_LEN) {
                return 0;
            }
            memcpy(full_path, normalized_current, dir_len);
            full_path[dir_len] = '\0';
            if (strlen(full_path) + strlen(file_path) >= MAX_PATH_LEN) {
                return 0;
            }
            strcat(full_path, file_path);
        } else {
            if (strlen(file_path) >= MAX_PATH_LEN) {
                return 0;
            }
            strcpy(full_path, file_path);
        }
    } else {
        if (strlen(file_path) >= MAX_PATH_LEN) {
            return 0;
        }
        strcpy(full_path, file_path);
    }

    if (!normalize_path(full_path, MAX_PATH_LEN)) {
        return 0;
    }
    return 1;
}

// 读取模块文件
char* read_module_file(const char* file_path, const char* current_file) {
    char full_path[MAX_PATH_LEN];
    if (!module_resolve_path(full_path, file_path, current_file)) {
        return NULL;
    }
    return read_file(full_path);
}

// 编译模块 - 通过函数指针调用，实现编译器与加载器解耦
// 编译模块 - 通过函数指针调用，实现编译器与加载器解耦
static ObjModule* compile_module_dispatch(const char* source, const char* module_name, ExportList* exports) {
    ModuleCompileFunc compile_func = get_module_compile_func();
    if (!compile_func) {
        fprintf(stderr, "[错误] 模块编译器未注册\n");
        return NULL;
    }
    // 将 ExportList 转换为 char[][MAX_EXPORT_NAME] 格式（堆分配避免栈溢出）
    char (*export_names)[MAX_EXPORT_NAME] = (char(*)[MAX_EXPORT_NAME])malloc(exports->count * MAX_EXPORT_NAME);
    if (!export_names) {
        fprintf(stderr, "[错误] 内存分配失败\n");
        return NULL;
    }
    for (int i = 0; i < exports->count && i < MAX_EXPORTS; i++) {
        strncpy(export_names[i], exports->names[i], MAX_EXPORT_NAME - 1);
        export_names[i][MAX_EXPORT_NAME - 1] = '\0';
    }
    ObjModule* result = compile_func(source, module_name, export_names, exports->count);
    free(export_names);
    return result;
}

// 重置已加载模块列表
void reset_loaded_modules(void) {
    loaded_modules.count = 0;
}

// 标记所有已加载模块（供 GC 使用）
void loaded_modules_mark_all(void) {
    for (int i = 0; i < loaded_modules.count; i++) {
        if (loaded_modules.modules[i]) {
            extern void gc_mark_object(Object* obj);
            gc_mark_object((Object*)loaded_modules.modules[i]);
        }
    }
}

// v33（收敛 S10）：此处**曾经**有一套"导出项扫描缓存"（ExportScanCacheEntry +
// module_file_stamp + export_scan_cache_lookup/store），专门缓存上面那份复刻扫描器的结果。
// 扫描器删除后它已无意义 —— 符号表侧本就有同机制的进程内记忆化，按「绝对路径 + mtime/size」
// 失效（module_symbol_table_get_shared → sym_memo_lookup），且与语义分析**共用同一张表**，
// 比原先"语义分析扫一遍 + 导出名再扫一遍"更省。
// ============================================================================
// 导出名提供者（S10 迁移：导出名的来源可以从"文本扫描链"切到"parser AST"）
// ============================================================================
// 为什么需要这一层间接：本文件在 `sources_core.txt`（**VM-only 也编**），而 lexer/parser
//   只在 `sources_compiler.txt` ⇒ 这里**不能**直接调 parser（否则 `build_vm.bat` 链接必炸，
//   论证见 docs/待办_单一事实来源与重复实现收敛.md:2713-2717）。于是把"取某模块的导出名"
//   抽象成可注册的函数指针：
//     · 编译期（main.c）：注册基于 parser AST 的实现 ⇒ 导出名的唯一来源变成**语法** ✓
//     · VM-only：不注册 ⇒ 自动回退扫描链（VM 侧本来也只消费已有产物 ✓）
//   签名与 copy_module_export_names 同形（固定二维缓冲）⇒ 调用点改动最小 ✓
static ModuleExportNamesProvider g_export_names_provider = NULL;

void module_set_export_names_provider(ModuleExportNamesProvider provider) {
    g_export_names_provider = provider;
}

// v33（收敛 S10）：把某模块的导出名拷进固定容量缓冲区。
//   来源优先级：① 已注册的 parser AST 提供者（编译期）；② 符号表扫描链（回退 / VM-only）
// file_path 可以是裸包名/相对路径/绝对路径：解析口径交给 module_symbol_table_get_shared
// （它与解析器/加载器共用 package_resolve_import_spec，比旧的"加载器自己拼路径"更准，
//   见 S9 的收敛结论）。返回拷入的名字数；失败返回 -1（与旧行为一致 ⇒ 调用方据此报错）。
static int copy_module_export_names(const char* file_path, const char* current_file,
                                    char (*out)[MAX_EXPORT_NAME], int max_names) {
    // ① 优先走 parser AST（唯一来源 = 语法）。
    //    ⚠ 提供者内部失败（读不了 / 语法错）返回 <0 ⇒ **回退**扫描链：新路径永远不会比原来更差 ✓
    if (g_export_names_provider) {
        int n = g_export_names_provider(file_path, current_file, out, max_names);
        if (n >= 0) return n;
    }

    // ② 回退：符号表扫描链（与 S10 收敛时的行为逐字一致 ✓）
    ModuleSymbolTable* table = module_symbol_table_get_shared(file_path, current_file);
    if (!table) return -1;
    int total = 0;
    const char* const* names = module_symbol_table_export_names(table, &total);
    if (!names || total <= 0) return 0;
    int count = total < max_names ? total : max_names;
    for (int i = 0; i < count; i++) {
        strncpy(out[i], names[i], MAX_EXPORT_NAME - 1);
        out[i][MAX_EXPORT_NAME - 1] = '\0';
    }
    return count;
}

// 从模块文件中提取导出项（用于语义分析）
int extract_module_exports_from_file(const char* file_path, const char* current_file,
                                      char exports[][MAX_EXPORT_NAME], int max_exports) {
    return copy_module_export_names(file_path, current_file, exports, max_exports);
}

// 检查模块中是否存在指定的方法
// ⚠ 语义必须逐字坚持"查本模块顶层 export 声明的名字"，**不是** funcs[]
//   （那张表是 export ∪ 本地非导出函数 + 导入别名 ⇒ 拿它判会让
//   「模块 'm' 中没有方法 'x'」**少报**，正是收敛时最容易改坏的地方）。
//   现在优先走与 copy_module_export_names **同一份**名字清单（provider）⇒ 两者不会漂移 ✓
int module_has_method(const char* file_path, const char* current_file, const char* method_name) {
    if (g_export_names_provider) {
        char (*names)[MAX_EXPORT_NAME] =
            (char(*)[MAX_EXPORT_NAME])malloc(sizeof(char) * MAX_EXPORT_NAME * MAX_EXPORTS);
        if (names) {
            int n = g_export_names_provider(file_path, current_file, names, MAX_EXPORTS);
            if (n >= 0) {
                int found = 0;
                for (int i = 0; i < n && !found; i++) {
                    if (strcmp(names[i], method_name) == 0) found = 1;
                }
                free(names);
                return found;
            }
            free(names);   // 提供者失败 ⇒ 回退扫描链 ✓
        }
    }
    ModuleSymbolTable* table = module_symbol_table_get_shared(file_path, current_file);
    if (!table) return -1;
    return module_symbol_table_has_export(table, method_name) ? 1 : 0;
}

// 加载并编译模块文件
ObjModule* load_module_file(const char* file_path, const char* current_file, const char* alias_name) {
    char full_path[MAX_PATH_LEN];
    char normalized_current[MAX_PATH_LEN];

    if (current_file != NULL) {
        strncpy(normalized_current, current_file, MAX_PATH_LEN - 1);
        normalized_current[MAX_PATH_LEN - 1] = '\0';
        for (int i = 0; normalized_current[i]; i++) {
            if (normalized_current[i] == '/') normalized_current[i] = '\\';
        }
    }

    if (current_file != NULL && file_path[0] != '/' && file_path[0] != '\\' &&
        !(file_path[1] == ':' && (file_path[2] == '/' || file_path[2] == '\\'))) {
        const char* last_slash = strrchr(normalized_current, '\\');

        if (last_slash != NULL) {
            size_t dir_len = last_slash - normalized_current + 1;
            if (dir_len >= MAX_PATH_LEN) {
                fprintf(stderr, "[错误] 路径过长\n");
                return NULL;
            }
            memcpy(full_path, normalized_current, dir_len);
            full_path[dir_len] = '\0';
            if (strlen(full_path) + strlen(file_path) >= MAX_PATH_LEN) {
                fprintf(stderr, "[错误] 路径过长\n");
                return NULL;
            }
            strcat(full_path, file_path);
        } else {
            if (strlen(file_path) >= MAX_PATH_LEN) {
                fprintf(stderr, "[错误] 路径过长\n");
                return NULL;
            }
            strcpy(full_path, file_path);
        }
    } else {
        if (strlen(file_path) >= MAX_PATH_LEN) {
            fprintf(stderr, "[错误] 路径过长\n");
            return NULL;
        }
        strcpy(full_path, file_path);
    }

    if (!normalize_path(full_path, MAX_PATH_LEN)) {
        fprintf(stderr, "[错误] 路径规范化失败\n");
        return NULL;
    }

    ObjModule* existing_module = find_loaded_module(full_path);
    if (existing_module != NULL) {
        return existing_module;
    }

    // 磁盘缓存查找：命中则跳过编译直接反序列化
    if (g_cache_enabled && g_cache_dir) {
        char* cache_path = module_cache_path_for(full_path, g_cache_dir);
        if (cache_path) {
            ObjModule* cached = module_cache_deserialize(cache_path, full_path);
            free(cache_path);
            if (cached) {
                // DCE 兜底：命中缓存的模块**没走 codegen** ⇒ 引用图缺它这一块 ⇒ 整张图不完整，
                // 本轮一律不剪（否则该模块里"其实被调用"的函数会被误判不可达、静默返回 null）。
                // 正常路径不该走到这里：-c / -p 在编译前就把模块缓存关了（见 main.c），
                // 这条只是防御（例如将来多出别的写出入口时，行为仍安全）。
                dce_note_module_cached();
                return cached;
            }
        }
    }

    char* source = read_file(full_path);
    if (!source) {
        // 文件不存在时，注册到错误收集器（而非仅 fprintf stderr），
        // 确保根因错误出现在格式化错误输出中，不被下游 any 类型错误掩盖
        char err_msg[BUFFER_MEDIUM];
        snprintf(err_msg, sizeof(err_msg), "找不到模块文件: %s", full_path);
        error_add_at(ERR_SEMANTIC, 1, 0, err_msg);
        return NULL;
    }

    char module_name[MAX_MODULE_NAME];
    if (alias_name != NULL && alias_name[0] != '\0') {
        strncpy(module_name, alias_name, MAX_MODULE_NAME - 1);
        module_name[MAX_MODULE_NAME - 1] = '\0';
    } else {
        extract_module_name(file_path, module_name, MAX_MODULE_NAME);
    }

    ExportList* exports = (ExportList*)malloc(sizeof(ExportList));
    if (!exports) {
        free(source);
        return NULL;
    }
    // v33（收敛 S10）：导出名唯一来源 = 扫描器（旧实现在这里跑那份复刻的文本扫描器）
    //   传 full_path（已解析的绝对路径）作 module_path、current_file 传 NULL —— 扫描器内部同样会走
    //   "绝对化 + 进程内记忆化"，因此与语义分析侧**命中同一张表**（不会各扫一遍）
    {
        int n = copy_module_export_names(full_path, NULL, exports->names, MAX_EXPORTS);
        exports->count = (n > 0) ? n : 0;
    }

    // 保存原始文件名（在设置模块文件名之前）
    const char* original_filename_ptr = error_get_filename();
    char original_filename[MAX_PATH_LEN];
    if (original_filename_ptr) {
        strncpy(original_filename, original_filename_ptr, MAX_PATH_LEN - 1);
        original_filename[MAX_PATH_LEN - 1] = '\0';
    } else {
        original_filename[0] = '\0';
    }
    
    error_set_filename(full_path);

    // 在编译之前，先创建一个占位符模块并添加到已加载列表
    // 这样可以防止循环导入导致的无限递归
    ObjModule* placeholder_module = module_new(module_name);
    if (!placeholder_module) {
        error_set_filename(original_filename[0] ? original_filename : NULL);
        free(source);
        free(exports);
        return NULL;
    }
    placeholder_module->source_path = strdup(full_path);
    
    // 将导出项添加到占位符模块的导出表
    // 这样循环依赖中的其他模块可以看到本模块的导出（虽然值暂时为null）
    for (int i = 0; i < exports->count; i++) {
        ObjString* key = str_copy(exports->names[i], (int)strlen(exports->names[i]));
        dict_set(placeholder_module->exports, val_obj((Object*)key), val_null());
    }
    
    // 提前添加到已加载列表，防止循环导入
    add_loaded_module(full_path, placeholder_module);

    ObjModule* module = compile_module_dispatch(source, module_name, exports);

    // 恢复原始文件名
    error_set_filename(original_filename[0] ? original_filename : NULL);

    free(exports);
    // 注意：source 延后释放，编译成功后用于写缓存的源文件哈希计算

    if (module) {
        // 编译成功，将编译后的模块内容复制到占位符模块
        ObjDict* exports_dict = module->exports;
        for (int i = 0; i < exports_dict->capacity; i++) {
            ObjDictEntry* entry = &exports_dict->entries[i];
            Value entry_key = entry->key;
            if (!val_is_null(entry_key) && entry_key != DICT_TOMBSTONE_VAL) {
                dict_set(placeholder_module->exports, entry_key, entry->value);
            }
        }
        // 复制全局变量表
        if (module->globals && module->global_count > 0) {
            if (placeholder_module->globals) {
                free(placeholder_module->globals);
            }
            placeholder_module->globals = (Value*)malloc(module->global_count * sizeof(Value));
            if (placeholder_module->globals) {
                memcpy(placeholder_module->globals, module->globals, module->global_count * sizeof(Value));
            }
        }
        placeholder_module->global_count = module->global_count;
        placeholder_module->global_capacity = module->global_capacity;
        // 复制模块名称
        if (placeholder_module->name) {
            free(placeholder_module->name);
        }
        placeholder_module->name = module->name ? strdup(module->name) : NULL;
        // 复制原生模块引用
        if (module->native_imports && module->native_import_count > 0) {
            placeholder_module->native_imports = (char**)malloc(module->native_import_count * sizeof(char*));
            for (int ni = 0; ni < module->native_import_count; ni++) {
                placeholder_module->native_imports[ni] = strdup(module->native_imports[ni]);
            }
            placeholder_module->native_import_count = module->native_import_count;
        }
        // 复制模块帧
        placeholder_module->frame = module->frame;
        module->frame = NULL;
        // 转移 init_chunk
        placeholder_module->init_chunk = module->init_chunk;
        module->init_chunk = NULL;
        placeholder_module->initialized = module->initialized;
        // 复制 export_mappings
        if (module->export_mappings && module->export_mapping_count > 0) {
            placeholder_module->export_mappings = (ExportGlobalMapping*)malloc(module->export_mapping_count * sizeof(ExportGlobalMapping));
            for (int ei = 0; ei < module->export_mapping_count; ei++) {
                placeholder_module->export_mappings[ei].name = strdup(module->export_mappings[ei].name);
                placeholder_module->export_mappings[ei].global_index = module->export_mappings[ei].global_index;
            }
            placeholder_module->export_mapping_count = module->export_mapping_count;
        }
        // 复制 use 导入的需要 re-export 的类型信息
        if (module->use_reexport_names && module->use_reexport_count > 0) {
            placeholder_module->use_reexport_names = (char**)malloc(module->use_reexport_count * sizeof(char*));
            placeholder_module->use_reexport_kinds = (int*)malloc(module->use_reexport_count * sizeof(int));
            for (int ei = 0; ei < module->use_reexport_count; ei++) {
                placeholder_module->use_reexport_names[ei] = strdup(module->use_reexport_names[ei]);
                placeholder_module->use_reexport_kinds[ei] = module->use_reexport_kinds[ei];
            }
            placeholder_module->use_reexport_count = module->use_reexport_count;
        }
        // 更新所有函数/闭包的 module 指针（统一调用）
        update_module_function_ptrs(module, placeholder_module);
        // 更新已加载列表
        update_loaded_module(full_path, placeholder_module);
        // 清空原 module 的内容（避免 GC 重复释放已转移的资源）
        module->exports = dict_new(16);
        module->globals = NULL;
        module->global_count = 0;
        module->global_capacity = 0;
        module->init_chunk = NULL;
        module->initialized = 0;
        module->export_mappings = NULL;
        module->export_mapping_count = 0;
        char* transferred_name = module->name;
        module->name = NULL;
        if (transferred_name) free(transferred_name);
        // 编译成功后写回缓存（source 仍可用，用于计算源文件哈希）
        if (g_cache_enabled && g_cache_dir && placeholder_module->source_path) {
            char* cache_path = module_cache_path_for(full_path, g_cache_dir);
            if (cache_path) {
                module_cache_serialize(cache_path, placeholder_module, source);
                free(cache_path);
            }
        }
        free(source);
        return placeholder_module;
    } else {
        free(source);
        return NULL;
    }
}
