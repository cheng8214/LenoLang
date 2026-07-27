/* Leno FFI 模块 - 简化外部函数接口
 * 灵感来源于 LuaJIT FFI 和 Python ctypes
 * 无外部依赖，跨平台支持 (Windows/Linux)
 *
 * 提供的功能:
 *   ffi.load(path)                  - 加载动态链接库
 *   ffi.free(x)                    - 统一释放 FFI 资源（指针/库/回调）
 *   ffi.call(lib, name, ...)        - 调用函数（返回 any，实际为 int，需转换）
 *   ffi.call_int(lib, name, ...)    - 调用函数（明确返回 int）
 *   ffi.call_double(lib, name, ...) - 调用函数（返回 double）
 *   ffi.call_void(lib, name, ...)   - 调用函数（无返回值）
 *   ffi.call_ptr(lib, name, ...)    - 调用函数（返回指针）
 *   ffi.call_bool(lib, name, ...)   - 调用函数（返回布尔值）
 *   ffi.alloc(type_name, value)    - 分配指定类型内存并初始化（value 可选）
 *   ffi.malloc(size)                - 分配内存
 *   ffi.calloc(count, size)         - 分配并清零内存
 *   ffi.realloc(ptr, size)          - 重新分配内存
 *   ffi.free(ptr)                   - 释放内存
 *   ffi.memcpy(dest, src, size)     - 内存拷贝
 *   ffi.memset(ptr, value, size)    - 内存填充
 *   ffi.sizeof(ptr)                 - 获取指针指向的内存大小
 *   ffi.nullptr()                   - 返回空指针
 *   ffi.read_byte(ptr, off)         - 读取 uint8
 *   ffi.read_int8(ptr, off)         - 读取 int8
 *   ffi.read_int16(ptr, off)        - 读取 int16
 *   ffi.read_uint16(ptr, off)       - 读取 uint16
 *   ffi.read_int(ptr, off)          - 读取 int32
 *   ffi.read_uint(ptr, off)         - 读取 uint32
 *   ffi.read_int64(ptr, off)        - 读取 int64
 *   ffi.read_uint64(ptr, off)       - 读取 uint64
 *   ffi.read_float(ptr, off)        - 读取 float
 *   ffi.read_double(ptr, off)       - 读取 double
 *   ffi.read_ptr(ptr, off)          - 读取指针
 *   ffi.read_bool(ptr, off)         - 读取布尔值
 *   ffi.read_string(ptr, off)       - 读取字符串
 *   ffi.read_string_n(ptr, off, len)- 读取指定长度字符串
 *   ffi.write_byte(ptr, off, val)   - 写入 uint8
 *   ffi.write_int8(ptr, off, val)   - 写入 int8
 *   ffi.write_int16(ptr, off, val)  - 写入 int16
 *   ffi.write_int(ptr, off, val)    - 写入 int32
 *   ffi.write_uint(ptr, off, val)   - 写入 uint32
 *   ffi.write_int64(ptr, off, val)  - 写入 int64
 *   ffi.write_uint64(ptr, off, val) - 写入 uint64
 *   ffi.write_float(ptr, off, val)  - 写入 float
 *   ffi.write_double(ptr, off, val) - 写入 double
 *   ffi.write_ptr(ptr, off, val)    - 写入指针
 *   ffi.write_string(ptr, off, str) - 写入字符串
 *   ffi.offset(ptr, off)           - 指针偏移（返回新指针）
 *   ffi.ptr_from_int(address)      - 从整数地址创建指针
 *   ffi.ptr_to_int(ptr)            - 将指针地址值转换为整数
 *   ffi.string_bytes(str)          - 获取字符串字节长度
 *   ffi.utf8_to_utf16(str)         - UTF-8 转 UTF-16（Windows）
 *   ffi.utf16_to_utf8(ptr)         - UTF-16 转 UTF-8（Windows）
 *   ffi.alignof(type_name)          - 获取类型对齐要求
 *   ffi.sizeof_type(type_name)      - 获取 C 类型大小
 */

#include "include/native.h"
#include "include/leno_vm.h"
#include "include/leno_value.h"
#include "include/platform.h"
#include "include/platform_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include "leno_ffi.h"
#include <sys/types.h>
#include <sys/stat.h>

#ifdef _WIN32
/* Windows 上 S_ISREG 未定义，自行补充 */
#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif
#else
#include <unistd.h>
#endif

#define MAX_FFI_CALLBACKS 128

typedef struct {
    Value func_val;
    FFISignature* sig;
    int active;
} FFICallbackEntry;

static FFICallbackEntry g_callback_registry[MAX_FFI_CALLBACKS];
static int g_callback_count = 0;

/* ===== 上次 FFI 调用的错误码缓存 ===== */
static int64_t g_last_error = 0;

static void free_executable_memory(void* ptr, size_t size);

/* ===== 平台相关头文件 ===== */
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/* ===== FFI 库对象类型 ===== */
/* ObjType 枚举值在 leno_value.h 中定义: OBJ_FFI_LIBRARY, OBJ_FFI_POINTER */

typedef struct {
    Object header;
    char* path;       /* 库文件路径（strdup 分配） */
    int freed;        /* 是否已释放（防止 use-after-free） */
#ifdef _WIN32
    HMODULE handle;   /* Windows: 模块句柄 */
#else
    void* handle;     /* Linux: dlopen 句柄 */
#endif
} ObjFFILibrary;

/* ===== 辅助函数：类型判断 ===== */

static ObjFFILibrary* val_as_ffi_lib(Value v) {
    return (ObjFFILibrary*)val_as_obj(v);
}

static int val_is_ffi_ptr(Value v) {
    return val_is_obj(v) && val_as_obj(v)->type == OBJ_FFI_POINTER;
}

void ffi_library_free_resources(Object* obj) {
    ObjFFILibrary* lib = (ObjFFILibrary*)obj;
    free(lib->path);
    lib->path = NULL;
    if (lib->handle) {
#ifdef _WIN32
        FreeLibrary((HMODULE)lib->handle);
#else
        dlclose(lib->handle);
#endif
        lib->handle = NULL;
    }
    lib->freed = 1;
}

char* ffi_library_get_path(Object* obj) {
    ObjFFILibrary* lib = (ObjFFILibrary*)obj;
    return lib->path;
}

void ffi_pointer_free_resources(Object* obj) {
    ObjFFIPointer* ptr = (ObjFFIPointer*)obj;
    if (ptr->owned && !ptr->freed && ptr->ptr) {
        free(ptr->ptr);
        ptr->freed = 1;
    }
}

size_t ffi_pointer_get_data_size(Object* obj) {
    ObjFFIPointer* ptr = (ObjFFIPointer*)obj;
    if (ptr->owned && !ptr->freed) return ptr->size;
    return 0;
}

static ObjFFIPointer* val_as_ffi_ptr(Value v) {
    return (ObjFFIPointer*)val_as_obj(v);
}

static int val_is_cstruct(Value v) {
    return val_is_obj(v) && val_as_obj(v)->type == OBJ_CSTRUCT;
}

static ObjCStruct* val_as_cstruct(Value v) {
    return (ObjCStruct*)val_as_obj(v);
}

/* ===== 引用 gc.c 中的内存分配函数 ===== */
extern Object* gc_alloc(size_t size, ObjType type);

/* ===== 运行时安全检查宏 ===== */
/* 注意: 参数类型和数量由编译器和 VM 保证，这里只检查运行时才能判断的情况 */

/* 空指针 / 已释放指针检查 */
#define CHECK_NULL_PTR(p) \
    do { \
        if (!(p)->ptr || (p)->freed) { \
            native_throw_error("空指针引用或指针已释放"); \
            return val_null(); \
        } \
    } while(0)

/* 已释放的库对象检查 */
#define CHECK_LIB_FREED(lib) \
    do { \
        if ((lib)->freed || !(lib)->handle) { \
            native_throw_error("库对象已释放，不能再使用"); \
            return val_null(); \
        } \
    } while(0)

/* 边界检查宏（对 owned 指针有效） */
#define CHECK_BOUNDS(ptr, offset, access_size) \
    do { \
        if ((ptr)->owned && (ptr)->size > 0) { \
            if ((size_t)(offset) + (access_size) > (ptr)->size) { \
                native_throw_error("内存访问越界"); \
                return val_null(); \
            } \
        } \
    } while(0)

/* 辅助函数：解析偏移参数 */
static int parse_offset(int argc, Value* args, int idx) {
    if (argc > idx && val_is_int(args[idx]))
        return val_as_int(args[idx]);
    return 0;
}

/* ==================== FFI 库操作函数 ==================== */

/* 辅助函数：判断路径是否为绝对路径 */
static int is_absolute_path(const char* path) {
#ifdef _WIN32
    /* Windows: C:\, \\, 或 X: 开头 */
    if (path[0] && path[1] == ':') return 1;
    if (path[0] == '\\' && path[1] == '\\') return 1;
#else
    /* Linux/macOS: / 开头 */
    if (path[0] == '/') return 1;
#endif
    return 0;
}

/* 辅助函数：从路径中提取目录部分（含末尾分隔符） */
static void extract_dir(const char* path, char* dir, int dir_size) {
    strncpy(dir, path, dir_size - 1);
    dir[dir_size - 1] = '\0';
    int len = (int)strlen(dir);
    /* 查找最后一个路径分隔符 */
    for (int i = len - 1; i >= 0; i--) {
        if (dir[i] == '/' || dir[i] == '\\') {
            dir[i + 1] = '\0';
            return;
        }
    }
    /* 没有分隔符，返回当前目录 */
    dir[0] = '.'; dir[1] = '/'; dir[2] = '\0';
}

/* 辅助函数：跨平台检查文件是否存在（支持中文路径） */
static int file_exists_utf8(const char* path) {
#ifdef _WIN32
    wchar_t* wpath = utf8_to_utf16(path);
    if (!wpath) return 0;
    DWORD attrs = GetFileAttributesW(wpath);
    free(wpath);
    return (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY));
#else
    struct stat st;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
#endif
}

/* 辅助函数：跨平台加载动态库（Windows 使用 LoadLibraryW 支持中文路径） */
static void* load_library_utf8(const char* path) {
#ifdef _WIN32
    wchar_t* wpath = utf8_to_utf16(path);
    if (!wpath) return NULL;
    HMODULE handle = LoadLibraryW(wpath);
    free(wpath);
    return (void*)handle;
#else
    return dlopen(path, RTLD_LAZY);
#endif
}

/* 辅助函数：获取 exe 所在目录（含末尾分隔符） */
static void get_exe_dir(char* dir, int dir_size) {
#ifdef _WIN32
    wchar_t wexe[MAX_PATH_LEN];
    GetModuleFileNameW(NULL, wexe, MAX_PATH_LEN);
    /* 转为 UTF-8 */
    char utf8_exe[MAX_PATH_LEN];
    int conv = WideCharToMultiByte(CP_UTF8, 0, wexe, -1, utf8_exe, MAX_PATH_LEN, NULL, NULL);
    if (conv > 0) {
        extract_dir(utf8_exe, dir, dir_size);
    } else {
        dir[0] = '\0';
    }
#else
    char exe_path[MAX_PATH_LEN];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len > 0) {
        exe_path[len] = '\0';
        extract_dir(exe_path, dir, dir_size);
    } else {
        dir[0] = '\0';
    }
#endif
}

/* 辅助函数：获取当前模块目录（含末尾分隔符） */
static void get_current_module_dir(char* dir, int dir_size) {
    dir[0] = '\0';
    VM* vm_ptr = current_exec_vm;
    if (!vm_ptr) return;
    ModuleFrame* mf = vm_ptr->current_module_frame;
    if (!mf || !mf->module || !mf->module->source_path) return;
    const char* src = mf->module->source_path;
    extract_dir(src, dir, dir_size);
}

/* ffi.load(path) - 加载动态链接库
 * 自动搜索顺序（仅对相对路径/纯文件名生效）：
 *   1) 给定路径直接加载
 *   2) 当前模块目录/bin/<文件名>
 *   3) 当前模块目录/<文件名>
 *   4) 所有已加载模块目录/<文件名>（DLL 与 .leno 同目录，包自包含）
 *   5) exe 所在目录/<文件名>
 *   6) 系统PATH（交给系统加载器处理）
 * Windows 使用 LoadLibraryW 支持中文路径
 */
static Value ffi_load_func(int argc, Value* args) {
    (void)argc;
    ObjString* path_str = (ObjString*)val_as_obj(args[0]);
    const char* path = path_str->chars;
    char* resolved_path = NULL;  /* 自动搜索时分配，需在最后释放 */

    /* 1) 直接尝试加载给定路径 */
    void* handle = load_library_utf8(path);

    /* 2) 加载失败且为相对路径时，自动搜索 */
    if (!handle && !is_absolute_path(path)) {
        /* 提取纯文件名（去掉可能的目录前缀） */
        const char* filename = path;
        const char* last_sep = strrchr(path, '/');
        const char* last_sep2 = strrchr(path, '\\');
        if (last_sep2 && (!last_sep || last_sep2 > last_sep)) last_sep = last_sep2;
        if (last_sep) filename = last_sep + 1;

        char search_path[MAX_PATH_LEN];

        /* 2a) 当前模块目录/bin/<文件名> */
        char mod_dir[MAX_PATH_LEN];
        get_current_module_dir(mod_dir, sizeof(mod_dir));
        if (mod_dir[0] != '\0') {
            snprintf(search_path, sizeof(search_path), "%sbin%c%s", mod_dir,
#ifdef _WIN32
                     '\\',
#else
                     '/',
#endif
                     filename);
            if (file_exists_utf8(search_path)) {
                handle = load_library_utf8(search_path);
                if (handle) resolved_path = strdup(search_path);
            }
        }

        /* 2b) 当前模块目录/<文件名> */
        if (!handle && mod_dir[0] != '\0') {
            snprintf(search_path, sizeof(search_path), "%s%s", mod_dir, filename);
            if (file_exists_utf8(search_path)) {
                handle = load_library_utf8(search_path);
                if (handle) resolved_path = strdup(search_path);
            }
        }

        /* 2c) 所有已加载模块目录/<文件名>（DLL 与 .leno 同目录，包自包含） */
        if (!handle) {
            extern int loaded_modules_get_count(void);
            extern ObjModule* loaded_modules_get(int index);
            int mod_count = loaded_modules_get_count();
            for (int mi = 0; mi < mod_count && !handle; mi++) {
                ObjModule* m = loaded_modules_get(mi);
                if (!m || !m->source_path) continue;
                char mdir[MAX_PATH_LEN];
                extract_dir(m->source_path, mdir, sizeof(mdir));
                /* 跳过与当前模块目录相同的（2b 已搜索过） */
                if (mod_dir[0] != '\0' && strcmp(mdir, mod_dir) == 0) continue;
                snprintf(search_path, sizeof(search_path), "%s%s", mdir, filename);
                if (file_exists_utf8(search_path)) {
                    handle = load_library_utf8(search_path);
                    if (handle) resolved_path = strdup(search_path);
                }
            }
        }

        /* 2d) exe 所在目录/<文件名> */
        if (!handle) {
            char exe_dir[MAX_PATH_LEN];
            get_exe_dir(exe_dir, sizeof(exe_dir));
            if (exe_dir[0] != '\0') {
                snprintf(search_path, sizeof(search_path), "%s%s", exe_dir, filename);
                if (file_exists_utf8(search_path)) {
                    handle = load_library_utf8(search_path);
                    if (handle) resolved_path = strdup(search_path);
                }
            }
        }

        /* 2e) 系统PATH（交给系统加载器处理，传入纯文件名） */
        if (!handle) {
            handle = load_library_utf8(filename);
            if (handle) resolved_path = strdup(filename);
        }
    }

    if (!handle) {
        free(resolved_path);
        char msg[512];
#ifdef _WIN32
        DWORD error = GetLastError();
        snprintf(msg, sizeof(msg), "加载库 '%s' 失败，错误码: %lu（已搜索: 模块目录/bin、模块目录、已导入模块目录、exe目录、系统PATH）",
                 path_str->chars, error);
#else
        const char* dl_err = dlerror();
        snprintf(msg, sizeof(msg), "加载库 '%s' 失败: %s（已搜索: 模块目录/bin、模块目录、已导入模块目录、exe目录、系统PATH）",
                 path_str->chars, dl_err ? dl_err : "未知错误");
#endif
        native_throw_error(msg);
        return val_null();
    }

    ObjFFILibrary* lib = (ObjFFILibrary*)gc_alloc(sizeof(ObjFFILibrary), OBJ_FFI_LIBRARY);
    if (!lib) {
#ifdef _WIN32
        FreeLibrary((HMODULE)handle);
#else
        dlclose(handle);
#endif
        free(resolved_path);
        native_throw_error("内存不足");
        return val_null();
    }

    lib->path = strdup(resolved_path ? resolved_path : path);
    lib->handle = handle;
    lib->freed = 0;
    free(resolved_path);

    return val_obj((Object*)lib);
}

