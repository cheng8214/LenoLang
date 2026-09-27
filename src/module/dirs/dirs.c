#include "include/lenolang.h"
#include "include/native.h"
// dirs.res_dir() 需要读打包模式下的资源释放目录（定义在 core 的 vm.c）
#include "include/leno_vm_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 跨平台头文件
#ifdef _WIN32
    #include <windows.h>
    #include <direct.h>
    #include <io.h>
    #include <errno.h>
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
// ★ 使用 gc_track_memory 追踪 elements 缓冲区的内存占用，
// 确保 GC 正确计算内存使用量（与 object_array.c 的 arr_new 一致）。
static ObjArray* arr_new_with_capacity(int capacity) {
    ObjArray* arr = (ObjArray*)gc_alloc(sizeof(ObjArray), OBJ_ARRAY);
    if (!arr) return NULL;
    
    arr->elements = (Value*)malloc(sizeof(Value) * capacity);
    if (!arr->elements) {
        return NULL;
    }
    
    // 追踪 elements 缓冲区的内存占用
    gc_track_memory((Object*)arr, 0, capacity * sizeof(Value));
    
    arr->capacity = capacity;
    arr->count = 0;
    arr->type_info = NULL;
    
    // 初始化为 null，防止 GC 扫描到垃圾值
    for (int i = 0; i < capacity; i++) {
        arr->elements[i] = val_null();
    }
    
    return arr;
}

// 向数组添加元素
// ★ 使用 gc_write_barrier 确保写屏障正确（与 object_array.c 一致），
// 防止老年代数组引用的年轻代对象在 Minor GC 时被遗漏。
static void arr_push(ObjArray* arr, Value value) {
    if (arr->count >= arr->capacity) {
        int new_capacity = arr->capacity * 2;
        Value* new_elements = (Value*)realloc(arr->elements, sizeof(Value) * new_capacity);
        if (!new_elements) return;
        arr->elements = new_elements;
        arr->capacity = new_capacity;
        // 追踪扩容后的内存变化
        gc_track_memory((Object*)arr, (arr->count) * sizeof(Value), new_capacity * sizeof(Value));
    }
    arr->elements[arr->count++] = value;
    gc_write_barrier((Object*)arr, value);
}

// ==================== 路径操作 ====================

