// 解析导入语句，建立别名到模块名的映射
// 支持: import maths, import maths as b, import "./test.leno" as test
typedef struct {
    char* alias;
    char* module_name;
} ImportAlias;

static ImportAlias* parse_imports(const char* content, int* count) {
    *count = 0;
    if (!content) return NULL;
    
    ImportAlias* aliases = malloc(sizeof(ImportAlias) * 64);
    if (!aliases) return NULL;
    
    const char* p = content;
    int idx = 0;
    
    while (*p && idx < 64) {
        // 跳过空白
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        
        // 检查是否是 import
        if (strncmp(p, "import", 6) == 0 && !isalnum((unsigned char)p[6]) && p[6] != '_') {
            p += 6;
            
            // 跳过空白
            while (*p && isspace((unsigned char)*p)) p++;
            if (!*p) break;
            
            // 解析模块名或路径
            char module_name[128] = {0};
            int mod_len = 0;
            
            if (*p == '"') {
                // 字符串路径: import "./test.leno"
                p++;
                while (*p && *p != '"' && mod_len < 127) {
                    module_name[mod_len++] = *p++;
                }
                if (*p == '"') p++;
                module_name[mod_len] = '\0';
                // 从路径提取文件名作为模块名（保留.leno后缀，因为需要用它来读取文件）
                char* slash = strrchr(module_name, '/');
                char* backslash = strrchr(module_name, '\\');
                char* last_sep = slash > backslash ? slash : backslash;
                if (last_sep) {
                    memmove(module_name, last_sep + 1, strlen(last_sep + 1) + 1);
                    mod_len = strlen(module_name);
                }
                // 注意：保留 .leno 后缀在 module_name 中，因为 module_symbol_table_scan 需要完整文件名
            } else {
                // 标识符: import maths
                while (*p && (isalnum((unsigned char)*p) || *p == '_') && mod_len < 127) {
                    module_name[mod_len++] = *p++;
                }
                module_name[mod_len] = '\0';
            }
            
            if (mod_len == 0) continue;
            
            // 跳过空白
            while (*p && isspace((unsigned char)*p)) p++;
            
            // 检查是否有 as 别名
            char alias[128] = {0};
            int alias_len = 0;
            
            if (strncmp(p, "as", 2) == 0 && !isalnum((unsigned char)p[2]) && p[2] != '_') {
                p += 2;
                while (*p && isspace((unsigned char)*p)) p++;
                
                // 解析别名
                while (*p && (isalnum((unsigned char)*p) || *p == '_') && alias_len < 127) {
                    alias[alias_len++] = *p++;
                }
                alias[alias_len] = '\0';
            }
            
            // 存储映射
            aliases[idx].module_name = strdup(module_name);
            if (alias_len > 0) {
                aliases[idx].alias = strdup(alias);
            } else {
                // 没有别名，使用模块名（去掉.leno后缀）作为别名
                // 因为用户代码中引用模块时不会带.leno后缀
                char alias_without_ext[128];
                strncpy(alias_without_ext, module_name, sizeof(alias_without_ext) - 1);
                alias_without_ext[sizeof(alias_without_ext) - 1] = '\0';
                char* dot = strrchr(alias_without_ext, '.');
                if (dot && strcmp(dot, ".leno") == 0) {
                    *dot = '\0';
                }
                aliases[idx].alias = strdup(alias_without_ext);
            }
            idx++;
        } else {
            // 跳过这一行
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
        }
    }
    
    *count = idx;
    return aliases;
}

static void free_import_aliases(ImportAlias* aliases, int count) {
    if (!aliases) return;
    for (int i = 0; i < count; i++) {
        free(aliases[i].alias);
        free(aliases[i].module_name);
    }
    free(aliases);
}

// 根据别名查找模块名
static const char* find_module_by_alias(ImportAlias* aliases, int count, const char* alias) {
    for (int i = 0; i < count; i++) {
        if (strcmp(aliases[i].alias, alias) == 0) {
            return aliases[i].module_name;
        }
    }
    // 如果没有找到别名映射，直接返回别名本身（可能是直接使用模块名）
    return alias;
}

// 根据别名查找模块路径（用于 .leno 文件导入）
static const char* find_module_path_by_alias(ImportAlias* aliases, int count, const char* alias) {
    for (int i = 0; i < count; i++) {
        if (strcmp(aliases[i].alias, alias) == 0) {
            return aliases[i].module_name;
        }
    }
    return NULL;
}