Value ffi_reload_library(const char* path) {
    void* handle = load_library_utf8(path);
    if (!handle) {
        return val_null();
    }

    ObjFFILibrary* lib = (ObjFFILibrary*)gc_alloc(sizeof(ObjFFILibrary), OBJ_FFI_LIBRARY);
    if (!lib) {
#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        return val_null();
    }

    lib->path = strdup(path);
    lib->handle = handle;
    lib->freed = 0;

    return val_obj((Object*)lib);
}

/* ==================== FFI 函数调用 ==================== */

/* TypeKind → FFIType 映射（用于 clib 调用时推导 libffi 签名） */
static FFIType typekind_to_ffitype(TypeKind tk) {
    switch (tk) {
        case TYPE_NULL:      return FFI_TYPE_VOID;
        case TYPE_BOOL:      return FFI_TYPE_BOOL;
        case TYPE_I8:        return FFI_TYPE_INT8;
        case TYPE_U8:        return FFI_TYPE_UINT8;
        case TYPE_I16:       return FFI_TYPE_INT16;
        case TYPE_U16:       return FFI_TYPE_UINT16;
        case TYPE_I32:       return FFI_TYPE_INT32;
        case TYPE_U32:       return FFI_TYPE_UINT32;
        case TYPE_I64:       return FFI_TYPE_INT;     /* int64_t */
        case TYPE_U64:       return FFI_TYPE_INT;     /* uint64_t 用 int64_t 传递 */
        case TYPE_F32:       return FFI_TYPE_FLOAT;
        case TYPE_F64:       return FFI_TYPE_DOUBLE;
        case TYPE_FLOAT:     return FFI_TYPE_DOUBLE;
        case TYPE_PTR:       return FFI_TYPE_POINTER;
        case TYPE_PTR_GENERIC: return FFI_TYPE_POINTER;
        case TYPE_STR8:      return FFI_TYPE_POINTER;
        case TYPE_STR16:     return FFI_TYPE_POINTER;
        default:             return FFI_TYPE_INT;
    }
}

/* ffi.call 的核心实现 - ret_type 指定返回值类型
 * 所有 ffi.call/ffi.call_double/ffi.call_void/ffi.call_ptr/ffi.call_bool 共用此函数
 * ret_type_kind: TypeKind 枚举值（clib 路径），-1 表示旧路径（使用 ret_ffitype）
 */
/* 前置声明：ffi_pump_callbacks_func 定义在 callback_dispatch 之后 */
static Value ffi_pump_callbacks_func(int argc, Value* args);

/* 前置声明：跨线程回调编组全局变量，定义在 CallbackRegState 之后 */
static int g_callback_marshal_initialized;
static int g_pending_count;

static Value ffi_call_impl(int argc, Value* args, int ret_type_kind, const int* arg_types) {
    ObjFFILibrary* lib = val_as_ffi_lib(args[0]);
    CHECK_LIB_FREED(lib);

    ObjString* name_str = (ObjString*)val_as_obj(args[1]);
    const char* func_name = name_str->chars;

    /* 函数参数从第3个（index=2）开始 */
    int arg_start = 2;

    /* 获取函数地址 */
#ifdef _WIN32
    void* func = (void*)GetProcAddress(lib->handle, func_name);
#else
    void* func = (void*)dlsym(lib->handle, func_name);
#endif
    if (!func) {
        char msg[256];
#ifdef _WIN32
        snprintf(msg, sizeof(msg), "在库 '%s' 中找不到函数 '%s'，错误码: %lu",
                 lib->path ? lib->path : "未知", func_name, GetLastError());
#else
        snprintf(msg, sizeof(msg), "在库 '%s' 中找不到函数 '%s': %s",
                 lib->path ? lib->path : "未知", func_name, dlerror());
#endif
        native_throw_error(msg);
        return val_null();
    }

    /* 推导 FFI 返回类型 */
    FFIType ffi_ret_type = typekind_to_ffitype((TypeKind)ret_type_kind);

    /* 构建 FFI 签名和参数 */
    FFISignature sig;
    sig.ret_type = ffi_ret_type;
    sig.nargs = argc - arg_start;
    if (sig.nargs > FFI_MAX_ARGS) sig.nargs = FFI_MAX_ARGS;

    FFIArg ffi_args[FFI_MAX_ARGS];
    memset(ffi_args, 0, sizeof(ffi_args));

    /* 将 Leno 值转换为 FFI 参数（根据 arg_types 做窄化和类型感知转换） */
    for (int i = 0; i < sig.nargs; i++) {
        Value arg = args[i + arg_start];
        TypeKind param_tk = (arg_types && i < FFI_MAX_ARGS) ? (TypeKind)arg_types[i] : TYPE_UNKNOWN;

        if (val_is_int(arg)) {
            int64_t ival = (int64_t)val_as_int(arg);

            /* 参数窄化：根据 clib 声明的参数类型做范围检查和 FFI 签名设置 */
            switch (param_tk) {
                case TYPE_I8:
                    if (ival < INT8_MIN || ival > INT8_MAX) {
                        char msg[128]; snprintf(msg, sizeof(msg), "参数 %d 值 %lld 超出 i8 范围 [-128, 127]", i+1, (long long)ival);
                        native_throw_error(msg); return val_null();
                    }
                    sig.arg_types[i] = FFI_TYPE_INT8; ffi_args[i].type = FFI_TYPE_INT8;
                    ffi_args[i].value.i = (int64_t)(int8_t)ival;
                    break;
                case TYPE_U8:
                    if (ival < 0 || ival > UINT8_MAX) {
                        char msg[128]; snprintf(msg, sizeof(msg), "参数 %d 值 %lld 超出 u8 范围 [0, 255]", i+1, (long long)ival);
                        native_throw_error(msg); return val_null();
                    }
                    sig.arg_types[i] = FFI_TYPE_UINT8; ffi_args[i].type = FFI_TYPE_UINT8;
                    ffi_args[i].value.i = (int64_t)(uint8_t)ival;
                    break;
                case TYPE_I16:
                    if (ival < INT16_MIN || ival > INT16_MAX) {
                        char msg[128]; snprintf(msg, sizeof(msg), "参数 %d 值 %lld 超出 i16 范围 [-32768, 32767]", i+1, (long long)ival);
                        native_throw_error(msg); return val_null();
                    }
                    sig.arg_types[i] = FFI_TYPE_INT16; ffi_args[i].type = FFI_TYPE_INT16;
                    ffi_args[i].value.i = (int64_t)(int16_t)ival;
                    break;
                case TYPE_U16:
                    if (ival < 0 || ival > UINT16_MAX) {
                        char msg[128]; snprintf(msg, sizeof(msg), "参数 %d 值 %lld 超出 u16 范围 [0, 65535]", i+1, (long long)ival);
                        native_throw_error(msg); return val_null();
                    }
                    sig.arg_types[i] = FFI_TYPE_UINT16; ffi_args[i].type = FFI_TYPE_UINT16;
                    ffi_args[i].value.i = (int64_t)(uint16_t)ival;
                    break;
                case TYPE_I32:
                    if (ival < INT32_MIN || ival > INT32_MAX) {
                        char msg[128]; snprintf(msg, sizeof(msg), "参数 %d 值 %lld 超出 i32 范围", i+1, (long long)ival);
                        native_throw_error(msg); return val_null();
                    }
                    sig.arg_types[i] = FFI_TYPE_INT32; ffi_args[i].type = FFI_TYPE_INT32;
                    ffi_args[i].value.i = (int64_t)(int32_t)ival;
                    break;
                case TYPE_U32:
                    if (ival < 0 || (uint64_t)ival > UINT32_MAX) {
                        char msg[128]; snprintf(msg, sizeof(msg), "参数 %d 值 %lld 超出 u32 范围", i+1, (long long)ival);
                        native_throw_error(msg); return val_null();
                    }
                    sig.arg_types[i] = FFI_TYPE_UINT32; ffi_args[i].type = FFI_TYPE_UINT32;
                    ffi_args[i].value.i = (int64_t)(uint32_t)ival;
                    break;
                case TYPE_I64:
                    sig.arg_types[i] = FFI_TYPE_INT; ffi_args[i].type = FFI_TYPE_INT;
                    ffi_args[i].value.i = ival;
                    break;
                case TYPE_U64:
                    if (ival < 0) {
                        char msg[128]; snprintf(msg, sizeof(msg), "参数 %d 负值 %lld 不能传给 u64", i+1, (long long)ival);
                        native_throw_error(msg); return val_null();
                    }
                    sig.arg_types[i] = FFI_TYPE_INT; ffi_args[i].type = FFI_TYPE_INT;
                    ffi_args[i].value.i = ival;
                    break;
                case TYPE_BOOL:
                    sig.arg_types[i] = FFI_TYPE_INT; ffi_args[i].type = FFI_TYPE_INT;
                    ffi_args[i].value.i = ival ? 1 : 0;
                    break;
                default:
                    /* 旧路径或未知类型，默认 FFI_TYPE_INT */
                    sig.arg_types[i] = FFI_TYPE_INT; ffi_args[i].type = FFI_TYPE_INT;
                    ffi_args[i].value.i = ival;
                    break;
            }
        }
        else if (val_is_bigint(arg)) {
            int64_t ival = bigint_to_int64(val_as_bigint(arg));
            switch (param_tk) {
                case TYPE_I8:
                    if (ival < INT8_MIN || ival > INT8_MAX) {
                        char msg[128]; snprintf(msg, sizeof(msg), "参数 %d 值 %lld 超出 i8 范围", i+1, (long long)ival);
                        native_throw_error(msg); return val_null();
                    }
                    sig.arg_types[i] = FFI_TYPE_INT8; ffi_args[i].type = FFI_TYPE_INT8;
                    ffi_args[i].value.i = (int64_t)(int8_t)ival;
                    break;
                case TYPE_U8:
                    if (ival < 0 || ival > UINT8_MAX) {
                        char msg[128]; snprintf(msg, sizeof(msg), "参数 %d 值 %lld 超出 u8 范围", i+1, (long long)ival);
                        native_throw_error(msg); return val_null();
                    }
                    sig.arg_types[i] = FFI_TYPE_UINT8; ffi_args[i].type = FFI_TYPE_UINT8;
                    ffi_args[i].value.i = (int64_t)(uint8_t)ival;
                    break;
                case TYPE_I16:
                    if (ival < INT16_MIN || ival > INT16_MAX) {
                        char msg[128]; snprintf(msg, sizeof(msg), "参数 %d 值 %lld 超出 i16 范围", i+1, (long long)ival);
                        native_throw_error(msg); return val_null();
                    }
                    sig.arg_types[i] = FFI_TYPE_INT16; ffi_args[i].type = FFI_TYPE_INT16;
                    ffi_args[i].value.i = (int64_t)(int16_t)ival;
                    break;
                case TYPE_U16:
                    if (ival < 0 || ival > UINT16_MAX) {
                        char msg[128]; snprintf(msg, sizeof(msg), "参数 %d 值 %lld 超出 u16 范围", i+1, (long long)ival);
                        native_throw_error(msg); return val_null();
                    }
                    sig.arg_types[i] = FFI_TYPE_UINT16; ffi_args[i].type = FFI_TYPE_UINT16;
                    ffi_args[i].value.i = (int64_t)(uint16_t)ival;
                    break;
                case TYPE_I32:
                    if (ival < INT32_MIN || ival > INT32_MAX) {
                        char msg[128]; snprintf(msg, sizeof(msg), "参数 %d 值 %lld 超出 i32 范围", i+1, (long long)ival);
                        native_throw_error(msg); return val_null();
                    }
                    sig.arg_types[i] = FFI_TYPE_INT32; ffi_args[i].type = FFI_TYPE_INT32;
                    ffi_args[i].value.i = (int64_t)(int32_t)ival;
                    break;
                case TYPE_U32:
                    if (ival < 0 || ival > UINT32_MAX) {
                        char msg[128]; snprintf(msg, sizeof(msg), "参数 %d 值 %lld 超出 u32 范围", i+1, (long long)ival);
                        native_throw_error(msg); return val_null();
                    }
                    sig.arg_types[i] = FFI_TYPE_UINT32; ffi_args[i].type = FFI_TYPE_UINT32;
                    ffi_args[i].value.i = (int64_t)(uint32_t)ival;
                    break;
                case TYPE_I64:
                case TYPE_U64:
                    sig.arg_types[i] = FFI_TYPE_INT; ffi_args[i].type = FFI_TYPE_INT;
                    ffi_args[i].value.i = ival;
                    break;
                default:
                    sig.arg_types[i] = FFI_TYPE_INT; ffi_args[i].type = FFI_TYPE_INT;
                    ffi_args[i].value.i = ival;
                    break;
            }
        }
        else if (val_is_float(arg)) {
            double dval = val_as_num(arg);
            switch (param_tk) {
        case TYPE_F32:
            sig.arg_types[i] = FFI_TYPE_FLOAT; ffi_args[i].type = FFI_TYPE_FLOAT;
            ffi_args[i].value.f = (float)dval;
            break;
                default:
                    sig.arg_types[i] = FFI_TYPE_DOUBLE; ffi_args[i].type = FFI_TYPE_DOUBLE;
                    ffi_args[i].value.d = dval;
                    break;
            }
        }
        else if (val_is_string(arg)) {
            sig.arg_types[i] = FFI_TYPE_POINTER;
            ffi_args[i].type = FFI_TYPE_POINTER;
            /* clib 声明为 str16 时，自动将 UTF-8 字符串转为 UTF-16 */
            if (arg_types && arg_types[i] == TYPE_STR16) {
#ifdef _WIN32
                wchar_t* wstr = utf8_to_utf16(((ObjString*)val_as_obj(arg))->chars);
                if (wstr) {
                    ffi_args[i].value.p = wstr;
                    ffi_args[i].owned = 1;  /* 标记需要释放 */
                } else {
                    ffi_args[i].value.p = NULL;
                    ffi_args[i].owned = 0;
                }
#else
                /* Linux/macOS: str16 按 UTF-8 传递 */
                ffi_args[i].value.p = ((ObjString*)val_as_obj(arg))->chars;
                ffi_args[i].owned = 0;
#endif
            } else {
                ffi_args[i].value.p = ((ObjString*)val_as_obj(arg))->chars;
                ffi_args[i].owned = 0;
            }
        }
        else if (val_is_ffi_ptr(arg)) {
            ObjFFIPointer* p = val_as_ffi_ptr(arg);
            sig.arg_types[i] = FFI_TYPE_POINTER;
            ffi_args[i].type = FFI_TYPE_POINTER;
            ffi_args[i].value.p = p->freed ? NULL : p->ptr;
        }
        else if (val_is_obj(arg) && val_as_obj(arg)->type == OBJ_FFI_CALLBACK) {
            ObjFFICallback* cb = (ObjFFICallback*)val_as_obj(arg);
            sig.arg_types[i] = FFI_TYPE_POINTER;
            ffi_args[i].type = FFI_TYPE_POINTER;
            ffi_args[i].value.p = cb->trampoline;
        }
        /* 自动转换 cstruct 实例为指针 */
        else if (val_is_cstruct(arg)) {
            ObjCStruct* cstruct = val_as_cstruct(arg);
            sig.arg_types[i] = FFI_TYPE_POINTER;
            ffi_args[i].type = FFI_TYPE_POINTER;
            ffi_args[i].value.p = (cstruct && cstruct->data) ? cstruct->data : NULL;
        }
        else if (val_is_null(arg)) {
            sig.arg_types[i] = FFI_TYPE_POINTER;
            ffi_args[i].type = FFI_TYPE_POINTER;
            ffi_args[i].value.p = NULL;
        }
        else if (val_is_bool(arg)) {
            sig.arg_types[i] = FFI_TYPE_INT;
            ffi_args[i].type = FFI_TYPE_INT;
            ffi_args[i].value.i = val_is_truthy(arg) ? 1 : 0;
        }
        else {
            sig.arg_types[i] = FFI_TYPE_INT;
            ffi_args[i].type = FFI_TYPE_INT;
            ffi_args[i].value.i = 0;
        }
    }

    /* 调用函数 */
    FFIValue result = ffi_call(func, &sig, ffi_args);

    /* 立即缓存错误码（在 Leno 内部操作覆盖之前） */
#ifdef _WIN32
    g_last_error = (int64_t)GetLastError();
#else
    g_last_error = (int64_t)errno;
#endif

    /* 释放 str16 自动转换分配的临时 UTF-16 内存 */
    for (int i = 0; i < sig.nargs; i++) {
        if (ffi_args[i].owned && ffi_args[i].value.p) {
            free(ffi_args[i].value.p);
            ffi_args[i].value.p = NULL;
            ffi_args[i].owned = 0;
        }
    }

    /* 自动泵送跨线程回调：每次 clib 调用后检查是否有待处理的回调，
     * 如有则立即在主线程 VM 上执行。零开销（仅 int 比较）当无待处理回调时。
     * 这样用户不需要在事件循环中手动调用 ffi.pump_callbacks()。 */
    if (g_callback_marshal_initialized && g_pending_count > 0) {
        Value dummy;
        ffi_pump_callbacks_func(0, &dummy);
    }

    /* 根据返回类型自动展开（C → Leno 零摩擦） */
    if (ret_type_kind >= 0) {
        /* clib 路径：根据 TypeKind 做精确转换 */
        switch ((TypeKind)ret_type_kind) {
            case TYPE_NULL:   return val_null();  /* void */
            case TYPE_BOOL:   return val_bool(result.i != 0);
            case TYPE_I8:     return val_int_safe((int64_t)(int8_t)result.i);
            case TYPE_U8:     return val_int_safe((int64_t)(uint8_t)result.i);
            case TYPE_I16:    return val_int_safe((int64_t)(int16_t)result.i);
            case TYPE_U16:    return val_int_safe((int64_t)(uint16_t)result.i);
            case TYPE_I32:    return val_int_safe((int64_t)(int32_t)result.i);
            case TYPE_U32:    return val_int_safe((int64_t)(uint32_t)result.i);
            case TYPE_I64:    return val_int_safe(result.i);
            case TYPE_U64: {
                uint64_t uval = (uint64_t)result.i;
                if (uval <= INT64_MAX) return val_int_safe((int64_t)uval);
                return val_bigint_from_uint64(uval);
            }
            case TYPE_F32:    return val_float((double)result.f);
            case TYPE_F64:
            case TYPE_FLOAT:  return val_float(result.d);
            case TYPE_PTR:
            case TYPE_PTR_GENERIC: {
                if (result.p == NULL) return val_null();
                ObjFFIPointer* ret_ptr = (ObjFFIPointer*)gc_alloc(sizeof(ObjFFIPointer), OBJ_FFI_POINTER);
                if (!ret_ptr) { native_throw_error("内存不足"); return val_null(); }
                ret_ptr->ptr = result.p; ret_ptr->size = 0; ret_ptr->owned = 0; ret_ptr->freed = 0;
                ret_ptr->element_type = TYPE_PTR;
                return val_obj((Object*)ret_ptr);
            }
            case TYPE_STR8: {
                /* char* → string 自动转换 */
                if (result.p == NULL) return val_null();
                return val_obj((Object*)str_copy((const char*)result.p, (int)strlen((const char*)result.p)));
            }
            case TYPE_STR16: {
                /* wchar_t* → string 自动转换（UTF-16 → UTF-8） */
                if (result.p == NULL) return val_null();
#ifdef _WIN32
                char* utf8 = utf16_to_utf8((const wchar_t*)result.p);
                if (utf8) {
                    Value ret = val_obj((Object*)str_copy(utf8, (int)strlen(utf8)));
                    free(utf8);
                    return ret;
                }
#endif
                return val_null();
            }
            default:
                return val_int_safe(result.i);
        }
    }

    /* 不会到达这里：ret_type_kind 现在总是 >= 0 */
    return val_int_safe(result.i);
}