// dirs.cwd() - 获取当前工作目录
static Value native_dirs_cwd(int argCount, Value* args) {
    (void)argCount;
    (void)args;

#ifdef _WIN32
    wchar_t wbuffer[4096];
    if (_wgetcwd(wbuffer, sizeof(wbuffer) / sizeof(wchar_t)) == NULL) {
        return val_null();
    }
    char* utf8 = utf16_to_utf8(wbuffer);
    if (!utf8) {
        return val_null();
    }
    Value result = val_obj((Object*)str_copy(utf8, (int)strlen(utf8)));
    free(utf8);
    return result;
#else
    char buffer[4096];
    if (getcwd(buffer, sizeof(buffer)) == NULL) {
        return val_null();
    }
    return val_obj((Object*)str_copy(buffer, (int)strlen(buffer)));
#endif
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
    wchar_t* wpath = utf8_to_utf16(path);
    if (!wpath) {
        return val_null();
    }
    wchar_t wbuffer[4096];
    if (_wfullpath(wbuffer, wpath, sizeof(wbuffer) / sizeof(wchar_t)) == NULL) {
        free(wpath);
        return val_null();
    }
    free(wpath);
    char* abs_path = utf16_to_utf8(wbuffer);
    if (!abs_path) {
        return val_null();
    }
    Value result = val_obj((Object*)str_copy(abs_path, (int)strlen(abs_path)));
    free(abs_path);
    return result;
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
        int n = snprintf(buffer, sizeof(buffer), "%s/%s", cwd, path);
        if (n >= (int)sizeof(buffer)) { buffer[sizeof(buffer)-1] = '\0'; }
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

// dirs.script_dir() - 获取脚本所在目录
// 如果通过命令行运行脚本（如 leno d:\project\main.leno），返回脚本所在目录
// 如果直接运行 exe 或 REPL 模式，返回 exe 所在目录
static Value native_dirs_script_dir(int argCount, Value* args) {
    (void)argCount;
    (void)args;

    // 外部声明全局参数
    extern int g_argc;
    extern char** g_argv;

    const char* target = NULL;
    char* target_heap = NULL;  // 若 target 来自 GetModuleFileNameW，需 free

    // 优先从参数中找脚本路径（第一个非选项参数）
    for (int i = 1; i < g_argc; i++) {
        if (g_argv[i][0] != '-') {
            target = g_argv[i];
            break;
        }
    }

    // 没有脚本路径，使用 exe 路径
    if (!target && g_argc > 0 && g_argv[0]) {
        target = g_argv[0];
#ifdef _WIN32
        // Windows: g_argv[0] 可能是相对路径（如"game.exe"），
        // 在快捷方式 / 批处理中 CWD 可能不等于 exe 所在目录。
        // 用 GetModuleFileNameW 获取当前 exe 的真实绝对路径，不依赖 CWD。
        wchar_t exe_path[4096];
        DWORD len = GetModuleFileNameW(NULL, exe_path, 4096);
        if (len > 0 && len < 4096) {
            target_heap = utf16_to_utf8(exe_path);
            if (target_heap) {
                target = target_heap;
            }
        }
#endif
    }

    if (!target) {
        return val_null();
    }

    // 转换为绝对路径
#ifdef _WIN32
    wchar_t* wtarget = utf8_to_utf16(target);
    if (!wtarget) { return val_null(); }
    wchar_t wabs[4096];
    if (_wfullpath(wabs, wtarget, sizeof(wabs) / sizeof(wchar_t)) == NULL) {
        free(wtarget);
        return val_null();
    }
    free(wtarget);
    char* abs_utf8 = utf16_to_utf8(wabs);
    if (!abs_utf8) { return val_null(); }
    char abs_path[4096];
    strncpy(abs_path, abs_utf8, sizeof(abs_path) - 1);
    abs_path[sizeof(abs_path) - 1] = '\0';
    free(abs_utf8);
#else
    char abs_path[4096];
    if (realpath(target, abs_path) == NULL) {
        // realpath 失败，尝试拼接 cwd
        char cwd[4096];
        if (target[0] == '/') {
            strncpy(abs_path, target, sizeof(abs_path) - 1);
            abs_path[sizeof(abs_path) - 1] = '\0';
        } else if (getcwd(cwd, sizeof(cwd))) {
            int n = snprintf(abs_path, sizeof(abs_path), "%s/%s", cwd, target);
            if (n >= (int)sizeof(abs_path)) { abs_path[sizeof(abs_path)-1] = '\0'; }
        } else {
            return val_null();
        }
    }
#endif

    // 截取目录部分（去掉最后一个路径分隔符之后的内容）
    int len = (int)strlen(abs_path);
    while (len > 0 && abs_path[len - 1] != '/' && abs_path[len - 1] != '\\') {
        len--;
    }

    // 去掉末尾的分隔符（保留根目录的情况如 "C:\"）
    if (len > 1) {
        len--;
    }

    if (len == 0) {
        if (target_heap) free(target_heap);
        return val_obj((Object*)str_copy(".", 1));
    }

    ObjString* result = str_copy(abs_path, len);
    if (target_heap) free(target_heap);
    return val_obj((Object*)result);
}

// dirs.res_dir() - 获取**随包资源**所在目录（双模式）
//   · 单文件打包（-p --onefile）运行：内嵌资源段释放到的缓存目录（按内容哈希分目录）；
//   · 未打包（脚本 / exe / .lenb）：与 script_dir() 完全一致（资源就在脚本/exe 旁边）。
// 为什么必须和 script_dir() 分开（只增不改，语义各自独立）：
//   · 读随包资源（图片/音频/字体/DLL…）→ 用 res_dir()；
//   · 写用户数据（存档、书签、配置）→ 必须用 script_dir()。
//   资源目录是按内容哈希分目录的，**每次重新打包 hash 一变目录就变**，
//   把用户数据写进去等于"升级即丢"，而且不会有任何报错。
static Value native_dirs_res_dir(int argCount, Value* args) {
    // 打包模式：VM 运行时解包完成后会写入释放目录（vm_res_dir() 未打包时为空串）
    const char* packed = vm_res_dir();
    if (packed && packed[0]) {
        int len = (int)strlen(packed);
        // 去掉结尾分隔符，与 script_dir() 的返回形式保持一致，
        // 这样 dirs.join(res_dir(), "a.png") 和 res_dir() + "/a.png" 都不会出岔子
        while (len > 1 && (packed[len - 1] == '/' || packed[len - 1] == '\\')) {
            len--;
        }
        return val_obj((Object*)str_copy(packed, len));
    }
    // 未打包：与 script_dir() 同源，直接复用（避免两份逻辑日后走偏）
    return native_dirs_script_dir(argCount, args);
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
    wchar_t* wpath = utf8_to_utf16(path);
    if (!wpath) { return val_bool(0); }
    DWORD attr = GetFileAttributesW(wpath);
    free(wpath);
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
    wchar_t* wpath = utf8_to_utf16(path);
    if (!wpath) { return val_bool(0); }
    DWORD attr = GetFileAttributesW(wpath);
    free(wpath);
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
    wchar_t* wpath = utf8_to_utf16(path);
    if (!wpath) { return val_bool(0); }
    DWORD attr = GetFileAttributesW(wpath);
    free(wpath);
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

// dirs.is_symlink(path) - 检查是否是符号链接/junction（reparse point）
// 用于递归搜索时跳过，防止无限递归导致栈溢出
static Value native_dirs_is_symlink(int argCount, Value* args) {
    if (argCount < 1) {
        native_throw_error("is_symlink 需要路径参数");
        return val_null();
    }
    
    const char* path = get_string(args[0]);
    if (!path) {
        native_throw_error("is_symlink 参数必须是字符串");
        return val_null();
    }
    
#ifdef _WIN32
    // 使用 FindFirstFileW 检查 reparse point（junction/symlink）
    // FindFirstFileW 不跟随符号链接，返回链接本身的属性
    wchar_t* wpath = utf8_to_utf16(path);
    if (!wpath) { return val_bool(0); }
    
    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW(wpath, &findData);
    free(wpath);
    
    if (hFind == INVALID_HANDLE_VALUE) {
        return val_bool(0);
    }
    
    FindClose(hFind);
    // FILE_ATTRIBUTE_REPARSE_POINT 涵盖 junction、symlink、mount point 等
    return val_bool(findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    struct stat st;
    // lstat 不跟随符号链接，返回链接本身的属性
    if (lstat(path, &st) != 0) {
        return val_bool(0);
    }
    return val_bool(S_ISLNK(st.st_mode));
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
    wchar_t* wpath = utf8_to_utf16(path);
    if (!wpath) {
        return val_bool(0);
    }
    int result = _wmkdir(wpath);
    free(wpath);
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
            wchar_t* wtmp = utf8_to_utf16(temp);
            int r = wtmp ? _wmkdir(wtmp) : -1;
            free(wtmp);
            (void)r;
#else
            mkdir(temp, 0755);
#endif
            
            *p = sep;
        }
    }
    
    // 创建最后一层
#ifdef _WIN32
    wchar_t* wtmp = utf8_to_utf16(temp);
    int result = wtmp ? _wmkdir(wtmp) : -1;
    free(wtmp);
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
    wchar_t* wpath = utf8_to_utf16(path);
    if (!wpath) {
        return val_bool(0);
    }
    int result = _wrmdir(wpath);
    free(wpath);
#else
    int result = rmdir(path);
#endif
    
    return val_bool(result == 0);
}

// ---- 递归删除辅助 ----
#ifdef _WIN32
// 返回 1 成功 / 0 失败。目录递归删除其子项后移除，文件直接删除。
static int dirs_recursive_delete_w(const wchar_t* wpath) {
    DWORD attr = GetFileAttributesW(wpath);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        return 0;
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        // ⚠ reparse point（junction / symlink / mount point）**只摘链接、绝不递归进去**：
        //   `FindFirstFileW(L"link\\*")` 会**透过**链接列出 **target 的内容** ⇒ 原实现先把
        //   目标目录里的东西全删掉、再摘链接（删一个链接 = 删掉目标目录的内容，数据丢失 ✗）。
        //   `RemoveDirectoryW` 对 reparse point 就是摘链接本身、不动 target ✓
        if (attr & FILE_ATTRIBUTE_REPARSE_POINT) {
            return RemoveDirectoryW(wpath) ? 1 : 0;
        }
        wchar_t wsearch[4096];
        swprintf(wsearch, sizeof(wsearch) / sizeof(wchar_t), L"%ls\\*", wpath);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(wsearch, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
                    continue;
                }
                wchar_t child[4096];
                swprintf(child, sizeof(child) / sizeof(wchar_t), L"%ls\\%ls", wpath, fd.cFileName);
                dirs_recursive_delete_w(child);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        return RemoveDirectoryW(wpath) ? 1 : 0;
    }
    return DeleteFileW(wpath) ? 1 : 0;
}
#else
static int dirs_recursive_delete_u(const char* path) {
    // 用 **lstat** 拿链接自身属性（原实现用 stat ⇒ 跟随链接）：符号链接**只删链接本身**
    //   （`remove` 即 unlink）。否则 `opendir(link)` 会进 target 删光内容，而最后那句
    //   `rmdir(link)` 在 POSIX 上必然失败（ENOTDIR）⇒ **内容没了却报失败**，最坏的那种组合 ✗
    struct stat st;
    if (lstat(path, &st) != 0) {
        return 0;
    }
    if (S_ISLNK(st.st_mode)) {
        return remove(path) == 0 ? 1 : 0;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR* d = opendir(path);
        if (d) {
            struct dirent* e;
            while ((e = readdir(d)) != NULL) {
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
                    continue;
                }
                char child[4096];
                snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
                dirs_recursive_delete_u(child);
            }
            closedir(d);
        }
        return rmdir(path) == 0 ? 1 : 0;
    }
    return remove(path) == 0 ? 1 : 0;
}
#endif

