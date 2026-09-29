#include "include/native.h"
#include "include/leno_vm.h"
#include "include/leno_hash.h"
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

// 组合两个字符串的哈希（用于模块方法：模块名+方法名）
static uint32_t hash_module_method(const char* module_name, const char* method_name) {
    uint32_t hash = leno_fnv1a(module_name);
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
    uint32_t hash = leno_fnv1a(module_name);
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
    meta->return_spec = NULL;   // 规格通道（v3.2.8）另用 native_register_meta_spec() 声明

    // 复制参数类型
    if (param_types && arity > 0) {
        int count = arity < MAX_METHOD_PARAMS ? arity : MAX_METHOD_PARAMS;
        for (int i = 0; i < count; i++) {
            meta->param_types[i] = param_types[i];
        }
        for (int i = count; i < MAX_METHOD_PARAMS; i++) {
            meta->param_types[i] = TYPE_ANY;
        }
        meta->param_type_count = count;
    } else {
        // ⚠ 可变参数（arity == -1）走这里 ⇒ 与模块方法通道同一口径：**整份忽略** param_types。
        //   要声明可变参数的参数类型，注册之后调 `native_set_builtin_vararg_params()`。
        for (int i = 0; i < MAX_METHOD_PARAMS; i++) {
            meta->param_types[i] = TYPE_ANY;
        }
        meta->param_type_count = 0;
    }
}

// 获取所有注册的 native 函数
const NativeFunctionMeta* native_get_all_functions(int* count) {
    *count = functionCount;
    return functionRegistry;
}

// 给全局内置函数声明完整返回类型规格（v3.2.8）：内置通道此前只有 Kind 槽，
//   `_exec` 的 `[output, code]` 只能表达成 `Array[any]` ⇒ 补这条规格通道。
// 语义与模块方法那条一致：规格非 NULL 时优先于 Kind 槽（见 semantic_type.c 的取用点）。
// ⚠ 必须在 `vm_register_native()` **之后**调用：native_register_meta 对同名是"直接 return"的，
//   所以这里做成**查找并更新**（条目不存在时才以最小信息新建，避免与已注册的撞名/漏注册）。
void native_register_meta_spec(const char* name, const NativeTypeSpec* return_spec) {
    for (int i = 0; i < functionCount; i++) {
        if (strcmp(functionRegistry[i].name, name) == 0) {
            functionRegistry[i].return_spec = return_spec;
            return;
        }
    }

    if (functionCount >= MAX_NATIVE_FUNCTIONS) return;
    NativeFunctionMeta* meta = &functionRegistry[functionCount++];
    meta->name = name;
    meta->arity = -1;
    meta->min_arity = 0;
    meta->max_arity = -1;
    meta->return_type = TYPE_ANY;
    meta->return_element_type = TYPE_UNKNOWN;
    for (int i = 0; i < MAX_METHOD_PARAMS; i++) {
        meta->param_types[i] = TYPE_ANY;
    }
    meta->return_spec = return_spec;
}