/* ffi.call(lib, name, ...) - 调用库中的函数（返回 int） */
static Value ffi_call_func(int argc, Value* args) {
    return ffi_call_impl(argc, args, TYPE_I32, NULL);
}

/* ffi.call_int(lib, name, ...) - 调用库中的函数（明确返回 int） */
static Value ffi_call_int_func(int argc, Value* args) {
    return ffi_call_impl(argc, args, TYPE_I32, NULL);
}

/* ffi.call_double(lib, name, ...) - 调用库中的函数（返回 double） */
static Value ffi_call_double_func(int argc, Value* args) {
    return ffi_call_impl(argc, args, TYPE_F64, NULL);
}

/* ffi.call_void(lib, name, ...) - 调用库中的函数（无返回值） */
static Value ffi_call_void_func(int argc, Value* args) {
    return ffi_call_impl(argc, args, TYPE_NULL, NULL);
}

/* ffi.call_ptr(lib, name, ...) - 调用库中的函数（返回指针） */
static Value ffi_call_ptr_func(int argc, Value* args) {
    return ffi_call_impl(argc, args, TYPE_PTR, NULL);
}

/* ffi.call_bool(lib, name, ...) - 调用库中的函数（返回布尔值） */
static Value ffi_call_bool_func(int argc, Value* args) {
    return ffi_call_impl(argc, args, TYPE_BOOL, NULL);
}

/* ffi.last_error() - 获取上次 FFI 调用后的错误码
 * Windows: 返回 GetLastError() 的缓存值
 * Linux:   返回 errno 的缓存值
 * 在每次 ffi.call_xxx 或 clib 调用后立即缓存，避免 Leno 内部操作覆盖
 */
static Value ffi_last_error_func(int argc, Value* args) {
    (void)argc;
    (void)args;
    return val_int_safe(g_last_error);
}

/* ==================== FFI 内存操作函数 ==================== */

/* ffi.malloc(size) - 分配指定大小的内存 */
static Value ffi_malloc_func(int argc, Value* args) {
    (void)argc;
    size_t size = (size_t)val_as_int(args[0]);

    // 多分配 8 字节放哨兵值，用于 free 时检测堆溢出
    void* ptr = malloc(size + 8);
    if (!ptr) {
        native_throw_error("内存不足");
        return val_null();
    }
    memset(ptr, 0, size);
    // 尾部哨兵: 0xDEADBEEFDEADBEEF，被覆盖说明溢出
    *(uint64_t*)((char*)ptr + size) = 0xDEADBEEFDEADBEEFULL;

    ObjFFIPointer* ffi_ptr = (ObjFFIPointer*)gc_alloc(sizeof(ObjFFIPointer), OBJ_FFI_POINTER);
    if (!ffi_ptr) {
        free(ptr);
        native_throw_error("内存不足");
        return val_null();
    }

    ffi_ptr->ptr = ptr;
    ffi_ptr->size = size;
    ffi_ptr->owned = 1;
    ffi_ptr->freed = 0;
    ffi_ptr->element_type = TYPE_PTR;

    return val_obj((Object*)ffi_ptr);
}

/* ffi.calloc(count, size) - 分配并清零内存 */
static Value ffi_calloc_func(int argc, Value* args) {
    (void)argc;
    size_t count = (size_t)val_as_int(args[0]);
    size_t size  = (size_t)val_as_int(args[1]);
    size_t total  = count * size;

    void* ptr = malloc(total + 8);
    if (!ptr) {
        native_throw_error("内存不足");
        return val_null();
    }
    memset(ptr, 0, total);
    *(uint64_t*)((char*)ptr + total) = 0xDEADBEEFDEADBEEFULL;

    ObjFFIPointer* ffi_ptr = (ObjFFIPointer*)gc_alloc(sizeof(ObjFFIPointer), OBJ_FFI_POINTER);
    if (!ffi_ptr) {
        free(ptr);
        native_throw_error("内存不足");
        return val_null();
    }

    ffi_ptr->ptr = ptr;
    ffi_ptr->size = total;
    ffi_ptr->owned = 1;
    ffi_ptr->freed = 0;
    ffi_ptr->element_type = TYPE_PTR;

    return val_obj((Object*)ffi_ptr);
}

/* ffi.realloc(ptr, new_size) - 重新分配内存 */
static Value ffi_realloc_func(int argc, Value* args) {
    (void)argc;
    ObjFFIPointer* ffi_ptr = val_as_ffi_ptr(args[0]);

    if (ffi_ptr->freed) {
        native_throw_error("指针已释放");
        return val_null();
    }

    size_t new_size = (size_t)val_as_int(args[1]);

    void* new_ptr = realloc(ffi_ptr->ptr, new_size + 8);
    if (!new_ptr && new_size > 0) {
        native_throw_error("内存不足");
        return val_null();
    }

    ffi_ptr->ptr = new_ptr;
    ffi_ptr->size = new_size;
    // 重置哨兵
    *(uint64_t*)((char*)new_ptr + new_size) = 0xDEADBEEFDEADBEEFULL;

    return val_obj((Object*)ffi_ptr);
}

/* ffi.free(x) - 统一释放 FFI 资源（指针/库/回调） */
static Value ffi_free_func(int argc, Value* args) {
    (void)argc;
    if (!val_is_obj(args[0])) return val_null();

    Object* obj = val_as_obj(args[0]);
    switch (obj->type) {
    case OBJ_FFI_POINTER: {
        ObjFFIPointer* ptr = (ObjFFIPointer*)obj;
        if (ptr->freed) return val_null();
        if (ptr->owned && ptr->ptr) {
            // 检查尾部哨兵，检测堆溢出
            if (ptr->size > 0) {
                uint64_t sentinel = *(uint64_t*)((char*)ptr->ptr + ptr->size);
                if (sentinel != 0xDEADBEEFDEADBEEFULL) {
                    native_throw_error("堆缓冲区溢出！尾部哨兵被破坏。请用 ffi.assert_size() 检查缓冲区大小");
                    ptr->freed = 1;  // 标记已释放，避免重复检测
                    return val_null();
                }
            }
            free(ptr->ptr);
            ptr->ptr = NULL;
            ptr->size = 0;
            ptr->owned = 0;
            ptr->freed = 1;
        }
        break;
    }
    case OBJ_FFI_LIBRARY: {
        ObjFFILibrary* lib = (ObjFFILibrary*)obj;
        if (lib->freed) return val_null();
        if (lib->handle) {
#ifdef _WIN32
            FreeLibrary((HMODULE)lib->handle);
#else
            dlclose(lib->handle);
#endif
            lib->handle = NULL;
        }
        if (lib->path) {
            free(lib->path);
            lib->path = NULL;
        }
        lib->freed = 1;
        break;
    }
    case OBJ_FFI_CALLBACK: {
        ObjFFICallback* cb = (ObjFFICallback*)obj;
        if (cb->callback_id >= 0 && cb->callback_id < MAX_FFI_CALLBACKS) {
            g_callback_registry[cb->callback_id].active = 0;
        }
        if (cb->trampoline) {
            free_executable_memory(cb->trampoline, 256);
            cb->trampoline = NULL;
        }
        if (cb->sig) {
            free(cb->sig);
            cb->sig = NULL;
        }
        cb->callback_id = -1;
        break;
    }
    default:
        break;
    }

    return val_null();
}

/* ffi.sizeof(ptr) - 获取指针指向的内存大小 */
static Value ffi_sizeof_func(int argc, Value* args) {
    (void)argc;
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
    return val_int((int)ptr->size);
}

/* ffi.assert_size(ptr, min_bytes) - 运行时断言缓冲区足够大
 * 如果 ptr 大小未知(size=0)或不足 min_bytes，抛出明确错误。
 * 用于在调用 clib 函数前验证输出缓冲区，避免堆溢出后崩溃。
 */
static Value ffi_assert_size_func(int argc, Value* args) {
    (void)argc;
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
    size_t min_bytes = (size_t)val_as_int(args[1]);

    if (ptr->size == 0) {
        native_throw_error("缓冲区大小未知（非 ffi.malloc/calloc 分配），无法验证");
        return val_null();
    }
    if (ptr->size < min_bytes) {
        char msg[128];
        snprintf(msg, sizeof(msg), "缓冲区溢出！需要 %zu 字节，实际只有 %zu 字节", min_bytes, ptr->size);
        native_throw_error(msg);
        return val_null();
    }
    return val_bool(1);
}

/* ffi.nullptr() - 返回一个空指针值（可用于传参） */
static Value ffi_nullptr_func(int argc, Value* args) {
    (void)argc; (void)args;
    ObjFFIPointer* ffi_ptr = (ObjFFIPointer*)gc_alloc(sizeof(ObjFFIPointer), OBJ_FFI_POINTER);
    if (!ffi_ptr) {
        native_throw_error("内存不足");
        return val_null();
    }
    ffi_ptr->ptr = NULL;
    ffi_ptr->size = 0;
    ffi_ptr->owned = 0;
    ffi_ptr->freed = 0;
    ffi_ptr->element_type = TYPE_PTR;
    return val_obj((Object*)ffi_ptr);
}

/* ffi.ptr_from_int(address) - 从整数地址创建一个指针对象（用于传递 HANDLE/HKEY 等） */
static Value ffi_ptr_from_int_func(int argc, Value* args) {
    (void)argc;
    // 支持 int 和 float 类型的整数值（如 0x80000000）
    int64_t addr;
    if (val_is_int(args[0])) {
        addr = (int64_t)val_as_int(args[0]);
    } else if (val_is_float(args[0])) {
        addr = (int64_t)val_as_num(args[0]);
    } else if (val_is_bigint(args[0])) {
        addr = bigint_to_int64(val_as_bigint(args[0]));
    } else {
        native_throw_error("ptr_from_int 参数必须是数字类型");
        return val_null();
    }
    ObjFFIPointer* ffi_ptr = (ObjFFIPointer*)gc_alloc(sizeof(ObjFFIPointer), OBJ_FFI_POINTER);
    if (!ffi_ptr) {
        native_throw_error("内存不足");
        return val_null();
    }
    ffi_ptr->ptr = (void*)addr;
    ffi_ptr->size = 0;
    ffi_ptr->owned = 0;
    ffi_ptr->freed = 0;
    ffi_ptr->element_type = TYPE_PTR;
    return val_obj((Object*)ffi_ptr);
}

/* ffi.ptr_to_int(ptr) - 将 FFI 指针的地址值转换为整数（用于比较句柄、调试打印等） */
static Value ffi_ptr_to_int_func(int argc, Value* args) {
    (void)argc;
    if (val_is_ffi_ptr(args[0])) {
        ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
        if (ptr->freed) {
            native_throw_error("指针已释放");
            return val_null();
        }
        int64_t addr = (int64_t)(intptr_t)ptr->ptr;
        return val_int_safe(addr);
    } else if (val_is_obj(args[0]) && val_as_obj(args[0])->type == OBJ_FFI_CALLBACK) {
        ObjFFICallback* cb = (ObjFFICallback*)val_as_obj(args[0]);
        int64_t addr = (int64_t)(intptr_t)cb->trampoline;
        return val_int_safe(addr);
    }
    native_throw_error("ptr_to_int 参数必须是 FFI 指针或回调对象");
    return val_null();
}

/* ffi.is_ptr(value) - 检查值是否是 FFI 指针或回调对象
 * 返回: bool - true 如果是 FFIPointer 或 FFICallback 对象，否则 false
 */
static Value ffi_is_ptr_func(int argc, Value* args) {
    (void)argc;
    if (val_is_obj(args[0])) {
        ObjType t = val_as_obj(args[0])->type;
        return val_bool(t == OBJ_FFI_POINTER || t == OBJ_FFI_CALLBACK);
    }
    return val_bool(false);
}

/* ffi.offset(ptr, offset) - 指针偏移，返回新的指针对象
 * 新指针与原指针共享底层内存（非拥有）
 */
