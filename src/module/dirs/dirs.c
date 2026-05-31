#include "include/lenolang.h"
#include "include/native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 跨平台头文件
#ifdef _WIN32
    #include <windows.h>
    #include <direct.h>
    #include <io.h>
    #define PATH_SEP '\\'
    #define PATH_SEP_STR "\\"
#else
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <dirent.h>
    #include <unistd.h>
    #include <errno.h>
    #define PATH_SEP '/'
    #define PATH_SEP_STR "/"
#endif

// ==================== 辅助函数 ====================

// 检查值是否是字符串
static int is_string_value(Value value) {
    return val_is_obj(value) && val_as_obj(value)->type == OBJ_STRING;
}

#ifdef _WIN32
// UTF-16 宽字符转换为 UTF-8 字符串
// 返回动态分配的内存，调用者需要释放
static char* utf16_to_utf8(const wchar_t* wstr) {
    if (!wstr) return NULL;
    
    // 计算需要的缓冲区大小
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
    if (size_needed <= 0) return NULL;
    
    char* str = (char*)malloc(size_needed);
    if (!str) return NULL;
    
    // 执行转换
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, str, size_needed, NULL, NULL);
    return str;
}

// 将 UTF-8 路径转换为宽字符路径（用于 Windows API）
static wchar_t* utf8_to_utf16(const char* str) {
    if (!str) return NULL;
    
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str, -1, NULL, 0);
    if (size_needed <= 0) return NULL;
    
    wchar_t* wstr = (wchar_t*)malloc(size_needed * sizeof(wchar_t));
    if (!wstr) return NULL;
    
    MultiByteToWideChar(CP_UTF8, 0, str, -1, wstr, size_needed);
    return wstr;
}
#endif

// 获取字符串值
static const char* get_string(Value value) {
    if (is_string_value(value)) {
        return ((ObjString*)val_as_obj(value))->chars;
    }
    return NULL;
}

// 创建数组辅助函数
static ObjArray* arr_new_with_capacity(int capacity) {
    ObjArray* arr = (ObjArray*)gc_alloc(sizeof(ObjArray), OBJ_ARRAY);
    if (!arr) return NULL;
    
    arr->elements = (Value*)malloc(sizeof(Value) * capacity);
    if (!arr->elements) {
        return NULL;
    }
    
    arr->capacity = capacity;
    arr->count = 0;
    return arr;
}

// 向数组添加元素
static void arr_push(ObjArray* arr, Value value) {
    if (arr->count >= arr->capacity) {
        int new_capacity = arr->capacity * 2;
        Value* new_elements = (Value*)realloc(arr->elements, sizeof(Value) * new_capacity);
        if (!new_elements) return;
        arr->elements = new_elements;
        arr->capacity = new_capacity;
    }
    arr->elements[arr->count++] = value;
}

// ==================== 路径操作 ====================

// dirs.cwd() - 获取当前工作目录
static Value native_dirs_cwd(int argCount, Value* args) {
    (void)argCount;
    (void)args;
    
    char buffer[4096];
    
#ifdef _WIN32
    if (_getcwd(buffer, sizeof(buffer)) == NULL) {
        return val_null();
    }
#else
    if (getcwd(buffer, sizeof(buffer)) == NULL) {
        return val_null();
    }
#endif
    
    return val_obj((Object*)str_copy(buffer, (int)strlen(buffer)));
}

// dirs.abspath(path) - 转换为绝对路径
static Value native_dirs_abspath(int argCount, Value* args) {
    if (argCount < 1) {
        native_throw_error("abspath 需要路径参数");
        return val_null();
    }
    
    const char* path = get_string(args[0]);
    if (!path) {
        native_throw_error("abspath 参数必须是字符串");
        return val_null();
    }
    
#ifdef _WIN32
    char buffer[4096];
    if (_fullpath(buffer, path, sizeof(buffer)) == NULL) {
        return val_null();
    }
    return val_obj((Object*)str_copy(buffer, (int)strlen(buffer)));
#else
    char buffer[4096];
    if (realpath(path, buffer) == NULL) {
        // 如果 realpath 失败，尝试简单拼接
        char cwd[4096];
        if (getcwd(cwd, sizeof(cwd)) == NULL) {
            return val_null();
        }
        if (path[0] == '/') {
            return val_obj((Object*)str_copy(path, (int)strlen(path)));
        }
        snprintf(buffer, sizeof(buffer), "%s/%s", cwd, path);
        return val_obj((Object*)str_copy(buffer, (int)strlen(buffer)));
    }
    return val_obj((Object*)str_copy(buffer, (int)strlen(buffer)));
#endif
}

