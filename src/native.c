#include "include/native.h"
#include "include/leno_vm.h"
#include "include/platform_thread.h"
#include <string.h>

// 直接使用全局 VM（主程序效率第一）
extern VM vm;

// 模块别名最大数量
#define MAX_MODULE_ALIASES 64

// ============================================================================
// 内部模块名称列表
// ============================================================================

static const char* builtin_module_names[] = {
    "io", "times", "arrays", "strings", "maths", "rands",
    "files", "asyncs", "dirs", "jsons", "sockets", "ffi", "threads", "regexs",
    NULL
};

// 检查名称是否是内部模块名称
bool native_is_builtin_module(const char* name) {
    for (int i = 0; builtin_module_names[i] != NULL; i++) {
        if (strcmp(name, builtin_module_names[i]) == 0) {
            return true;
        }
    }
    return false;
}

// 模块初始化函数前向声明
extern void io_init_module(void);
extern void times_init_module(void);
extern void arrays_init_module(void);
extern void strings_init_module(void);
extern void maths_init_module(void);
extern void rands_init_module(void);
extern void files_init_module(void);
extern void asyncs_init_module(void);
extern void dirs_init_module(void);
extern void jsons_init_module(void);
extern void sockets_init_module(void);
extern void ffi_init_module(void);
extern void threads_init_module(void);
extern void regexs_init_module(void);

// 模块初始化函数映射表
typedef void (*ModuleInitFunc)(void);

typedef struct {
    const char* name;
    ModuleInitFunc init_func;
} ModuleInitEntry;

static ModuleInitEntry module_init_table[] = {
    {"io", io_init_module},
    {"times", times_init_module},
    {"arrays", arrays_init_module},
    {"strings", strings_init_module},
    {"maths", maths_init_module},
    {"rands", rands_init_module},
    {"files", files_init_module},
    {"asyncs", asyncs_init_module},
    {"dirs", dirs_init_module},
    {"jsons", jsons_init_module},
    {"sockets", sockets_init_module},
    {"ffi", ffi_init_module},
    {"threads", threads_init_module},
    {"regexs", regexs_init_module},
    {NULL, NULL}
};

// 全局函数初始化前向声明
extern void io_init_globals(void);
extern void types_init_globals(void);
extern void strings_init_globals(void);
extern void times_init_globals(void);
extern void threads_init_globals(void);
extern void sys_init_globals(void);

// ============================================================================
// 哈希表工具函数（FNV-1a算法）
// ============================================================================

static uint32_t native_hash_string(const char* str) {
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= (unsigned char)(*str);
        hash *= 16777619;
        str++;
    }
    return hash;
}

// 组合两个字符串的哈希（用于模块方法：模块名+方法名）
static uint32_t hash_module_method(const char* module_name, const char* method_name) {
    uint32_t hash = native_hash_string(module_name);
    // 混合方法名哈希
    while (*method_name) {
        hash ^= (unsigned char)(*method_name);
        hash *= 16777619;
        method_name++;
    }
    return hash;
}

// ============================================================================
// 模块方法哈希表
// ============================================================================

#define MODULE_METHOD_TABLE_INITIAL_CAPACITY 64
#define MODULE_METHOD_TABLE_MAX_LOAD 0.75

typedef struct ModuleMethodEntry {
    char module_name[32];
    char method_name[32];
    ModuleMethodMeta meta;
    struct ModuleMethodEntry* next;
} ModuleMethodEntry;

typedef struct {
    ModuleMethodEntry** entries;
    int capacity;
    int count;
} ModuleMethodTable;

static THREAD_LOCAL ModuleMethodTable moduleMethodTable = {NULL, 0, 0};

// 初始化模块方法表
static void module_method_table_init(void) {
    moduleMethodTable.capacity = MODULE_METHOD_TABLE_INITIAL_CAPACITY;
    moduleMethodTable.count = 0;
    moduleMethodTable.entries = (ModuleMethodEntry**)calloc(moduleMethodTable.capacity, sizeof(ModuleMethodEntry*));
}

// 释放模块方法表
static void module_method_table_free(void) {
    if (!moduleMethodTable.entries) return;
    
    for (int i = 0; i < moduleMethodTable.capacity; i++) {
        ModuleMethodEntry* entry = moduleMethodTable.entries[i];
        while (entry) {
            ModuleMethodEntry* next = entry->next;
            free(entry);
            entry = next;
        }
    }
    free(moduleMethodTable.entries);
    moduleMethodTable.entries = NULL;
    moduleMethodTable.capacity = 0;
    moduleMethodTable.count = 0;
}

// 扩容模块方法表
static void module_method_table_resize(void) {
    int old_capacity = moduleMethodTable.capacity;
    ModuleMethodEntry** old_entries = moduleMethodTable.entries;
    
    int new_capacity = old_capacity * 2;
    ModuleMethodEntry** new_entries = (ModuleMethodEntry**)calloc(new_capacity, sizeof(ModuleMethodEntry*));
    if (!new_entries) return;
    
    // 重新哈希
    for (int i = 0; i < old_capacity; i++) {
        ModuleMethodEntry* entry = old_entries[i];
        while (entry) {
            ModuleMethodEntry* next = entry->next;
            uint32_t hash = hash_module_method(entry->module_name, entry->method_name);
            int index = hash & (new_capacity - 1);
            entry->next = new_entries[index];
            new_entries[index] = entry;
            entry = next;
        }
    }
    
    free(old_entries);
    moduleMethodTable.entries = new_entries;
    moduleMethodTable.capacity = new_capacity;
}

// ============================================================================
// 模块常量哈希表（原生模块导出的 int 常量）
// ============================================================================

typedef struct ModuleConstEntry {
    char module_name[32];
    char const_name[64];
    int value;
    struct ModuleConstEntry* next;
} ModuleConstEntry;

typedef struct {
    ModuleConstEntry** entries;
    int capacity;
    int count;
} ModuleConstTable;

static THREAD_LOCAL ModuleConstTable moduleConstTable = {NULL, 0, 0};

static uint32_t hash_module_const(const char* module_name, const char* const_name) {
    uint32_t hash = native_hash_string(module_name);
    while (*const_name) {
        hash ^= (unsigned char)(*const_name);
        hash *= 16777619;
        const_name++;
    }
    return hash;
}

static void module_const_table_init(void) {
    moduleConstTable.capacity = MODULE_METHOD_TABLE_INITIAL_CAPACITY;
    moduleConstTable.count = 0;
    moduleConstTable.entries = (ModuleConstEntry**)calloc(moduleConstTable.capacity, sizeof(ModuleConstEntry*));
}