// dirs.delete(path) - 删除文件或目录（目录递归删除）
//   ⚠ **不跟随链接**：链接（junction / symlink / mount point）只删链接本身，绝不动 target ——
//     否则 `delete(link)` 会透过链接把**目标目录的内容**删光（原实现就是这样，属数据丢失）。
static Value native_dirs_delete(int argCount, Value* args) {
    if (argCount < 1) {
        native_throw_error("delete 需要路径参数");
        return val_null();
    }

    const char* path = get_string(args[0]);
    if (!path) {
        native_throw_error("delete 参数必须是字符串");
        return val_null();
    }

#ifdef _WIN32
    wchar_t* wpath = utf8_to_utf16(path);
    if (!wpath) {
        return val_bool(0);
    }
    int result = dirs_recursive_delete_w(wpath);
    free(wpath);
#else
    int result = dirs_recursive_delete_u(path);
#endif
    return val_bool(result ? 1 : 0);
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
    
#ifdef _WIN32
    wchar_t* wold = utf8_to_utf16(old_path);
    wchar_t* wnew = utf8_to_utf16(new_path);
    if (!wold || !wnew) {
        free(wold);
        free(wnew);
        return val_bool(0);
    }
    int result = _wrename(wold, wnew);
    free(wold);
    free(wnew);
#else
    int result = rename(old_path, new_path);
#endif
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
    
    // ★ 将 arr 注册为 GC 额外根，防止 str_copy → gc_alloc 内部
    // malloc 失败时触发 gc_major_collect() 误回收 arr 及其已添加的字符串。
    // arr 是 C 局部变量，不在 VM 栈/帧局部变量中，GC mark_roots 看不到它。
    Value arr_val = val_obj((Object*)arr);
    gc_push_root(&arr_val);
    
#ifdef _WIN32
    // 将 UTF-8 路径转换为宽字符
    wchar_t* wpath = utf8_to_utf16(path);
    if (!wpath) {
        gc_pop_root();
        return val_null();
    }
    
    // 构建搜索路径
    wchar_t search_path[4096];
    swprintf(search_path, sizeof(search_path) / sizeof(wchar_t), L"%ls\\*", wpath);
    free(wpath);
    
    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW(search_path, &findData);
    
    if (hFind == INVALID_HANDLE_VALUE) {
        gc_pop_root();
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
        gc_pop_root();
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
    
    gc_pop_root();
    return val_obj((Object*)arr);
}

// walk 的内部实现：扫描单个目录，把分类结果装进一个 `DirEntry` 追加到 result，再递归子目录。
//   dir_names 是**子目录名**（不含路径，与 files 同形）、subdirs 是子目录**完整路径**（只用于递归）
static void walk_scan_dir(const char* path, ObjArray* result) {
    ObjArray* dir_names = arr_new_with_capacity(8);
    ObjArray* file_names = arr_new_with_capacity(8);
    ObjArray* subdirs = arr_new_with_capacity(8);  // 存放子目录完整路径，用于递归

    if (!dir_names || !file_names || !subdirs) {
        return;
    }

    // ★ 将临时数组注册为 GC 额外根，防止 str_copy → gc_alloc 内部
    // malloc 失败时触发 gc_major_collect() 误回收这些 C 局部变量持有的数组。
    Value dir_names_val = val_obj((Object*)dir_names);
    Value file_names_val = val_obj((Object*)file_names);
    Value subdirs_val = val_obj((Object*)subdirs);
    gc_push_root(&dir_names_val);
    gc_push_root(&file_names_val);
    gc_push_root(&subdirs_val);

#ifdef _WIN32
    wchar_t* wpath = utf8_to_utf16(path);
    if (!wpath) { gc_pop_root(); gc_pop_root(); gc_pop_root(); return; }

    wchar_t wsearch[4096];
    swprintf(wsearch, sizeof(wsearch) / sizeof(wchar_t), L"%ls\\*", wpath);
    free(wpath);

    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW(wsearch, &findData);

    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(findData.cFileName, L".") == 0 || wcscmp(findData.cFileName, L"..") == 0) {
                continue;
            }

            char* utf8_name = utf16_to_utf8(findData.cFileName);
            if (!utf8_name) { continue; }

            ObjString* name = str_copy(utf8_name, (int)strlen(utf8_name));
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                arr_push(dir_names, val_obj((Object*)name));
                // ⚠ reparse point（junction / symlink / mount point）**只列、不递归**：
                //   跟随它会走出原目录树 —— 指回祖先的 junction 会一路拼出
                //   `a\b\loop\b\loop\…` 直到路径超长（实测：一条自指 junction 产出 66 条垃圾条目），
                //   指向大目录的链接更会成倍放大。口径同 `find`（默认不跟随链接）✓
                //   要跟随：业务层先 `dirs.is_symlink(p)` 判一下，再自己 walk 那个目标。
                if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    char full_path[4096];
                    snprintf(full_path, sizeof(full_path), "%s\\%s", path, utf8_name);
                    arr_push(subdirs, val_obj((Object*)str_copy(full_path, (int)strlen(full_path))));
                }
            } else {
                arr_push(file_names, val_obj((Object*)name));
            }
            free(utf8_name);
        } while (FindNextFileW(hFind, &findData));
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

            char full_path[4096];
            snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
            // lstat 拿**链接自身**的属性：符号链接**只列、不递归**（口径同 Windows 的 reparse point）。
            // 指向目录的链接仍按"目录"归类（stat 跟随一次判一下），只是不进 subdirs ✓
            struct stat lst;
            if (lstat(full_path, &lst) != 0) {
                arr_push(file_names, val_obj((Object*)name));
            } else if (S_ISLNK(lst.st_mode)) {
                struct stat tst;
                if (stat(full_path, &tst) == 0 && S_ISDIR(tst.st_mode)) {
                    arr_push(dir_names, val_obj((Object*)name));   // 目录链接：列出，不递归
                } else {
                    arr_push(file_names, val_obj((Object*)name));  // 文件链接：当文件
                }
            } else if (S_ISDIR(lst.st_mode)) {
                arr_push(dir_names, val_obj((Object*)name));
                arr_push(subdirs, val_obj((Object*)str_copy(full_path, (int)strlen(full_path))));
            } else {
                arr_push(file_names, val_obj((Object*)name));
            }
        }
        closedir(dir);
    }
