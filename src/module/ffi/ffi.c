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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "leno_ffi.h"

#define MAX_FFI_CALLBACKS 128

typedef struct {
    Value func_val;
    FFISignature* sig;
    int active;
} FFICallbackEntry;

static FFICallbackEntry g_callback_registry[MAX_FFI_CALLBACKS];
static int g_callback_count = 0;

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
    if (lib->handle) {
#ifdef _WIN32
        FreeLibrary((HMODULE)lib->handle);
#else
        dlclose(lib->handle);
#endif
    }
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

/* ffi.load(path) - 加载动态链接库 */
static Value ffi_load_func(int argc, Value* args) {
    (void)argc;
    ObjString* path_str = (ObjString*)val_as_obj(args[0]);
    const char* path = path_str->chars;

#ifdef _WIN32
    HMODULE handle = LoadLibraryA(path);
    if (!handle) {
        DWORD error = GetLastError();
        char msg[256];
        snprintf(msg, sizeof(msg), "加载库 '%s' 失败，错误码: %lu", path, error);
        native_throw_error(msg);
        return val_null();
    }
#else
    void* handle = dlopen(path, RTLD_LAZY);
    if (!handle) {
        char msg[256];
        snprintf(msg, sizeof(msg), "加载库 '%s' 失败: %s", path, dlerror());
        native_throw_error(msg);
        return val_null();
    }
#endif

    ObjFFILibrary* lib = (ObjFFILibrary*)gc_alloc(sizeof(ObjFFILibrary), OBJ_FFI_LIBRARY);
    if (!lib) {
#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        native_throw_error("内存不足");
        return val_null();
    }

    lib->path = strdup(path);
    lib->handle = handle;
    lib->freed = 0;

    return val_obj((Object*)lib);
}