static void module_const_table_free(void) {
    if (!moduleConstTable.entries) return;
    for (int i = 0; i < moduleConstTable.capacity; i++) {
        ModuleConstEntry* entry = moduleConstTable.entries[i];
        while (entry) {
            ModuleConstEntry* next = entry->next;
            free(entry);
            entry = next;
        }
    }
    free(moduleConstTable.entries);
    moduleConstTable.entries = NULL;
    moduleConstTable.capacity = 0;
    moduleConstTable.count = 0;
}

static void module_const_table_resize(void) {
    int old_capacity = moduleConstTable.capacity;
    ModuleConstEntry** old_entries = moduleConstTable.entries;
    
    int new_capacity = old_capacity * 2;
    ModuleConstEntry** new_entries = (ModuleConstEntry**)calloc(new_capacity, sizeof(ModuleConstEntry*));
    if (!new_entries) return;
    
    for (int i = 0; i < old_capacity; i++) {
        ModuleConstEntry* entry = old_entries[i];
        while (entry) {
            ModuleConstEntry* next = entry->next;
            uint32_t hash = hash_module_const(entry->module_name, entry->const_name);
            int index = hash & (new_capacity - 1);
            entry->next = new_entries[index];
            new_entries[index] = entry;
            entry = next;
        }
    }
    
    free(old_entries);
    moduleConstTable.entries = new_entries;
    moduleConstTable.capacity = new_capacity;
}

void native_register_module_const(const char* module_name, const char* const_name, int value) {
    if (!moduleConstTable.entries) {
        module_const_table_init();
    }
    
    if (moduleConstTable.count >= moduleConstTable.capacity * MODULE_METHOD_TABLE_MAX_LOAD) {
        module_const_table_resize();
    }
    
    uint32_t hash = hash_module_const(module_name, const_name);
    int index = hash & (moduleConstTable.capacity - 1);
    
    // 检查是否已存在（更新值）
    ModuleConstEntry* entry = moduleConstTable.entries[index];
    while (entry) {
        if (strcmp(entry->module_name, module_name) == 0 &&
            strcmp(entry->const_name, const_name) == 0) {
            entry->value = value;
            return;
        }
        entry = entry->next;
    }
    
    // 创建新条目
    ModuleConstEntry* new_entry = (ModuleConstEntry*)malloc(sizeof(ModuleConstEntry));
    if (!new_entry) return;
    
    int mod_len = strlen(module_name);
    int const_len = strlen(const_name);
    if (mod_len > 31) mod_len = 31;
    if (const_len > 63) const_len = 63;
    
    memcpy(new_entry->module_name, module_name, mod_len);
    new_entry->module_name[mod_len] = '\0';
    
    memcpy(new_entry->const_name, const_name, const_len);
    new_entry->const_name[const_len] = '\0';
    
    new_entry->value = value;
    
    new_entry->next = moduleConstTable.entries[index];
    moduleConstTable.entries[index] = new_entry;
    moduleConstTable.count++;
}

int native_find_module_const(const char* module_name, const char* const_name, bool* found) {
    if (found) *found = false;
    if (!moduleConstTable.entries || moduleConstTable.count == 0) return 0;
    
    uint32_t hash = hash_module_const(module_name, const_name);
    int index = hash & (moduleConstTable.capacity - 1);
    
    ModuleConstEntry* entry = moduleConstTable.entries[index];
    while (entry) {
        if (strcmp(entry->module_name, module_name) == 0 &&
            strcmp(entry->const_name, const_name) == 0) {
            if (found) *found = true;
            return entry->value;
        }
        entry = entry->next;
    }
    return 0;
}

char** native_get_module_consts(const char* module_name, int* count) {
    if (!moduleConstTable.entries || moduleConstTable.count == 0 || !module_name || !count) {
        if (count) *count = 0;
        return NULL;
    }
    
    int const_count = 0;
    for (int i = 0; i < moduleConstTable.capacity; i++) {
        ModuleConstEntry* entry = moduleConstTable.entries[i];
        while (entry) {
            if (strcmp(entry->module_name, module_name) == 0) {
                const_count++;
            }
            entry = entry->next;
        }
    }
    
    if (const_count == 0) {
        *count = 0;
        return NULL;
    }
    
    char** consts = (char**)malloc(sizeof(char*) * const_count);
    if (!consts) {
        *count = 0;
        return NULL;
    }
    
    int idx = 0;
    for (int i = 0; i < moduleConstTable.capacity && idx < const_count; i++) {
        ModuleConstEntry* entry = moduleConstTable.entries[i];
        while (entry && idx < const_count) {
            if (strcmp(entry->module_name, module_name) == 0) {
                consts[idx] = strdup(entry->const_name);
                idx++;
            }
            entry = entry->next;
        }
    }
    
    *count = const_count;
    return consts;
}

void native_free_module_const_list(char** consts, int count) {
    if (!consts) return;
    for (int i = 0; i < count; i++) {
        free(consts[i]);
    }
    free(consts);
}

// 编译时和运行时共用的元信息表
#define MAX_NATIVE_FUNCTIONS 128

static THREAD_LOCAL NativeFunctionMeta functionRegistry[MAX_NATIVE_FUNCTIONS];
static THREAD_LOCAL int functionCount = 0;

// 运行时 native 函数对象表
static THREAD_LOCAL ObjNative* nativeFunctionObjects[MAX_NATIVE_FUNCTIONS];
static THREAD_LOCAL int nativeFunctionObjectCount = 0;

// 标记所有 native 函数对象（供 GC 使用）
void native_mark_all_functions(void) {
    for (int i = 0; i < nativeFunctionObjectCount; i++) {
        if (nativeFunctionObjects[i]) {
            gc_mark_object((Object*)nativeFunctionObjects[i]);
        }
    }
}

// 模块别名注册表
typedef struct {
    char alias[32];
    char module_name[32];
} ModuleAlias;

static THREAD_LOCAL ModuleAlias moduleAliases[MAX_MODULE_ALIASES];
static THREAD_LOCAL int moduleAliasCount = 0;