#endif

    // 创建 `DirEntry{ root, dirs, files }` 条目
    //   字段顺序 = DIRENTRY_STRUCT_SPEC 的声明顺序（root, dirs, files）—— 那份 spec 同时是
    //   编译期字段表与运行期 ObjStructDef 的来源 ⇒ 编译期索引与运行期槽位不可能漂 ✓
    ObjStruct* entry = native_struct_new("DirEntry");
    if (entry) {
        Value entry_val = val_obj((Object*)entry);
        gc_push_root(&entry_val);   // 填字段期间它还没进 result ⇒ 必须自己护住
        native_struct_set(entry, "root", val_obj((Object*)str_copy(path, (int)strlen(path))));
        native_struct_set(entry, "dirs", val_obj((Object*)dir_names));
        native_struct_set(entry, "files", val_obj((Object*)file_names));
        arr_push(result, entry_val);
        gc_pop_root();
    }

    // 递归处理子目录
    for (int i = 0; i < subdirs->count; i++) {
        const char* subdir_path = get_string(subdirs->elements[i]);
        if (subdir_path) {
            walk_scan_dir(subdir_path, result);
        }
    }

    gc_pop_root();  // subdirs_val
    gc_pop_root();  // file_names_val
    gc_pop_root();  // dir_names_val
}