Value ffi_reload_library(const char* path) {
#ifdef _WIN32
    HMODULE handle = LoadLibraryA(path);
    if (!handle) {
        return val_null();
    }
#else
    void* handle = dlopen(path, RTLD_LAZY);
    if (!handle) {
        return val_null();
    }
#endif

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

/* ffi.call 的核心实现 - ret_type 指定返回值类型
 * 所有 ffi.call/ffi.call_double/ffi.call_void/ffi.call_ptr/ffi.call_bool 共用此函数
 */
static Value ffi_call_impl(int argc, Value* args, FFIType ret_type) {
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
        snprintf(msg, sizeof(msg), "在库中找不到函数 '%s'，错误码: %lu", func_name, GetLastError());
#else
        snprintf(msg, sizeof(msg), "在库中找不到函数 '%s': %s", func_name, dlerror());
#endif
        native_throw_error(msg);
        return val_null();
    }

    /* 构建 FFI 签名和参数 */
    FFISignature sig;
    sig.ret_type = ret_type;
    sig.nargs = argc - arg_start;
    if (sig.nargs > FFI_MAX_ARGS) sig.nargs = FFI_MAX_ARGS;

    FFIArg ffi_args[FFI_MAX_ARGS];
    memset(ffi_args, 0, sizeof(ffi_args));

    /* 将 Leno 值转换为 FFI 参数 */
    for (int i = 0; i < sig.nargs; i++) {
        Value arg = args[i + arg_start];

        if (val_is_int(arg)) {
            sig.arg_types[i] = FFI_TYPE_INT;
            ffi_args[i].type = FFI_TYPE_INT;
            ffi_args[i].value.i = (int64_t)val_as_int(arg);
        }
        else if (val_is_bigint(arg)) {
            sig.arg_types[i] = FFI_TYPE_INT;
            ffi_args[i].type = FFI_TYPE_INT;
            ffi_args[i].value.i = bigint_to_int64(val_as_bigint(arg));
        }
        else if (val_is_float(arg)) {
            sig.arg_types[i] = FFI_TYPE_DOUBLE;
            ffi_args[i].type = FFI_TYPE_DOUBLE;
            ffi_args[i].value.d = val_as_num(arg);
        }
        else if (val_is_string(arg)) {
            sig.arg_types[i] = FFI_TYPE_POINTER;
            ffi_args[i].type = FFI_TYPE_POINTER;
            ffi_args[i].value.p = ((ObjString*)val_as_obj(arg))->chars;
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

    /* 根据返回类型转换结果 */
    switch (ret_type) {
        case FFI_TYPE_DOUBLE:
        case FFI_TYPE_FLOAT:
            return val_float(result.d);
        case FFI_TYPE_BOOL:
            return val_bool(result.i != 0);
        case FFI_TYPE_POINTER: {
            if (result.p == NULL) return val_null();
            ObjFFIPointer* ret_ptr = (ObjFFIPointer*)gc_alloc(sizeof(ObjFFIPointer), OBJ_FFI_POINTER);
            if (!ret_ptr) {
                native_throw_error("内存不足");
                return val_null();
            }
            ret_ptr->ptr = result.p;
            ret_ptr->size = 0;
            ret_ptr->owned = 0;
            ret_ptr->freed = 0;
            ret_ptr->element_type = TYPE_PTR;
            return val_obj((Object*)ret_ptr);
        }
        case FFI_TYPE_VOID:
            return val_null();
        case FFI_TYPE_INT:
        default:
            return val_int((int)result.i);
    }
}

/* ffi.call(lib, name, ...) - 调用库中的函数（返回 any，实际值为 int） */
static Value ffi_call_func(int argc, Value* args) {
    return ffi_call_impl(argc, args, FFI_TYPE_INT);
}

/* ffi.call_int(lib, name, ...) - 调用库中的函数（明确返回 int） */
static Value ffi_call_int_func(int argc, Value* args) {
    return ffi_call_impl(argc, args, FFI_TYPE_INT);
}

/* ffi.call_double(lib, name, ...) - 调用库中的函数（返回 double） */
static Value ffi_call_double_func(int argc, Value* args) {
    return ffi_call_impl(argc, args, FFI_TYPE_DOUBLE);
}

/* ffi.call_void(lib, name, ...) - 调用库中的函数（无返回值） */
static Value ffi_call_void_func(int argc, Value* args) {
    return ffi_call_impl(argc, args, FFI_TYPE_VOID);
}

/* ffi.call_ptr(lib, name, ...) - 调用库中的函数（返回指针） */
static Value ffi_call_ptr_func(int argc, Value* args) {
    return ffi_call_impl(argc, args, FFI_TYPE_POINTER);
}

/* ffi.call_bool(lib, name, ...) - 调用库中的函数（返回布尔值） */
static Value ffi_call_bool_func(int argc, Value* args) {
    return ffi_call_impl(argc, args, FFI_TYPE_BOOL);
}

/* ==================== FFI 内存操作函数 ==================== */

/* ffi.malloc(size) - 分配指定大小的内存 */
static Value ffi_malloc_func(int argc, Value* args) {
    (void)argc;
    size_t size = (size_t)val_as_int(args[0]);

    void* ptr = malloc(size);
    if (!ptr) {
        native_throw_error("内存不足");
        return val_null();
    }
    memset(ptr, 0, size);

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

    void* ptr = calloc(count, size);
    if (!ptr) {
        native_throw_error("内存不足");
        return val_null();
    }

    ObjFFIPointer* ffi_ptr = (ObjFFIPointer*)gc_alloc(sizeof(ObjFFIPointer), OBJ_FFI_POINTER);
    if (!ffi_ptr) {
        free(ptr);
        native_throw_error("内存不足");
        return val_null();
    }

    ffi_ptr->ptr = ptr;
    ffi_ptr->size = count * size;
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

    void* new_ptr = realloc(ffi_ptr->ptr, new_size);
    if (!new_ptr && new_size > 0) {
        native_throw_error("内存不足");
        return val_null();
    }

    ffi_ptr->ptr = new_ptr;
    ffi_ptr->size = new_size;

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

    size_t byte_len = strlen(str->chars);
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
    
    // 使用 GC 分配 ObjFFIPointer
    ObjFFIPointer* ptr = (ObjFFIPointer*)gc_alloc(sizeof(ObjFFIPointer), OBJ_FFI_POINTER);
    if (!ptr) {
        free(wstr);
        native_throw_error("内存分配失败");
        return val_null();
    }
    
    ptr->ptr = wstr;  // 直接保存 wchar_t* 指针
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

static FFIValue callback_dispatch(int cb_id, CallbackRegState* regs) {
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
    // ========== Windows x64 calling convention ==========
    // Integer args: RCX, RDX, R8, R9
    // Float args: XMM0-XMM3
    // Shadow space: 32 bytes required

    // Save integer argument registers to shadow space / stack
    // 注意：regs 指向 [rbp-8]，所以 regs->int_args[0] = [rbp-8], regs->int_args[1] = [rbp]
    // 为了让 regs->int_args[0] = RCX, regs->int_args[1] = RDX，我们需要：
    // [rbp-0x10] = RCX (arg1) -> regs->int_args[0] (因为 regs = [rbp-8], [rbp-8] - 8 = [rbp-16])
    // [rbp-0x08] = RDX (arg2) -> regs->int_args[1] (因为 regs + 8 = [rbp])
    // 等等，这不对。让我重新计算：
    // regs = [rbp-8] 的地址
    // regs->int_args[0] = [rbp-8]
    // regs->int_args[1] = [rbp]
    // 所以 [rbp-8] 应该是 RCX，[rbp] 应该是 RDX
    // 但 [rbp] 是旧的 RBP，不能覆盖
    // 解决方案：让 regs 指向 [rbp-16]，这样 regs->int_args[0] = [rbp-16], regs->int_args[1] = [rbp-8]
    // [rbp-0x10] = RCX (arg1)
    // [rbp-0x08] = RDX (arg2)
    // [rbp-0x18] = R8  (arg3)
    // [rbp-0x20] = R9  (arg4)
    code[pos++] = 0x48; code[pos++] = 0x89; code[pos++] = 0x4D; code[pos++] = 0xF0; // mov [rbp-16], rcx
    code[pos++] = 0x48; code[pos++] = 0x89; code[pos++] = 0x55; code[pos++] = 0xF8; // mov [rbp-8], rdx
    code[pos++] = 0x4C; code[pos++] = 0x89; code[pos++] = 0x45; code[pos++] = 0xE8; // mov [rbp-24], r8
    code[pos++] = 0x4C; code[pos++] = 0x89; code[pos++] = 0x4D; code[pos++] = 0xE0; // mov [rbp-32], r9

    // Save float argument registers
    // [rbp-0x28] = XMM0
    // [rbp-0x30] = XMM1
    // [rbp-0x38] = XMM2
    // [rbp-0x40] = XMM3
    code[pos++] = 0xF2; code[pos++] = 0x0F; code[pos++] = 0x11; code[pos++] = 0x45; code[pos++] = 0xD8; // movsd [rbp-40], xmm0
    code[pos++] = 0xF2; code[pos++] = 0x0F; code[pos++] = 0x11; code[pos++] = 0x4D; code[pos++] = 0xD0; // movsd [rbp-48], xmm1
    code[pos++] = 0xF2; code[pos++] = 0x0F; code[pos++] = 0x11; code[pos++] = 0x55; code[pos++] = 0xC8; // movsd [rbp-56], xmm2
    code[pos++] = 0xF2; code[pos++] = 0x0F; code[pos++] = 0x11; code[pos++] = 0x5D; code[pos++] = 0xC0; // movsd [rbp-64], xmm3

    // mov rcx, cb_id (immediate)
    code[pos++] = 0x48; code[pos++] = 0xB9;
    int64_t cb_id_ext = (int64_t)cb_id;
    memcpy(code + pos, &cb_id_ext, sizeof(int64_t));
    pos += 8;

    // lea rdx, [rbp-0x10]  (pointer to saved register state)
    // 让 regs 指向 [rbp-16]，这样 regs->int_args[0] = [rbp-16] = RCX, regs->int_args[1] = [rbp-8] = RDX
    code[pos++] = 0x48; code[pos++] = 0x8D; code[pos++] = 0x55; code[pos++] = 0xF0;

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

    // Save integer argument registers
    // [rbp-0x08] = RDI (arg1)
    // [rbp-0x10] = RSI (arg2)
    // [rbp-0x18] = RDX (arg3)
    // [rbp-0x20] = RCX (arg4)
    // [rbp-0x28] = R8  (arg5)
    // [rbp-0x30] = R9  (arg6)
    code[pos++] = 0x48; code[pos++] = 0x89; code[pos++] = 0x7D; code[pos++] = 0xF8; // mov [rbp-8], rdi
    code[pos++] = 0x48; code[pos++] = 0x89; code[pos++] = 0x75; code[pos++] = 0xF0; // mov [rbp-16], rsi
    code[pos++] = 0x48; code[pos++] = 0x89; code[pos++] = 0x55; code[pos++] = 0xE8; // mov [rbp-24], rdx
    code[pos++] = 0x48; code[pos++] = 0x89; code[pos++] = 0x4D; code[pos++] = 0xE0; // mov [rbp-32], rcx
    code[pos++] = 0x4C; code[pos++] = 0x89; code[pos++] = 0x45; code[pos++] = 0xD8; // mov [rbp-40], r8
    code[pos++] = 0x4C; code[pos++] = 0x89; code[pos++] = 0x4D; code[pos++] = 0xD0; // mov [rbp-48], r9

    // Save float argument registers (XMM0-XMM7)
    // [rbp-0x38] = XMM0
    // [rbp-0x40] = XMM1
    // [rbp-0x48] = XMM2
    // [rbp-0x50] = XMM3
    // [rbp-0x58] = XMM4
    // [rbp-0x60] = XMM5
    // [rbp-0x68] = XMM6
    // [rbp-0x70] = XMM7
    code[pos++] = 0xF2; code[pos++] = 0x0F; code[pos++] = 0x11; code[pos++] = 0x45; code[pos++] = 0xC8; // movsd [rbp-56], xmm0
    code[pos++] = 0xF2; code[pos++] = 0x0F; code[pos++] = 0x11; code[pos++] = 0x4D; code[pos++] = 0xC0; // movsd [rbp-64], xmm1
    code[pos++] = 0xF2; code[pos++] = 0x0F; code[pos++] = 0x11; code[pos++] = 0x55; code[pos++] = 0xB8; // movsd [rbp-72], xmm2
    code[pos++] = 0xF2; code[pos++] = 0x0F; code[pos++] = 0x11; code[pos++] = 0x5D; code[pos++] = 0xB0; // movsd [rbp-80], xmm3
    code[pos++] = 0xF2; code[pos++] = 0x0F; code[pos++] = 0x11; code[pos++] = 0x65; code[pos++] = 0xA8; // movsd [rbp-88], xmm4
    code[pos++] = 0xF2; code[pos++] = 0x0F; code[pos++] = 0x11; code[pos++] = 0x6D; code[pos++] = 0xA0; // movsd [rbp-96], xmm5
    code[pos++] = 0xF2; code[pos++] = 0x0F; code[pos++] = 0x11; code[pos++] = 0x75; code[pos++] = 0x98; // movsd [rbp-104], xmm6
    code[pos++] = 0xF2; code[pos++] = 0x0F; code[pos++] = 0x11; code[pos++] = 0x7D; code[pos++] = 0x90; // movsd [rbp-112], xmm7

    // mov rdi, cb_id (immediate) - first arg for callback_dispatch
    code[pos++] = 0x48; code[pos++] = 0xBF;
    int64_t cb_id_ext_linux = (int64_t)cb_id;
    memcpy(code + pos, &cb_id_ext_linux, sizeof(int64_t));
    pos += 8;

    // lea rsi, [rbp-0x08]  (pointer to saved register state) - second arg
    code[pos++] = 0x48; code[pos++] = 0x8D; code[pos++] = 0x75; code[pos++] = 0xF8;

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

static FFIType parse_ffi_type(const char* name) {
    if (!name) return FFI_TYPE_INT;
    if (strcmp(name, "void") == 0)    return FFI_TYPE_VOID;
    if (strcmp(name, "int") == 0)     return FFI_TYPE_INT;
    if (strcmp(name, "int8") == 0)    return FFI_TYPE_INT8;
    if (strcmp(name, "uint8") == 0 || strcmp(name, "byte") == 0) return FFI_TYPE_UINT8;
    if (strcmp(name, "int16") == 0)   return FFI_TYPE_INT16;
    if (strcmp(name, "uint16") == 0)  return FFI_TYPE_UINT16;
    if (strcmp(name, "int32") == 0)   return FFI_TYPE_INT32;
    if (strcmp(name, "uint32") == 0)  return FFI_TYPE_UINT32;
    if (strcmp(name, "int64") == 0)   return FFI_TYPE_INT;
    if (strcmp(name, "double") == 0)  return FFI_TYPE_DOUBLE;
    if (strcmp(name, "float") == 0)   return FFI_TYPE_FLOAT;
    if (strcmp(name, "pointer") == 0 || strcmp(name, "ptr") == 0) return FFI_TYPE_POINTER;
    if (strcmp(name, "bool") == 0)    return FFI_TYPE_BOOL;
    return FFI_TYPE_INT;
}

static Value ffi_callback_func(int argc, Value* args) {
    (void)argc;
    if (!val_is_obj(args[0]) ||
        (val_as_obj(args[0])->type != OBJ_CLOSURE &&
         val_as_obj(args[0])->type != OBJ_FUNCTION)) {
        native_throw_error("ffi.callback 第一个参数必须是函数");
        return val_null();
    }

    const char* ret_type_str = ((ObjString*)val_as_obj(args[1]))->chars;
    FFIType ret_type = parse_ffi_type(ret_type_str);

    int nargs = 0;
    FFIType arg_types[FFI_MAX_ARGS];
    if (argc > 2 && val_is_obj(args[2]) && val_as_obj(args[2])->type == OBJ_ARRAY) {
        ObjArray* arr = (ObjArray*)val_as_obj(args[2]);
        nargs = arr->count > FFI_MAX_ARGS ? FFI_MAX_ARGS : arr->count;
        for (int i = 0; i < nargs; i++) {
            Value elem = arr->elements[i];
            if (val_is_string(elem)) {
                arg_types[i] = parse_ffi_type(((ObjString*)val_as_obj(elem))->chars);
            } else {
                arg_types[i] = FFI_TYPE_INT;
            }
        }
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
    sig->ret_type = ret_type;
    sig->nargs = nargs;
    memcpy(sig->arg_types, arg_types, sizeof(FFIType) * nargs);

    void* trampoline = create_callback_trampoline(cb_id, ret_type);
    if (!trampoline) {
        free(sig);
        native_throw_error("无法分配可执行内存");
        return val_null();
    }

    g_callback_registry[cb_id].func_val = args[0];
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
    cb->func_val = args[0];

    return val_obj((Object*)cb);
}

/* ==================== 模块初始化 ==================== */

void ffi_init_module(void) {
    /* ===== 库操作函数 ===== */
    TypeKind load_params[] = {TYPE_STRING};
    native_register_module_method("ffi", "load", ffi_load_func, 1, -1, -1, TYPE_PTR, load_params);

    /* ===== 函数调用 ===== */
    TypeKind call_params[] = {TYPE_PTR, TYPE_STRING};
    /* ffi.call 返回 any 类型，但实际值是 int，需要用 _int()、_ptr() 等转换 */
    native_register_module_method("ffi", "call", ffi_call_func, -1, 2, -1, TYPE_ANY, call_params);
    native_register_module_method("ffi", "call_int", ffi_call_int_func, -1, 2, -1, TYPE_INT, call_params);
    native_register_module_method("ffi", "call_double", ffi_call_double_func, -1, 2, -1, TYPE_FLOAT, call_params);
    native_register_module_method("ffi", "call_void", ffi_call_void_func, -1, 2, -1, TYPE_NULL, call_params);
    native_register_module_method("ffi", "call_ptr", ffi_call_ptr_func, -1, 2, -1, TYPE_PTR, call_params);
    native_register_module_method("ffi", "call_bool", ffi_call_bool_func, -1, 2, -1, TYPE_BOOL, call_params);

    /* ===== 内存操作函数 ===== */
    TypeKind malloc_params[] = {TYPE_INT};
    native_register_module_method("ffi", "malloc", ffi_malloc_func, 1, -1, -1, TYPE_PTR, malloc_params);

    TypeKind calloc_params[] = {TYPE_INT, TYPE_INT};
    native_register_module_method("ffi", "calloc", ffi_calloc_func, 2, -1, -1, TYPE_PTR, calloc_params);

    TypeKind realloc_params[] = {TYPE_PTR, TYPE_INT};
    native_register_module_method("ffi", "realloc", ffi_realloc_func, 2, -1, -1, TYPE_PTR, realloc_params);

    TypeKind free_params[] = {TYPE_ANY};
    native_register_module_method("ffi", "free", ffi_free_func, 1, -1, -1, TYPE_NULL, free_params);

    TypeKind sizeof_params[] = {TYPE_PTR};
    native_register_module_method("ffi", "sizeof", ffi_sizeof_func, 1, -1, -1, TYPE_INT, sizeof_params);

    native_register_module_method("ffi", "nullptr", ffi_nullptr_func, 0, -1, -1, TYPE_PTR, NULL);

    TypeKind ptr_from_int_params[] = {TYPE_INT};
    native_register_module_method("ffi", "ptr_from_int", ffi_ptr_from_int_func, 1, -1, -1, TYPE_PTR, ptr_from_int_params);

    TypeKind ptr_to_int_params[] = {TYPE_PTR};
    native_register_module_method("ffi", "ptr_to_int", ffi_ptr_to_int_func, 1, -1, -1, TYPE_INT, ptr_to_int_params);

    TypeKind is_ptr_params[] = {TYPE_ANY};
    native_register_module_method("ffi", "is_ptr", ffi_is_ptr_func, 1, -1, -1, TYPE_BOOL, is_ptr_params);

    TypeKind offset_params[] = {TYPE_PTR, TYPE_INT};
    native_register_module_method("ffi", "offset", ffi_offset_func, 2, -1, -1, TYPE_PTR, offset_params);

    /* ===== 读取函数 ===== */
    TypeKind read_params[] = {TYPE_PTR, TYPE_INT};  // ptr, offset
    native_register_module_method("ffi", "read_byte",    ffi_read_byte_func,    2, -1, -1, TYPE_INT, read_params);
    native_register_module_method("ffi", "read_int8",    ffi_read_int8_func,    2, -1, -1, TYPE_INT, read_params);
    native_register_module_method("ffi", "read_int16",   ffi_read_int16_func,   2, -1, -1, TYPE_INT, read_params);
    native_register_module_method("ffi", "read_uint16",  ffi_read_uint16_func,  2, -1, -1, TYPE_INT, read_params);
    native_register_module_method("ffi", "read_int",     ffi_read_int_func,     2, -1, -1, TYPE_INT, read_params);
    native_register_module_method("ffi", "read_uint",    ffi_read_uint_func,    2, -1, -1, TYPE_INT, read_params);
    native_register_module_method("ffi", "read_int64",   ffi_read_int64_func,   2, -1, -1, TYPE_INT, read_params);
    native_register_module_method("ffi", "read_uint64",  ffi_read_uint64_func,  2, -1, -1, TYPE_INT, read_params);
    native_register_module_method("ffi", "read_float",   ffi_read_float_func,   2, -1, -1, TYPE_FLOAT, read_params);
    native_register_module_method("ffi", "read_double",  ffi_read_double_func,  2, -1, -1, TYPE_FLOAT, read_params);
    native_register_module_method("ffi", "read_ptr",     ffi_read_ptr_func,     2, -1, -1, TYPE_PTR, read_params);
    native_register_module_method("ffi", "read_bool",    ffi_read_bool_func,    2, -1, -1, TYPE_BOOL, read_params);
    native_register_module_method("ffi", "read_string",  ffi_read_string_func,  2, -1, -1, TYPE_STRING, read_params);

    TypeKind read_str_n_params[] = {TYPE_PTR, TYPE_INT, TYPE_INT};  // ptr, offset, length
    native_register_module_method("ffi", "read_string_n", ffi_read_string_n_func, 3, -1, -1, TYPE_STRING, read_str_n_params);

    /* ===== 写入函数 ===== */
    TypeKind write_byte_params[] = {TYPE_PTR, TYPE_INT, TYPE_INT};  // ptr, offset, value
    native_register_module_method("ffi", "write_byte",   ffi_write_byte_func,   3, -1, -1, TYPE_NULL, write_byte_params);
    native_register_module_method("ffi", "write_int8",   ffi_write_int8_func,   3, -1, -1, TYPE_NULL, write_byte_params);
    native_register_module_method("ffi", "write_int16",  ffi_write_int16_func,  3, -1, -1, TYPE_NULL, write_byte_params);
    native_register_module_method("ffi", "write_int",    ffi_write_int_func,    3, -1, -1, TYPE_NULL, write_byte_params);
    native_register_module_method("ffi", "write_uint",   ffi_write_uint_func,   3, -1, -1, TYPE_NULL, write_byte_params);
    native_register_module_method("ffi", "write_int64",  ffi_write_int64_func,  3, -1, -1, TYPE_NULL, write_byte_params);
    native_register_module_method("ffi", "write_uint64", ffi_write_uint64_func, 3, -1, -1, TYPE_NULL, write_byte_params);

    TypeKind write_float_params[] = {TYPE_PTR, TYPE_INT, TYPE_FLOAT};  // ptr, offset, value
    native_register_module_method("ffi", "write_float",  ffi_write_float_func,  3, -1, -1, TYPE_NULL, write_float_params);
    native_register_module_method("ffi", "write_double", ffi_write_double_func, 3, -1, -1, TYPE_NULL, write_float_params);

    TypeKind write_ptr_params[] = {TYPE_PTR, TYPE_INT, TYPE_PTR};  // ptr, offset, value
    native_register_module_method("ffi", "write_ptr",    ffi_write_ptr_func,    3, -1, -1, TYPE_NULL, write_ptr_params);

    TypeKind write_str_params[] = {TYPE_PTR, TYPE_INT, TYPE_STRING};  // ptr, offset, string
    native_register_module_method("ffi", "write_string", ffi_write_string_func, 3, -1, -1, TYPE_NULL, write_str_params);

    /* ===== 字符串工具函数 ===== */
    TypeKind string_params[] = {TYPE_STRING};
    native_register_module_method("ffi", "string_bytes", ffi_string_bytes_func, 1, -1, -1, TYPE_INT, string_params);

    TypeKind memcpy_params[] = {TYPE_PTR, TYPE_PTR, TYPE_INT};
    native_register_module_method("ffi", "memcpy", ffi_memcpy_func, 3, -1, -1, TYPE_NULL, memcpy_params);

    TypeKind memset_params[] = {TYPE_PTR, TYPE_INT, TYPE_INT};
    native_register_module_method("ffi", "memset", ffi_memset_func, 3, -1, -1, TYPE_NULL, memset_params);

#ifdef _WIN32
    /* ===== 宽字符转换函数 (Windows) ===== */
    native_register_module_method("ffi", "utf8_to_utf16", ffi_utf8_to_utf16_func, 1, -1, -1, TYPE_PTR, string_params);
    TypeKind ptr_params[] = {TYPE_PTR};
    native_register_module_method("ffi", "utf16_to_utf8", ffi_utf16_to_utf8_func, 1, -1, -1, TYPE_STRING, ptr_params);
#endif // _WIN32

    /* ===== 类型信息函数 ===== */
    TypeKind type_name_params[] = {TYPE_STRING};
    native_register_module_method("ffi", "sizeof_type", ffi_sizeof_type_func, 1, -1, -1, TYPE_INT, type_name_params);
    native_register_module_method("ffi", "alignof",     ffi_alignof_func,     1, -1, -1, TYPE_INT, type_name_params);

    /* ===== 回调函数 ===== */
    TypeKind callback_params[] = {TYPE_ANY, TYPE_STRING};
    native_register_module_method("ffi", "callback", ffi_callback_func, -1, 2, -1, TYPE_PTR, callback_params);
}