// 编译时注册 native 函数元信息
// min_arity/max_arity: 当 arity == -1（可变参数）时，指定最小/最大允许参数个数；其他情况传 -1
void native_register_meta(const char* name, int arity, int min_arity, int max_arity, TypeKind return_type, TypeKind return_element_type, TypeKind* param_types) {
    if (functionCount >= MAX_NATIVE_FUNCTIONS) return;

    // 检查是否已存在同名函数
    for (int i = 0; i < functionCount; i++) {
        if (strcmp(functionRegistry[i].name, name) == 0) {
            return;  // 已存在，直接返回
        }
    }

    NativeFunctionMeta* meta = &functionRegistry[functionCount++];
    meta->name = name;  // 注意：这里假设 name 是静态字符串
    meta->arity = arity;
    meta->min_arity = min_arity;
    meta->max_arity = max_arity;
    meta->return_type = return_type;
    meta->return_element_type = return_element_type;

    // 复制参数类型
    if (param_types && arity > 0) {
        int count = arity < MAX_METHOD_PARAMS ? arity : MAX_METHOD_PARAMS;
        for (int i = 0; i < count; i++) {
            meta->param_types[i] = param_types[i];
        }
        for (int i = count; i < MAX_METHOD_PARAMS; i++) {
            meta->param_types[i] = TYPE_ANY;
        }
    } else {
        for (int i = 0; i < MAX_METHOD_PARAMS; i++) {
            meta->param_types[i] = TYPE_ANY;
        }
    }
}

// 获取所有注册的 native 函数
const NativeFunctionMeta* native_get_all_functions(int* count) {
    *count = functionCount;
    return functionRegistry;
}

// 根据函数名获取返回类型
TypeKind native_get_return_type(const char* name) {
    for (int i = 0; i < functionCount; i++) {
        if (strcmp(functionRegistry[i].name, name) == 0) {
            return functionRegistry[i].return_type;
        }
    }
    return TYPE_ANY;
}

// 获取全局函数的返回数组元素类型
TypeKind native_get_return_element_type(const char* name) {
    for (int i = 0; i < functionCount; i++) {
        if (strcmp(functionRegistry[i].name, name) == 0) {
            return functionRegistry[i].return_element_type;
        }
    }
    return TYPE_UNKNOWN;
}

// 获取全局函数的参数类型
TypeKind native_get_global_function_param_type(const char* name, int param_index) {
    for (int i = 0; i < functionCount; i++) {
        if (strcmp(functionRegistry[i].name, name) == 0) {
            if (param_index >= 0 && param_index < functionRegistry[i].arity && param_index < MAX_METHOD_PARAMS) {
                return functionRegistry[i].param_types[param_index];
            }
            break;
        }
    }
    return TYPE_ANY;
}

// 重置注册表（编译前调用）
void native_reset_registry(void) {
    functionCount = 0;
    nativeFunctionObjectCount = 0;
    module_method_table_free();
    module_const_table_free();
    moduleAliasCount = 0;
}

// 创建 Native 函数对象
static ObjNative* new_native(NativeFn function, const char* name, int arity) {
    ObjNative* native = (ObjNative*)gc_alloc(sizeof(ObjNative), OBJ_NATIVE);
    if (!native) return NULL;

    native->function = function;
    native->arity = arity;

    // 复制名称
    int nameLen = (int)strlen(name);
    native->name = (char*)malloc(nameLen + 1);
    if (native->name) {
        memcpy(native->name, name, nameLen + 1);
    }

    return native;
}

// 注册全局 Native 函数（内部使用）
static void register_native_internal(const char* name, NativeFn function, int arity) {
    // 创建 native 函数对象
    ObjNative* native = new_native(function, name, arity);
    if (!native) return;

    // 添加到 native 函数对象表
    if (nativeFunctionObjectCount < MAX_NATIVE_FUNCTIONS) {
        nativeFunctionObjects[nativeFunctionObjectCount++] = native;
    }
}

// 运行时注册 native 函数
// min_arity/max_arity: 当 arity == -1（可变参数）时，指定最小/最大允许参数个数；其他情况传 -1
void vm_register_native(const char* name, NativeFn function, int arity, int min_arity, int max_arity, TypeKind return_type, TypeKind return_element_type, TypeKind* param_types) {
    // 先注册元信息（编译时和运行时都需要）
    native_register_meta(name, arity, min_arity, max_arity, return_type, return_element_type, param_types);

    // 如果 VM 已初始化，注册函数到 VM
    extern int vm_initialized;
    if (vm_initialized) {
        register_native_internal(name, function, arity);
    }
}

// 获取 native 函数的返回类型（运行时使用）
TypeKind vm_get_native_return_type(const char* name) {
    return native_get_return_type(name);
}

// 根据名称查找 native 函数对象（运行时使用）
ObjNative* native_find_function(const char* name) {
    for (int i = 0; i < nativeFunctionObjectCount; i++) {
        if (nativeFunctionObjects[i] && 
            strcmp(nativeFunctionObjects[i]->name, name) == 0) {
            return nativeFunctionObjects[i];
        }
    }
    return NULL;
}

// 注册所有全局模块的 native 函数元信息（编译时调用）
void native_register_all_module_metas(void) {
    // 调用 io 模块的初始化函数来注册元信息
    // vm_register_native 内部会调用 native_register_meta 注册元信息
    extern void io_init_globals(void);
    io_init_globals();
    // 调用 types 模块的初始化函数来注册元信息
    extern void types_init_globals(void);
    types_init_globals();
    // 调用 strings 模块的初始化函数来注册元信息
    extern void strings_init_globals(void);
    strings_init_globals();
    // 调用 times 模块初始化函数注册 sleep 元信息
    extern void times_init_globals(void);
    times_init_globals();
    // 调用 threads 模块初始化函数注册线程元信息
    extern void threads_init_globals(void);
    threads_init_globals();
    extern void assert_init_globals(void);
    assert_init_globals();
    extern void sys_init_globals(void);
    sys_init_globals();

    // 初始化模块方法表（用于编译期类型推断）
    // 注意：这里只注册方法元信息，不创建函数对象（运行时再做）
    extern void io_init_module(void);
    io_init_module();
    extern void times_init_module(void);
    times_init_module();
    extern void strings_init_module(void);
    strings_init_module();
    extern void maths_init_module(void);
    maths_init_module();
    extern void arrays_init_module(void);
    arrays_init_module();
    extern void rands_init_module(void);
    rands_init_module();
    extern void files_init_module(void);
    files_init_module();
    extern void asyncs_init_module(void);
    asyncs_init_module();
    extern void dirs_init_module(void);
    dirs_init_module();
    extern void jsons_init_module(void);
    jsons_init_module();
    extern void sockets_init_module(void);
    sockets_init_module();
    extern void ffi_init_module(void);
    ffi_init_module();
    extern void threads_init_module(void);
    threads_init_module();
    extern void regexs_init_module(void);
    regexs_init_module();
}