static Value ffi_offset_func(int argc, Value* args) {
    (void)argc;
    ObjFFIPointer* src = val_as_ffi_ptr(args[0]);

    if (src->freed || !src->ptr) {
        native_throw_error("空指针引用或指针已释放");
        return val_null();
    }

    int offset = val_as_int(args[1]);

    /* 边界检查 */
    if (src->owned && src->size > 0) {
        if ((size_t)offset >= src->size) {
            native_throw_error("指针偏移越界");
            return val_null();
        }
    }

    ObjFFIPointer* new_ptr = (ObjFFIPointer*)gc_alloc(sizeof(ObjFFIPointer), OBJ_FFI_POINTER);
    if (!new_ptr) {
        native_throw_error("内存不足");
        return val_null();
    }

    new_ptr->ptr = (char*)src->ptr + offset;
    new_ptr->size = (src->size > (size_t)offset) ? (src->size - offset) : 0;
    new_ptr->owned = 0;  /* 非拥有，由原指针管理生命周期 */
    new_ptr->freed = 0;

    return val_obj((Object*)new_ptr);
}

/* ==================== FFI 读取函数 ==================== */

/* ffi.read_byte(ptr, off) - 读取 uint8 */
static Value ffi_read_byte_func(int argc, Value* args) {
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
    int offset = parse_offset(argc, args, 1);
    CHECK_NULL_PTR(ptr);
    CHECK_BOUNDS(ptr, offset, sizeof(uint8_t));
    return val_int((int)*((uint8_t*)((char*)ptr->ptr + offset)));
}

/* ffi.read_int8(ptr, off) - 读取 int8 */
static Value ffi_read_int8_func(int argc, Value* args) {
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
    int offset = parse_offset(argc, args, 1);
    CHECK_NULL_PTR(ptr);
    CHECK_BOUNDS(ptr, offset, sizeof(int8_t));
    int8_t val;
    memcpy(&val, (char*)ptr->ptr + offset, sizeof(val));
    return val_int((int)val);
}

/* ffi.read_int16(ptr, off) - 读取 int16 */
static Value ffi_read_int16_func(int argc, Value* args) {
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
    int offset = parse_offset(argc, args, 1);
    CHECK_NULL_PTR(ptr);
    CHECK_BOUNDS(ptr, offset, sizeof(int16_t));
    int16_t val;
    memcpy(&val, (char*)ptr->ptr + offset, sizeof(val));
    return val_int((int)val);
}

/* ffi.read_uint16(ptr, off) - 读取 uint16 */
static Value ffi_read_uint16_func(int argc, Value* args) {
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
    int offset = parse_offset(argc, args, 1);
    CHECK_NULL_PTR(ptr);
    CHECK_BOUNDS(ptr, offset, sizeof(uint16_t));
    uint16_t val;
    memcpy(&val, (char*)ptr->ptr + offset, sizeof(val));
    return val_int((int)val);
}

/* ffi.read_int(ptr, off) - 读取 int32 */
static Value ffi_read_int_func(int argc, Value* args) {
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
    int offset = parse_offset(argc, args, 1);
    CHECK_NULL_PTR(ptr);
    CHECK_BOUNDS(ptr, offset, sizeof(int32_t));
    int32_t val;
    memcpy(&val, (char*)ptr->ptr + offset, sizeof(val));
    return val_int((int)val);
}

/* ffi.read_uint(ptr, off) - 读取 uint32
 * 使用 int64_t 保留无符号值（0x80000000 等值不会被解释为负数）
 */
static Value ffi_read_uint_func(int argc, Value* args) {
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
    int offset = parse_offset(argc, args, 1);
    CHECK_NULL_PTR(ptr);
    CHECK_BOUNDS(ptr, offset, sizeof(uint32_t));
    uint32_t val;
    memcpy(&val, (char*)ptr->ptr + offset, sizeof(val));
    return val_int_safe((int64_t)val);
}

/* ffi.read_int64(ptr, off) - 读取 int64
 * 使用 val_int_safe 支持完整的 64 位范围
 */
static Value ffi_read_int64_func(int argc, Value* args) {
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
    int offset = parse_offset(argc, args, 1);
    CHECK_NULL_PTR(ptr);
    CHECK_BOUNDS(ptr, offset, sizeof(int64_t));
    int64_t val;
    memcpy(&val, (char*)ptr->ptr + offset, sizeof(val));
    return val_int_safe(val);
}

/* ffi.read_uint64(ptr, off) - 读取 uint64
 * 使用 val_bigint_from_uint64 保留无符号值（0x8000000000000000 等值显示为正数）
 */
static Value ffi_read_uint64_func(int argc, Value* args) {
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
    int offset = parse_offset(argc, args, 1);
    CHECK_NULL_PTR(ptr);
    CHECK_BOUNDS(ptr, offset, sizeof(uint64_t));
    uint64_t val;
    memcpy(&val, (char*)ptr->ptr + offset, sizeof(val));
    if (val <= INT32_MAX) {
        return val_int((int)val);
    }
    return val_bigint_from_uint64(val);
}

/* ffi.read_float(ptr, off) - 读取 float */
static Value ffi_read_float_func(int argc, Value* args) {
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
    int offset = parse_offset(argc, args, 1);
    CHECK_NULL_PTR(ptr);
    CHECK_BOUNDS(ptr, offset, sizeof(float));
    float val;
    memcpy(&val, (char*)ptr->ptr + offset, sizeof(val));
    return val_float((double)val);
}

/* ffi.read_double(ptr, off) - 读取 double */
static Value ffi_read_double_func(int argc, Value* args) {
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
    int offset = parse_offset(argc, args, 1);
    CHECK_NULL_PTR(ptr);
    CHECK_BOUNDS(ptr, offset, sizeof(double));
    double val;
    memcpy(&val, (char*)ptr->ptr + offset, sizeof(val));
    return val_float(val);
}

/* ffi.read_ptr(ptr, off) - 读取指针值 */
static Value ffi_read_ptr_func(int argc, Value* args) {
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
    int offset = parse_offset(argc, args, 1);
    CHECK_NULL_PTR(ptr);
    CHECK_BOUNDS(ptr, offset, sizeof(void*));
    void* val;
    memcpy(&val, (char*)ptr->ptr + offset, sizeof(val));
    if (val == NULL) return val_null();
    ObjFFIPointer* ret_ptr = (ObjFFIPointer*)gc_alloc(sizeof(ObjFFIPointer), OBJ_FFI_POINTER);
    if (!ret_ptr) {
        native_throw_error("内存不足");
        return val_null();
    }
    ret_ptr->ptr = val;
    ret_ptr->size = 0;
    ret_ptr->owned = 0;
    ret_ptr->freed = 0;
    return val_obj((Object*)ret_ptr);
}

/* ffi.read_bool(ptr, off) - 读取布尔值 */
static Value ffi_read_bool_func(int argc, Value* args) {
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
    int offset = parse_offset(argc, args, 1);
    CHECK_NULL_PTR(ptr);
    CHECK_BOUNDS(ptr, offset, sizeof(int));
    int val;
    memcpy(&val, (char*)ptr->ptr + offset, sizeof(val));
    return val_bool(val != 0);
}

/* ffi.read_at(ptr, index) - 根据元素类型按索引读取（Ptr[T] 专用）
 * byte_offset = index * sizeof(T)
 * 要求 ptr 必须有 element_type（即 Ptr[T] 而非裸 Ptr）
 */
static Value ffi_read_at_func(int argc, Value* args) {
    (void)argc;
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
    int index = val_as_int(args[1]);
    CHECK_NULL_PTR(ptr);

    TypeKind et = ptr->element_type;
    if (et == TYPE_PTR || et == TYPE_ANY || et == TYPE_UNKNOWN) {
        native_throw_error("read_at 需要 Ptr[T] 类型指针（如 Ptr[u32]），不支持无类型 Ptr");
        return val_null();
    }

    /* 根据元素类型计算大小和偏移 */
    size_t elem_size = 0;
    switch (et) {
        case TYPE_U8:    case TYPE_I8:    case TYPE_BOOL:   elem_size = 1; break;
        case TYPE_U16:   case TYPE_I16:                    elem_size = 2; break;
        case TYPE_U32:   case TYPE_I32:   case TYPE_INT:   elem_size = 4; break;
        case TYPE_U64:   case TYPE_I64:                    elem_size = 8; break;
        case TYPE_F32:                                     elem_size = 4; break;
        case TYPE_F64:   case TYPE_FLOAT:                  elem_size = 8; break;
        case TYPE_PTR_GENERIC: case TYPE_PTR: case TYPE_STR8: case TYPE_STR16:
            elem_size = sizeof(void*); break;
        default:
            native_throw_error("read_at 不支持的元素类型");
            return val_null();
    }

    int offset = (int)(index * elem_size);
    CHECK_BOUNDS(ptr, offset, elem_size);

    /* 根据元素类型读取 */
    char* addr = (char*)ptr->ptr + offset;
    switch (et) {
        case TYPE_U8: {
            uint8_t val; memcpy(&val, addr, sizeof(val));
            return val_int((int)val);
        }
        case TYPE_I8: {
            int8_t val; memcpy(&val, addr, sizeof(val));
            return val_int((int)val);
        }
        case TYPE_U16: {
            uint16_t val; memcpy(&val, addr, sizeof(val));
            return val_int((int)val);
        }
        case TYPE_I16: {
            int16_t val; memcpy(&val, addr, sizeof(val));
            return val_int((int)val);
        }
        case TYPE_U32: {
            uint32_t val; memcpy(&val, addr, sizeof(val));
            return val_int_safe((int64_t)val);
        }
        case TYPE_I32: case TYPE_INT: {
            int32_t val; memcpy(&val, addr, sizeof(val));
            return val_int((int)val);
        }
        case TYPE_U64: {
            uint64_t val; memcpy(&val, addr, sizeof(val));
            if (val <= INT32_MAX) return val_int((int)val);
            return val_bigint_from_uint64(val);
        }
        case TYPE_I64: {
            int64_t val; memcpy(&val, addr, sizeof(val));
            return val_int_safe(val);
        }
        case TYPE_F32: {
            float val; memcpy(&val, addr, sizeof(val));
            return val_float((double)val);
        }
        case TYPE_F64: case TYPE_FLOAT: {
            double val; memcpy(&val, addr, sizeof(val));
            return val_float(val);
        }
        case TYPE_BOOL: {
            int val; memcpy(&val, addr, sizeof(int));
            return val_bool(val != 0);
        }
        case TYPE_PTR_GENERIC: case TYPE_PTR: case TYPE_STR8: case TYPE_STR16: {
            void* val; memcpy(&val, addr, sizeof(void*));
            if (val == NULL) return val_null();
            ObjFFIPointer* ret_ptr = (ObjFFIPointer*)gc_alloc(sizeof(ObjFFIPointer), OBJ_FFI_POINTER);
            if (!ret_ptr) { native_throw_error("内存不足"); return val_null(); }
            ret_ptr->ptr = val;
            ret_ptr->size = 0;
            ret_ptr->owned = 0;
            ret_ptr->freed = 0;
            return val_obj((Object*)ret_ptr);
        }
        default:
            native_throw_error("read_at 不支持的元素类型");
            return val_null();
    }
}

/* ffi.write_at(ptr, index, value) - 根据元素类型按索引写入（Ptr[T] 专用）
 * byte_offset = index * sizeof(T)
 * 要求 ptr 必须有 element_type（即 Ptr[T] 而非裸 Ptr）
 */
static Value ffi_write_at_func(int argc, Value* args) {
    (void)argc;
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
    int index = val_as_int(args[1]);
    CHECK_NULL_PTR(ptr);

    TypeKind et = ptr->element_type;
    if (et == TYPE_PTR || et == TYPE_ANY || et == TYPE_UNKNOWN) {
        native_throw_error("write_at 需要 Ptr[T] 类型指针（如 Ptr[u32]），不支持无类型 Ptr");
        return val_null();
    }

    /* 根据元素类型计算大小和偏移 */
    size_t elem_size = 0;
    switch (et) {
        case TYPE_U8:    case TYPE_I8:    case TYPE_BOOL:   elem_size = 1; break;
        case TYPE_U16:   case TYPE_I16:                    elem_size = 2; break;
        case TYPE_U32:   case TYPE_I32:   case TYPE_INT:   elem_size = 4; break;
        case TYPE_U64:   case TYPE_I64:                    elem_size = 8; break;
        case TYPE_F32:                                     elem_size = 4; break;
        case TYPE_F64:   case TYPE_FLOAT:                  elem_size = 8; break;
        case TYPE_PTR_GENERIC: case TYPE_PTR: case TYPE_STR8: case TYPE_STR16:
            elem_size = sizeof(void*); break;
        default:
            native_throw_error("write_at 不支持的元素类型");
            return val_null();
    }

    int offset = (int)(index * elem_size);
    CHECK_BOUNDS(ptr, offset, elem_size);

    /* 根据元素类型写入 */
    char* addr = (char*)ptr->ptr + offset;
    switch (et) {
        case TYPE_U8: {
            uint8_t val = (uint8_t)val_as_int(args[2]);
            memcpy(addr, &val, sizeof(val)); break;
        }
        case TYPE_I8: {
            int8_t val = (int8_t)val_as_int(args[2]);
            memcpy(addr, &val, sizeof(val)); break;
        }
        case TYPE_U16: {
            uint16_t val = (uint16_t)val_as_int(args[2]);
            memcpy(addr, &val, sizeof(val)); break;
        }
        case TYPE_I16: {
            int16_t val = (int16_t)val_as_int(args[2]);
            memcpy(addr, &val, sizeof(val)); break;
        }
        case TYPE_U32: {
            uint32_t val;
            if (val_is_int(args[2])) val = (uint32_t)(uint64_t)val_as_int(args[2]);
            else if (val_is_bigint(args[2])) {
                ObjBigInt* bi = val_as_bigint(args[2]);
                uint64_t raw = 0;
                if (bi->limb_count >= 1) raw = (uint64_t)bi->limbs[0];
                if (bi->limb_count >= 2) raw |= (uint64_t)bi->limbs[1] << 32;
                val = (uint32_t)raw;
            } else { native_throw_error("write_at: 需要 int 或 bigint"); return val_null(); }
            memcpy(addr, &val, sizeof(val)); break;
        }
        case TYPE_I32: case TYPE_INT: {
            int32_t val = (int32_t)val_as_int(args[2]);
            memcpy(addr, &val, sizeof(val)); break;
        }
        case TYPE_U64: {
            uint64_t val;
            if (val_is_int(args[2])) val = (uint64_t)val_as_int(args[2]);
            else if (val_is_bigint(args[2])) val = (uint64_t)bigint_to_int64(val_as_bigint(args[2]));
            else { native_throw_error("write_at: 需要 int 或 bigint"); return val_null(); }
            memcpy(addr, &val, sizeof(val)); break;
        }
        case TYPE_I64: {
            int64_t val;
            if (val_is_int(args[2])) val = (int64_t)val_as_int(args[2]);
            else if (val_is_bigint(args[2])) val = bigint_to_int64(val_as_bigint(args[2]));
            else { native_throw_error("write_at: 需要 int 或 bigint"); return val_null(); }
            memcpy(addr, &val, sizeof(val)); break;
        }
        case TYPE_F32: {
            float val = (float)val_as_num(args[2]);
            memcpy(addr, &val, sizeof(val)); break;
        }
        case TYPE_F64: case TYPE_FLOAT: {
            double val = val_as_num(args[2]);
            memcpy(addr, &val, sizeof(val)); break;
        }
        case TYPE_BOOL: {
            int val = val_is_truthy(args[2]) ? 1 : 0;
            memcpy(addr, &val, sizeof(int)); break;
        }
        case TYPE_PTR_GENERIC: case TYPE_PTR: case TYPE_STR8: case TYPE_STR16: {
            void* val = NULL;
            if (val_is_ffi_ptr(args[2])) {
                ObjFFIPointer* p = val_as_ffi_ptr(args[2]);
                val = p->freed ? NULL : p->ptr;
            } else if (val_is_null(args[2])) {
                val = NULL;
            }
            memcpy(addr, &val, sizeof(void*)); break;
        }
        default:
            native_throw_error("write_at 不支持的元素类型");
            return val_null();
    }
    return val_null();
}