// dirs.basename(path) - 获取文件名
static Value native_dirs_basename(int argCount, Value* args) {
    if (argCount < 1) {
        native_throw_error("basename 需要路径参数");
        return val_null();
    }
    
    const char* path = get_string(args[0]);
    if (!path) {
        native_throw_error("basename 参数必须是字符串");
        return val_null();
    }
    
    // 找到最后一个路径分隔符
    const char* last_sep = NULL;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            last_sep = p;
        }
    }
    
    if (last_sep == NULL) {
        // 没有分隔符，整个就是文件名
        return val_obj((Object*)str_copy(path, (int)strlen(path)));
    }
    
    // 返回分隔符后面的部分
    return val_obj((Object*)str_copy(last_sep + 1, (int)strlen(last_sep + 1)));
}

// dirs.dirname(path) - 获取目录名
static Value native_dirs_dirname(int argCount, Value* args) {
    if (argCount < 1) {
        native_throw_error("dirname 需要路径参数");
        return val_null();
    }
    
    const char* path = get_string(args[0]);
    if (!path) {
        native_throw_error("dirname 参数必须是字符串");
        return val_null();
    }
    
    // 找到最后一个路径分隔符
    const char* last_sep = NULL;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            last_sep = p;
        }
    }
    
    if (last_sep == NULL) {
        // 没有分隔符，返回当前目录 "."
        return val_obj((Object*)str_copy(".", 1));
    }
    
    // 返回分隔符前面的部分
    int len = (int)(last_sep - path);
    if (len == 0) {
        // 根目录
        return val_obj((Object*)str_copy(PATH_SEP_STR, 1));
    }
    return val_obj((Object*)str_copy(path, len));
}

// dirs.extname(path) - 获取扩展名
static Value native_dirs_extname(int argCount, Value* args) {
    if (argCount < 1) {
        native_throw_error("extname 需要路径参数");
        return val_null();
    }
    
    const char* path = get_string(args[0]);
    if (!path) {
        native_throw_error("extname 参数必须是字符串");
        return val_null();
    }
    
    // 先找到文件名（去掉目录）
    const char* filename = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            filename = p + 1;
        }
    }
    
    // 找到最后一个点
    const char* last_dot = NULL;
    for (const char* p = filename; *p; p++) {
        if (*p == '.') {
            last_dot = p;
        }
    }
    
    if (last_dot == NULL || last_dot == filename) {
        // 没有扩展名，或隐藏文件（如 .bashrc）
        return val_obj((Object*)str_copy("", 0));
    }
    
    return val_obj((Object*)str_copy(last_dot, (int)strlen(last_dot)));
}

// dirs.join(part1, part2, ...) - 拼接路径
static Value native_dirs_join(int argCount, Value* args) {
    if (argCount < 1) {
        return val_obj((Object*)str_copy("", 0));
    }
    
    // 计算总长度
    int total_len = 0;
    for (int i = 0; i < argCount; i++) {
        const char* part = get_string(args[i]);
        if (part) {
            total_len += (int)strlen(part);
            if (i < argCount - 1) {
                total_len += 1; // 分隔符
            }
        }
    }
    
    char* buffer = (char*)malloc(total_len + 1);
    if (!buffer) {
        return val_null();
    }
    
    buffer[0] = '\0';
    for (int i = 0; i < argCount; i++) {
        const char* part = get_string(args[i]);
        if (part) {
            strcat(buffer, part);
            if (i < argCount - 1) {
                // 移除末尾已有的分隔符，避免重复
                int len = (int)strlen(buffer);
                if (len > 0 && (buffer[len-1] == '/' || buffer[len-1] == '\\')) {
                    buffer[len-1] = PATH_SEP;
                    buffer[len] = '\0';
                } else {
                    strcat(buffer, PATH_SEP_STR);
                }
            }
        }
    }
    
    ObjString* result = str_copy(buffer, (int)strlen(buffer));
    free(buffer);
    return val_obj((Object*)result);
}

// dirs.sep() - 获取路径分隔符
static Value native_dirs_sep(int argCount, Value* args) {
    (void)argCount;
    (void)args;
    return val_obj((Object*)str_copy(PATH_SEP_STR, 1));
}

// ==================== 目录操作 ====================