// ============================================================================
// 模块方法支持（哈希表实现 - O(1) 查找）
// ============================================================================

// 注册模块方法（带参数类型）
// min_arity/max_arity: 当 arity == -1（可变参数）时，指定最小/最大允许参数个数；其他情况传 -1
// param_types: 参数类型数组，长度为 arity，如果为 NULL 则所有参数默认为 TYPE_ANY
// return_element_type: 返回数组时的元素类型，非数组返回类型时传 TYPE_UNKNOWN
void native_register_module_method(const char* module_name, const char* method_name,
                                   NativeFn function, int arity, int min_arity, int max_arity,
                                   TypeKind return_type, TypeKind return_element_type, TypeKind* param_types) {
    if (!moduleMethodTable.entries) {
        module_method_table_init();
    }

    // 检查是否需要扩容
    if (moduleMethodTable.count >= moduleMethodTable.capacity * MODULE_METHOD_TABLE_MAX_LOAD) {
        module_method_table_resize();
    }

    // 计算哈希索引
    uint32_t hash = hash_module_method(module_name, method_name);
    int index = hash & (moduleMethodTable.capacity - 1);

    // 检查是否已存在
    ModuleMethodEntry* entry = moduleMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->module_name, module_name) == 0 &&
            strcmp(entry->method_name, method_name) == 0) {
            // 已存在，更新
            entry->meta.function = function;
            entry->meta.arity = arity;
            entry->meta.min_arity = min_arity;
            entry->meta.max_arity = max_arity;
            entry->meta.return_type = return_type;
            entry->meta.return_element_type = return_element_type;
            if (param_types && arity > 0) {
                int count = arity < MAX_METHOD_PARAMS ? arity : MAX_METHOD_PARAMS;
                for (int i = 0; i < count; i++) {
                    entry->meta.param_types[i] = param_types[i];
                }
                for (int i = count; i < MAX_METHOD_PARAMS; i++) {
                    entry->meta.param_types[i] = TYPE_ANY;
                }
            } else {
                for (int i = 0; i < MAX_METHOD_PARAMS; i++) {
                    entry->meta.param_types[i] = TYPE_ANY;
                }
            }
            return;
        }
        entry = entry->next;
    }

    // 创建新条目
    ModuleMethodEntry* new_entry = (ModuleMethodEntry*)malloc(sizeof(ModuleMethodEntry));
    if (!new_entry) return;

    // 复制模块名和方法名
    int mod_len = strlen(module_name);
    int meth_len = strlen(method_name);
    if (mod_len > 31) mod_len = 31;
    if (meth_len > 31) meth_len = 31;

    memcpy(new_entry->module_name, module_name, mod_len);
    new_entry->module_name[mod_len] = '\0';

    memcpy(new_entry->method_name, method_name, meth_len);
    new_entry->method_name[meth_len] = '\0';

    // 同时复制到 meta 中（LSP 使用）
    memcpy(new_entry->meta.module_name, module_name, mod_len);
    new_entry->meta.module_name[mod_len] = '\0';

    memcpy(new_entry->meta.method_name, method_name, meth_len);
    new_entry->meta.method_name[meth_len] = '\0';

    new_entry->meta.function = function;
    new_entry->meta.arity = arity;
    new_entry->meta.min_arity = min_arity;
    new_entry->meta.max_arity = max_arity;
    new_entry->meta.return_type = return_type;
    new_entry->meta.return_element_type = return_element_type;

    // 复制参数类型
    if (param_types && arity > 0) {
        int count = arity < MAX_METHOD_PARAMS ? arity : MAX_METHOD_PARAMS;
        for (int i = 0; i < count; i++) {
            new_entry->meta.param_types[i] = param_types[i];
        }
        for (int i = count; i < MAX_METHOD_PARAMS; i++) {
            new_entry->meta.param_types[i] = TYPE_ANY;
        }
    } else {
        for (int i = 0; i < MAX_METHOD_PARAMS; i++) {
            new_entry->meta.param_types[i] = TYPE_ANY;
        }
    }

    // 插入到哈希表
    new_entry->next = moduleMethodTable.entries[index];
    moduleMethodTable.entries[index] = new_entry;
    moduleMethodTable.count++;
}

// 根据模块名和方法名查找模块方法（O(1)）
ModuleMethodMeta* native_find_module_method(const char* module_name, const char* method_name) {
    if (!moduleMethodTable.entries || moduleMethodTable.count == 0) {
        return NULL;
    }
    
    uint32_t hash = hash_module_method(module_name, method_name);
    int index = hash & (moduleMethodTable.capacity - 1);
    
    ModuleMethodEntry* entry = moduleMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->module_name, module_name) == 0 &&
            strcmp(entry->method_name, method_name) == 0) {
            return &entry->meta;
        }
        entry = entry->next;
    }
    
    return NULL;
}

// 获取模块方法的返回类型
TypeKind native_get_module_method_return_type(const char* module_name, const char* method_name) {
    ModuleMethodMeta* meta = native_find_module_method(module_name, method_name);
    if (meta) return meta->return_type;
    return TYPE_ANY;
}

// 获取模块方法返回数组时的元素类型（编译时调用）
TypeKind native_get_module_method_return_element_type(const char* module_name, const char* method_name) {
    ModuleMethodMeta* meta = native_find_module_method(module_name, method_name);
    if (meta) return meta->return_element_type;
    return TYPE_UNKNOWN;
}

// 获取模块方法的参数数量
int native_get_module_method_arity(const char* module_name, const char* method_name) {
    ModuleMethodMeta* meta = native_find_module_method(module_name, method_name);
    if (meta) {
        return meta->arity;
    }
    return -1;
}

// 获取模块方法的参数类型
TypeKind native_get_module_method_param_type(const char* module_name, const char* method_name, int param_index) {
    ModuleMethodMeta* meta = native_find_module_method(module_name, method_name);
    if (meta && param_index >= 0 && param_index < meta->arity && param_index < MAX_METHOD_PARAMS) {
        return meta->param_types[param_index];
    }
    return TYPE_ANY;
}