/* ffi.read_string(ptr, off) - 读取以 null 结尾的字符串 */
static Value ffi_read_string_func(int argc, Value* args) {
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
    int offset = parse_offset(argc, args, 1);
    CHECK_NULL_PTR(ptr);

    char* str_ptr = (char*)ptr->ptr + offset;

    /* 对 owned 指针，确认 offset 在范围内 */
    if (ptr->owned && ptr->size > 0 && (size_t)offset >= ptr->size) {
        native_throw_error("读取偏移越界");
        return val_null();
    }

    /* 安全长度检测：owned 指针限制到内存边界，非 owned 默认 4096 */
    size_t max_len = (ptr->owned && ptr->size > 0) ? (ptr->size - (size_t)offset) : 4096;
    size_t len = 0;
    while (len < max_len && str_ptr[len] != '\0') len++;

    if (len >= max_len && str_ptr[len] != '\0') {
        native_throw_error("字符串未以 null 结尾或超出安全长度");
        return val_null();
    }

    ObjString* str = str_copy(str_ptr, (int)len);
    return val_obj((Object*)str);
}

/* ffi.read_string_n(ptr, off, len) - 读取指定长度的字符串 */
static Value ffi_read_string_n_func(int argc, Value* args) {
    (void)argc;
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
    int offset = val_as_int(args[1]);
    int len = val_as_int(args[2]);

    CHECK_NULL_PTR(ptr);

    if (len <= 0) {
        native_throw_error("读取长度必须为正整数");
        return val_null();
    }

    CHECK_BOUNDS(ptr, offset, (size_t)len);

    char* str_ptr = (char*)ptr->ptr + offset;
    ObjString* str = str_copy(str_ptr, len);
    return val_obj((Object*)str);
}

/* ==================== FFI 写入函数 ==================== */

/* ffi.write_byte(ptr, off, val) - 写入 uint8 */
static Value ffi_write_byte_func(int argc, Value* args) {
    (void)argc;
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
    int offset = val_as_int(args[1]);
    uint8_t value = (uint8_t)val_as_int(args[2]);
    CHECK_NULL_PTR(ptr);
    CHECK_BOUNDS(ptr, offset, sizeof(uint8_t));
    *((uint8_t*)((char*)ptr->ptr + offset)) = value;
    return val_null();
}

/* ffi.write_int8(ptr, off, val) - 写入 int8 */
static Value ffi_write_int8_func(int argc, Value* args) {
    (void)argc;
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
    int offset = val_as_int(args[1]);
    int8_t value = (int8_t)val_as_int(args[2]);
    CHECK_NULL_PTR(ptr);
    CHECK_BOUNDS(ptr, offset, sizeof(int8_t));
    *((int8_t*)((char*)ptr->ptr + offset)) = value;
    return val_null();
}

/* ffi.write_int16(ptr, off, val) - 写入 int16 */
static Value ffi_write_int16_func(int argc, Value* args) {
    (void)argc;
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
    int offset = val_as_int(args[1]);
    int16_t value = (int16_t)val_as_int(args[2]);
    CHECK_NULL_PTR(ptr);
    CHECK_BOUNDS(ptr, offset, sizeof(int16_t));
    memcpy((char*)ptr->ptr + offset, &value, sizeof(value));
    return val_null();
}

/* ffi.write_uint16(ptr, off, val) - 写入 uint16 */
static Value ffi_write_uint16_func(int argc, Value* args) {
    (void)argc;
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
    int offset = val_as_int(args[1]);
    uint16_t value = (uint16_t)val_as_int(args[2]);
    CHECK_NULL_PTR(ptr);
    CHECK_BOUNDS(ptr, offset, sizeof(uint16_t));
    memcpy((char*)ptr->ptr + offset, &value, sizeof(value));
    return val_null();
}

/* ffi.write_int(ptr, off, val) - 写入 int32 */
static Value ffi_write_int_func(int argc, Value* args) {
    (void)argc;
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
    int offset = val_as_int(args[1]);
    int32_t value = (int32_t)val_as_int(args[2]);
    CHECK_NULL_PTR(ptr);
    CHECK_BOUNDS(ptr, offset, sizeof(int32_t));
    memcpy((char*)ptr->ptr + offset, &value, sizeof(value));
    return val_null();
}

/* ffi.write_uint(ptr, off, val) - 写入 uint32
 * 支持 int 和 bigint 输入（如 0x80000000 等超过 2^31 的值）
 */
static Value ffi_write_uint_func(int argc, Value* args) {
    (void)argc;
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
    int offset = val_as_int(args[1]);
    CHECK_NULL_PTR(ptr);
    CHECK_BOUNDS(ptr, offset, sizeof(uint32_t));

    uint32_t value;
    if (val_is_int(args[2])) {
        value = (uint32_t)(uint64_t)val_as_int(args[2]);
    } else if (val_is_bigint(args[2])) {
        ObjBigInt* bi = val_as_bigint(args[2]);
        uint64_t raw = 0;
        if (bi->limb_count >= 1) raw = (uint64_t)bi->limbs[0];
        if (bi->limb_count >= 2) raw |= (uint64_t)bi->limbs[1] << 32;
        value = (uint32_t)raw;
    } else {
        native_throw_error("write_uint 需要 int 或 bigint 类型的值");
        return val_null();
    }

    memcpy((char*)ptr->ptr + offset, &value, sizeof(value));
    return val_null();
}

/* ffi.write_int64(ptr, off, val) - 写入 int64
 * 支持 int 和 bigint 输入
 */
static Value ffi_write_int64_func(int argc, Value* args) {
    (void)argc;
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
    int offset = val_as_int(args[1]);
    CHECK_NULL_PTR(ptr);
    CHECK_BOUNDS(ptr, offset, sizeof(int64_t));
    
    int64_t value;
    if (val_is_int(args[2])) {
        value = (int64_t)val_as_int(args[2]);
    } else if (val_is_bigint(args[2])) {
        value = bigint_to_int64(val_as_bigint(args[2]));
    } else {
        native_throw_error("write_int64 需要 int 或 bigint 类型的值");
        return val_null();
    }
    
    memcpy((char*)ptr->ptr + offset, &value, sizeof(value));
    return val_null();
}

/* ffi.write_uint64(ptr, off, val) - 写入 uint64
 * 支持 int 和 bigint 输入
 * bigint 直接提取位模式（不做溢出饱和处理）
 */
static Value ffi_write_uint64_func(int argc, Value* args) {
    (void)argc;
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
    int offset = val_as_int(args[1]);
    CHECK_NULL_PTR(ptr);
    CHECK_BOUNDS(ptr, offset, sizeof(uint64_t));

    uint64_t value;
    if (val_is_int(args[2])) {
        value = (uint64_t)(int64_t)val_as_int(args[2]);
    } else if (val_is_bigint(args[2])) {
        ObjBigInt* bi = val_as_bigint(args[2]);
        if (bi->limb_count == 0) {
            value = 0;
        } else if (bi->limb_count == 1) {
            value = (uint64_t)bi->limbs[0];
            if (bi->is_negative) value = (uint64_t)(-(int64_t)value);
        } else if (bi->limb_count == 2) {
            value = ((uint64_t)bi->limbs[1] << 32) | (uint64_t)bi->limbs[0];
            if (bi->is_negative) value = (uint64_t)(-(int64_t)value);
        } else {
            value = UINT64_MAX;
        }
    } else {
        native_throw_error("write_uint64 需要 int 或 bigint 类型的值");
        return val_null();
    }

    memcpy((char*)ptr->ptr + offset, &value, sizeof(value));
    return val_null();
}

/* ffi.write_float(ptr, off, val) - 写入 float */
static Value ffi_write_float_func(int argc, Value* args) {
    (void)argc;
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
    int offset = val_as_int(args[1]);
    float value = (float)val_as_num(args[2]);
    CHECK_NULL_PTR(ptr);
    CHECK_BOUNDS(ptr, offset, sizeof(float));
    memcpy((char*)ptr->ptr + offset, &value, sizeof(value));
    return val_null();
}

/* ffi.write_double(ptr, off, val) - 写入 double */
static Value ffi_write_double_func(int argc, Value* args) {
    (void)argc;
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
    int offset = val_as_int(args[1]);
    double value = val_as_num(args[2]);
    CHECK_NULL_PTR(ptr);
    CHECK_BOUNDS(ptr, offset, sizeof(double));
    memcpy((char*)ptr->ptr + offset, &value, sizeof(value));
    return val_null();
}

/* ffi.write_ptr(ptr, off, value_ptr) - 写入指针值 */
static Value ffi_write_ptr_func(int argc, Value* args) {
    (void)argc;
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
    int offset = val_as_int(args[1]);

    void* value = NULL;
    if (val_is_ffi_ptr(args[2])) {
        ObjFFIPointer* src = val_as_ffi_ptr(args[2]);
        value = src->freed ? NULL : src->ptr;
    } else if (val_is_null(args[2])) {
        value = NULL;
    } else if (val_is_int(args[2])) {
        value = (void*)(intptr_t)val_as_int(args[2]);
    }

    CHECK_NULL_PTR(ptr);
    CHECK_BOUNDS(ptr, offset, sizeof(void*));
    memcpy((char*)ptr->ptr + offset, &value, sizeof(value));
    return val_null();
}

/* ffi.write_bool(ptr, off, val) - 写入 bool（1 字节，0 或 1） */
static Value ffi_write_bool_func(int argc, Value* args) {
    (void)argc;
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
    int offset = val_as_int(args[1]);
    uint8_t value = val_is_truthy(args[2]) ? 1 : 0;
    CHECK_NULL_PTR(ptr);
    CHECK_BOUNDS(ptr, offset, sizeof(uint8_t));
    *((uint8_t*)((char*)ptr->ptr + offset)) = value;
    return val_null();
}

/* ffi.write_string(ptr, off, string) - 向指针写入字符串（含末尾 '\0'） */
static Value ffi_write_string_func(int argc, Value* args) {
    (void)argc;
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);

    int offset = val_as_int(args[1]);
    ObjString* str = (ObjString*)val_as_obj(args[2]);

    CHECK_NULL_PTR(ptr);

    size_t str_len = (size_t)str->len + 1;  /* +1 为末尾 '\0' */
    char* dest = (char*)ptr->ptr + offset;

    /* 对 owned 指针进行边界检查 */
    if (ptr->owned && ptr->size > 0) {
        if ((size_t)offset + str_len > ptr->size) {
            /* 截断写入，但保证 null 终止 */
            size_t avail = ptr->size - (size_t)offset;
            if (avail > 0) {
                memcpy(dest, str->chars, avail - 1);
                dest[avail - 1] = '\0';
            }
            return val_null();
        }
    }

    memcpy(dest, str->chars, str_len);
    return val_null();
}

/* ffi.string_bytes(str) - 获取字符串的字节长度（UTF-8 编码） */
static Value ffi_string_bytes_func(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);

    size_t byte_len = (size_t)str->len;
    return val_int((int)byte_len);
}

/* ffi.memcpy(dest, src, size) - 内存拷贝
 * 参数: dest - 目标指针, src - 源指针, size - 拷贝字节数
 * 返回: null
 */
static Value ffi_memcpy_func(int argc, Value* args) {
    (void)argc;
    ObjFFIPointer* dest = val_as_ffi_ptr(args[0]);
    ObjFFIPointer* src = val_as_ffi_ptr(args[1]);
    int size = val_as_int(args[2]);
    
    if (!dest || !src) {
        native_throw_error("memcpy 参数不能为 null");
        return val_null();
    }
    
    if (size < 0) {
        native_throw_error("memcpy 大小不能为负数");
        return val_null();
    }

    CHECK_BOUNDS(dest, 0, (size_t)size);
    CHECK_BOUNDS(src, 0, (size_t)size);
    
    memcpy(dest->ptr, src->ptr, (size_t)size);
    return val_null();
}

/* ffi.memset(ptr, value, size) - 内存填充
 * 参数: ptr - 目标指针, value - 填充值(0-255), size - 填充字节数
 * 返回: null
 */
static Value ffi_memset_func(int argc, Value* args) {
    (void)argc;
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
    int value = val_as_int(args[1]);
    int size = val_as_int(args[2]);

    CHECK_NULL_PTR(ptr);

    if (size < 0) {
        native_throw_error("memset 大小不能为负数");
        return val_null();
    }

    CHECK_BOUNDS(ptr, 0, (size_t)size);

    memset(ptr->ptr, value, (size_t)size);
    return val_null();
}

#ifdef _WIN32
/* ffi.utf8_to_utf16(str) - 将 UTF-8 字符串转换为 UTF-16 (Windows)
 * 返回: FFI 指针对象，包含 UTF-16 编码的宽字符数据
 * 使用完后需要用 ffi.free() 释放
 */
static Value ffi_utf8_to_utf16_func(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);

    // 使用 platform.h 中的转换函数
    wchar_t* wstr = utf8_to_utf16(str->chars);
    if (!wstr) {
        native_throw_error("UTF-8 到 UTF-16 转换失败");
        return val_null();
    }
    
    // 计算宽字符长度（包含 null 终止符）
    size_t wlen = wcslen(wstr) + 1;
    size_t byte_size = wlen * sizeof(wchar_t);
    
    // 重新分配内存，加上 8 字节哨兵（与 ffi.malloc 一致，用于 ffi.free 溢出检测）
    void* buf = malloc(byte_size + 8);
    if (!buf) {
        free(wstr);
        native_throw_error("内存分配失败");
        return val_null();
    }
    memcpy(buf, wstr, byte_size);
    *(uint64_t*)((char*)buf + byte_size) = 0xDEADBEEFDEADBEEFULL;
    free(wstr);
    
    // 使用 GC 分配 ObjFFIPointer
    ObjFFIPointer* ptr = (ObjFFIPointer*)gc_alloc(sizeof(ObjFFIPointer), OBJ_FFI_POINTER);
    if (!ptr) {
        free(buf);
        native_throw_error("内存分配失败");
        return val_null();
    }
    
    ptr->ptr = buf;
    ptr->size = byte_size;
    ptr->owned = 1;   // 标记为 owned，ffi.free 时会释放
    ptr->freed = 0;
    
    return val_obj((Object*)ptr);
}

/* ffi.utf16_to_utf8(ptr) - 将 UTF-16 宽字符指针转换为 UTF-8 字符串 (Windows)
 * 参数: FFI 指针，指向以 null 结尾的 UTF-16 宽字符数据
 * 返回: UTF-8 编码的字符串
 */
static Value ffi_utf16_to_utf8_func(int argc, Value* args) {
    (void)argc;
    ObjFFIPointer* ptr = val_as_ffi_ptr(args[0]);
    CHECK_NULL_PTR(ptr);
    
    // 使用 platform.h 中的转换函数
    char* str = utf16_to_utf8((wchar_t*)ptr->ptr);
    if (!str) {
        native_throw_error("UTF-16 到 UTF-8 转换失败");
        return val_null();
    }
    
    ObjString* result = str_new(str, strlen(str));
    free(str);
    return val_obj((Object*)result);
}
#endif // _WIN32

/* ffi.alloc(type_name, value) - 分配指定类型的内存并初始化
 * 简化语法：ffi.alloc("int", 42) 等价于 ffi.malloc(4) + ffi.write_int(ptr, 0, 42)
 * value 可选，不传则清零初始化
 */
