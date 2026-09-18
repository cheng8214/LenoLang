// ============================================================================
// 循环依赖检测：扫描栈
// 跟踪当前正在扫描的模块路径链，在发生循环时报告编译错误而非崩溃
// ============================================================================
#define MAX_CIRCULAR_STACK 64
static char g_scan_stack[MAX_CIRCULAR_STACK][MAX_PATH_LEN];
static int g_scan_stack_count = 0;

static int scan_stack_contains(const char* path) {
    for (int i = 0; i < g_scan_stack_count; i++) {
        if (strcmp(g_scan_stack[i], path) == 0) return 1;
    }
    return 0;
}

static void scan_stack_push(const char* path) {
    if (g_scan_stack_count < MAX_CIRCULAR_STACK) {
        snprintf(g_scan_stack[g_scan_stack_count], MAX_PATH_LEN, "%s", path);
        g_scan_stack[g_scan_stack_count][MAX_PATH_LEN - 1] = '\0';
        g_scan_stack_count++;
    }
}

static void scan_stack_pop(void) {
    if (g_scan_stack_count > 0) g_scan_stack_count--;
}

void module_symbol_table_reset_scan_stack(void) {
    g_scan_stack_count = 0;
}

// 添加依赖模块路径（用于 .lenosymc 缓存失效判定）
void module_symbol_table_add_dep(ModuleSymbolTable* table, const char* path) {
    if (!table || !path || !path[0]) return;
    // 去重
    for (int i = 0; i < table->dep_count; i++) {
        if (strcmp(table->dep_paths[i], path) == 0) return;
    }
    // 扩容
    if (table->dep_count >= table->dep_capacity) {
        int new_cap = table->dep_capacity == 0 ? 16 : table->dep_capacity * 2;
        char** new_arr = (char**)realloc(table->dep_paths, sizeof(char*) * new_cap);
        if (!new_arr) return;
        table->dep_paths = new_arr;
        table->dep_capacity = new_cap;
    }
    table->dep_paths[table->dep_count++] = strdup(path);
}

// 解析模块的绝对规范化路径（与 load_module_file 路径解析逻辑一致）
static void resolve_module_full_path(char* full_path, int max_len,
                                       const char* module_path, const char* current_file) {
    full_path[0] = '\0';
    if (!module_path) return;

    // ---- 包名写法（不含 .leno）先走搜索路径，与 parser_module.c 的 import 解析**同一口径** ----
    // 为什么必须有这一步：解析器在 AST 里已经把 "Music" 换成了解析后的真路径，而扫描器是去
    // **原始源码文本**里反查字面串的（scan/scan_pass2_init.inc、scan/scan_var.inc），拿到的
    // 仍是裸名。此前这里只按"相对当前文件"拼 ⇒ 得到 <目录>/Music 这种**不存在**的路径 ⇒
    // 依赖符号表永远取不回来 ⇒ 该模块导出的 struct 里"用那个包的类型声明的字段"静默降级成 any
    // （实测：声明方用裸名导入时消费方看它的字段类型全是 any；声明方**自己**那一遍完全正常 ——
    //   那条路走的是语义分析器，不是这张表。相对路径/绝对路径能解析，所以只有裸名塌方。）
    // 搜索路径由 main.c 在编译前装好（项目 lib/、源文件所在目录、exe_dir/leno_module/<包>/lib/、
    // 全局缓存包），且都是绝对路径，解析结果可直接交给下面的归一化/绝对化。
    // VM-only 构建（build_vm.bat）不编 src/package/*：那里扫描链运行期够不到（见
    // module_symbol_table.c 顶部说明），故整体挡掉，避免为一个不可达路径拖入依赖。
#ifndef LENO_VM_ONLY
    char pkg_resolved[MAX_PATH_LEN];
    if (strstr(module_path, ".leno") == NULL) {
        extern int package_resolve_module_file(const char* module_name, char* out_path, int out_len);
        if (package_resolve_module_file(module_path, pkg_resolved, MAX_PATH_LEN) == 1) {
            module_path = pkg_resolved;
        }
    }
#endif

    if (module_path[0] != '/' && module_path[0] != '\\' &&
        !(module_path[1] == ':' && (module_path[2] == '/' || module_path[2] == '\\'))) {
        // 相对路径，基于 current_file 解析
        if (current_file) {
            strncpy(full_path, current_file, max_len - 1);
            full_path[max_len - 1] = '\0';
            char* ls = strrchr(full_path, '\\');
            if (!ls) ls = strrchr(full_path, '/');
            if (ls) {
                *(ls + 1) = '\0';
                strncat(full_path, module_path, max_len - (int)strlen(full_path) - 1);
            } else {
                strncpy(full_path, module_path, max_len - 1);
            }
        } else {
            strncpy(full_path, module_path, max_len - 1);
        }
    } else {
        strncpy(full_path, module_path, max_len - 1);
    }
    full_path[max_len - 1] = '\0';
    normalize_path(full_path, max_len);

    // 绝对化：记忆化与磁盘缓存都以「完整路径」为键。同一文件若有时按相对路径解析
    // （CLI 传入的 argv、error_get_filename）、有时按绝对路径解析，就会得到两个键 ⇒
    // 同一文件被扫两遍（循环依赖也会报两遍）。这里统一转成绝对路径。
#ifdef _WIN32
    {
        char abs_buf[MAX_PATH_LEN];
        if (_fullpath(abs_buf, full_path, MAX_PATH_LEN)) {
            strncpy(full_path, abs_buf, max_len - 1);
            full_path[max_len - 1] = '\0';
        }
    }
#else
    {
        // realpath 需要文件存在；不存在时保留原路径（读取方自会报「找不到模块文件」）
        extern char* realpath(const char* path, char* resolved_path);
        char abs_buf[MAX_PATH_LEN];
        if (realpath(full_path, abs_buf)) {
            strncpy(full_path, abs_buf, max_len - 1);
            full_path[max_len - 1] = '\0';
        }
    }
#endif
}