// 获取模块的所有方法名（LSP 使用）
// 返回方法名数组，通过 count 返回数量，需要调用者用 free_module_method_list 释放
char** native_get_module_methods(const char* module_name, int* count) {
    if (!moduleMethodTable.entries || moduleMethodTable.count == 0 || !module_name || !count) {
        if (count) *count = 0;
        return NULL;
    }
    
    // 先统计该模块的方法数量
    int method_count = 0;
    for (int i = 0; i < moduleMethodTable.capacity; i++) {
        ModuleMethodEntry* entry = moduleMethodTable.entries[i];
        while (entry) {
            if (strcmp(entry->module_name, module_name) == 0) {
                method_count++;
            }
            entry = entry->next;
        }
    }
    
    if (method_count == 0) {
        *count = 0;
        return NULL;
    }
    
    // 分配数组
    char** methods = (char**)malloc(sizeof(char*) * method_count);
    if (!methods) {
        *count = 0;
        return NULL;
    }
    
    // 填充方法名
    int idx = 0;
    for (int i = 0; i < moduleMethodTable.capacity && idx < method_count; i++) {
        ModuleMethodEntry* entry = moduleMethodTable.entries[i];
        while (entry && idx < method_count) {
            if (strcmp(entry->module_name, module_name) == 0) {
                methods[idx] = strdup(entry->method_name);
                idx++;
            }
            entry = entry->next;
        }
    }
    
    *count = method_count;
    return methods;
}

// 释放模块方法名列表
void native_free_module_method_list(char** methods, int count) {
    if (!methods) return;
    for (int i = 0; i < count; i++) {
        free(methods[i]);
    }
    free(methods);
}