// dirs.walk(path) - 递归遍历目录树
// 返回 `Array[DirEntry]`：每项是 `DirEntry{ string root, Array[string] dirs, Array[string] files }`
//   · `root`  = 本次扫到的目录路径；
//   · `dirs`  = 它的直接**子目录名**；`files` = 直接**文件名**（都不含路径 ⇒ 要完整路径用
//               `dirs.join(e.root, name)`）；
//   · 递归深度 = 目录树深度，每层一条（先父后子）。
// ⚠ 返回形态历史（v3.2.3 起）：旧形态是 `Array[Array]` 的 `[root, dirs, files]` 三元组 ⇒ 元素是
//   `any`，只能靠 `e[0] / e[1] / e[2]` 位置索引取值（不可读，且顺序一改就静默错位）。现在改为带
//   **字段类型**的 struct（native 类型规格，见 `NativeTypeSpec`）⇒ 字段名与类型编译期已知，
//   调用点零收窄。扫描仍是同一份口径（`walk_scan_dir`）单遍直接装结构体 ⇒ 不存在"两套 API 不许漂"
//   的对账负担。
// ⚠ **不跟随链接**（junction / symlink / mount point）：这类目录会出现在 `dirs` 里（看得见），
//   但**不会递归进去** —— 跟随它会走出原目录树，遇到指回祖先的链接就一路拼出
//   `a\b\loop\b\loop\…`（实测一条自指 junction 产出 66 条垃圾条目，直到路径超长才停）。
//   口径与 `find`（默认不跟随）一致；要跟随请自己 `dirs.is_symlink(p)` 判一下再 walk 目标。
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

    ObjArray* result = arr_new_with_capacity(16);
    if (!result) {
        return val_null();
    }

    // ★ 将 result 注册为 GC 额外根，防止 walk_scan_dir 内部 str_copy
    // → gc_alloc malloc 失败时触发 gc_major_collect() 误回收 result。
    Value result_val = val_obj((Object*)result);
    gc_push_root(&result_val);

    walk_scan_dir(path, result);

    gc_pop_root();
    return val_obj((Object*)result);
}