static Value ffi_new_func(int argc, Value* args) {
    const char* name = ((ObjString*)val_as_obj(args[0]))->chars;

    /* 获取类型大小 */
    size_t size = 0;
    int type_kind = 0;  /* 0=other, 1=int, 2=uint, 3=float, 4=ptr, 5=bool, 6=int8, 7=uint8, 8=int16, 9=uint16, 10=int64, 11=uint64 */

    if (strcmp(name, "int8")    == 0) { size = 1; type_kind = 6; }
    else if (strcmp(name, "uint8")   == 0 || strcmp(name, "byte") == 0) { size = 1; type_kind = 7; }
    else if (strcmp(name, "int16")   == 0) { size = 2; type_kind = 8; }
    else if (strcmp(name, "uint16")  == 0) { size = 2; type_kind = 9; }
    else if (strcmp(name, "int")     == 0 || strcmp(name, "int32") == 0) { size = 4; type_kind = 1; }
    else if (strcmp(name, "uint32")  == 0) { size = 4; type_kind = 2; }
    else if (strcmp(name, "int64")   == 0) { size = 8; type_kind = 10; }
    else if (strcmp(name, "uint64")  == 0) { size = 8; type_kind = 11; }
    else if (strcmp(name, "float")   == 0) { size = 4; type_kind = 3; }
    else if (strcmp(name, "double")  == 0) { size = 8; type_kind = 3; }
    else if (strcmp(name, "pointer") == 0 || strcmp(name, "ptr") == 0) { size = sizeof(void*); type_kind = 4; }
    else if (strcmp(name, "bool")    == 0) { size = sizeof(int); type_kind = 5; }
    else if (strcmp(name, "long")    == 0) { size = sizeof(long); type_kind = 1; }
    else if (strcmp(name, "size_t")  == 0) { size = sizeof(size_t); type_kind = 2; }
    else {
        char msg[128];
        snprintf(msg, sizeof(msg), "ffi.alloc 不支持的类型: '%s'", name);
        native_throw_error(msg);
        return val_null();
    }

    /* 分配内存 +8 哨兵 */
    void* ptr = malloc(size + 8);
    if (!ptr) {
        native_throw_error("内存不足");
        return val_null();
    }
    memset(ptr, 0, size);
    *(uint64_t*)((char*)ptr + size) = 0xDEADBEEFDEADBEEFULL;

    /* 如果提供了初始值，写入 */
    if (argc >= 2 && !val_is_null(args[1])) {
        Value val = args[1];
        switch (type_kind) {
            case 6: { /* int8 */
                int8_t v = (int8_t)val_as_int(val);
                memcpy(ptr, &v, sizeof(v));
                break;
            }
            case 7: { /* uint8 */
                uint8_t v = (uint8_t)val_as_int(val);
                memcpy(ptr, &v, sizeof(v));
                break;
            }
            case 8: { /* int16 */
                int16_t v = (int16_t)val_as_int(val);
                memcpy(ptr, &v, sizeof(v));
                break;
            }
            case 9: { /* uint16 */
                uint16_t v = (uint16_t)val_as_int(val);
                memcpy(ptr, &v, sizeof(v));
                break;
            }
            case 1: { /* int32 */
                int32_t v;
                if (val_is_int(val)) {
                    v = (int32_t)val_as_int(val);
                } else if (val_is_bigint(val)) {
                    v = (int32_t)bigint_to_int64(val_as_bigint(val));
                } else {
                    v = 0;
                }
                memcpy(ptr, &v, sizeof(v));
                break;
            }
            case 2: { /* uint32 */
                uint32_t v;
                if (val_is_int(val)) {
                    v = (uint32_t)(uint64_t)val_as_int(val);
                } else if (val_is_bigint(val)) {
                    v = (uint32_t)bigint_to_int64(val_as_bigint(val));
                } else {
                    v = 0;
                }
                memcpy(ptr, &v, sizeof(v));
                break;
            }
            case 10: { /* int64 */
                int64_t v;
                if (val_is_int(val)) {
                    v = (int64_t)val_as_int(val);
                } else if (val_is_bigint(val)) {
                    v = bigint_to_int64(val_as_bigint(val));
                } else {
                    v = 0;
                }
                memcpy(ptr, &v, sizeof(v));
                break;
            }
            case 11: { /* uint64 */
                uint64_t v;
                if (val_is_int(val)) {
                    v = (uint64_t)(int64_t)val_as_int(val);
                } else if (val_is_bigint(val)) {
                    ObjBigInt* bi = val_as_bigint(val);
                    if (bi->limb_count == 0) v = 0;
                    else if (bi->limb_count == 1) v = (uint64_t)bi->limbs[0];
                    else if (bi->limb_count >= 2) v = ((uint64_t)bi->limbs[1] << 32) | (uint64_t)bi->limbs[0];
                    else v = 0;
                } else {
                    v = 0;
                }
                memcpy(ptr, &v, sizeof(v));
                break;
            }
            case 3: { /* float/double */
                if (strcmp(name, "float") == 0) {
                    float v = (float)val_as_num(val);
                    memcpy(ptr, &v, sizeof(v));
                } else {
                    double v = val_as_num(val);
                    memcpy(ptr, &v, sizeof(v));
                }
                break;
            }
            case 4: { /* pointer */
                void* v = NULL;
                if (val_is_ffi_ptr(val)) {
                    ObjFFIPointer* p = val_as_ffi_ptr(val);
                    v = p->freed ? NULL : p->ptr;
                } else if (val_is_null(val)) {
                    v = NULL;
                } else if (val_is_int(val)) {
                    v = (void*)(intptr_t)val_as_int(val);
                }
                memcpy(ptr, &v, sizeof(v));
                break;
            }
            case 5: { /* bool */
                int v = val_is_truthy(val) ? 1 : 0;
                memcpy(ptr, &v, sizeof(v));
                break;
            }
        }
    }

    /* 创建 FFI 指针对象 */
    ObjFFIPointer* ffi_ptr = (ObjFFIPointer*)gc_alloc(sizeof(ObjFFIPointer), OBJ_FFI_POINTER);
    if (!ffi_ptr) {
        free(ptr);
        native_throw_error("内存不足");
        return val_null();
    }
    ffi_ptr->ptr = ptr;
    ffi_ptr->size = size;
    ffi_ptr->owned = 1;
    ffi_ptr->freed = 0;
    ffi_ptr->element_type = TYPE_PTR;

    return val_obj((Object*)ffi_ptr);
}

/* ==================== 类型信息函数 ==================== */

/* ffi.sizeof_type(type_name) - 获取 C 类型的大小（字节） */
static Value ffi_sizeof_type_func(int argc, Value* args) {
    (void)argc;
    const char* name = ((ObjString*)val_as_obj(args[0]))->chars;

    if (strcmp(name, "void")    == 0) return val_int(0);
    if (strcmp(name, "bool")    == 0) return val_int(sizeof(int));
    if (strcmp(name, "int8")    == 0) return val_int(1);
    if (strcmp(name, "uint8")   == 0) return val_int(1);
    if (strcmp(name, "byte")    == 0) return val_int(1);
    if (strcmp(name, "int16")   == 0) return val_int(2);
    if (strcmp(name, "uint16")  == 0) return val_int(2);
    if (strcmp(name, "short")   == 0) return val_int(2);
    if (strcmp(name, "int")     == 0) return val_int(4);
    if (strcmp(name, "int32")   == 0) return val_int(4);
    if (strcmp(name, "uint32")  == 0) return val_int(4);
    if (strcmp(name, "int64")   == 0) return val_int(8);
    if (strcmp(name, "uint64")  == 0) return val_int(8);
    if (strcmp(name, "long")    == 0) return val_int(sizeof(long));
    if (strcmp(name, "float")   == 0) return val_int(4);
    if (strcmp(name, "double")  == 0) return val_int(8);
    if (strcmp(name, "pointer") == 0) return val_int(sizeof(void*));
    if (strcmp(name, "ptr")     == 0) return val_int(sizeof(void*));
    if (strcmp(name, "size_t")  == 0) return val_int(sizeof(size_t));

    char msg[128];
    snprintf(msg, sizeof(msg), "未知类型: '%s'", name);
    native_throw_error(msg);
    return val_null();
}

/* ffi.alignof(type_name) - 获取 C 类型的对齐要求 */
static Value ffi_alignof_func(int argc, Value* args) {
    (void)argc;
    const char* name = ((ObjString*)val_as_obj(args[0]))->chars;

    /* 使用偏移技巧计算对齐值 */
    #define ALIGNOF(type) ((size_t)&((struct { char c; type d; }*)0)->d)

    if (strcmp(name, "int8")   == 0 || strcmp(name, "uint8") == 0 || strcmp(name, "byte") == 0) return val_int((int)ALIGNOF(int8_t));
    if (strcmp(name, "int16")  == 0 || strcmp(name, "uint16") == 0 || strcmp(name, "short") == 0) return val_int((int)ALIGNOF(int16_t));
    if (strcmp(name, "int")    == 0 || strcmp(name, "int32") == 0 || strcmp(name, "uint32") == 0) return val_int((int)ALIGNOF(int32_t));
    if (strcmp(name, "int64")  == 0 || strcmp(name, "uint64") == 0) return val_int((int)ALIGNOF(int64_t));
    if (strcmp(name, "float")  == 0) return val_int((int)ALIGNOF(float));
    if (strcmp(name, "double") == 0) return val_int((int)ALIGNOF(double));
    if (strcmp(name, "pointer") == 0 || strcmp(name, "ptr") == 0) return val_int((int)ALIGNOF(void*));

    char msg[128];
    snprintf(msg, sizeof(msg), "未知类型: '%s'", name);
    native_throw_error(msg);
    return val_null();

    #undef ALIGNOF
}

/* ==================== FFI 回调机制 ==================== */

typedef struct {
    int64_t int_args[6];    // Windows: RCX, RDX, R8, R9; Linux: RDI, RSI, RDX, RCX, R8, R9
    double float_args[8];   // Windows: XMM0-XMM3; Linux: XMM0-XMM7
} CallbackRegState;

/* 全局变量用于传递浮点返回值 */
static double g_callback_float_result = 0.0;

/* ===== 跨线程回调编组（marshalling） ===== */
/* 当 C 库在工作线程触发 FFI 回调时（如 SDL3 对话框），Leno VM 不可跨线程重入。
 * 此机制将回调参数打包到线程安全队列，阻塞工作线程等待，
 * 由主线程通过 ffi.pump_callbacks() 取出并在主线程 VM 上执行回调，
 * 完成后唤醒工作线程放行。
 */
#define MAX_PENDING_CALLBACKS 64

typedef struct {
    int cb_id;                    /* 回调 ID */
    CallbackRegState regs;        /* 寄存器参数快照 */
    FFIValue result;              /* 主线程执行后的返回值 */
    double float_result;          /* 浮点返回值 */
    int completed;                /* 主线程是否已执行完（0=等待，1=完成） */
    int in_use;                   /* 槽位是否被占用（0=空闲，1=占用） */
} PendingCallback;

static PendingCallback g_pending_queue[MAX_PENDING_CALLBACKS];
/* g_pending_count 和 g_callback_marshal_initialized 前置声明在 ffi_call_impl 之前 */

static PlatformMutex g_pending_mutex;      /* 队列互斥锁 */
static PlatformCondVar g_pending_cond;     /* 队列有新项/有空位条件变量 */
static PlatformCondVar g_complete_cond;    /* 单项完成条件变量 */
/* g_callback_marshal_initialized 前置声明在 ffi_call_impl 之前 */

/* 记录主线程 ID（在 ffi_callback_marshal_init 中由主线程设置） */
static PlatformThreadID g_main_thread_id;
static int g_main_thread_id_set = 0;

static void ffi_callback_marshal_init(void) {
    if (g_callback_marshal_initialized) return;
    platform_mutex_init(&g_pending_mutex);
    platform_cond_init(&g_pending_cond);
    platform_cond_init(&g_complete_cond);
    g_pending_count = 0;
    memset(g_pending_queue, 0, sizeof(g_pending_queue));
    /* 在主线程初始化时记录主线程 ID（ffi_init_module 由主线程调用） */
    g_main_thread_id = platform_thread_self();
    g_main_thread_id_set = 1;
    g_callback_marshal_initialized = 1;
}

/* ===== 同线程直接执行回调（原有逻辑） ===== */
static FFIValue callback_dispatch_direct(int cb_id, CallbackRegState* regs) {
    FFIValue result = {0};
    if (cb_id < 0 || cb_id >= MAX_FFI_CALLBACKS || !g_callback_registry[cb_id].active) {
        return result;
    }

    FFICallbackEntry* entry = &g_callback_registry[cb_id];
    FFISignature* sig = entry->sig;
    int total = sig->nargs > FFI_MAX_ARGS ? FFI_MAX_ARGS : sig->nargs;

    FFIArg ffi_args[FFI_MAX_ARGS];
    memset(ffi_args, 0, sizeof(ffi_args));

    // 参数提取：根据平台不同，寄存器映射不同
    // Windows: int_args[0]=RCX, [1]=RDX, [2]=R8, [3]=R9; float_args[0-3]=XMM0-XMM3
    // Linux:   int_args[0]=RDI, [1]=RSI, [2]=RDX, [3]=RCX, [4]=R8, [5]=R9; float_args[0-7]=XMM0-XMM7
    int int_idx = 0, float_idx = 0;
    for (int i = 0; i < total; i++) {
        FFIType atype = sig->arg_types[i];
        ffi_args[i].type = atype;
        if (atype == FFI_TYPE_DOUBLE || atype == FFI_TYPE_FLOAT) {
            // Linux 支持最多 8 个浮点寄存器，Windows 支持 4 个
            int max_float_regs = 4;
            #ifndef _WIN32
            max_float_regs = 8;
            #endif
            if (float_idx < max_float_regs)
                ffi_args[i].value.d = regs->float_args[float_idx++];
            else
                ffi_args[i].value.d = 0.0;
        } else {
            // Linux 支持最多 6 个整数寄存器，Windows 支持 4 个
            int max_int_regs = 4;
            #ifndef _WIN32
            max_int_regs = 6;
            #endif
            if (int_idx < max_int_regs)
                ffi_args[i].value.i = regs->int_args[int_idx++];
            else
                ffi_args[i].value.i = 0;
        }
    }

    VM* vm_ptr = current_exec_vm;
    if (!vm_ptr) vm_ptr = &vm;
    if (!vm_initialized) return result;

    int saved_sp = vm_ptr->sp;

    // 先推送参数（按照 Leno 调用约定，参数在函数值之前）
    for (int i = 0; i < total; i++) {
        Value leno_arg;
        switch (ffi_args[i].type) {
            case FFI_TYPE_INT:
            case FFI_TYPE_INT8:
            case FFI_TYPE_INT16:
            case FFI_TYPE_INT32:
            case FFI_TYPE_UINT8:
            case FFI_TYPE_UINT16:
            case FFI_TYPE_UINT32:
            case FFI_TYPE_BOOL:
                leno_arg = val_int((int)ffi_args[i].value.i);
                break;
            case FFI_TYPE_DOUBLE:
            case FFI_TYPE_FLOAT:
                leno_arg = val_float(ffi_args[i].value.d);
                break;
            case FFI_TYPE_POINTER:
                if (ffi_args[i].value.i == 0) {
                    leno_arg = val_null();
                } else {
                    ObjFFIPointer* p = (ObjFFIPointer*)gc_alloc(sizeof(ObjFFIPointer), OBJ_FFI_POINTER);
                    if (!p) { leno_arg = val_null(); break; }
                    p->ptr = (void*)(intptr_t)ffi_args[i].value.i;
                    p->size = 0;
                    p->owned = 0;
                    p->freed = 0;
                    p->element_type = TYPE_PTR;
                    leno_arg = val_obj((Object*)p);
                }
                break;
            default:
                leno_arg = val_int((int)ffi_args[i].value.i);
                break;
        }
        vm_stack_push(vm_ptr, leno_arg);
    }

    // 最后推送函数值（callee 必须在栈顶）
    vm_stack_push(vm_ptr, entry->func_val);

    // 保存 frame_cnt 以便在失败时恢复
    int saved_frame_cnt = vm_ptr->frame_cnt;

    // 使用 vm_call_value 调用函数
    // 注意：vm_call_value 内部会调用 call_value，它会从栈上获取 callee
    int call_result = vm_call_value(entry->func_val, total, 0);
    Value ret_val = vm_ptr->last_return_value;
    if (call_result != 1) {  // vm_call_value 返回 1 表示成功
        // 调用失败，清理 VM 状态
        vm_ptr->has_exception = 0;
        vm_ptr->exception = val_null();
        vm_ptr->frame_cnt = saved_frame_cnt;  // 恢复 frame_cnt
        ret_val = val_int(0);  // 默认返回 0
    }
    vm_ptr->sp = saved_sp;
    switch (sig->ret_type) {
        case FFI_TYPE_DOUBLE:
        case FFI_TYPE_FLOAT:
            if (val_is_float(ret_val)) result.d = val_as_num(ret_val);
            else if (val_is_int(ret_val)) result.d = (double)val_as_int(ret_val);
            else result.d = 0.0;
            break;
        case FFI_TYPE_POINTER:
            if (val_is_ffi_ptr(ret_val)) {
                ObjFFIPointer* p = (ObjFFIPointer*)val_as_obj(ret_val);
                result.p = p->freed ? NULL : p->ptr;
            } else if (val_is_null(ret_val)) {
                result.p = NULL;
            } else if (val_is_int(ret_val)) {
                result.p = (void*)(intptr_t)val_as_int(ret_val);
            }
            break;
        case FFI_TYPE_BOOL:
            result.i = val_is_truthy(ret_val) ? 1 : 0;
            break;
        case FFI_TYPE_VOID:
            break;
        default:
            if (val_is_int(ret_val)) result.i = (int64_t)val_as_int(ret_val);
            else if (val_is_float(ret_val)) result.i = (int64_t)val_as_num(ret_val);
            else result.i = 0;
            break;
    }

    /* 同时设置浮点返回值全局变量，供 trampoline 使用 */
    g_callback_float_result = result.d;

    return result;
}