// dirs.exists(path) - 检查路径是否存在
static Value native_dirs_exists(int argCount, Value* args) {
    if (argCount < 1) {
        native_throw_error("exists 需要路径参数");
        return val_null();
    }
    
    const char* path = get_string(args[0]);
    if (!path) {
        native_throw_error("exists 参数必须是字符串");
        return val_null();
    }
    
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    return val_bool(attr != INVALID_FILE_ATTRIBUTES);
#else
    struct stat st;
    return val_bool(stat(path, &st) == 0);
#endif
}

// dirs.is_file(path) - 检查是否是文件
static Value native_dirs_is_file(int argCount, Value* args) {
    if (argCount < 1) {
        native_throw_error("is_file 需要路径参数");
        return val_null();
    }
    
    const char* path = get_string(args[0]);
    if (!path) {
        native_throw_error("is_file 参数必须是字符串");
        return val_null();
    }
    
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        return val_bool(0);
    }
    return val_bool(!(attr & FILE_ATTRIBUTE_DIRECTORY));
#else
    struct stat st;
    if (stat(path, &st) != 0) {
        return val_bool(0);
    }
    return val_bool(S_ISREG(st.st_mode));
#endif
}

// dirs.is_dir(path) - 检查是否是目录
static Value native_dirs_is_dir(int argCount, Value* args) {
    if (argCount < 1) {
        native_throw_error("is_dir 需要路径参数");
        return val_null();
    }
    
    const char* path = get_string(args[0]);
    if (!path) {
        native_throw_error("is_dir 参数必须是字符串");
        return val_null();
    }
    
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        return val_bool(0);
    }
    return val_bool(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    if (stat(path, &st) != 0) {
        return val_bool(0);
    }
    return val_bool(S_ISDIR(st.st_mode));
#endif
}

// dirs.mkdir(path) - 创建目录
static Value native_dirs_mkdir(int argCount, Value* args) {
    if (argCount < 1) {
        native_throw_error("mkdir 需要路径参数");
        return val_null();
    }
    
    const char* path = get_string(args[0]);
    if (!path) {
        native_throw_error("mkdir 参数必须是字符串");
        return val_null();
    }
    
#ifdef _WIN32
    int result = _mkdir(path);
#else
    int result = mkdir(path, 0755);
#endif
    
    return val_bool(result == 0);
}

// dirs.mkdir_p(path) - 递归创建目录
static Value native_dirs_mkdir_p(int argCount, Value* args) {
    if (argCount < 1) {
        native_throw_error("mkdir_p 需要路径参数");
        return val_null();
    }
    
    const char* path = get_string(args[0]);
    if (!path) {
        native_throw_error("mkdir_p 参数必须是字符串");
        return val_null();
    }
    
    char* temp = strdup(path);
    if (!temp) {
        return val_bool(0);
    }
    
    // 逐层创建
    for (char* p = temp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char sep = *p;
            *p = '\0';
            
#ifdef _WIN32
            _mkdir(temp);
#else
            mkdir(temp, 0755);
#endif
            
            *p = sep;
        }
    }
    
    // 创建最后一层
#ifdef _WIN32
    int result = _mkdir(temp);
#else
    int result = mkdir(temp, 0755);
#endif
    
    free(temp);
    return val_bool(result == 0 || errno == EEXIST);
}

// dirs.rmdir(path) - 删除空目录
static Value native_dirs_rmdir(int argCount, Value* args) {
    if (argCount < 1) {
        native_throw_error("rmdir 需要路径参数");
        return val_null();
    }
    
    const char* path = get_string(args[0]);
    if (!path) {
        native_throw_error("rmdir 参数必须是字符串");
        return val_null();
    }
    
#ifdef _WIN32
    int result = _rmdir(path);
#else
    int result = rmdir(path);
#endif
    
    return val_bool(result == 0);
}

// dirs.remove(path) - 删除文件
static Value native_dirs_remove(int argCount, Value* args) {
    if (argCount < 1) {
        native_throw_error("remove 需要路径参数");
        return val_null();
    }
    
    const char* path = get_string(args[0]);
    if (!path) {
        native_throw_error("remove 参数必须是字符串");
        return val_null();
    }
    
    int result = remove(path);
    return val_bool(result == 0);
}

// dirs.rename(old, new) - 重命名
static Value native_dirs_rename(int argCount, Value* args) {
    if (argCount < 2) {
        native_throw_error("rename 需要两个参数");
        return val_null();
    }
    
    const char* old_path = get_string(args[0]);
    const char* new_path = get_string(args[1]);
    
    if (!old_path || !new_path) {
        native_throw_error("rename 参数必须是字符串");
        return val_null();
    }
    
    int result = rename(old_path, new_path);
    return val_bool(result == 0);
}

// ==================== 目录遍历 ====================