// 获取所有模块名称列表（LSP 使用）
// 返回模块名数组，通过 count 返回数量，需要调用者用 native_free_module_list 释放
char** native_get_all_modules(int* count) {
    if (!moduleMethodTable.entries || moduleMethodTable.count == 0 || !count) {
        if (count) *count = 0;
        return NULL;
    }

    // 使用哈希表去重，统计模块数量
    char** unique_modules = (char**)calloc(moduleMethodTable.count, sizeof(char*));
    if (!unique_modules) {
        *count = 0;
        return NULL;
    }

    int module_count = 0;
    for (int i = 0; i < moduleMethodTable.capacity; i++) {
        ModuleMethodEntry* entry = moduleMethodTable.entries[i];
        while (entry) {
            // 检查是否已存在
            bool found = false;
            for (int j = 0; j < module_count; j++) {
                if (strcmp(unique_modules[j], entry->meta.module_name) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                unique_modules[module_count] = strdup(entry->meta.module_name);
                if (unique_modules[module_count]) {
                    module_count++;
                }
            }
            entry = entry->next;
        }
    }

    if (module_count == 0) {
        free(unique_modules);
        *count = 0;
        return NULL;
    }

    *count = module_count;
    return unique_modules;
}

// 释放模块名列表
void native_free_module_list(char** modules, int count) {
    if (!modules) return;
    for (int i = 0; i < count; i++) {
        free(modules[i]);
    }
    free(modules);
}

// 获取模块的所有方法元数据（LSP 使用）
// 返回 ModuleMethodMeta 数组的副本，通过 count 返回数量，需要调用者释放
ModuleMethodMeta* native_get_module_method_metas(const char* module_name, int* count) {
    if (!moduleMethodTable.entries || moduleMethodTable.count == 0 || !module_name || !count) {
        if (count) *count = 0;
        return NULL;
    }

    // 先统计该模块的方法数量
    int method_count = 0;
    for (int i = 0; i < moduleMethodTable.capacity; i++) {
        ModuleMethodEntry* entry = moduleMethodTable.entries[i];
        while (entry) {
            if (strcmp(entry->module_name, module_name) == 0) {
                method_count++;
            }
            entry = entry->next;
        }
    }

    if (method_count == 0) {
        *count = 0;
        return NULL;
    }

    // 分配数组
    ModuleMethodMeta* metas = (ModuleMethodMeta*)malloc(sizeof(ModuleMethodMeta) * method_count);
    if (!metas) {
        *count = 0;
        return NULL;
    }

    // 填充元数据
    int idx = 0;
    for (int i = 0; i < moduleMethodTable.capacity && idx < method_count; i++) {
        ModuleMethodEntry* entry = moduleMethodTable.entries[i];
        while (entry && idx < method_count) {
            if (strcmp(entry->module_name, module_name) == 0) {
                memcpy(&metas[idx], &entry->meta, sizeof(ModuleMethodMeta));
                idx++;
            }
            entry = entry->next;
        }
    }

    *count = method_count;
    return metas;
}

// 释放模块方法元数据数组
void native_free_module_method_metas(ModuleMethodMeta* metas) {
    free(metas);
}

// ============================================================================
// 模块别名支持
// ============================================================================

// 注册模块别名
void native_register_module_alias(const char* alias, const char* module_name) {
    if (moduleAliasCount >= MAX_MODULE_ALIASES) return;
    
    ModuleAlias* ma = &moduleAliases[moduleAliasCount++];
    
    int alias_len = strlen(alias);
    int mod_len = strlen(module_name);
    if (alias_len > 31) alias_len = 31;
    if (mod_len > 31) mod_len = 31;
    
    memcpy(ma->alias, alias, alias_len);
    ma->alias[alias_len] = '\0';
    
    memcpy(ma->module_name, module_name, mod_len);
    ma->module_name[mod_len] = '\0';
}

// 根据别名查找实际模块名
const char* native_resolve_module_alias(const char* alias) {
    for (int i = 0; i < moduleAliasCount; i++) {
        if (strcmp(moduleAliases[i].alias, alias) == 0) {
            return moduleAliases[i].module_name;
        }
    }
    return alias;  // 如果没有找到别名映射，返回原名称
}

// 重置别名表
void native_reset_module_aliases(void) {
    moduleAliasCount = 0;
}

// ============================================================================
// 统一模块初始化
// ============================================================================

// 根据模块名初始化对应的模块
void native_init_module(const char* module_name) {
    for (int i = 0; module_init_table[i].name != NULL; i++) {
        if (strcmp(module_name, module_init_table[i].name) == 0) {
            module_init_table[i].init_func();
            return;
        }
    }
    // 未找到对应模块，不执行任何操作
}

// 注册所有内置 Native 函数（全局函数）
void native_register_globals(void) {
    io_init_globals();
    
    types_init_globals();
    
    strings_init_globals();
    
    times_init_globals();
    
    threads_init_globals();
    
    extern void asyncs_init_globals(void);
    asyncs_init_globals();

    extern void assert_init_globals(void);
    assert_init_globals();

    sys_init_globals();
}

// 检查模块名是否是原生模块（如 io, times, maths）
int native_is_module(const char* module_name) {
    if (module_name == NULL) return 0;

    // 解析别名
    const char* actual_module = native_resolve_module_alias(module_name);

    // 使用内部模块列表检查
    return native_is_builtin_module(actual_module);
}

// 创建原生函数对象的辅助函数
ObjNative* make_native(NativeFn fn, int arity, const char* name) {
    ObjNative* native = (ObjNative*)gc_alloc(sizeof(ObjNative), OBJ_NATIVE);
    if (!native) return NULL;
    native->function = fn;
    native->arity = arity;
    native->name = strdup(name);
    return native;
}

// ============================================================================
// 实例方法元信息支持（编译期检查用）- 哈希表实现
// ============================================================================

#define INSTANCE_METHOD_TABLE_INITIAL_CAPACITY 64
#define INSTANCE_METHOD_TABLE_MAX_LOAD 0.75

typedef struct InstanceMethodEntry {
    char type_name[32];
    char method_name[32];
    InstanceMethodMeta meta;
    struct InstanceMethodEntry* next;
} InstanceMethodEntry;

typedef struct {
    InstanceMethodEntry** entries;
    int capacity;
    int count;
} InstanceMethodTable;

static THREAD_LOCAL InstanceMethodTable instanceMethodTable = {NULL, 0, 0};

// 组合类型名和方法名的哈希
static uint32_t hash_instance_method(const char* type_name, const char* method_name) {
    uint32_t hash = native_hash_string(type_name);
    while (*method_name) {
        hash ^= (unsigned char)(*method_name);
        hash *= 16777619;
        method_name++;
    }
    return hash;
}

// 初始化实例方法表
static void instance_method_table_init(void) {
    instanceMethodTable.capacity = INSTANCE_METHOD_TABLE_INITIAL_CAPACITY;
    instanceMethodTable.count = 0;
    instanceMethodTable.entries = (InstanceMethodEntry**)calloc(instanceMethodTable.capacity, sizeof(InstanceMethodEntry*));
}

// 释放实例方法表
static void instance_method_table_free(void) {
    if (!instanceMethodTable.entries) return;
    
    for (int i = 0; i < instanceMethodTable.capacity; i++) {
        InstanceMethodEntry* entry = instanceMethodTable.entries[i];
        while (entry) {
            InstanceMethodEntry* next = entry->next;
            free(entry);
            entry = next;
        }
    }
    free(instanceMethodTable.entries);
    instanceMethodTable.entries = NULL;
    instanceMethodTable.capacity = 0;
    instanceMethodTable.count = 0;
}

// 扩容实例方法表
static void instance_method_table_resize(void) {
    int old_capacity = instanceMethodTable.capacity;
    InstanceMethodEntry** old_entries = instanceMethodTable.entries;
    
    int new_capacity = old_capacity * 2;
    InstanceMethodEntry** new_entries = (InstanceMethodEntry**)calloc(new_capacity, sizeof(InstanceMethodEntry*));
    if (!new_entries) return;
    
    for (int i = 0; i < old_capacity; i++) {
        InstanceMethodEntry* entry = old_entries[i];
        while (entry) {
            InstanceMethodEntry* next = entry->next;
            uint32_t hash = hash_instance_method(entry->type_name, entry->method_name);
            int index = hash & (new_capacity - 1);
            entry->next = new_entries[index];
            new_entries[index] = entry;
            entry = next;
        }
    }
    
    free(old_entries);
    instanceMethodTable.entries = new_entries;
    instanceMethodTable.capacity = new_capacity;
}

// 注册实例方法元信息（编译时调用）
void native_register_instance_method_meta(const char* type_name, const char* method_name, int arity, int min_arity, int max_arity, TypeKind return_type, TypeKind return_element_type) {
    TypeKind param_types[MAX_METHOD_PARAMS];
    for (int i = 0; i < MAX_METHOD_PARAMS; i++) {
        param_types[i] = TYPE_ANY;
    }
    native_register_instance_method_meta_with_params(type_name, method_name, arity, min_arity, max_arity, return_type, return_element_type, param_types);
}

// 注册实例方法元信息（带参数类型）
void native_register_instance_method_meta_with_params(const char* type_name, const char* method_name, int arity, int min_arity, int max_arity, TypeKind return_type, TypeKind return_element_type, TypeKind* param_types) {
    if (!instanceMethodTable.entries) {
        instance_method_table_init();
    }

    if (instanceMethodTable.count >= instanceMethodTable.capacity * INSTANCE_METHOD_TABLE_MAX_LOAD) {
        instance_method_table_resize();
    }

    uint32_t hash = hash_instance_method(type_name, method_name);
    int index = hash & (instanceMethodTable.capacity - 1);

    // 检查是否已存在
    InstanceMethodEntry* entry = instanceMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->type_name, type_name) == 0 &&
            strcmp(entry->method_name, method_name) == 0) {
            // 更新
            entry->meta.arity = arity;
            entry->meta.min_arity = min_arity;
            entry->meta.max_arity = max_arity;
            entry->meta.return_type = return_type;
            entry->meta.return_element_type = return_element_type;
            if (param_types && arity > 0) {
                int count = arity < MAX_METHOD_PARAMS ? arity : MAX_METHOD_PARAMS;
                for (int i = 0; i < count; i++) {
                    entry->meta.param_types[i] = param_types[i];
                }
                for (int i = count; i < MAX_METHOD_PARAMS; i++) {
                    entry->meta.param_types[i] = TYPE_ANY;
                }
            }
            return;
        }
        entry = entry->next;
    }

    // 创建新条目
    InstanceMethodEntry* new_entry = (InstanceMethodEntry*)malloc(sizeof(InstanceMethodEntry));
    if (!new_entry) return;

    int type_len = strlen(type_name);
    int method_len = strlen(method_name);
    if (type_len > 31) type_len = 31;
    if (method_len > 31) method_len = 31;

    memcpy(new_entry->type_name, type_name, type_len);
    new_entry->type_name[type_len] = '\0';

    memcpy(new_entry->method_name, method_name, method_len);
    new_entry->method_name[method_len] = '\0';

    new_entry->meta.arity = arity;
    new_entry->meta.min_arity = min_arity;
    new_entry->meta.max_arity = max_arity;
    new_entry->meta.return_type = return_type;
    new_entry->meta.return_element_type = return_element_type;

    if (param_types && arity > 0) {
        int count = arity < MAX_METHOD_PARAMS ? arity : MAX_METHOD_PARAMS;
        for (int i = 0; i < count; i++) {
            new_entry->meta.param_types[i] = param_types[i];
        }
        for (int i = count; i < MAX_METHOD_PARAMS; i++) {
            new_entry->meta.param_types[i] = TYPE_ANY;
        }
    } else {
        for (int i = 0; i < MAX_METHOD_PARAMS; i++) {
            new_entry->meta.param_types[i] = TYPE_ANY;
        }
    }

    new_entry->next = instanceMethodTable.entries[index];
    instanceMethodTable.entries[index] = new_entry;
    instanceMethodTable.count++;
}