/* ===== 跨线程回调编组：工作线程侧 ===== */
/* 工作线程将回调参数打包到槽位，阻塞等待主线程执行完成 */
static FFIValue callback_dispatch_marshal(int cb_id, CallbackRegState* regs) {
    FFIValue result = {0};

    platform_mutex_lock(&g_pending_mutex);

    /* 查找空闲槽位 */
    int slot = -1;
    for (int i = 0; i < MAX_PENDING_CALLBACKS; i++) {
        if (!g_pending_queue[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        /* 队列满，直接返回（极端情况，不应发生） */
        platform_mutex_unlock(&g_pending_mutex);
        return result;
    }

    /* 填入参数 */
    g_pending_queue[slot].cb_id = cb_id;
    g_pending_queue[slot].regs = *regs;      /* 拷贝寄存器快照 */
    g_pending_queue[slot].result = result;
    g_pending_queue[slot].float_result = 0.0;
    g_pending_queue[slot].completed = 0;
    g_pending_queue[slot].in_use = 1;
    g_pending_count++;

    /* 通知主线程有新的待处理回调 */
    platform_cond_signal(&g_pending_cond);

    /* 阻塞等待自己的槽位完成 */
    while (!g_pending_queue[slot].completed) {
        platform_cond_wait(&g_complete_cond, &g_pending_mutex);
    }

    /* 取回结果 */
    result = g_pending_queue[slot].result;
    g_callback_float_result = g_pending_queue[slot].float_result;

    /* 释放槽位 */
    g_pending_queue[slot].in_use = 0;
    g_pending_count--;

    /* 通知其他等待空位的线程 */
    platform_cond_signal(&g_pending_cond);

    platform_mutex_unlock(&g_pending_mutex);

    return result;
}

/* ===== callback_dispatch 入口：线程检测 + 调度 ===== */
static FFIValue callback_dispatch(int cb_id, CallbackRegState* regs) {
    /* 跨线程检测：g_main_thread_id 在 ffi_callback_marshal_init 中已由主线程设置 */
    if (g_main_thread_id_set && !platform_thread_equal(platform_thread_self(), g_main_thread_id)) {
        /* 非主线程：如果该线程有有效的 VM（如 Leno 语言级线程），可直接执行；
         * 只有 current_exec_vm==NULL 的 C 库工作线程（如 SDL3 对话框线程）
         * 才需要 marshal 到主线程，因为它们没有 VM，直接执行会崩溃。 */
        if (current_exec_vm) {
            return callback_dispatch_direct(cb_id, regs);
        }
        return callback_dispatch_marshal(cb_id, regs);
    }

    /* 主线程：直接执行 */
    return callback_dispatch_direct(cb_id, regs);
}

/* ===== 主线程泵送：ffi.pump_callbacks() ===== */
/* 主线程在事件循环中调用，处理所有排队的跨线程回调 */
static Value ffi_pump_callbacks_func(int argc, Value* args) {
    (void)argc;
    (void)args;

    if (!g_callback_marshal_initialized) return val_null();

    platform_mutex_lock(&g_pending_mutex);

    /* 处理所有待处理的回调 */
    while (true) {
        /* 查找一个 in_use 且未 completed 的槽位 */
        int slot = -1;
        for (int i = 0; i < MAX_PENDING_CALLBACKS; i++) {
            if (g_pending_queue[i].in_use && !g_pending_queue[i].completed) {
                slot = i;
                break;
            }
        }
        if (slot < 0) break;  /* 没有待处理的 */

        PendingCallback* pending = &g_pending_queue[slot];

        /* 在主线程上执行回调（复用直接执行逻辑） */
        FFIValue cb_result = callback_dispatch_direct(pending->cb_id, &pending->regs);

        pending->result = cb_result;
        pending->float_result = g_callback_float_result;
        pending->completed = 1;

        /* 唤醒等待的工作线程 */
        platform_cond_broadcast(&g_complete_cond);
    }

    platform_mutex_unlock(&g_pending_mutex);

    return val_null();
}

#ifdef _WIN32
#include <windows.h>

static void* alloc_executable_memory(size_t size) {
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
}

static void free_executable_memory(void* ptr, size_t size) {
    (void)size;
    VirtualFree(ptr, 0, MEM_RELEASE);
}
#else
#include <sys/mman.h>

static void* alloc_executable_memory(size_t size) {
    void* ptr = mmap(NULL, size, PROT_EXEC | PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (ptr == MAP_FAILED) ? NULL : ptr;
}

static void free_executable_memory(void* ptr, size_t size) {
    munmap(ptr, size);
}
#endif

void ffi_callback_free_resources(Object* obj) {
    ObjFFICallback* cb = (ObjFFICallback*)obj;
    if (cb->callback_id >= 0 && cb->callback_id < MAX_FFI_CALLBACKS) {
        g_callback_registry[cb->callback_id].active = 0;
    }
    if (cb->trampoline) {
        free_executable_memory(cb->trampoline, 256);
        cb->trampoline = NULL;
    }
    if (cb->sig) {
        free(cb->sig);
        cb->sig = NULL;
    }
}

static void* create_callback_trampoline(int cb_id, FFIType ret_type) {
    uint8_t* code = (uint8_t*)alloc_executable_memory(512);
    if (!code) return NULL;

    int pos = 0;

    // push rbp
    code[pos++] = 0x55;
    // mov rbp, rsp
    code[pos++] = 0x48; code[pos++] = 0x89; code[pos++] = 0xE5;
    // sub rsp, 0x88 (128 + 8 for alignment, must maintain 16-byte alignment)
    code[pos++] = 0x48; code[pos++] = 0x81; code[pos++] = 0xEC;
    code[pos++] = 0x88; code[pos++] = 0x00; code[pos++] = 0x00; code[pos++] = 0x00;

#ifdef _WIN32
    // Windows x64 调用约定：
    //   整数参数寄存器：RCX, RDX, R8, R9
    //   浮点参数寄存器：XMM0-XMM3
    //   调用 callback_dispatch 前需预留 32 字节 shadow space
    //
    // 在栈上构造 CallbackRegState，基址选在 rbp-0x80，不跨越 rbp 保存的帧指针和返回地址：
    //   [rbp-0x80] = int_args[0] = RCX
    //   [rbp-0x78] = int_args[1] = RDX
    //   [rbp-0x70] = int_args[2] = R8
    //   [rbp-0x68] = int_args[3] = R9
    //   [rbp-0x60] .. [rbp-0x50]  int_args[4..5]（保留，Windows 不用）
    //   [rbp-0x48] = float_args[0] = XMM0
    //   [rbp-0x40] = float_args[1] = XMM1
    //   [rbp-0x38] = float_args[2] = XMM2
    //   [rbp-0x30] = float_args[3] = XMM3
    // 之后把 regs 指针（= rbp-0x80）传给 callback_dispatch。
    code[pos++] = 0x48; code[pos++] = 0x89; code[pos++] = 0x4D; code[pos++] = 0x80; // mov [rbp-0x80], rcx
    code[pos++] = 0x48; code[pos++] = 0x89; code[pos++] = 0x55; code[pos++] = 0x88; // mov [rbp-0x78], rdx
    code[pos++] = 0x4C; code[pos++] = 0x89; code[pos++] = 0x45; code[pos++] = 0x90; // mov [rbp-0x70], r8
    code[pos++] = 0x4C; code[pos++] = 0x89; code[pos++] = 0x4D; code[pos++] = 0x98; // mov [rbp-0x68], r9

    code[pos++] = 0xF2; code[pos++] = 0x0F; code[pos++] = 0x11; code[pos++] = 0x45; code[pos++] = 0xB8; // movsd [rbp-0x48], xmm0
    code[pos++] = 0xF2; code[pos++] = 0x0F; code[pos++] = 0x11; code[pos++] = 0x4D; code[pos++] = 0xC0; // movsd [rbp-0x40], xmm1
    code[pos++] = 0xF2; code[pos++] = 0x0F; code[pos++] = 0x11; code[pos++] = 0x55; code[pos++] = 0xC8; // movsd [rbp-0x38], xmm2
    code[pos++] = 0xF2; code[pos++] = 0x0F; code[pos++] = 0x11; code[pos++] = 0x5D; code[pos++] = 0xD0; // movsd [rbp-0x30], xmm3

    // mov rcx, cb_id
    code[pos++] = 0x48; code[pos++] = 0xB9;
    int64_t cb_id_ext = (int64_t)cb_id;
    memcpy(code + pos, &cb_id_ext, sizeof(int64_t));
    pos += 8;

    // lea rdx, [rbp-0x80]
    code[pos++] = 0x48; code[pos++] = 0x8D; code[pos++] = 0x55; code[pos++] = 0x80;

    // sub rsp, 0x28 (32 bytes shadow space + 8 bytes for alignment = 40)
    // This ensures stack is 16-byte aligned before call (RSP will be 16-byte aligned after push return address)
    code[pos++] = 0x48; code[pos++] = 0x83; code[pos++] = 0xEC; code[pos++] = 0x28;

    // mov rax, callback_dispatch
    code[pos++] = 0x48; code[pos++] = 0xB8;
    void* dispatch_addr = (void*)callback_dispatch;
    memcpy(code + pos, &dispatch_addr, sizeof(void*));
    pos += 8;

    // call rax
    code[pos++] = 0xFF; code[pos++] = 0xD0;

    // add rsp, 0x28 (remove shadow space + alignment)
    code[pos++] = 0x48; code[pos++] = 0x83; code[pos++] = 0xC4; code[pos++] = 0x28;

#else
    // ========== System V AMD64 ABI (Linux/macOS) ==========
    // Integer args: RDI, RSI, RDX, RCX, R8, R9
    // Float args: XMM0-XMM7
    // No shadow space required
    //
    // CallbackRegState layout (C struct, addresses grow upward):
    //   offset 0:  int_args[0]  (RDI)
    //   offset 8:  int_args[1]  (RSI)
    //   offset 16: int_args[2]  (RDX)
    //   offset 24: int_args[3]  (RCX)
    //   offset 32: int_args[4]  (R8)
    //   offset 40: int_args[5]  (R9)
    //   offset 48: float_args[0] (XMM0)
    //   ...
    //   offset 104: float_args[7] (XMM7)
    //   Total: 112 bytes
    //
    // Strategy: use [rbp-0x78] as the base of CallbackRegState.
    //   [rbp-0x78+0]  = [rbp-0x78] = int_args[0] = RDI
    //   [rbp-0x78+8]  = [rbp-0x70] = int_args[1] = RSI
    //   [rbp-0x78+16] = [rbp-0x68] = int_args[2] = RDX
    //   [rbp-0x78+24] = [rbp-0x60] = int_args[3] = RCX
    //   [rbp-0x78+32] = [rbp-0x58] = int_args[4] = R8
    //   [rbp-0x78+40] = [rbp-0x50] = int_args[5] = R9
    //   [rbp-0x78+48] = [rbp-0x48] = float_args[0] = XMM0
    //   [rbp-0x78+56] = [rbp-0x40] = float_args[1] = XMM1
    //   [rbp-0x78+64] = [rbp-0x38] = float_args[2] = XMM2
    //   [rbp-0x78+72] = [rbp-0x30] = float_args[3] = XMM3
    //   [rbp-0x78+80] = [rbp-0x28] = float_args[4] = XMM4
    //   [rbp-0x78+88] = [rbp-0x20] = float_args[5] = XMM5
    //   [rbp-0x78+96] = [rbp-0x18] = float_args[6] = XMM6
    //   [rbp-0x78+104]=[rbp-0x10] = float_args[7] = XMM7
    //   [rbp-0x08] = free (used for alignment)

    // Save integer argument registers to CallbackRegState layout
    code[pos++] = 0x48; code[pos++] = 0x89; code[pos++] = 0x7D; code[pos++] = 0x88; // mov [rbp-0x78], rdi
    code[pos++] = 0x48; code[pos++] = 0x89; code[pos++] = 0x75; code[pos++] = 0x90; // mov [rbp-0x70], rsi
    code[pos++] = 0x48; code[pos++] = 0x89; code[pos++] = 0x55; code[pos++] = 0x98; // mov [rbp-0x68], rdx
    code[pos++] = 0x48; code[pos++] = 0x89; code[pos++] = 0x4D; code[pos++] = 0xA0; // mov [rbp-0x60], rcx
    code[pos++] = 0x4C; code[pos++] = 0x89; code[pos++] = 0x45; code[pos++] = 0xA8; // mov [rbp-0x58], r8
    code[pos++] = 0x4C; code[pos++] = 0x89; code[pos++] = 0x4D; code[pos++] = 0xB0; // mov [rbp-0x50], r9

    // Save float argument registers to CallbackRegState layout
    code[pos++] = 0xF2; code[pos++] = 0x0F; code[pos++] = 0x11; code[pos++] = 0x45; code[pos++] = 0xB8; // movsd [rbp-0x48], xmm0
    code[pos++] = 0xF2; code[pos++] = 0x0F; code[pos++] = 0x11; code[pos++] = 0x4D; code[pos++] = 0xC0; // movsd [rbp-0x40], xmm1
    code[pos++] = 0xF2; code[pos++] = 0x0F; code[pos++] = 0x11; code[pos++] = 0x55; code[pos++] = 0xC8; // movsd [rbp-0x38], xmm2
    code[pos++] = 0xF2; code[pos++] = 0x0F; code[pos++] = 0x11; code[pos++] = 0x5D; code[pos++] = 0xD0; // movsd [rbp-0x30], xmm3
    code[pos++] = 0xF2; code[pos++] = 0x0F; code[pos++] = 0x11; code[pos++] = 0x65; code[pos++] = 0xD8; // movsd [rbp-0x28], xmm4
    code[pos++] = 0xF2; code[pos++] = 0x0F; code[pos++] = 0x11; code[pos++] = 0x6D; code[pos++] = 0xE0; // movsd [rbp-0x20], xmm5
    code[pos++] = 0xF2; code[pos++] = 0x0F; code[pos++] = 0x11; code[pos++] = 0x75; code[pos++] = 0xE8; // movsd [rbp-0x18], xmm6
    code[pos++] = 0xF2; code[pos++] = 0x0F; code[pos++] = 0x11; code[pos++] = 0x7D; code[pos++] = 0xF0; // movsd [rbp-0x10], xmm7

    // mov rdi, cb_id (immediate) - first arg for callback_dispatch
    code[pos++] = 0x48; code[pos++] = 0xBF;
    int64_t cb_id_ext_linux = (int64_t)cb_id;
    memcpy(code + pos, &cb_id_ext_linux, sizeof(int64_t));
    pos += 8;

    // lea rsi, [rbp-0x78]  (pointer to CallbackRegState) - second arg
    code[pos++] = 0x48; code[pos++] = 0x8D; code[pos++] = 0x75; code[pos++] = 0x88;

    // No shadow space needed for System V AMD64 ABI
    // But we need to maintain 16-byte stack alignment
    // sub rsp, 0x08 (8 bytes for alignment)
    code[pos++] = 0x48; code[pos++] = 0x83; code[pos++] = 0xEC; code[pos++] = 0x08;

    // mov rax, callback_dispatch
    code[pos++] = 0x48; code[pos++] = 0xB8;
    void* dispatch_addr_linux = (void*)callback_dispatch;
    memcpy(code + pos, &dispatch_addr_linux, sizeof(void*));
    pos += 8;

    // call rax
    code[pos++] = 0xFF; code[pos++] = 0xD0;

    // add rsp, 0x08 (remove alignment padding)
    code[pos++] = 0x48; code[pos++] = 0x83; code[pos++] = 0xC4; code[pos++] = 0x08;
#endif

    // RAX now contains the FFIValue result (integer/pointer part)
    // For float return types, we need to load the float result from g_callback_float_result into XMM0

    if (ret_type == FFI_TYPE_DOUBLE || ret_type == FFI_TYPE_FLOAT) {
        // Save RAX (integer return value) to stack before loading float
        // push rax
        code[pos++] = 0x50;

        // mov rax, g_callback_float_result (address)
        code[pos++] = 0x48; code[pos++] = 0xB8;
        memcpy(code + pos, &g_callback_float_result, sizeof(void*));
        pos += 8;

        // movsd xmm0, [rax]
        code[pos++] = 0xF2; code[pos++] = 0x0F; code[pos++] = 0x10; code[pos++] = 0x00;

        // pop rax (restore integer return value)
        code[pos++] = 0x58;
    }

    // add rsp, 0x88
    code[pos++] = 0x48; code[pos++] = 0x81; code[pos++] = 0xC4;
    code[pos++] = 0x88; code[pos++] = 0x00; code[pos++] = 0x00; code[pos++] = 0x00;
    // pop rbp
    code[pos++] = 0x5D;
    // ret
    code[pos++] = 0xC3;

    return (void*)code;
}

/* ==================== 兼容函数（语义分析需要，运行时报错） ==================== */

static Value ffi_callback_compat_func(int argc, Value* args) {
    (void)argc;
    (void)args;
    native_throw_error("ffi.callback 必须使用 cfunc 声明式回调，例如: cfunc Compare(Ptr a, Ptr b): i32; ffi.callback(func, Compare)");
    return val_null();
}

/* ==================== cfunc 回调创建（编译期签名） ==================== */

Value ffi_callback_create_with_sig(Value func_val, int ret_type, int param_count, const uint8_t* param_types) {
    if (!val_is_obj(func_val) ||
        (val_as_obj(func_val)->type != OBJ_CLOSURE &&
         val_as_obj(func_val)->type != OBJ_FUNCTION)) {
        native_throw_error("ffi.callback 第一个参数必须是函数");
        return val_null();
    }

    int cb_id = -1;
    for (int i = 0; i < MAX_FFI_CALLBACKS; i++) {
        if (!g_callback_registry[i].active) {
            cb_id = i;
            break;
        }
    }
    if (cb_id < 0) {
        native_throw_error("回调数量已达上限");
        return val_null();
    }

    FFISignature* sig = (FFISignature*)malloc(sizeof(FFISignature));
    if (!sig) {
        native_throw_error("内存不足");
        return val_null();
    }
    sig->ret_type = (FFIType)ret_type;
    sig->nargs = param_count;
    for (int i = 0; i < param_count; i++) {
        sig->arg_types[i] = (FFIType)param_types[i];
    }

    void* trampoline = create_callback_trampoline(cb_id, (FFIType)ret_type);
    if (!trampoline) {
        free(sig);
        native_throw_error("无法分配可执行内存");
        return val_null();
    }

    g_callback_registry[cb_id].func_val = func_val;
    g_callback_registry[cb_id].sig = sig;
    g_callback_registry[cb_id].active = 1;
    g_callback_count++;

    ObjFFICallback* cb = (ObjFFICallback*)gc_alloc(sizeof(ObjFFICallback), OBJ_FFI_CALLBACK);
    if (!cb) {
        g_callback_registry[cb_id].active = 0;
        free(sig);
        free_executable_memory(trampoline, 256);
        native_throw_error("内存不足");
        return val_null();
    }
    cb->callback_id = cb_id;
    cb->sig = sig;
    cb->trampoline = trampoline;
    cb->func_val = func_val;

    return val_obj((Object*)cb);
}

/* ==================== FFI Clib 公开接口 ==================== */

#include "ffi_clib.h"

Value ffi_clib_call(int argc, Value* args, int ret_type_kind, const int* arg_types) {
    return ffi_call_impl(argc, args, ret_type_kind, arg_types);
}

/* ==================== 模块初始化 ==================== */

void ffi_init_module(void) {
    /* ===== 库操作函数 ===== */
    TypeKind load_params[] = {TYPE_STRING};
    native_register_module_method("ffi", "load", ffi_load_func, 1, -1, -1, TYPE_PTR, TYPE_UNKNOWN, load_params);

    /* ===== 函数调用 ===== */
    TypeKind call_params[] = {TYPE_PTR, TYPE_STRING};
    /* ffi.call 返回 any 类型，但实际值是 int，需要用 _int()、_ptr() 等转换 */
    native_register_module_method("ffi", "call", ffi_call_func, -1, 2, -1, TYPE_ANY, TYPE_UNKNOWN, call_params);
    native_register_module_method("ffi", "call_int", ffi_call_int_func, -1, 2, -1, TYPE_INT, TYPE_UNKNOWN, call_params);
    native_register_module_method("ffi", "call_double", ffi_call_double_func, -1, 2, -1, TYPE_FLOAT, TYPE_UNKNOWN, call_params);
    native_register_module_method("ffi", "call_void", ffi_call_void_func, -1, 2, -1, TYPE_NULL, TYPE_UNKNOWN, call_params);
    native_register_module_method("ffi", "call_ptr", ffi_call_ptr_func, -1, 2, -1, TYPE_PTR, TYPE_UNKNOWN, call_params);
    native_register_module_method("ffi", "call_bool", ffi_call_bool_func, -1, 2, -1, TYPE_BOOL, TYPE_UNKNOWN, call_params);

    /* ===== 错误码 ===== */
    native_register_module_method("ffi", "last_error", ffi_last_error_func, 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, NULL);

    /* ===== 内存操作函数 ===== */
    TypeKind malloc_params[] = {TYPE_INT};
    native_register_module_method("ffi", "malloc", ffi_malloc_func, 1, -1, -1, TYPE_PTR, TYPE_UNKNOWN, malloc_params);

    TypeKind new_params[] = {TYPE_STRING, TYPE_ANY};
    native_register_module_method("ffi", "alloc", ffi_new_func, -1, 1, 2, TYPE_PTR, TYPE_UNKNOWN, new_params);

    TypeKind calloc_params[] = {TYPE_INT, TYPE_INT};
    native_register_module_method("ffi", "calloc", ffi_calloc_func, 2, -1, -1, TYPE_PTR, TYPE_UNKNOWN, calloc_params);

    TypeKind realloc_params[] = {TYPE_PTR, TYPE_INT};
    native_register_module_method("ffi", "realloc", ffi_realloc_func, 2, -1, -1, TYPE_PTR, TYPE_UNKNOWN, realloc_params);

    TypeKind free_params[] = {TYPE_ANY};
    native_register_module_method("ffi", "free", ffi_free_func, 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, free_params);

    TypeKind sizeof_params[] = {TYPE_PTR};
    native_register_module_method("ffi", "sizeof", ffi_sizeof_func, 1, -1, -1, TYPE_INT, TYPE_UNKNOWN, sizeof_params);

    TypeKind assert_size_params[] = {TYPE_PTR, TYPE_INT};
    native_register_module_method("ffi", "assert_size", ffi_assert_size_func, 2, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, assert_size_params);

    native_register_module_method("ffi", "nullptr", ffi_nullptr_func, 0, -1, -1, TYPE_PTR, TYPE_UNKNOWN, NULL);

    TypeKind ptr_from_int_params[] = {TYPE_INT};
    native_register_module_method("ffi", "ptr_from_int", ffi_ptr_from_int_func, 1, -1, -1, TYPE_PTR, TYPE_UNKNOWN, ptr_from_int_params);

    TypeKind ptr_to_int_params[] = {TYPE_PTR};
    native_register_module_method("ffi", "ptr_to_int", ffi_ptr_to_int_func, 1, -1, -1, TYPE_INT, TYPE_UNKNOWN, ptr_to_int_params);

    TypeKind is_ptr_params[] = {TYPE_ANY};
    native_register_module_method("ffi", "is_ptr", ffi_is_ptr_func, 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, is_ptr_params);

    TypeKind offset_params[] = {TYPE_PTR, TYPE_INT};
    native_register_module_method("ffi", "offset", ffi_offset_func, 2, -1, -1, TYPE_PTR, TYPE_UNKNOWN, offset_params);

    /* ===== 读取函数 ===== */
    TypeKind read_params[] = {TYPE_PTR, TYPE_INT};  // ptr, offset
    native_register_module_method("ffi", "read_byte",    ffi_read_byte_func,    2, -1, -1, TYPE_INT, TYPE_UNKNOWN, read_params);
    native_register_module_method("ffi", "read_int8",    ffi_read_int8_func,    2, -1, -1, TYPE_INT, TYPE_UNKNOWN, read_params);
    native_register_module_method("ffi", "read_int16",   ffi_read_int16_func,   2, -1, -1, TYPE_INT, TYPE_UNKNOWN, read_params);
    native_register_module_method("ffi", "read_uint16",  ffi_read_uint16_func,  2, -1, -1, TYPE_INT, TYPE_UNKNOWN, read_params);
    native_register_module_method("ffi", "read_int",     ffi_read_int_func,     2, -1, -1, TYPE_INT, TYPE_UNKNOWN, read_params);
    native_register_module_method("ffi", "read_uint",    ffi_read_uint_func,    2, -1, -1, TYPE_INT, TYPE_UNKNOWN, read_params);
    native_register_module_method("ffi", "read_int64",   ffi_read_int64_func,   2, -1, -1, TYPE_INT, TYPE_UNKNOWN, read_params);
    native_register_module_method("ffi", "read_uint64",  ffi_read_uint64_func,  2, -1, -1, TYPE_INT, TYPE_UNKNOWN, read_params);
    native_register_module_method("ffi", "read_float",   ffi_read_float_func,   2, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, read_params);
    native_register_module_method("ffi", "read_double",  ffi_read_double_func,  2, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, read_params);
    native_register_module_method("ffi", "read_ptr",     ffi_read_ptr_func,     2, -1, -1, TYPE_PTR, TYPE_UNKNOWN, read_params);
    native_register_module_method("ffi", "read_bool",    ffi_read_bool_func,    2, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, read_params);
    native_register_module_method("ffi", "read_at",      ffi_read_at_func,      2, -1, -1, TYPE_ANY, TYPE_UNKNOWN, read_params);  // Ptr[T] 按索引读取
    native_register_module_method("ffi", "read_string",  ffi_read_string_func,  2, -1, -1, TYPE_STRING, TYPE_UNKNOWN, read_params);

    TypeKind read_str_n_params[] = {TYPE_PTR, TYPE_INT, TYPE_INT};  // ptr, offset, length
    native_register_module_method("ffi", "read_string_n", ffi_read_string_n_func, 3, -1, -1, TYPE_STRING, TYPE_UNKNOWN, read_str_n_params);

    /* ===== 写入函数 ===== */
    TypeKind write_byte_params[] = {TYPE_PTR, TYPE_INT, TYPE_INT};  // ptr, offset, value
    native_register_module_method("ffi", "write_byte",   ffi_write_byte_func,   3, -1, -1, TYPE_NULL, TYPE_UNKNOWN, write_byte_params);
    native_register_module_method("ffi", "write_int8",   ffi_write_int8_func,   3, -1, -1, TYPE_NULL, TYPE_UNKNOWN, write_byte_params);
    native_register_module_method("ffi", "write_int16",  ffi_write_int16_func,  3, -1, -1, TYPE_NULL, TYPE_UNKNOWN, write_byte_params);
    native_register_module_method("ffi", "write_uint16", ffi_write_uint16_func, 3, -1, -1, TYPE_NULL, TYPE_UNKNOWN, write_byte_params);
    native_register_module_method("ffi", "write_int",    ffi_write_int_func,    3, -1, -1, TYPE_NULL, TYPE_UNKNOWN, write_byte_params);
    native_register_module_method("ffi", "write_uint",   ffi_write_uint_func,   3, -1, -1, TYPE_NULL, TYPE_UNKNOWN, write_byte_params);
    native_register_module_method("ffi", "write_int64",  ffi_write_int64_func,  3, -1, -1, TYPE_NULL, TYPE_UNKNOWN, write_byte_params);
    native_register_module_method("ffi", "write_uint64", ffi_write_uint64_func, 3, -1, -1, TYPE_NULL, TYPE_UNKNOWN, write_byte_params);

    TypeKind write_float_params[] = {TYPE_PTR, TYPE_INT, TYPE_FLOAT};  // ptr, offset, value
    native_register_module_method("ffi", "write_float",  ffi_write_float_func,  3, -1, -1, TYPE_NULL, TYPE_UNKNOWN, write_float_params);
    native_register_module_method("ffi", "write_double", ffi_write_double_func, 3, -1, -1, TYPE_NULL, TYPE_UNKNOWN, write_float_params);

    TypeKind write_ptr_params[] = {TYPE_PTR, TYPE_INT, TYPE_PTR};  // ptr, offset, value
    native_register_module_method("ffi", "write_ptr",    ffi_write_ptr_func,    3, -1, -1, TYPE_NULL, TYPE_UNKNOWN, write_ptr_params);

    TypeKind write_bool_params[] = {TYPE_PTR, TYPE_INT, TYPE_ANY};  // ptr, offset, value (接受 bool 和 int)
    native_register_module_method("ffi", "write_bool",   ffi_write_bool_func,   3, -1, -1, TYPE_NULL, TYPE_UNKNOWN, write_bool_params);

    TypeKind write_str_params[] = {TYPE_PTR, TYPE_INT, TYPE_STRING};  // ptr, offset, string
    native_register_module_method("ffi", "write_string", ffi_write_string_func, 3, -1, -1, TYPE_NULL, TYPE_UNKNOWN, write_str_params);

    /* ===== Ptr[T] 元素级访问 ===== */
    TypeKind at_params[] = {TYPE_PTR, TYPE_INT, TYPE_ANY};  // ptr, index, value
    native_register_module_method("ffi", "write_at",     ffi_write_at_func,     3, -1, -1, TYPE_NULL, TYPE_UNKNOWN, at_params);

    /* ===== 字符串工具函数 ===== */
    TypeKind string_params[] = {TYPE_STRING};
    native_register_module_method("ffi", "string_bytes", ffi_string_bytes_func, 1, -1, -1, TYPE_INT, TYPE_UNKNOWN, string_params);

    TypeKind memcpy_params[] = {TYPE_PTR, TYPE_PTR, TYPE_INT};
    native_register_module_method("ffi", "memcpy", ffi_memcpy_func, 3, -1, -1, TYPE_NULL, TYPE_UNKNOWN, memcpy_params);

    TypeKind memset_params[] = {TYPE_PTR, TYPE_INT, TYPE_INT};
    native_register_module_method("ffi", "memset", ffi_memset_func, 3, -1, -1, TYPE_NULL, TYPE_UNKNOWN, memset_params);

#ifdef _WIN32
    /* ===== 宽字符转换函数 (Windows) ===== */
    native_register_module_method("ffi", "utf8_to_utf16", ffi_utf8_to_utf16_func, 1, -1, -1, TYPE_PTR, TYPE_UNKNOWN, string_params);
    TypeKind ptr_params[] = {TYPE_PTR};
    native_register_module_method("ffi", "utf16_to_utf8", ffi_utf16_to_utf8_func, 1, -1, -1, TYPE_STRING, TYPE_UNKNOWN, ptr_params);
#endif // _WIN32

    /* ===== 类型信息函数 ===== */
    TypeKind type_name_params[] = {TYPE_STRING};
    native_register_module_method("ffi", "sizeof_type", ffi_sizeof_type_func, 1, -1, -1, TYPE_INT, TYPE_UNKNOWN, type_name_params);
    native_register_module_method("ffi", "alignof",     ffi_alignof_func,     1, -1, -1, TYPE_INT, TYPE_UNKNOWN, type_name_params);

    /* ===== 回调函数 ===== */
    // ffi.callback(func, CfuncName) - 第二个参数必须是 cfunc 类型
    TypeKind callback_params[] = {TYPE_ANY, TYPE_CFUNC};
    native_register_module_method("ffi", "callback", ffi_callback_compat_func, 2, -1, -1, TYPE_PTR, TYPE_UNKNOWN, callback_params);

    /* ===== 跨线程回调泵送 ===== */
    native_register_module_method("ffi", "pump_callbacks", ffi_pump_callbacks_func, 0, -1, -1, TYPE_NULL, TYPE_UNKNOWN, NULL);

    /* 初始化跨线程回调编组设施 */
    ffi_callback_marshal_init();
}
