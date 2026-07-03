#include "include/leno_vm_runtime.h"
#include "include/module_dispatch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
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

// 添加已加载模块
static void add_loaded_module(const char* path, ObjModule* module) {
    if (loaded_modules.count >= MAX_LOADED_MODULES) return;
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
static void extract_exports(const char* source, ExportList* list) {
    list->count = 0;
    const char* p = source;

    while (*p) {
        // 跳过空白字符
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;

        if (!*p) break;

        // 处理单行注释 //
        if (*p == '/' && *(p+1) == '/') {
            while (*p && *p != '\n') p++;
            continue;
        }

        // 处理多行注释 /* ... */
        if (*p == '/' && *(p+1) == '*') {
            p += 2;  // 跳过 /*
            while (*p && !(*p == '*' && *(p+1) == '/')) p++;
            if (*p) p += 2;  // 跳过 */
            continue;
        }

        // 处理双引号字符串 "..."
        if (*p == '"') {
            p++;  // 跳过起始 "
            while (*p && *p != '"') {
                if (*p == '\\' && *(p+1)) p += 2;  // 跳过转义字符
                else p++;
            }
            if (*p) p++;  // 跳过结束 "
            continue;
        }

        // 处理单引号字符串 '...'
        if (*p == '\'') {
            p++;  // 跳过起始 '
            while (*p && *p != '\'') {
                if (*p == '\\' && *(p+1)) p += 2;  // 跳过转义字符
                else p++;
            }
            if (*p) p++;  // 跳过结束 '
            continue;
        }

        // 处理原始字符串 `...`
        if (*p == '`') {
            p++;  // 跳过起始 `
            while (*p && *p != '`') p++;
            if (*p) p++;  // 跳过结束 `
            continue;
        }

        // 查找 export 关键字
        if (strncmp(p, "export", 6) == 0 && !isalnum((unsigned char)p[6]) && p[6] != '_') {
            p += 6;
            while (*p && (*p == ' ' || *p == '\t')) p++;

            // 跳过可能的 "func"、"var"、"struct"、"cstruct"、"enum" 关键字
            if (strncmp(p, "func", 4) == 0 && !isalnum((unsigned char)p[4]) && p[4] != '_') {
                p += 4;
                while (*p && (*p == ' ' || *p == '\t')) p++;
            } else if (strncmp(p, "var", 3) == 0 && !isalnum((unsigned char)p[3]) && p[3] != '_') {
                p += 3;
                while (*p && (*p == ' ' || *p == '\t')) p++;
            } else if (strncmp(p, "cstruct", 7) == 0 && !isalnum((unsigned char)p[7]) && p[7] != '_') {
                p += 7;
                while (*p && (*p == ' ' || *p == '\t')) p++;
            } else if (strncmp(p, "struct", 6) == 0 && !isalnum((unsigned char)p[6]) && p[6] != '_') {
                p += 6;
                while (*p && (*p == ' ' || *p == '\t')) p++;
            } else if (strncmp(p, "enum", 4) == 0 && !isalnum((unsigned char)p[4]) && p[4] != '_') {
                p += 4;
                while (*p && (*p == ' ' || *p == '\t')) p++;
            }

            // 读取标识符名称
            const char* start = p;
            while (*p && (isalnum(*p) || *p == '_')) p++;

            int len = (int)(p - start);
            if (len > 0 && len < MAX_EXPORT_NAME && list->count < MAX_EXPORTS) {
                strncpy(list->names[list->count], start, len);
                list->names[list->count][len] = '\0';
                list->count++;
            }
            continue;
        }

        p++;
    }
}

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

// 读取模块文件
char* read_module_file(const char* file_path, const char* current_file) {
    char full_path[MAX_PATH_LEN];
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
                return NULL;
            }
            memcpy(full_path, normalized_current, dir_len);
            full_path[dir_len] = '\0';
            if (strlen(full_path) + strlen(file_path) >= MAX_PATH_LEN) {
                return NULL;
            }
            strcat(full_path, file_path);
        } else {
            if (strlen(file_path) >= MAX_PATH_LEN) {
                return NULL;
            }
            strcpy(full_path, file_path);
        }
    } else {
        if (strlen(file_path) >= MAX_PATH_LEN) {
            return NULL;
        }
        strcpy(full_path, file_path);
    }

    if (!normalize_path(full_path, MAX_PATH_LEN)) {
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

// 从模块文件中提取导出项（用于语义分析）
int extract_module_exports_from_file(const char* file_path, const char* current_file,
                                      char exports[][MAX_EXPORT_NAME], int max_exports) {
    char* source = read_module_file(file_path, current_file);
    if (!source) return -1;

    ExportList list;
    extract_exports(source, &list);
    free(source);

    int count = list.count < max_exports ? list.count : max_exports;
    for (int i = 0; i < count; i++) {
        strncpy(exports[i], list.names[i], MAX_EXPORT_NAME - 1);
        exports[i][MAX_EXPORT_NAME - 1] = '\0';
    }

    return count;
}

// 检查模块中是否存在指定的方法
int module_has_method(const char* file_path, const char* current_file, const char* method_name) {
    char exports[MAX_EXPORTS][MAX_EXPORT_NAME];
    int count = extract_module_exports_from_file(file_path, current_file, exports, MAX_EXPORTS);

    if (count < 0) {
        return -1;
    }

    for (int i = 0; i < count; i++) {
        if (strcmp(exports[i], method_name) == 0) {
            return 1;
        }
    }

    return 0;
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

    char* source = read_file(full_path);
    if (!source) {
        fprintf(stderr, "[错误] 无法读取文件: %s\n", full_path);
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
    extract_exports(source, exports);

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
    placeholder_module->source_path = strdup(file_path);
    
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

    free(source);
    free(exports);

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
        return placeholder_module;
    } else {
        // exports already freed above
        return NULL;
    }
}
