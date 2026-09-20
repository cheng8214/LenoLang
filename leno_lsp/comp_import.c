/**
 * 导入解析模块（共享）
 * 
 * 提供 ImportAlias 结构的解析、查找和释放功能
 * 供 comp_context.c 和 comp_symbols.c 使用
 */

#include "lsp_completion.h"
#include "leno_lsp.h"
#include "../src/include/leno_types.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ========== 解析 ========== */

ImportAlias* parse_imports(const char* content, int* count) {
    *count = 0;
    if (!content) return NULL;
    
    // 声明 package_resolve_module_file（在 package_resolve.c 中实现）
    extern int package_resolve_module_file(const char* module_name, char* out_path, int out_len);
    
    ImportAlias* aliases = (ImportAlias*)malloc(sizeof(ImportAlias) * 64);
    if (!aliases) return NULL;
    
    const char* p = content;
    int idx = 0;
    
    while (*p && idx < 64) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        
        if (strncmp(p, "import", 6) == 0 && !isalnum((unsigned char)p[6]) && p[6] != '_') {
            p += 6;
            while (*p && isspace((unsigned char)*p)) p++;
            if (!*p) break;
            
            char module_name[128] = {0};
            char full_module_name[MAX_PATH_LEN] = {0};
            int mod_len = 0;
            int full_mod_len = 0;
            
            if (*p == '"') {
                p++;
                while (*p && *p != '"' && mod_len < 127 && full_mod_len < 127) {
                    char c = *p++;
                    module_name[mod_len++] = c;
                    full_module_name[full_mod_len++] = c;
                }
                if (*p == '"') p++;
                module_name[mod_len] = '\0';
                full_module_name[full_mod_len] = '\0';
                
                // 如果路径不含 .leno，尝试通过包搜索路径解析
                if (strstr(full_module_name, ".leno") == NULL) {
                    char resolved[MAX_PATH_LEN];
                    if (package_resolve_module_file(full_module_name, resolved, sizeof(resolved)) == 1) {
                        snprintf(full_module_name, sizeof(full_module_name), "%s", resolved);
                    }
                }
                
                // 从路径提取文件名用于别名匹配
                char* slash = strrchr(module_name, '/');
                char* backslash = strrchr(module_name, '\\');
                char* last_sep = (slash && backslash) ? (slash > backslash ? slash : backslash) : (slash ? slash : (backslash ? backslash : NULL));
                if (last_sep) {
                    memmove(module_name, last_sep + 1, strlen(last_sep + 1) + 1);
                    mod_len = strlen(module_name);
                }
            } else {
                // 标识符形式: import module_name
                while (*p && (isalnum((unsigned char)*p) || *p == '_') && mod_len < 127) {
                    module_name[mod_len++] = *p++;
                }
                module_name[mod_len] = '\0';
                
                // 复制到 full_module_name（保存原始模块名）
                snprintf(full_module_name, sizeof(full_module_name), "%s", module_name);
                full_mod_len = mod_len;
                
                // 尝试通过包搜索路径解析为文件路径
                char resolved[MAX_PATH_LEN];
                if (package_resolve_module_file(module_name, resolved, sizeof(resolved)) == 1) {
                    snprintf(full_module_name, sizeof(full_module_name), "%s", resolved);
                }
                // 如果无法解析，full_module_name 保持原始模块名
                // module_symbol_table_scan 内部可能有进一步处理
            }
            
            if (mod_len == 0) continue;
            
            while (*p && isspace((unsigned char)*p)) p++;
            
            char alias[128] = {0};
            int alias_len = 0;
            
            if (strncmp(p, "as", 2) == 0 && !isalnum((unsigned char)p[2]) && p[2] != '_') {
                p += 2;
                while (*p && isspace((unsigned char)*p)) p++;
                
                while (*p && (isalnum((unsigned char)*p) || *p == '_') && alias_len < 127) {
                    alias[alias_len++] = *p++;
                }
                alias[alias_len] = '\0';
            }
            
            aliases[idx].module_name = strdup(full_module_name[0] ? full_module_name : module_name);
            if (alias_len > 0) {
                aliases[idx].alias = strdup(alias);
            } else {
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
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
        }
    }
    
    *count = idx;
    return aliases;
}

void free_import_aliases(ImportAlias* aliases, int count) {
    if (!aliases) return;
    for (int i = 0; i < count; i++) {
        free(aliases[i].alias);
        free(aliases[i].module_name);
    }
    free(aliases);
}

const char* find_module_by_alias(ImportAlias* aliases, int count, const char* alias) {
    for (int i = 0; i < count; i++) {
        if (strcmp(aliases[i].alias, alias) == 0) {
            return aliases[i].module_name;
        }
    }
    return alias;
}

const char* find_module_path_by_alias(ImportAlias* aliases, int count, const char* alias) {
    for (int i = 0; i < count; i++) {
        if (strcmp(aliases[i].alias, alias) == 0) {
            return aliases[i].module_name;
        }
    }
    return NULL;
}
