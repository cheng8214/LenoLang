// 公开接口，从深度 0 开始扫描
int module_symbol_table_scan(ModuleSymbolTable* table, const char* current_file) {
    // 符号表缓存：尝试从磁盘加载
    if (module_loader_is_cache_enabled() && table->module_path) {
        // 解析绝对路径（与 load_module_file 一致的路径解析逻辑）
        char full_path[MAX_PATH_LEN] = {0};
        // 如果是相对路径，基于 current_file 解析
        if (table->module_path[0] != '/' && table->module_path[0] != '\\' &&
            !(table->module_path[1] == ':' && (table->module_path[2] == '/' || table->module_path[2] == '\\'))) {
            if (current_file) {
                strncpy(full_path, current_file, MAX_PATH_LEN - 1);
                char* ls = strrchr(full_path, '\\');
                if (!ls) ls = strrchr(full_path, '/');
                if (ls) {
                    *(ls + 1) = '\0';
                    strncat(full_path, table->module_path, MAX_PATH_LEN - strlen(full_path) - 1);
                } else {
                    strncpy(full_path, table->module_path, MAX_PATH_LEN - 1);
                }
            } else {
                strncpy(full_path, table->module_path, MAX_PATH_LEN - 1);
            }
        } else {
            strncpy(full_path, table->module_path, MAX_PATH_LEN - 1);
        }
        normalize_path(full_path, MAX_PATH_LEN);

        char* cache_path = sym_cache_path_for(full_path);
        if (cache_path) {
            if (sym_cache_deserialize(cache_path, table, full_path) == 0) {
                free(cache_path);
                return 0;  // 缓存命中，跳过源文件扫描
            }
            // 缓存未命中/失效，走正常扫描
            free(cache_path);

            // 扫描后写回缓存
            int result = module_symbol_table_scan_depth(table, current_file, 0);
            if (result == 0) {
                // 计算源文件哈希用于缓存失效判定
                char* source = read_module_file(table->module_path, current_file);
                if (source) {
                    uint64_t src_hash = serialize_source_hash(source, strlen(source));
                    uint64_t src_size = 0;
                    struct stat st;
                    if (stat(full_path, &st) == 0) {
                        src_size = (uint64_t)st.st_size;
                    } else {
                        src_size = (uint64_t)strlen(source);
                    }
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

    return module_symbol_table_scan_depth(table, current_file, 0);
}