// ==================== DirEntry 的类型规格（walk 的返回元素，v3.2.3） ====================
// 病根曾是"native 元数据只有一个 TypeKind 槽"，表达不了 `Array[DirEntry]` ⇒ 现在用
//   NativeTypeSpec 声明（**编译期字段表 + 运行期 ObjStructDef 同一来源**，字段顺序同源 ⇒
//   编译期索引与运行期槽位不可能漂）。
// 历史：v3.2.3 一开始是**并行**加了 `walk_entries`（另开函数、保留 `walk` 的 `Array[Array]`
//   形态以免破坏既有调用点）；随后确认"两套 API"本身才是负担 —— 位置索引既不可读、类型也表达
//   不出来，于是合并回**单个 `walk`**（返回 `Array[DirEntry]`）、删掉 `walk_entries`，全部调用点
//   同步改成字段取值。现在只有一条扫描路径、一种返回形态 ✓
static const NativeTypeSpec S_STR_SPEC          = { NTYPE_STRING, NULL, NULL, NULL };
static const NativeTypeSpec S_STRARR_SPEC       = { NTYPE_ARRAY, NULL, &S_STR_SPEC, NULL };
static const NativeTypeSpec S_DIRENTRY_SPEC     = { NTYPE_STRUCT, "DirEntry", NULL, NULL };
static const NativeTypeSpec S_DIRENTRY_ARR_SPEC = { NTYPE_ARRAY, NULL, &S_DIRENTRY_SPEC, NULL };

static const char* DIRENTRY_FIELD_NAMES[] = { "root", "dirs", "files" };
static const NativeTypeSpec* DIRENTRY_FIELD_TYPES[] = { &S_STR_SPEC, &S_STRARR_SPEC, &S_STRARR_SPEC };
static const NativeStructSpec DIRENTRY_STRUCT_SPEC = {
    "dirs", "DirEntry", 3, DIRENTRY_FIELD_NAMES, DIRENTRY_FIELD_TYPES
};

// ==================== DirInfo 的类型规格（stat 的返回，v3.2.4） ====================
// 为什么不是 `Dict[K, V]`：stat 的五个键**类型不齐** —— exists / is_file / is_dir 是 bool，
//   size / mtime 是 int ⇒ 同质的 Dict 表达不了"这个键是 bool、那个键是 int"。
// 字段名与旧的 Dict 键**逐字相同** ⇒ `st.size` / `st.exists` 这类调用点不用改；
//   要改的是 `st["size"]` 下标式与 `if st is Dict` 收窄（编译期即被挡住）。
static const NativeTypeSpec S_BOOL_SPEC = { NTYPE_BOOL, NULL, NULL, NULL };
static const NativeTypeSpec S_INT_SPEC  = { NTYPE_INT,  NULL, NULL, NULL };
static const NativeTypeSpec S_DIRINFO_SPEC = { NTYPE_STRUCT, "DirInfo", NULL, NULL };

static const char* DIRINFO_FIELD_NAMES[] = { "exists", "size", "is_file", "is_dir", "mtime" };
static const NativeTypeSpec* DIRINFO_FIELD_TYPES[] = {
    &S_BOOL_SPEC, &S_INT_SPEC, &S_BOOL_SPEC, &S_BOOL_SPEC, &S_INT_SPEC
};
static const NativeStructSpec DIRINFO_STRUCT_SPEC = {
    "dirs", "DirInfo", 5, DIRINFO_FIELD_NAMES, DIRINFO_FIELD_TYPES
};

// ==================== 文件信息 ====================

// dirs.stat(path) - 获取文件信息（返回 `DirInfo`，**字段类型编译期已知** —— v3.2.4）
// 字段：exists(bool) / size(int) / is_file(bool) / is_dir(bool) / mtime(int)
//   · 路径不存在（或没权限）⇒ exists=false、其余保持默认值 0（与旧 Dict 形态逐字同口径）
//   · size 是**该条目自身**的大小（目录在 Windows 报 0、POSIX 报 st_size），不是递归总大小
//   · mtime 是 **Unix 秒（UTC）**：Windows 由 FILETIME 换算、POSIX 取 st_mtime（均为 int64，
//     不走 int32 截断）；拿不到时间 ⇒ 0（2026-09-27 前 Windows 恒 0，属已知限制，现已实现）
// ⚠ 与旧形态（无类型 `Dict`）**逐字段同义**：键名一字未改 ⇒ `st.size` / `st.exists` 这类调用点
//   不用动；但 `st["size"]` 下标式与 `if st is Dict` 收窄不再适用。
// 📌 取单个字段就写 `dirs.stat(p).size` —— 2026-09-27 起**删掉了并行的 `dirs.size()`**：
//   它当初只是"stat 返回无类型 Dict 时补的类型化入口"（P21 收窄收进标准库那一类活），
//   stat 定型后它就是**第二个入口** ⇒ 收敛掉，避免同一件事两种写法长期共存。
//   口径本就一致（不存在/参数不对 ⇒ size == 0，同 stat 的默认值）；要区分「空文件」与
//   「不存在」先 `dirs.exists()` 判 ✓
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
    
    ObjStruct* info = native_struct_new("DirInfo");
    if (!info) {
        return val_null();
    }
    Value info_val = val_obj((Object*)info);
    gc_push_root(&info_val);   // 填字段期间它还没进任何数组/局部根 ⇒ 自己护住

    // 默认值（与旧 Dict 形态逐字相同）
    native_struct_set(info, "exists",  val_bool(0));
    native_struct_set(info, "size",    val_int(0));
    native_struct_set(info, "is_file", val_bool(0));
    native_struct_set(info, "is_dir",  val_bool(0));
    native_struct_set(info, "mtime",   val_int(0));
    