// 取内置函数的返回规格（无则 NULL）
const NativeTypeSpec* native_get_return_spec(const char* name) {
    for (int i = 0; i < functionCount; i++) {
        if (strcmp(functionRegistry[i].name, name) == 0) {
            return functionRegistry[i].return_spec;
        }
    }
    return NULL;
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
// ⚠ v3.2.8：判据由 `param_index < arity` 改为 `param_index < param_type_count`
//   —— 与模块方法通道（v3.2.7，实例二十三）同一口径。老写法对 `arity == -1`（如 `input`）
//   恒假 ⇒ 那些函数的 param_types 永远读不到（声明了也白声明）。
TypeKind native_get_global_function_param_type(const char* name, int param_index) {
    for (int i = 0; i < functionCount; i++) {
        if (strcmp(functionRegistry[i].name, name) == 0) {
            if (param_index >= 0 && param_index < functionRegistry[i].param_type_count &&
                param_index < MAX_METHOD_PARAMS) {
                return functionRegistry[i].param_types[param_index];
            }
            break;
        }
    }
    return TYPE_ANY;
}

// 为**全局内置函数**声明可变参数的参数类型（见 native.h 的说明）。
//   与模块方法通道的 `native_set_method_vararg_params()` 逐字一致：
//   先把整份 param_types 填成 tail_type，再盖上前 prefix_count 个。
void native_set_builtin_vararg_params(const char* name, int prefix_count,
                                      const TypeKind* prefix, TypeKind tail_type) {
    for (int i = 0; i < functionCount; i++) {
        if (strcmp(functionRegistry[i].name, name) == 0) {
            for (int j = 0; j < MAX_METHOD_PARAMS; j++) {
                functionRegistry[i].param_types[j] = tail_type;
            }
            for (int j = 0; j < prefix_count && j < MAX_METHOD_PARAMS; j++) {
                functionRegistry[i].param_types[j] = prefix ? prefix[j] : TYPE_ANY;
            }
            functionRegistry[i].param_type_count = MAX_METHOD_PARAMS;
            return;
        }
    }
    // 没注册过就静默忽略（与注册表其它部分的风格一致：编译期常量，不该失败）
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

// 供语义层"未定义函数"相似名提示遍历（C2）
int native_get_name_count(void) {
    return nativeFunctionObjectCount;
}

const char* native_get_name(int index) {
    if (index < 0 || index >= nativeFunctionObjectCount) return NULL;
    return nativeFunctionObjects[index] ? nativeFunctionObjects[index]->name : NULL;
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

// 前向声明：规格 → TypeKind（定义在本文件后面的「native 类型规格」一节）
static TypeKind native_spec_kind(const NativeTypeSpec* spec);

// 注册模块方法（**唯一入口**，v3.2.3 起）：返回类型用完整类型规格声明。
// min_arity/max_arity: 当 arity == -1（可变参数）时，指定最小/最大允许参数个数；其他情况传 -1
// param_types: 参数类型数组，长度为 arity，如果为 NULL 则所有参数默认为 TYPE_ANY
// return_spec: 完整返回类型规格（见 leno_types.h；常用的 19 种已预制为 NATIVE_T_*）。
//              顶层 Kind / 元素 Kind 由规格**推导**出来一并写进元数据 ⇒ 只认 Kind 的老消费者
//              （旧的实例方法查询路径、LSP 老渲染等）也拿到大致正确的类型。
void native_register_module_method_spec(const char* module_name, const char* method_name,
                                        NativeFn function, int arity, int min_arity, int max_arity,
                                        const NativeTypeSpec* return_spec, TypeKind* param_types) {
    // 规格 → (顶层 Kind, 数组元素 Kind)：与老的 `(return_type, return_element_type)` 逐字对应
    TypeKind return_type = TYPE_ANY;
    TypeKind return_element_type = TYPE_UNKNOWN;
    if (return_spec) {
        return_type = native_spec_kind(return_spec);
        if (return_spec->tag == NTYPE_ARRAY && return_spec->sub) {
            return_element_type = native_spec_kind(return_spec->sub);
        }
    }

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
            entry->meta.return_spec = return_spec;   // 可空：NULL = 走老的 Kind 路径
            if (param_types && arity > 0) {
                int count = arity < MAX_METHOD_PARAMS ? arity : MAX_METHOD_PARAMS;
                for (int i = 0; i < count; i++) {
                    entry->meta.param_types[i] = param_types[i];
                }
                for (int i = count; i < MAX_METHOD_PARAMS; i++) {
                    entry->meta.param_types[i] = TYPE_ANY;
                }
                entry->meta.param_type_count = count;
            } else {
                // ⚠ 可变参数（arity == -1）走这里 ⇒ 老行为是**整份忽略** param_types（全 ANY）。
                //   要声明可变参数的参数类型，注册之后调 `native_set_method_vararg_params()`。
                for (int i = 0; i < MAX_METHOD_PARAMS; i++) {
                    entry->meta.param_types[i] = TYPE_ANY;
                }
                entry->meta.param_type_count = 0;
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
    new_entry->meta.return_spec = return_spec;

    // 复制参数类型
    if (param_types && arity > 0) {
        int count = arity < MAX_METHOD_PARAMS ? arity : MAX_METHOD_PARAMS;
        for (int i = 0; i < count; i++) {
            new_entry->meta.param_types[i] = param_types[i];
        }
        for (int i = count; i < MAX_METHOD_PARAMS; i++) {
            new_entry->meta.param_types[i] = TYPE_ANY;
        }
        new_entry->meta.param_type_count = count;
    } else {
        // 可变参数：见上面分支的说明（要声明就注册后调 native_set_method_vararg_params）
        for (int i = 0; i < MAX_METHOD_PARAMS; i++) {
            new_entry->meta.param_types[i] = TYPE_ANY;
        }
        new_entry->meta.param_type_count = 0;
    }

    // 插入到哈希表
    new_entry->next = moduleMethodTable.entries[index];
    moduleMethodTable.entries[index] = new_entry;
    moduleMethodTable.count++;
}

// 取模块方法的返回类型规格（编译期：语义侧构造返回类型时优先用它）
const NativeTypeSpec* native_get_module_method_return_spec(const char* module_name, const char* method_name) {
    ModuleMethodMeta* meta = native_find_module_method(module_name, method_name);
    return meta ? meta->return_spec : NULL;
}

// ============================================================================
// native 类型规格：注册表 + 规格→TypeInfo/字符串 + native 结构体的运行期支持
// ----------------------------------------------------------------------------
// 一份 static 规格同时服务两处（**这是"字段布局不漂"的全部保障**）：
//   · 编译期：native_type_spec_to_info() → TypeInfo（struct 名带进 struct_name）
//   · 运行期：native_struct_def_for() 按**同一字段顺序**造 ObjStructDef 并注册进 struct_def_table
// 表满/重名都静默忽略（与 native 注册表其它部分的风格一致：模块声明是编译期常量，不该失败）。
// ============================================================================
// ---- 预制规格（覆盖各模块现有的 19 种组合；语义与老的 (Kind, element) 逐字对应）----
const NativeTypeSpec NATIVE_T_ANY         = { NTYPE_ANY,    NULL, NULL, NULL, 0, -1 };
const NativeTypeSpec NATIVE_T_INT         = { NTYPE_INT,    NULL, NULL, NULL, 0, -1 };
const NativeTypeSpec NATIVE_T_FLOAT       = { NTYPE_FLOAT,  NULL, NULL, NULL, 0, -1 };
const NativeTypeSpec NATIVE_T_STRING      = { NTYPE_STRING, NULL, NULL, NULL, 0, -1 };
const NativeTypeSpec NATIVE_T_BOOL        = { NTYPE_BOOL,   NULL, NULL, NULL, 0, -1 };
const NativeTypeSpec NATIVE_T_NULL        = { NTYPE_NULL,   NULL, NULL, NULL, 0, -1 };
const NativeTypeSpec NATIVE_T_PTR         = { NTYPE_PTR,    NULL, NULL, NULL, 0, -1 };
const NativeTypeSpec NATIVE_T_FILE        = { NTYPE_FILE,   NULL, NULL, NULL, 0, -1 };
const NativeTypeSpec NATIVE_T_SOCKET      = { NTYPE_SOCKET, NULL, NULL, NULL, 0, -1 };
const NativeTypeSpec NATIVE_T_CHANNEL     = { NTYPE_CHANNEL,NULL, NULL, NULL, 0, -1 };
const NativeTypeSpec NATIVE_T_THREAD      = { NTYPE_THREAD, NULL, NULL, NULL, 0, -1 };
const NativeTypeSpec NATIVE_T_FUTURE      = { NTYPE_FUTURE, NULL, NULL, NULL, 0, -1 };
const NativeTypeSpec NATIVE_T_ARR         = { NTYPE_ARRAY,  NULL, NULL, NULL, 0, -1 };       // 元素未指定
const NativeTypeSpec NATIVE_T_ARR_ANY     = { NTYPE_ARRAY,  NULL, &NATIVE_T_ANY,    NULL, 0, -1 };
const NativeTypeSpec NATIVE_T_ARR_INT     = { NTYPE_ARRAY,  NULL, &NATIVE_T_INT,    NULL, 0, -1 };
const NativeTypeSpec NATIVE_T_ARR_STRING  = { NTYPE_ARRAY,  NULL, &NATIVE_T_STRING, NULL, 0, -1 };
const NativeTypeSpec NATIVE_T_ARR_ARR     = { NTYPE_ARRAY,  NULL, &NATIVE_T_ARR,    NULL, 0, -1 };
const NativeTypeSpec NATIVE_T_ARR_DICT    = { NTYPE_ARRAY,  NULL, &NATIVE_T_DICT,   NULL, 0, -1 };
const NativeTypeSpec NATIVE_T_DICT        = { NTYPE_DICT,   NULL, NULL, NULL, 0, -1 };       // K/V 未指定
// ---- 关系型标签（v3.2.6）：返回类型**跟第 0 个实参走**（实例方法里 receiver 即第 0 个）----
// 它们不是"一个类型"，而是"一条关系" ⇒ 只有能拿到实参类型的地方（native 方法调用点）才算得出结果；
// 拿不到（如方法被当值传递）就退化成 any（保守），不会比老路径更差。
// 为什么上提为预制：arrays / dicts / rands / sockets 四个模块都要用同一份 ⇒ 单一来源，
// 免得每个模块各自抄一份 `static const NativeTypeSpec`（抄 4 份就有 4 个可能漂的口子）。
const NativeTypeSpec NATIVE_T_ARG0_ELEM     = { NTYPE_ARG0_ELEM,  NULL, NULL, NULL, 0, -1 };        // 第 0 个实参的元素类型
const NativeTypeSpec NATIVE_T_ARG0_KEY      = { NTYPE_ARG0_KEY,   NULL, NULL, NULL, 0, -1 };        // 第 0 个实参的键类型
const NativeTypeSpec NATIVE_T_ARG0_VALUE    = { NTYPE_ARG0_VALUE, NULL, NULL, NULL, 0, -1 };        // 第 0 个实参的值类型
// ---- 回调返回类型族（2026-09-28，⓪ 族声明化：map/reduce/start 的语义特判 → 注册即数据）----
//   下标约定与 ARG0_* 一致（实例形态接收者 = 0；模块形态 0 = 首实参）。acc_index 见 leno_types.h。
const NativeTypeSpec NATIVE_T_ARG_CB_RET0   = { NTYPE_ARG_CB_RET, NULL, NULL, NULL, 0, -1 };        // 第 0 实参是回调（threads.start）
const NativeTypeSpec NATIVE_T_ARG_CB_RET1   = { NTYPE_ARG_CB_RET, NULL, NULL, NULL, 1, -1 };        // 第 1 实参是回调（Array 实例 map：接收者=0）
const NativeTypeSpec NATIVE_T_ARR_CB_RET1   = { NTYPE_ARRAY, NULL, &NATIVE_T_ARG_CB_RET1, NULL, 0, -1 };  // Array[回调返回]（map）
const NativeTypeSpec NATIVE_T_ARG_REDUCE12  = { NTYPE_ARG_CB_RET, NULL, NULL, NULL, 1, 2 };         // 回调=1、累加器=2（reduce）
const NativeTypeSpec NATIVE_T_THREAD_CB_RET0 = { NTYPE_THREAD, NULL, &NATIVE_T_ARG_CB_RET0, NULL, 0, -1 }; // Thread[回调返回]（start）
const NativeTypeSpec NATIVE_T_ARR_ARG0_ELEM = { NTYPE_ARRAY, NULL, &NATIVE_T_ARG0_ELEM, NULL, 0, -1 }; // Array[第 0 个实参的元素]

#define NATIVE_STRUCT_SPEC_MAX 64
// 与 struct_def_table（object_struct.c）同为 THREAD_LOCAL：每个线程初始化 native 模块时
// 各自登记一遍同一批 static 规格 ⇒ 内容一致、无跨线程共享写（子线程初始化见 object_thread.c）
static THREAD_LOCAL const NativeStructSpec* nativeStructSpecs[NATIVE_STRUCT_SPEC_MAX];
static THREAD_LOCAL int nativeStructSpecCount = 0;

void native_register_struct_spec(const NativeStructSpec* spec) {
    if (!spec || !spec->name || spec->field_count < 0) return;
    for (int i = 0; i < nativeStructSpecCount; i++) {
        if (strcmp(nativeStructSpecs[i]->name, spec->name) == 0) {
            nativeStructSpecs[i] = spec;   // 同名重注册：后注册者生效（与 struct_def_register 同口径）
            return;
        }
    }
    if (nativeStructSpecCount >= NATIVE_STRUCT_SPEC_MAX) return;
    nativeStructSpecs[nativeStructSpecCount++] = spec;
}

const NativeStructSpec* native_find_struct_spec(const char* name) {
    if (!name) return NULL;
    for (int i = 0; i < nativeStructSpecCount; i++) {
        if (strcmp(nativeStructSpecs[i]->name, name) == 0) return nativeStructSpecs[i];
    }
    return NULL;
}

// 按 (模块名, 类型名) 查（2026-09-27）—— `use <module>.<Type>` 通道的唯一来源。
//   为什么不能只按名字查：`use` 要回答的是"**这个模块**导出了哪些类型"，
//   只按名字查就退化成"全局有没有这个名字"，与 `use` 的语义不符（也就没法做"可用类型"诊断）。
const NativeStructSpec* native_find_module_struct_spec(const char* module_name, const char* type_name) {
    if (!module_name || !type_name) return NULL;
    for (int i = 0; i < nativeStructSpecCount; i++) {
        const NativeStructSpec* s = nativeStructSpecs[i];
        if (s->module_name && strcmp(s->module_name, module_name) == 0 &&
            strcmp(s->name, type_name) == 0) {
            return s;
        }
    }
    return NULL;
}

int native_list_module_struct_specs(const char* module_name, const char** out_names, int max) {
    if (!module_name || !out_names || max <= 0) return 0;
    int n = 0;
    for (int i = 0; i < nativeStructSpecCount && n < max; i++) {
        const NativeStructSpec* s = nativeStructSpecs[i];
        if (s->module_name && strcmp(s->module_name, module_name) == 0) {
            out_names[n++] = s->name;
        }
    }
    return n;
}

// 规格 → TypeKind（只取顶层；NTYPE_STRUCT 的**名字**由调用方另行处理）
static TypeKind native_spec_kind(const NativeTypeSpec* spec) {
    if (!spec) return TYPE_ANY;
    switch (spec->tag) {
        case NTYPE_INT:         return TYPE_INT;
        case NTYPE_FLOAT:       return TYPE_FLOAT;
        case NTYPE_STRING:      return TYPE_STRING;
        case NTYPE_BOOL:        return TYPE_BOOL;
        case NTYPE_NULL:        return TYPE_NULL;
        case NTYPE_ARRAY:       return TYPE_ARRAY;
        case NTYPE_DICT:        return TYPE_DICT;
        case NTYPE_STRUCT:      return TYPE_STRUCT;
        case NTYPE_PTR:         return TYPE_PTR;
        case NTYPE_PTR_GENERIC: return TYPE_PTR_GENERIC;
        case NTYPE_FILE:        return TYPE_FILE;
        case NTYPE_SOCKET:      return TYPE_SOCKET;
        case NTYPE_CHANNEL:     return TYPE_CHANNEL;
        case NTYPE_THREAD:      return TYPE_THREAD;
        case NTYPE_FUTURE:      return TYPE_FUTURE;
        default:                return TYPE_ANY;
    }
}

// 关系型标签（NTYPE_ARG0_*）→ 实参类型里的对应部分；解析不出 ⇒ NULL（调用方退化成 any）
static TypeInfo* native_arg0_part_to_info(TypeInfo* arg0, NativeTypeTag tag) {
    if (!arg0) return NULL;
    // ARG0_ELEM 同时服务两种容器：Array[T] 的 T（copy/pop/...）与 Thread[T] 的 T
    //   （join 的返回类型 = threads.start 时闭包的返回类型，2026-09-28 ✓）
    if (tag == NTYPE_ARG0_ELEM &&
        (arg0->kind == TYPE_ARRAY || arg0->kind == TYPE_THREAD) && arg0->element_type) {
        return type_copy(arg0->element_type);
    }
    if (tag == NTYPE_ARG0_KEY && arg0->kind == TYPE_DICT && arg0->key_type) {
        return type_copy(arg0->key_type);
    }
    if (tag == NTYPE_ARG0_VALUE && arg0->kind == TYPE_DICT && arg0->value_type) {
        return type_copy(arg0->value_type);
    }
    return NULL;
}

// 规格里是否含"引用实参"的标签（递归）
int native_type_spec_has_arg_ref(const NativeTypeSpec* spec) {
    if (!spec) return 0;
    if (spec->tag == NTYPE_ARG0_ELEM || spec->tag == NTYPE_ARG0_KEY ||
        spec->tag == NTYPE_ARG0_VALUE || spec->tag == NTYPE_ARG_CB_RET) {
        return 1;
    }
    return native_type_spec_has_arg_ref(spec->sub) || native_type_spec_has_arg_ref(spec->sub2);
}

// 内部实现：cb_ret = 预计算的回调返回类型（仅 NTYPE_ARG_CB_RET 节点用，可 NULL）
static TypeInfo* native_type_spec_to_info_impl(const NativeTypeSpec* spec, TypeInfo* arg0_type,
                                               TypeInfo* cb_ret);

// 规格 → TypeInfo（**新分配，调用方 type_free**），关系型标签用 arg0_type 解析。
TypeInfo* native_type_spec_to_info_with_args(const NativeTypeSpec* spec, TypeInfo* arg0_type) {
    return native_type_spec_to_info_impl(spec, arg0_type, NULL);
}

// 带预计算回调返回类型的版本（⓪ 族声明化，2026-09-28）：NTYPE_ARG_CB_RET 节点无法被
//   结构解析（需要语义侧推闭包函数体）⇒ 语义层先推好 T，从这里代入；cb_ret 为 NULL ⇒
//   该节点给 NULL（顶层自然回落 Kind 槽旧行为，宁漏勿误报 ✓）。
TypeInfo* native_type_spec_to_info_with_cb(const NativeTypeSpec* spec, TypeInfo* arg0_type,
                                           TypeInfo* cb_ret) {
    return native_type_spec_to_info_impl(spec, arg0_type, cb_ret);
}

// 内部实现：cb_ret = 预计算的回调返回类型（仅 NTYPE_ARG_CB_RET 节点用，可 NULL）
static TypeInfo* native_type_spec_to_info_impl(const NativeTypeSpec* spec, TypeInfo* arg0_type,
                                               TypeInfo* cb_ret) {
    if (!spec) return NULL;

    // 关系型标签：`NTYPE_ARG0_ELEM` 等 ⇒ 取实参里对应的那个类型。
    //   ⚠ 取不到（实参缺失 / 实参是**裸 Array / 裸 Dict** —— 元素类型"未指定"）⇒ 返回 **NULL**，
    //     **不是** `Array[any]`。区别是实打实的：`var empty = []` 的元素是"未指定"，它
    //     `empty.copy()` 出来必须仍是"未指定"（能匹配 `Array[int]`），而不是显式的 `Array[any]`
    //     （后者会**拒绝**赋给 `Array[int]` —— 实测 3 个排序用例因此回归）。
    //     顶层返回 NULL ⇒ 调用方自然回落到原来的 Kind 路径（与旧行为逐字一致）。
    if (spec->tag == NTYPE_ARG0_ELEM || spec->tag == NTYPE_ARG0_KEY ||
        spec->tag == NTYPE_ARG0_VALUE) {
        return native_arg0_part_to_info(arg0_type, spec->tag);
    }
    // 回调返回类型：**结构解析器解析不了**（要语义侧推闭包函数体）⇒ 只认预计算值；
    //   没有预计算（老入口 / 推不出）⇒ NULL ⇒ 顶层回落 Kind 槽（宁漏勿误报 ✓）
    if (spec->tag == NTYPE_ARG_CB_RET) {
        return cb_ret ? type_copy(cb_ret) : NULL;
    }

    TypeInfo* t = type_new(native_spec_kind(spec));
    if (!t) return NULL;

    if (spec->tag == NTYPE_STRUCT && spec->name) {
        t->struct_name = strdup(spec->name);
    } else if (spec->tag == NTYPE_ARRAY || spec->tag == NTYPE_PTR_GENERIC ||
               spec->tag == NTYPE_THREAD) {
        // Thread[T]（T = join 的返回类型，2026-09-28）与 Array/Ptr 同走 sub 槽
        t->element_type = native_type_spec_to_info_impl(spec->sub, arg0_type, cb_ret);
    } else if (spec->tag == NTYPE_DICT) {
        t->key_type = native_type_spec_to_info_impl(spec->sub, arg0_type, cb_ret);
        t->value_type = native_type_spec_to_info_impl(spec->sub2, arg0_type, cb_ret);
    }
    return t;
}

// 不带实参的版本（等价于 arg0_type == NULL）：关系型标签会退化成 any。
//   老调用点（返回类型与实参无关的那 230+ 个方法）行为逐字不变 ✓
TypeInfo* native_type_spec_to_info(const NativeTypeSpec* spec) {
    return native_type_spec_to_info_with_args(spec, NULL);
}

// 带**调用上下文**的规格定型（2026-09-28）：NTYPE_BY_ARITY 按**实参个数**选形态
//   （双形态内置如 `_env`：读/写返回类型不同）—— 结构解析器没有 argc，与 ARG_CB_RET
//   同属"需要调用点信息"的标签。这里纯做"规格 → 规格"替换（BY_ARITY ⇒ 选中的子形态），
//   结果交给 native_type_spec_to_info* 系列正常解析；非 BY_ARITY 原样返回。
const NativeTypeSpec* native_type_spec_resolve_for_call(const NativeTypeSpec* spec, int argc) {
    if (!spec) return NULL;
    if (spec->tag == NTYPE_BY_ARITY) {
        return native_type_spec_resolve_for_call(
            (argc < spec->arg_index) ? spec->sub : spec->sub2, argc);
    }
    return spec;
}

// 规格 → 可读类型串（LSP / 诊断）
void native_type_spec_to_string(const NativeTypeSpec* spec, char* out, int out_size) {
    if (!out || out_size <= 0) return;
    out[0] = '\0';
    if (!spec) return;

    char* p = out;
    int remain = out_size;
    #define NSPEC_APPEND(fmt, ...) \
        do { \
            int _n = snprintf(p, (size_t)remain, fmt, ##__VA_ARGS__); \
            if (_n > 0) { p += (_n < remain ? _n : remain - 1); remain -= (_n < remain ? _n : remain - 1); } \
        } while (0)

    switch (spec->tag) {
        case NTYPE_INT:    NSPEC_APPEND("int");    break;
        case NTYPE_FLOAT:  NSPEC_APPEND("float");  break;
        case NTYPE_STRING: NSPEC_APPEND("string"); break;
        case NTYPE_BOOL:   NSPEC_APPEND("bool");   break;
        case NTYPE_NULL:   NSPEC_APPEND("null");   break;
        case NTYPE_ANY:    NSPEC_APPEND("any");    break;
        case NTYPE_PTR:    NSPEC_APPEND("Ptr");    break;
        case NTYPE_FILE:   NSPEC_APPEND("File");   break;
        case NTYPE_SOCKET: NSPEC_APPEND("Socket"); break;
        case NTYPE_CHANNEL:NSPEC_APPEND("Channel");break;
        case NTYPE_THREAD:
            NSPEC_APPEND("Thread");
            if (spec->sub) {   // Thread[T]（T = join 的返回类型，2026-09-28）
                NSPEC_APPEND("[");
                native_type_spec_to_string(spec->sub, p, remain);
                p = out + strlen(out); remain = out_size - (int)strlen(out);
                NSPEC_APPEND("]");
            }
            break;
        case NTYPE_FUTURE: NSPEC_APPEND("Future"); break;
        case NTYPE_STRUCT:
            // 与 type.c 的 type_to_string 保持**同一风格**（`struct Name`）：同一个类型
            //   在报错信息与 LSP hover 里必须是同一种写法，否则用户会以为是两个东西
            NSPEC_APPEND("struct %s", spec->name ? spec->name : "");
            break;
        case NTYPE_ARRAY:
            NSPEC_APPEND("Array[");
            native_type_spec_to_string(spec->sub, p, remain);
            p = out + strlen(out); remain = out_size - (int)strlen(out);
            NSPEC_APPEND("]");
            break;
        case NTYPE_DICT:
            NSPEC_APPEND("Dict[");
            native_type_spec_to_string(spec->sub, p, remain);
            p = out + strlen(out); remain = out_size - (int)strlen(out);
            NSPEC_APPEND(", ");
            native_type_spec_to_string(spec->sub2, p, remain);
            p = out + strlen(out); remain = out_size - (int)strlen(out);
            NSPEC_APPEND("]");
            break;
        case NTYPE_PTR_GENERIC:
            NSPEC_APPEND("Ptr[");
            native_type_spec_to_string(spec->sub, p, remain);
            p = out + strlen(out); remain = out_size - (int)strlen(out);
            NSPEC_APPEND("]");
            break;
        // 关系型标签：渲染成"引用实参"的写法（诊断/LSP hover 里能一眼看出它依赖入参）
        case NTYPE_ARG0_ELEM:  NSPEC_APPEND("arg0.elem");  break;
        case NTYPE_ARG0_KEY:   NSPEC_APPEND("arg0.key");   break;
        case NTYPE_ARG0_VALUE: NSPEC_APPEND("arg0.value"); break;
        case NTYPE_ARG_CB_RET:
            if (spec->acc_index >= 0) {
                NSPEC_APPEND("argCb(%d, acc=%d)", spec->arg_index, spec->acc_index);
            } else {
                NSPEC_APPEND("argCb(%d)", spec->arg_index);
            }
            break;
        case NTYPE_BY_ARITY: {
            NSPEC_APPEND("byArity(<%d: ", spec->arg_index);
            native_type_spec_to_string(spec->sub, p, remain);
            p = out + strlen(out); remain = out_size - (int)strlen(out);
            NSPEC_APPEND(" | >=%d: ", spec->arg_index);
            native_type_spec_to_string(spec->sub2, p, remain);
            p = out + strlen(out); remain = out_size - (int)strlen(out);
            NSPEC_APPEND(")");
            break;
        }
        default: NSPEC_APPEND("any"); break;
    }
    #undef NSPEC_APPEND
}

// 已存在的同名定义与规格是否**同形**（字段个数 + 每个字段的名字/类型，按序比）
//   为什么要比：四张类型表全局**只按名字**索引，而字段序号是编译期定死的（OP_GET_FIELD 的
//   直接操作数就是序号）。native `DirEntry` 与用户自造的 `DirEntry` 撞名又不同形时，
//   混用会静默读到同序号的**错字段** —— 宁可响亮报错（同 S2 的判定哲学）。
static int native_def_matches_spec(ObjStructDef* def, const NativeStructSpec* spec) {
    if (!def || !spec) return 0;
    if (def->field_count != spec->field_count) return 0;
    for (int i = 0; i < spec->field_count; i++) {
        if (!def->fields[i].name || strcmp(def->fields[i].name, spec->field_names[i]) != 0) return 0;
        if (def->fields[i].type != native_spec_kind(spec->field_types[i])) return 0;
    }
    return 1;
}

// 按名取（必要时创建并注册）native 结构体定义。字段顺序 = spec 的字段顺序 ⇒ 与编译期索引同源。
ObjStructDef* native_struct_def_for(const char* name) {
    if (!name) return NULL;

    const NativeStructSpec* spec = native_find_struct_spec(name);
    ObjStructDef* def = struct_def_find(name);
    if (def) {
        // 没有规格 ⇒ 与旧行为一致（直接复用）；有规格但要**同形**才复用
        if (!spec || native_def_matches_spec(def, spec)) return def;
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "native 结构体 '%s' 与已存在的同名定义形状不一致（字段个数/名字/类型不同）——"
                 "类型表全局按名字索引、字段序号在编译期定死，混用会静默读到错的字段；请给其中一个改名",
                 name);
        native_throw_error(msg);
        return NULL;   // fail-closed：宁可不给实例，也不按错序号读字段
    }

    if (!spec) return NULL;

    def = struct_def_new(name, spec->field_count, 0);
    if (!def) return NULL;
    for (int i = 0; i < spec->field_count; i++) {
        const NativeTypeSpec* ft = spec->field_types[i];
        TypeKind kind = native_spec_kind(ft);
        const char* field_struct_name = (ft && ft->tag == NTYPE_STRUCT) ? ft->name : NULL;
        TypeKind elem_kind = TYPE_ANY;
        if (ft && (ft->tag == NTYPE_ARRAY || ft->tag == NTYPE_PTR_GENERIC) && ft->sub) {
            elem_kind = native_spec_kind(ft->sub);
        } else if (ft && ft->tag == NTYPE_DICT) {
            elem_kind = TYPE_ANY;   // Dict 的 K/V 在运行期字段元数据里没有两个槽（判定另走类型规格）
        }
        struct_def_set_field(def, i, spec->field_names[i], kind, field_struct_name,
                             val_null(), 0, elem_kind, 0);
    }
    struct_def_register(def);   // owner = NULL（来源未知）⇒ 与脚本同名类型冲突时按"来源未知"放行
    return def;
}

ObjStruct* native_struct_new(const char* name) {
    ObjStructDef* def = native_struct_def_for(name);
    if (!def) return NULL;
    return struct_instance_new(def);
}

int native_struct_set(ObjStruct* obj, const char* field_name, Value value) {
    if (!obj || !obj->def || !field_name) return 0;
    for (int i = 0; i < obj->def->field_count; i++) {
        if (obj->def->fields[i].name && strcmp(obj->def->fields[i].name, field_name) == 0) {
            struct_set_field(obj, i, value);   // inline：带 GC 写屏障（leno_value.h）
            return 1;
        }
    }
    return 0;
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
// ⚠ v3.2.7：判据由 `param_index < meta->arity` 改为 `param_index < meta->param_type_count`
//   —— 老的写法对**可变参数**方法（`arity == -1`）恒假 ⇒ 那些方法无论怎么声明参数类型都退回 ANY。
//   `param_type_count` 由注册时（定长 = arity；可变参数 = 0）与
//   `native_set_method_vararg_params()`（可变参数显式声明）共同维护。
TypeKind native_get_module_method_param_type(const char* module_name, const char* method_name, int param_index) {
    ModuleMethodMeta* meta = native_find_module_method(module_name, method_name);
    if (meta && param_index >= 0 && param_index < meta->param_type_count && param_index < MAX_METHOD_PARAMS) {
        return meta->param_types[param_index];
    }
    return TYPE_ANY;
}

// 为可变参数方法声明参数类型（见 native.h 的说明）。
// **必须在注册之后调用**（这里做"覆盖式"写入：先把整份 param_types 填成 tail_type，再盖上前缀）。
void native_set_method_vararg_params(const char* module_name, const char* method_name,
                                     int prefix_count, const TypeKind* prefix, TypeKind tail_type) {
    ModuleMethodEntry* entry = moduleMethodTable.entries
        ? moduleMethodTable.entries[hash_module_method(module_name, method_name) & (moduleMethodTable.capacity - 1)]
        : NULL;
    // 顺着链找同名条目（与 native_find_module_method 同一走法，只是这里要 meta 的可写指针）
    while (entry && !(strcmp(entry->module_name, module_name) == 0 &&
                      strcmp(entry->method_name, method_name) == 0)) {
        entry = entry->next;
    }
    if (!entry) return;   // 没注册过就静默忽略（与注册表其它部分的风格一致：编译期常量，不该失败）

    for (int i = 0; i < MAX_METHOD_PARAMS; i++) {
        entry->meta.param_types[i] = tail_type;
    }
    for (int i = 0; i < prefix_count && i < MAX_METHOD_PARAMS; i++) {
        entry->meta.param_types[i] = prefix ? prefix[i] : TYPE_ANY;
    }
    entry->meta.param_type_count = MAX_METHOD_PARAMS;
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

// 「只有全局函数命名空间、没有 init_module」的模块名。
// 依据：本文件的 *_init_globals 声明（io/types/strings/times/threads/sys）里，只有
// types 与 sys 没有对应的 *_init_module。import 它们合法，但运行时无需初始化 ——
// assert/test_types_module.leno 就是 `import types` 的既有用例，不得被判为未知模块。
static const char* native_namespace_only_modules[] = {
    "types",
    "sys",
    NULL
};

// 该模块是否在方法表里出现过（只比模块名，不带方法名）
static int module_has_any_method(const char* module_name) {
    if (!moduleMethodTable.entries || moduleMethodTable.count == 0) return 0;
    for (int i = 0; i < moduleMethodTable.capacity; i++) {
        for (ModuleMethodEntry* e = moduleMethodTable.entries[i]; e; e = e->next) {
            if (strcmp(e->module_name, module_name) == 0) return 1;
        }
    }
    return 0;
}

// 该模块是否在常量表里出现过（只比模块名，不带常量名）
static int module_has_any_const(const char* module_name) {
    if (!moduleConstTable.entries || moduleConstTable.count == 0) return 0;
    for (int i = 0; i < moduleConstTable.capacity; i++) {
        for (ModuleConstEntry* e = moduleConstTable.entries[i]; e; e = e->next) {
            if (strcmp(e->module_name, module_name) == 0) return 1;
        }
    }
    return 0;
}

// 该模块名是否是已注册的原生模块（编译期校验用）
// 口径与本文件各注册表一致：有 init_module 的、只有全局函数命名空间的、注册过方法或常量的，
// 都算「已知模块」；其余视为拼错的模块名。
int native_module_is_registered(const char* module_name) {
    if (!module_name) return 0;
    for (int i = 0; module_init_table[i].name != NULL; i++) {
        if (strcmp(module_name, module_init_table[i].name) == 0) return 1;
    }
    for (int i = 0; native_namespace_only_modules[i] != NULL; i++) {
        if (strcmp(module_name, native_namespace_only_modules[i]) == 0) return 1;
    }
    return module_has_any_method(module_name) || module_has_any_const(module_name);
}

// 已知原生模块名的逗号分隔列表（报错提示用；静态缓冲区，勿释放）
const char* native_module_names_csv(void) {
    static char buf[512];
    int pos = 0;
    buf[0] = '\0';
    for (int i = 0; module_init_table[i].name != NULL; i++) {
        if (pos >= (int)sizeof(buf) - 32) break;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s%s",
                        i > 0 ? ", " : "", module_init_table[i].name);
    }
    for (int i = 0; native_namespace_only_modules[i] != NULL; i++) {
        if (pos >= (int)sizeof(buf) - 32) break;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s%s",
                        pos > 0 ? ", " : "", native_namespace_only_modules[i]);
    }
    return buf;
}

// 根据模块名初始化对应的模块
// 返回 0 = 成功（含「合法但无需初始化」的模块），-1 = 未知模块名
int native_init_module(const char* module_name) {
    for (int i = 0; module_init_table[i].name != NULL; i++) {
        if (strcmp(module_name, module_init_table[i].name) == 0) {
            module_init_table[i].init_func();
            return 0;
        }
    }
    // 无 init_module 但已知的模块（types / sys 这类只有全局函数命名空间，方法/常量已在
    // 启动时注册）——无需初始化，属于正常情况
    if (native_module_is_registered(module_name)) return 0;
    // 未知模块名：显式失败。此前是静默 no-op —— 模块名拼错/字节码与运行时不一致时
    // 什么都不会发生，直到调用该模块的方法才报错（甚至可能永远不报错），归因跑偏。
    return -1;
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
    uint32_t hash = leno_fnv1a(type_name);
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
    new_entry->meta.return_spec = NULL;   // 由 native_register_instance_method_return_spec() 按需补

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

// 给**已注册**的实例方法补一条返回类型规格（见 native.h 的说明）
void native_register_instance_method_return_spec(const char* type_name, const char* method_name,
                                                 const NativeTypeSpec* spec) {
    if (!instanceMethodTable.entries || !type_name || !method_name) return;

    uint32_t hash = hash_instance_method(type_name, method_name);
    int index = hash & (instanceMethodTable.capacity - 1);

    InstanceMethodEntry* entry = instanceMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->type_name, type_name) == 0 &&
            strcmp(entry->method_name, method_name) == 0) {
            entry->meta.return_spec = spec;
            return;
        }
        entry = entry->next;
    }
    // 找不到 ⇒ 调用点写在了注册之前（或方法名拼错）⇒ 静默忽略。
    //   ⚠ 这不是"可以接受的静默"：spec 失效会让返回类型退化成 any，而断言里那些**不做收窄的
    //     强类型赋值**会立刻编译失败（test_arrays_module / test_dict_methods 都钉了），
    //     所以它不会长期潜伏（同 repo 的"错误要在最早能发现它的地方响亮地报"）。
}

const NativeTypeSpec* native_get_instance_method_return_spec(const char* type_name, const char* method_name) {
    const InstanceMethodMeta* meta = native_find_instance_method(type_name, method_name);
    return meta ? meta->return_spec : NULL;
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

// 紧凑 Levenshtein 编辑距离（C1 相似名提示用）
static int native_levenshtein(const char* a, const char* b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la == 0) return lb;
    if (lb == 0) return la;
    static int d_prev[256], d_cur[256];
    if (lb >= 256) return 3;
    for (int j = 0; j <= lb; j++) d_prev[j] = j;
    for (int i = 1; i <= la; i++) {
        d_cur[0] = i;
        for (int j = 1; j <= lb; j++) {
            int cost = (a[i-1] == b[j-1]) ? 0 : 1;
            int m = d_prev[j-1] + cost;
            if (d_prev[j] + 1 < m) m = d_prev[j] + 1;
            if (d_cur[j-1] + 1 < m) m = d_cur[j-1] + 1;
            d_cur[j] = m;
        }
        for (int j = 0; j <= lb; j++) d_prev[j] = d_cur[j];
    }
    return d_prev[lb];
}

// C1：在编译期实例方法表中找与 method_name 最相似的同类方法，
// 返回提示串（静态缓冲区；无相似名时为空串）。语义分析阶段方法运行表还没建，
// 但 native_register_all_instance_method_metas 已把元信息注册进 instanceMethodTable。
const char* native_instance_method_hint(const char* type_name, const char* method_name) {
    static char hint[128];
    hint[0] = '\0';
    if (!type_name || !method_name || !method_name[0] || !instanceMethodTable.entries) return hint;
    const char* best = NULL;
    int best_dist = 3;  // 最多允许 2 次编辑距离
    for (int i = 0; i < instanceMethodTable.capacity; i++) {
        for (InstanceMethodEntry* e = instanceMethodTable.entries[i]; e; e = e->next) {
            // method_name 是定长数组（地址恒非 NULL），无需判空
            if (strcmp(e->type_name, type_name) != 0) continue;
            int dist = native_levenshtein(method_name, e->method_name);
            if (dist < best_dist) {
                best_dist = dist;
                best = e->method_name;
                if (dist == 0) break;
            }
        }
        if (best_dist == 0) break;
    }
    if (best && strcmp(best, method_name) != 0) {
        snprintf(hint, sizeof(hint), "\n  提示: 是否想用 '%s'？", best);
    }
    return hint;
}

// D1：内置模块名相似提示。模块访问（如 stringd.trim）报"未定义的模块或变量"时，
// 在内置模块名里找编辑距离最近的候选，拼出"是否想用"提示。无相似名返回空串。
const char* native_builtin_module_hint(const char* name) {
    static char hint[128];
    hint[0] = '\0';
    if (!name || !name[0]) return hint;
    const char* best = NULL;
    int best_dist = 3;  // 最多允许 2 次编辑距离
    for (int i = 0; builtin_module_names[i] != NULL; i++) {
        int dist = native_levenshtein(name, builtin_module_names[i]);
        if (dist < best_dist) {
            best_dist = dist;
            best = builtin_module_names[i];
            if (dist == 0) break;
        }
    }
    if (best && strcmp(best, name) != 0) {
        snprintf(hint, sizeof(hint), "\n  提示: 是否想用 '%s'？", best);
    }
    return hint;
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
        error_add_at(ERR_RUNTIME, line, 0, msg);
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
                // 字符串是不可变的，直接共享引用（避免不必要的内存分配和字符数据拷贝）
                // GC 通过遍历 Value 标记对象，多个 Value 引用同一个 ObjString 是安全的
                return v;
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