// ============================================================================
// 进程内符号表记忆化
// ----------------------------------------------------------------------------
// 递归扫描（A 的 import 触发扫 B，B 又触发扫 C…）此前**唯一**的记忆化是磁盘上的
// .lenosymc 缓存。缓存一关，同一个模块就被反复重扫，扫描次数 = 依赖图上的**路径数**：
// SDL3 的 40 多个 lib 互相 import，路径数是天文数字 ⇒ 表现为编译"卡死"
// （实测 LENO_NO_CACHE=1 时 20 秒内扫描 5000+ 次，全在重复扫 sdl_core.leno）。
// 这里按解析后的绝对路径做进程内记忆化：同一路径只扫描一次，与缓存开关无关。
// 返回的表由本缓存持有（进程生命周期），调用方只读、不得 destroy。
//
// 失效：命中前会比对源文件的 mtime/size，文件改了就当没缓存、重新扫描。
// 这一点必须有 —— memo 命中发生在 .lenosymc 的哈希校验**之前**，若不做失效，
// 长驻进程（LSP）在用户改完依赖文件后会一直返回旧符号表，磁盘缓存的失效机制
// 完全被短路；同时 module_symbol_table_reset_memo() 供每次编译前重置。
// ============================================================================
typedef struct {
    char* full_path;
    ModuleSymbolTable* table;
    int64_t mtime;      // 源文件时间戳（0 = 取不到，退化为「进程内不变」）
    uint64_t size;      // 源文件大小
} SymTableMemoEntry;

static SymTableMemoEntry* g_sym_memo = NULL;
static int g_sym_memo_count = 0;
static int g_sym_memo_capacity = 0;

// 因文件已改而被换下来的旧表：不能立即 free —— 同一次扫描链路里上游可能还持有它
// （例如 scan_pass2_init 的 dep_table），先挂这里，reset_memo 时统一释放。
static ModuleSymbolTable** g_sym_memo_retired = NULL;
static int g_sym_memo_retired_count = 0;
static int g_sym_memo_retired_capacity = 0;

// 取文件 mtime/size（Windows 走宽字符 API 支持中文路径）；失败返回 -1
static int sym_memo_file_stamp(const char* path, int64_t* out_mtime, uint64_t* out_size) {
#ifdef _WIN32
    wchar_t wpath[MAX_PATH_LEN];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH_LEN) <= 0) return -1;
    struct _stat st;
    if (_wstat(wpath, &st) != 0) return -1;
#else
    struct stat st;
    if (stat(path, &st) != 0) return -1;