#ifdef _WIN32
    wchar_t* wpath = utf8_to_utf16(path);
    WIN32_FILE_ATTRIBUTE_DATA attrData;
    if (wpath && GetFileAttributesExW(wpath, GetFileExInfoStandard, &attrData)) {
        // 文件存在，更新信息
        native_struct_set(info, "exists", val_bool(1));
        
        // size
        // ⚠ 原来是 val_int((int)size.QuadPart) —— (int) 是 **32 位**，≥2GB 的文件会被截断 ✗
        //   实测（2026-09-26）：2GB+1KB 的文件读出 **-2147482624**（负数！）⇒ fmt_size 也跟着
        //   打出 "-2147482624 B"；5GB 则读成 0x40000000 = 1GB。本语言的 int 是 48 位 ⇒ 传 int64 ✓
        LARGE_INTEGER size;
        size.LowPart = attrData.nFileSizeLow;
        size.HighPart = attrData.nFileSizeHigh;
        native_struct_set(info, "size", val_int((int64_t)size.QuadPart));
        
        // is_file, is_dir
        int is_dir = attrData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
        native_struct_set(info, "is_file", val_bool(!is_dir));
        native_struct_set(info, "is_dir", val_bool(is_dir));
        
        // mtime：FILETIME（1601-01-01 起、100ns 单位）→ Unix 秒（1970-01-01 起）
        //   ⚠ 原先是**恒 0** 的"简化版" ⇒ `DirInfo.mtime` 声明了却永远没值（Windows 上）
        //      docs 里那条"mtime 在 Windows 恒为 0"的已知限制就是它 —— 现已实现。
        //   换算：ticks / 10^7 得秒，再减 1601→1970 的 11644473600 秒。FILETIME 本身是 UTC
        //   ⇒ 不涉及时区。ftLastWriteTime 为 0（无时间）时保持默认值 0，不写出负数。
        {
            ULARGE_INTEGER ft;
            ft.LowPart = attrData.ftLastWriteTime.dwLowDateTime;
            ft.HighPart = attrData.ftLastWriteTime.dwHighDateTime;
            if (ft.QuadPart > 0) {
                int64_t unix_sec = (int64_t)(ft.QuadPart / 10000000ULL) - 11644473600LL;
                native_struct_set(info, "mtime", val_int(unix_sec));
            }
        }
    }
    if (wpath) { free(wpath); }
#else
    struct stat st;
    if (stat(path, &st) == 0) {
        // 文件存在，更新信息
        native_struct_set(info, "exists", val_bool(1));
        
        // size（同上：不能 (int) 截断，见 Windows 分支的注释 ✓）
        native_struct_set(info, "size", val_int((int64_t)st.st_size));
        
        // is_file, is_dir
        native_struct_set(info, "is_file", val_bool(S_ISREG(st.st_mode)));
        native_struct_set(info, "is_dir", val_bool(S_ISDIR(st.st_mode)));
        
        // mtime（⚠ 用 int64：原先 (int) 是 32 位截断，2038 之后会溢出成负数）
        native_struct_set(info, "mtime", val_int((int64_t)st.st_mtime));
    }
#endif
    
    gc_pop_root();
    return info_val;
}

// dirs.list_drives() - 返回盘符列表
//   Windows: ["C:\\", "D:\\", ...]（按位枚举逻辑驱动器）
//   Unix:    ["/"]
static Value native_dirs_list_drives(int argCount, Value* args) {
    (void)argCount;
    (void)args;
    ObjArray* arr = arr_new_with_capacity(8);
    if (!arr) {
        return val_null();
    }

    // ★ GC root 保护（与 listdir 同理）
    Value arr_val = val_obj((Object*)arr);
    gc_push_root(&arr_val);

#ifdef _WIN32
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (mask & (1u << i)) {
            wchar_t buf[8];
            buf[0] = (wchar_t)('A' + i);
            buf[1] = L':';
            buf[2] = L'\\';
            buf[3] = L'\0';
            char* u = utf16_to_utf8(buf);
            if (u) {
                arr_push(arr, val_obj((Object*)str_copy(u, (int)strlen(u))));
                free(u);
            }
        }
    }
#else
    arr_push(arr, val_obj((Object*)str_copy("/", 1)));
#endif

    gc_pop_root();
    return val_obj((Object*)arr);
}

// ==================== 初始化 ====================