// 获取实例方法的参数数量（编译时调用）
int native_get_instance_method_arity(const char* type_name, const char* method_name) {
    if (!instanceMethodTable.entries || instanceMethodTable.count == 0) return -1;
    
    uint32_t hash = hash_instance_method(type_name, method_name);
    int index = hash & (instanceMethodTable.capacity - 1);
    
    InstanceMethodEntry* entry = instanceMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->type_name, type_name) == 0 &&
            strcmp(entry->method_name, method_name) == 0) {
            return entry->meta.arity;
        }
        entry = entry->next;
    }
    return -1;
}

// 获取实例方法的返回类型（编译时调用）
TypeKind native_get_instance_method_return_type(const char* type_name, const char* method_name, int* out_arity) {
    if (!instanceMethodTable.entries || instanceMethodTable.count == 0) {
        if (out_arity) *out_arity = -1;
        return TYPE_ANY;
    }
    
    uint32_t hash = hash_instance_method(type_name, method_name);
    int index = hash & (instanceMethodTable.capacity - 1);
    
    InstanceMethodEntry* entry = instanceMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->type_name, type_name) == 0 &&
            strcmp(entry->method_name, method_name) == 0) {
            if (out_arity) {
                *out_arity = entry->meta.arity;
            }
            return entry->meta.return_type;
        }
        entry = entry->next;
    }
    if (out_arity) {
        *out_arity = -1;
    }
    return TYPE_ANY;
}

// 获取实例方法返回数组时的元素类型（编译时调用）
TypeKind native_get_instance_method_return_element_type(const char* type_name, const char* method_name) {
    if (!instanceMethodTable.entries || instanceMethodTable.count == 0) {
        return TYPE_UNKNOWN;
    }
    
    uint32_t hash = hash_instance_method(type_name, method_name);
    int index = hash & (instanceMethodTable.capacity - 1);
    
    InstanceMethodEntry* entry = instanceMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->type_name, type_name) == 0 &&
            strcmp(entry->method_name, method_name) == 0) {
            return entry->meta.return_element_type;
        }
        entry = entry->next;
    }
    return TYPE_UNKNOWN;
}

// 获取实例方法的参数类型（编译时调用）
TypeKind native_get_instance_method_param_type(const char* type_name, const char* method_name, int param_index) {
    if (!instanceMethodTable.entries || instanceMethodTable.count == 0) return TYPE_ANY;
    
    uint32_t hash = hash_instance_method(type_name, method_name);
    int index = hash & (instanceMethodTable.capacity - 1);
    
    InstanceMethodEntry* entry = instanceMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->type_name, type_name) == 0 &&
            strcmp(entry->method_name, method_name) == 0) {
            if (param_index >= 0 && param_index < entry->meta.arity && param_index < MAX_METHOD_PARAMS) {
                return entry->meta.param_types[param_index];
            }
            break;
        }
        entry = entry->next;
    }
    return TYPE_ANY;
}

// 根据类型名和方法名查找实例方法元信息（编译时调用）
// 返回指向实例方法元信息的指针，未找到返回 NULL
const InstanceMethodMeta* native_find_instance_method(const char* type_name, const char* method_name) {
    if (!instanceMethodTable.entries || instanceMethodTable.count == 0) return NULL;
    
    uint32_t hash = hash_instance_method(type_name, method_name);
    int index = hash & (instanceMethodTable.capacity - 1);
    
    InstanceMethodEntry* entry = instanceMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->type_name, type_name) == 0 &&
            strcmp(entry->method_name, method_name) == 0) {
            return &entry->meta;
        }
        entry = entry->next;
    }
    return NULL;
}

// 根据方法名查找实例方法元信息（编译时调用）
const char* native_find_instance_method_type(const char* method_name, int* out_arity, TypeKind* out_return_type) {
    if (!instanceMethodTable.entries || instanceMethodTable.count == 0) {
        if (out_arity) *out_arity = -1;
        if (out_return_type) *out_return_type = TYPE_ANY;
        return NULL;
    }
    
    // 遍历所有条目查找匹配的方法名（需要线性搜索，因为只提供方法名）
    for (int i = 0; i < instanceMethodTable.capacity; i++) {
        InstanceMethodEntry* entry = instanceMethodTable.entries[i];
        while (entry) {
            if (strcmp(entry->method_name, method_name) == 0) {
                if (out_arity) {
                    *out_arity = entry->meta.arity;
                }
                if (out_return_type) {
                    *out_return_type = entry->meta.return_type;
                }
                return entry->type_name;
            }
            entry = entry->next;
        }
    }
    if (out_arity) {
        *out_arity = -1;
    }
    if (out_return_type) {
        *out_return_type = TYPE_ANY;
    }
    return NULL;
}

// 重置实例方法元信息表（编译前调用）
void native_reset_instance_method_metas(void) {
    instance_method_table_free();
}

// 获取类型的所有实例方法名（LSP 使用）
// 返回方法名数组，通过 count 返回数量，需要调用者用 free_instance_method_list 释放
char** native_get_instance_methods(const char* type_name, int* count) {
    if (!instanceMethodTable.entries || instanceMethodTable.count == 0 || !type_name || !count) {
        if (count) *count = 0;
        return NULL;
    }

    // 先统计该类型的方法数量
    int method_count = 0;
    for (int i = 0; i < instanceMethodTable.capacity; i++) {
        InstanceMethodEntry* entry = instanceMethodTable.entries[i];
        while (entry) {
            if (strcmp(entry->type_name, type_name) == 0) {
                method_count++;
            }
            entry = entry->next;
        }
    }

    if (method_count == 0) {
        *count = 0;
        return NULL;
    }

    // 分配数组
    char** methods = (char**)malloc(sizeof(char*) * method_count);
    if (!methods) {
        *count = 0;
        return NULL;
    }

    // 填充方法名
    int idx = 0;
    for (int i = 0; i < instanceMethodTable.capacity && idx < method_count; i++) {
        InstanceMethodEntry* entry = instanceMethodTable.entries[i];
        while (entry && idx < method_count) {
            if (strcmp(entry->type_name, type_name) == 0) {
                methods[idx] = strdup(entry->method_name);
                idx++;
            }
            entry = entry->next;
        }
    }

    *count = method_count;
    return methods;
}