#endif
    *out_mtime = (int64_t)st.st_mtime;
    *out_size = (uint64_t)st.st_size;
    return 0;
}

static void sym_memo_retire(ModuleSymbolTable* table) {
    if (!table) return;
    if (g_sym_memo_retired_count >= g_sym_memo_retired_capacity) {
        int new_cap = g_sym_memo_retired_capacity == 0 ? 8 : g_sym_memo_retired_capacity * 2;
        ModuleSymbolTable** grown = (ModuleSymbolTable**)realloc(
            g_sym_memo_retired, sizeof(ModuleSymbolTable*) * new_cap);
        if (!grown) return;   // 记不下就先留着（宁可暂时多留一份，也不要悬空）
        g_sym_memo_retired = grown;
        g_sym_memo_retired_capacity = new_cap;
    }
    g_sym_memo_retired[g_sym_memo_retired_count++] = table;
}

static ModuleSymbolTable* sym_memo_lookup(const char* full_path) {
    int64_t mtime = 0;
    uint64_t size = 0;
    int have_stamp = (sym_memo_file_stamp(full_path, &mtime, &size) == 0);
    for (int i = 0; i < g_sym_memo_count; i++) {
        if (strcmp(g_sym_memo[i].full_path, full_path) != 0) continue;
        if (have_stamp && g_sym_memo[i].mtime != 0 &&
            (g_sym_memo[i].mtime != mtime || g_sym_memo[i].size != size)) {
            // 文件已改：换下旧表并移除条目，调用方会重新扫描并写入新条目
            sym_memo_retire(g_sym_memo[i].table);
            free(g_sym_memo[i].full_path);
            g_sym_memo[i] = g_sym_memo[g_sym_memo_count - 1];   // 末尾元素补洞
            g_sym_memo_count--;
            return NULL;
        }
        return g_sym_memo[i].table;
    }
    return NULL;
}

static void sym_memo_store(const char* full_path, ModuleSymbolTable* table) {
    if (g_sym_memo_count >= g_sym_memo_capacity) {
        int new_cap = g_sym_memo_capacity == 0 ? 32 : g_sym_memo_capacity * 2;
        SymTableMemoEntry* grown = (SymTableMemoEntry*)realloc(g_sym_memo, sizeof(SymTableMemoEntry) * new_cap);
        if (!grown) return;
        g_sym_memo = grown;
        g_sym_memo_capacity = new_cap;
    }
    int64_t mtime = 0;
    uint64_t size = 0;
    if (sym_memo_file_stamp(full_path, &mtime, &size) != 0) {
        mtime = 0;
        size = 0;
    }
    g_sym_memo[g_sym_memo_count].full_path = strdup(full_path);
    g_sym_memo[g_sym_memo_count].table = table;
    g_sym_memo[g_sym_memo_count].mtime = mtime;
    g_sym_memo[g_sym_memo_count].size = size;
    g_sym_memo_count++;
}

// 清空进程内记忆化（含换下来的旧表）。每次编译前调用，避免同一进程里的多次编译
// 互相看到旧符号表；LSP 这类常驻宿主应在每次请求前调用。
void module_symbol_table_reset_memo(void) {
    for (int i = 0; i < g_sym_memo_count; i++) {
        free(g_sym_memo[i].full_path);
        if (g_sym_memo[i].table) {
            g_sym_memo[i].table->shared = 0;   // 清掉共享标记，destroy 才真正释放
            module_symbol_table_destroy(g_sym_memo[i].table);
        }
    }
    g_sym_memo_count = 0;
    for (int i = 0; i < g_sym_memo_retired_count; i++) {
        if (g_sym_memo_retired[i]) {
            g_sym_memo_retired[i]->shared = 0;
            module_symbol_table_destroy(g_sym_memo_retired[i]);
        }
    }
    g_sym_memo_retired_count = 0;
}

ModuleSymbolTable* module_symbol_table_get_shared(const char* module_path, const char* current_file) {
    if (!module_path || !module_path[0]) return NULL;

    char full_path[MAX_PATH_LEN] = {0};
    resolve_module_full_path(full_path, MAX_PATH_LEN, module_path, current_file);
    if (!full_path[0]) return NULL;

    ModuleSymbolTable* hit = sym_memo_lookup(full_path);
    if (hit) return hit;

    ModuleSymbolTable* table = module_symbol_table_create(full_path);
    if (!table) return NULL;
    table->shared = 1;
    if (module_symbol_table_scan(table, current_file) != 0) {
        table->shared = 0;   // 扫描失败不入缓存，让 destroy 真正释放
        module_symbol_table_destroy(table);
        return NULL;
    }
    sym_memo_store(full_path, table);
    return table;
}