// dirs.listdir(path) - 列出目录内容
static Value native_dirs_listdir(int argCount, Value* args) {
    if (argCount < 1) {
        native_throw_error("listdir 需要路径参数");
        return val_null();
    }
    
    const char* path = get_string(args[0]);
    if (!path) {
        native_throw_error("listdir 参数必须是字符串");
        return val_null();
    }
    
    ObjArray* arr = arr_new_with_capacity(16);
    if (!arr) {
        return val_null();
    }
    
#ifdef _WIN32
    // 将 UTF-8 路径转换为宽字符
    wchar_t* wpath = utf8_to_utf16(path);
    if (!wpath) {
        return val_null();
    }
    
    // 构建搜索路径
    wchar_t search_path[4096];
    swprintf(search_path, sizeof(search_path) / sizeof(wchar_t), L"%ls\\*", wpath);
    free(wpath);
    
    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW(search_path, &findData);
    
    if (hFind == INVALID_HANDLE_VALUE) {
        return val_null();
    }
    
    do {
        // 跳过 . 和 ..
        if (wcscmp(findData.cFileName, L".") == 0 || wcscmp(findData.cFileName, L"..") == 0) {
            continue;
        }
        
        // 将宽字符文件名转换为 UTF-8
        char* utf8_name = utf16_to_utf8(findData.cFileName);
        if (utf8_name) {
            arr_push(arr, val_obj((Object*)str_copy(utf8_name, (int)strlen(utf8_name))));
            free(utf8_name);
        }
    } while (FindNextFileW(hFind, &findData));
    
    FindClose(hFind);
#else
    DIR* dir = opendir(path);
    if (!dir) {
        return val_null();
    }
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        // 跳过 . 和 ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        arr_push(arr, val_obj((Object*)str_copy(entry->d_name, (int)strlen(entry->d_name))));
    }
    
    closedir(dir);
#endif
    
    return val_obj((Object*)arr);
}

// dirs.walk(path) - 遍历目录树
// 返回 [[root, dirs, files], ...]
static Value native_dirs_walk(int argCount, Value* args) {
    if (argCount < 1) {
        native_throw_error("walk 需要路径参数");
        return val_null();
    }
    
    const char* path = get_string(args[0]);
    if (!path) {
        native_throw_error("walk 参数必须是字符串");
        return val_null();
    }
    
    // 简化版：只返回一层
    // 完整版应该用递归或栈实现深度遍历
    ObjArray* result = arr_new_with_capacity(16);
    if (!result) {
        return val_null();
    }
    
    ObjArray* dirs = arr_new_with_capacity(8);
    ObjArray* files = arr_new_with_capacity(8);
    
    if (!dirs || !files) {
        return val_null();
    }
    
#ifdef _WIN32
    char search_path[4096];
    snprintf(search_path, sizeof(search_path), "%s\\*", path);
    
    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(search_path, &findData);
    
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(findData.cFileName, ".") == 0 || strcmp(findData.cFileName, "..") == 0) {
                continue;
            }
            
            ObjString* name = str_copy(findData.cFileName, (int)strlen(findData.cFileName));
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                arr_push(dirs, val_obj((Object*)name));
            } else {
                arr_push(files, val_obj((Object*)name));
            }
        } while (FindNextFileA(hFind, &findData));
        FindClose(hFind);
    }
#else
    DIR* dir = opendir(path);
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            
            ObjString* name = str_copy(entry->d_name, (int)strlen(entry->d_name));
            
            // 需要 stat 来判断类型
            char full_path[4096];
            snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
            struct stat st;
            if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                arr_push(dirs, val_obj((Object*)name));
            } else {
                arr_push(files, val_obj((Object*)name));
            }
        }
        closedir(dir);
    }
#endif
    
    // 创建 [root, dirs, files] 数组
    ObjArray* entry = arr_new_with_capacity(3);
    if (entry) {
        arr_push(entry, val_obj((Object*)str_copy(path, (int)strlen(path))));
        arr_push(entry, val_obj((Object*)dirs));
        arr_push(entry, val_obj((Object*)files));
        arr_push(result, val_obj((Object*)entry));
    }
    
    return val_obj((Object*)result);
}

// ==================== 文件信息 ====================

