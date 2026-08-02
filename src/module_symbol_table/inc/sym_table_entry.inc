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

// 解析模块的绝对规范化路径（与 load_module_file 路径解析逻辑一致）
static void resolve_module_full_path(char* full_path, int max_len,
                                       const char* module_path, const char* current_file) {
    full_path[0] = '\0';
    if (!module_path) return;

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
}

// 公开接口，从深度 0 开始扫描
int module_symbol_table_scan(ModuleSymbolTable* table, const char* current_file) {
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
        error_add(ERR_SEMANTIC, 0, msg);
        return -1;
    }

    // ---- 符号表缓存：尝试从磁盘加载 ----
    if (module_loader_is_cache_enabled() && table->module_path && full_path[0]) {
        char* cache_path = sym_cache_path_for(full_path);
        if (cache_path) {
            if (sym_cache_deserialize(cache_path, table, full_path) == 0) {
                free(cache_path);
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
            return result;
        }
    }

    // 无缓存路径
    if (full_path[0]) scan_stack_push(full_path);
    int result = module_symbol_table_scan_depth(table, current_file, 0);
    if (full_path[0]) scan_stack_pop();
    return result;
}