void dirs_init_module(void) {
    // 路径操作
    TypeKind string_params[] = {TYPE_STRING};
    TypeKind string2_params[] = {TYPE_STRING, TYPE_STRING};
    TypeKind no_params[] = {};

    native_register_module_method_spec("dirs", "list_drives", native_dirs_list_drives, 0, -1, -1, &NATIVE_T_ARR_STRING, no_params);

    native_register_module_method_spec("dirs", "cwd", native_dirs_cwd, 0, -1, -1, &NATIVE_T_STRING, no_params);
    native_register_module_method_spec("dirs", "abspath", native_dirs_abspath, 1, -1, -1, &NATIVE_T_STRING, string_params);
    native_register_module_method_spec("dirs", "basename", native_dirs_basename, 1, -1, -1, &NATIVE_T_STRING, string_params);
    native_register_module_method_spec("dirs", "dirname", native_dirs_dirname, 1, -1, -1, &NATIVE_T_STRING, string_params);
    native_register_module_method_spec("dirs", "extname", native_dirs_extname, 1, -1, -1, &NATIVE_T_STRING, string_params);
    native_register_module_method_spec("dirs", "join", native_dirs_join, -1, 0, -1, &NATIVE_T_STRING, string_params);
    native_register_module_method_spec("dirs", "sep", native_dirs_sep, 0, -1, -1, &NATIVE_T_STRING, no_params);
    native_register_module_method_spec("dirs", "script_dir", native_dirs_script_dir, 0, -1, -1, &NATIVE_T_STRING, no_params);
    // 随包资源目录：未打包时 == script_dir()，打包时 == 资源释放目录（见函数头注释）
    native_register_module_method_spec("dirs", "res_dir", native_dirs_res_dir, 0, -1, -1, &NATIVE_T_STRING, no_params);

    // 检查操作
    native_register_module_method_spec("dirs", "exists", native_dirs_exists, 1, -1, -1, &NATIVE_T_BOOL, string_params);
    native_register_module_method_spec("dirs", "is_file", native_dirs_is_file, 1, -1, -1, &NATIVE_T_BOOL, string_params);
    native_register_module_method_spec("dirs", "is_dir", native_dirs_is_dir, 1, -1, -1, &NATIVE_T_BOOL, string_params);
    native_register_module_method_spec("dirs", "is_symlink", native_dirs_is_symlink, 1, -1, -1, &NATIVE_T_BOOL, string_params);

    // 目录操作
    native_register_module_method_spec("dirs", "mkdir", native_dirs_mkdir, 1, -1, -1, &NATIVE_T_BOOL, string_params);
    native_register_module_method_spec("dirs", "mkdir_p", native_dirs_mkdir_p, 1, -1, -1, &NATIVE_T_BOOL, string_params);
    native_register_module_method_spec("dirs", "rmdir", native_dirs_rmdir, 1, -1, -1, &NATIVE_T_BOOL, string_params);
    native_register_module_method_spec("dirs", "delete", native_dirs_delete, 1, -1, -1, &NATIVE_T_BOOL, string_params);
    native_register_module_method_spec("dirs", "rename", native_dirs_rename, 2, -1, -1, &NATIVE_T_BOOL, string2_params);

    // 遍历操作
    native_register_module_method_spec("dirs", "listdir", native_dirs_listdir, 1, -1, -1, &NATIVE_T_ARR_STRING, string_params);
    // walk：带**完整返回类型规格** `Array[DirEntry]`（v3.2.3）—— 编译器因此认识条目字段类型
    //   （`e.root` 是 string、`e.files` 是 Array[string]）⇒ 调用点零收窄 ✓
    //   （`walk_entries` 已删除：它只是过渡期的并行 API，现与 walk 合并为同一条路径 —— 见上方
    //    `native_dirs_walk` 的历史注释。）
    native_register_struct_spec(&DIRENTRY_STRUCT_SPEC);
    native_register_module_method_spec("dirs", "walk", native_dirs_walk,
                                       1, -1, -1, &S_DIRENTRY_ARR_SPEC, string_params);

    // 文件信息
    // stat：返回 `DirInfo`（**字段类型编译期已知**，v3.2.4）—— 五个键类型不齐（bool 与 int 混）
    //   ⇒ 同质的 Dict 表达不了；改结构体后 `st.size` 直接是 int，不再需要 `_int(st.get("size", 0))`
    //   那层手动转换 ✓
    native_register_struct_spec(&DIRINFO_STRUCT_SPEC);
    native_register_module_method_spec("dirs", "stat", native_dirs_stat, 1, -1, -1, &S_DIRINFO_SPEC, string_params);
    // size：**已删除**（2026-09-27 收敛）—— `stat(p).size` 本身就是 int ⇒ 那个单键入口成了
    //   重复入口（它当初只是 stat 返回 any 时的补丁）⇒ 调用点统一写 `dirs.stat(p).size` ✓
}