// 释放实例方法名列表
void native_free_instance_method_list(char** methods, int count) {
    if (!methods) return;
    for (int i = 0; i < count; i++) {
        free(methods[i]);
    }
    free(methods);
}

// 前向声明：数组实例方法初始化（在 arrays.c 中定义）
void arrays_init_instance_methods(void);
// 前向声明：字符串实例方法初始化（在 strings.c 中定义）
void strings_init_instance_methods(void);
// 前向声明：数字实例方法初始化（在 maths.c 中定义）
void maths_init_instance_methods(void);
// 前向声明：字典实例方法初始化（在 dicts.c 中定义）
void dicts_init_instance_methods(void);
// 前向声明：文件实例方法初始化（在 files.c 中定义）
void files_init_instance_methods(void);
// 前向声明：结构体实例方法初始化（在 structs.c 中定义）
void structs_init_instance_methods(void);
// 前向声明：cstruct 实例方法初始化（在 cstructs.c 中定义）
void cstructs_init_methods(void);
// 前向声明：线程实例方法初始化（在 threads.c 中定义）
void threads_init_instance_methods(void);
// 前向声明：Socket 实例方法初始化（在 sockets.c 中定义）
void sockets_init_instance_methods(void);

void native_register_all_instance_method_metas(void) {
    native_reset_instance_method_metas();
    arrays_init_instance_methods();
    strings_init_instance_methods();
    maths_init_instance_methods();
    dicts_init_instance_methods();
    files_init_instance_methods();
    structs_init_instance_methods();
    cstructs_init_methods();
    threads_init_instance_methods();
    sockets_init_instance_methods();
}

// 根据 TypeKind 获取类型名称（编译时调用）
const char* native_get_type_name(TypeKind kind) {
    switch (kind) {
        case TYPE_ARRAY:  return "Array";
        case TYPE_STRING: return "string";
        case TYPE_DICT:   return "Dict";
        case TYPE_FILE:   return "File";
        case TYPE_STRUCT: return "struct";
        case TYPE_CSTRUCT: return "cstruct";
        case TYPE_THREAD:  return "Thread";
        case TYPE_CHANNEL: return "Channel";
        case TYPE_SOCKET:  return "Socket";
        case TYPE_INT:
        case TYPE_FLOAT:  return "number";
        default:          return NULL;
    }
}

// 获取当前执行行号（供原生函数使用）
int native_get_current_line(void) {
    extern THREAD_LOCAL VM* current_exec_vm;
    VM* target_vm = current_exec_vm ? current_exec_vm : &vm;
    if (target_vm->frame_cnt > 0) {
        CallFrame* frame = &target_vm->frames[target_vm->frame_cnt - 1];
        if (frame->ip && frame->chunk && frame->chunk->code) {
            int offset = (int)(frame->ip - frame->chunk->code);
            if (offset >= 0 && offset < frame->chunk->len && frame->chunk->lines) {
                return frame->chunk->lines[offset];
            }
        }
    }
    return target_vm->current_line;
}

// 抛出运行时错误（供原生函数使用）
void native_throw_error(const char* msg) {
    int line = native_get_current_line();

    ObjString* err_str = str_copy(msg, strlen(msg));
    if (!err_str) {
        error_add(ERR_RUNTIME, line, msg);
        return;
    }

    extern THREAD_LOCAL VM* current_exec_vm;
    VM* target_vm = current_exec_vm ? current_exec_vm : &vm;
    target_vm->exception = val_obj((Object*)err_str);
    target_vm->has_exception = 1;
    target_vm->exception_line = line;
}

// ============================================================================
// 深拷贝实现
// ============================================================================

// 前向声明：结构体拷贝方法（在 structs.c 中定义）
extern Value struct_copy_recursive(ObjStruct* source);

// 深拷贝一个 Value（递归处理数组、字典、结构体等引用类型）
Value value_copy(Value v) {
    if (val_is_obj(v)) {
        Object* obj = val_as_obj(v);
        switch (obj->type) {
            case OBJ_ARRAY: {
                // 递归深拷贝嵌套数组
                ObjArray* arr = (ObjArray*)obj;
                ObjArray* copy = (ObjArray*)gc_alloc(sizeof(ObjArray), OBJ_ARRAY);
                if (!copy) return val_null();
                copy->count = arr->count;
                copy->capacity = arr->count;
                copy->elements = NULL;
                copy->type_info = arr->type_info ? type_copy(arr->type_info) : NULL;
                if (arr->count > 0) {
                    copy->elements = (Value*)malloc(arr->count * sizeof(Value));
                    if (!copy->elements) {
                        native_throw_error("嵌套数组拷贝内存分配失败");
                        return val_null();
                    }
                    // 递归拷贝每个元素
                    for (int i = 0; i < arr->count; i++) {
                        copy->elements[i] = value_copy(arr->elements[i]);
                    }
                }
                return val_obj((Object*)copy);
            }
            case OBJ_STRING: {
                // 字符串是不可变的，可以共享（但这里选择拷贝）
                ObjString* str = (ObjString*)obj;
                return val_obj((Object*)str_copy(str->chars, str->len));
            }
            case OBJ_DICT: {
                // 字典深拷贝
                ObjDict* dict = (ObjDict*)obj;
                ObjDict* copy = dict_new(dict->capacity);
                if (!copy) return val_null();
                // 拷贝数组部分
                if (dict->array && dict->asize > 0) {
                    copy->array = (Value*)malloc(dict->asize * sizeof(Value));
                    if (copy->array) {
                        copy->asize = dict->asize;
                        for (int i = 0; i < dict->asize; i++) {
                            copy->array[i] = value_copy(dict->array[i]);
                        }
                    }
                }
                // 拷贝哈希部分
                for (int i = 0; i < dict->capacity; i++) {
                    Value entry_key = dict->entries[i].key;
                    if (!val_is_null(entry_key) && entry_key != DICT_TOMBSTONE_VAL) {
                        dict_set(copy, entry_key,
                                value_copy(dict->entries[i].value));
                    }
                }
                return val_obj((Object*)copy);
            }
            case OBJ_STRUCT: {
                // 递归深拷贝结构体实例
                ObjStruct* struct_obj = (ObjStruct*)obj;
                return struct_copy_recursive(struct_obj);
            }
            default:
                // 其他对象类型（函数、闭包等）通常不可拷贝，返回原引用
                return v;
        }
    }
    // 简单类型直接返回
    return v;
}