// 公开接口，从深度 0 开始扫描
int module_symbol_table_scan(ModuleSymbolTable* table, const char* current_file) {
    if (!table) return -1;
    // 幂等：已扫描完成就不再重复填充（重复填充既会追加重复符号，
    // 也是"关缓存时扫描次数 = 依赖图路径数"的放大器）
    if (table->scanned) return 0;

    // 解析绝对路径（用于循环依赖检测和缓存）
    char full_path[MAX_PATH_LEN] = {0};
    if (table->module_path) {
        resolve_module_full_path(full_path, MAX_PATH_LEN, table->module_path, current_file);
    }

    // ---- 循环依赖检测 ----
    // 如果当前模块路径已在扫描栈中，说明存在循环导入
    if (full_path[0] && scan_stack_contains(full_path)) {
        // 提取文件名（避免长路径超过 256 字节限制）
        const char* base = strrchr(full_path, '\\');
        if (!base) base = strrchr(full_path, '/');
        base = base ? base + 1 : full_path;

        char msg[BUFFER_MEDIUM];
        int off = snprintf(msg, sizeof(msg), "检测到循环依赖: %s", base);
        for (int i = g_scan_stack_count - 1; i >= 0; i--) {
            const char* bn = strrchr(g_scan_stack[i], '\\');
            if (!bn) bn = strrchr(g_scan_stack[i], '/');
            bn = bn ? bn + 1 : g_scan_stack[i];
            if (off < (int)sizeof(msg) - (int)strlen(bn) - 8)
                off += snprintf(msg + off, sizeof(msg) - off, " <- %s", bn);
        }
        if (off < (int)sizeof(msg) - 32)
            off += snprintf(msg + off, sizeof(msg) - off, "。请使用接口(face)解耦或移除相互导入中的任一方");
        error_add_at(ERR_SEMANTIC, 0, 0, msg);
        return -1;
    }

    // ---- 符号表缓存：尝试从磁盘加载 ----
    if (module_loader_is_cache_enabled() && table->module_path && full_path[0]) {
        char* cache_path = sym_cache_path_for(full_path);
        if (cache_path) {
            if (sym_cache_deserialize(cache_path, table, full_path) == 0) {
                free(cache_path);
                table->scanned = 1;
                return 0;  // 缓存命中，跳过源文件扫描
            }
            // 缓存未命中/失效，走正常扫描
            free(cache_path);

            // 压入扫描栈，防止循环依赖
            scan_stack_push(full_path);
            int result = module_symbol_table_scan_depth(table, current_file, 0);
            scan_stack_pop();

            if (result == 0) {
                // 计算源文件哈希用于缓存失效判定
                char* source = read_module_file(table->module_path, current_file);
                if (source) {
                    uint64_t src_hash = serialize_source_hash(source, strlen(source));
                    uint64_t src_size = 0;
#ifdef _WIN32
                    struct _stat st;
                    {
                        wchar_t* wp = utf8_to_utf16(full_path);
                        if (wp) {
                            if (_wstat(wp, &st) == 0) {
                                src_size = (uint64_t)st.st_size;
                            }
                            free(wp);
                        }
                    }
                    if (src_size == 0) src_size = (uint64_t)strlen(source);
#else
                    struct stat st;
                    if (stat(full_path, &st) == 0) {
                        src_size = (uint64_t)st.st_size;
                    } else {
                        src_size = (uint64_t)strlen(source);
                    }
#endif
                    char* cp = sym_cache_path_for(full_path);
                    if (cp) {
                        sym_cache_serialize(cp, table, src_hash, src_size);
                        free(cp);
                    }
                    free(source);
                }
            }
            if (result == 0) table->scanned = 1;
            return result;
        }
    }

    // 无缓存路径
    if (full_path[0]) scan_stack_push(full_path);
    int result = module_symbol_table_scan_depth(table, current_file, 0);
    if (full_path[0]) scan_stack_pop();
    if (result == 0) table->scanned = 1;
    return result;
}