// dirs.stat(path) - 获取文件信息
static Value native_dirs_stat(int argCount, Value* args) {
    if (argCount < 1) {
        native_throw_error("stat 需要路径参数");
        return val_null();
    }
    
    const char* path = get_string(args[0]);
    if (!path) {
        native_throw_error("stat 参数必须是字符串");
        return val_null();
    }
    
    ObjDict* dict = dict_new(8);
    if (!dict) {
        return val_null();
    }
    
    // 前向声明 dict_set
    extern void dict_set(ObjDict* dict, ObjString* key, Value value);
    
    // 初始化默认值
    dict_set(dict, str_copy("exists", 6), val_bool(0));
    dict_set(dict, str_copy("size", 4), val_int(0));
    dict_set(dict, str_copy("is_file", 7), val_bool(0));
    dict_set(dict, str_copy("is_dir", 6), val_bool(0));
    dict_set(dict, str_copy("mtime", 5), val_int(0));
    
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA attrData;
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &attrData)) {
        // 文件存在，更新信息
        dict_set(dict, str_copy("exists", 6), val_bool(1));
        
        // size
        LARGE_INTEGER size;
        size.LowPart = attrData.nFileSizeLow;
        size.HighPart = attrData.nFileSizeHigh;
        dict_set(dict, str_copy("size", 4), val_int((int)size.QuadPart));
        
        // is_file, is_dir
        int is_dir = attrData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
        dict_set(dict, str_copy("is_file", 7), val_bool(!is_dir));
        dict_set(dict, str_copy("is_dir", 6), val_bool(is_dir));
        
        // mtime (简化版，返回 0)
        dict_set(dict, str_copy("mtime", 5), val_int(0));
    }
#else
    struct stat st;
    if (stat(path, &st) == 0) {
        // 文件存在，更新信息
        dict_set(dict, str_copy("exists", 6), val_bool(1));
        
        // size
        dict_set(dict, str_copy("size", 4), val_int((int)st.st_size));
        
        // is_file, is_dir
        dict_set(dict, str_copy("is_file", 7), val_bool(S_ISREG(st.st_mode)));
        dict_set(dict, str_copy("is_dir", 6), val_bool(S_ISDIR(st.st_mode)));
        
        // mtime
        dict_set(dict, str_copy("mtime", 5), val_int((int)st.st_mtime));
    }
#endif
    
    return val_obj((Object*)dict);
}

// ==================== 初始化 ====================

void dirs_init_module(void) {
    // 路径操作
    TypeKind string_params[] = {TYPE_STRING};
    TypeKind string2_params[] = {TYPE_STRING, TYPE_STRING};
    TypeKind no_params[] = {};
    
    native_register_module_method("dirs", "cwd", native_dirs_cwd, 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, no_params);
    native_register_module_method("dirs", "abspath", native_dirs_abspath, 1, -1, -1, TYPE_STRING, TYPE_UNKNOWN, string_params);
    native_register_module_method("dirs", "basename", native_dirs_basename, 1, -1, -1, TYPE_STRING, TYPE_UNKNOWN, string_params);
    native_register_module_method("dirs", "dirname", native_dirs_dirname, 1, -1, -1, TYPE_STRING, TYPE_UNKNOWN, string_params);
    native_register_module_method("dirs", "extname", native_dirs_extname, 1, -1, -1, TYPE_STRING, TYPE_UNKNOWN, string_params);
    native_register_module_method("dirs", "join", native_dirs_join, -1, 0, -1, TYPE_STRING, TYPE_UNKNOWN, string_params);
    native_register_module_method("dirs", "sep", native_dirs_sep, 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, no_params);

    // 检查操作
    native_register_module_method("dirs", "exists", native_dirs_exists, 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, string_params);
    native_register_module_method("dirs", "is_file", native_dirs_is_file, 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, string_params);
    native_register_module_method("dirs", "is_dir", native_dirs_is_dir, 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, string_params);

    // 目录操作
    native_register_module_method("dirs", "mkdir", native_dirs_mkdir, 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, string_params);
    native_register_module_method("dirs", "mkdir_p", native_dirs_mkdir_p, 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, string_params);
    native_register_module_method("dirs", "rmdir", native_dirs_rmdir, 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, string_params);
    native_register_module_method("dirs", "remove", native_dirs_remove, 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, string_params);
    native_register_module_method("dirs", "rename", native_dirs_rename, 2, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, string2_params);

    // 遍历操作
    native_register_module_method("dirs", "listdir", native_dirs_listdir, 1, -1, -1, TYPE_ARRAY, TYPE_STRING, string_params);
    native_register_module_method("dirs", "walk", native_dirs_walk, 1, -1, -1, TYPE_ARRAY, TYPE_STRING, string_params);

    // 文件信息
    native_register_module_method("dirs", "stat", native_dirs_stat, 1, -1, -1, TYPE_DICT, TYPE_UNKNOWN, string_params);
}
