#include "../include/lenolang.h"
#include "../include/native.h"
#include "../include/leno_hash.h"
#include "../include/platform.h"
#include "../include/platform_thread.h"
#include "../include/method_table.h"
#include <string.h>

// ============================================================================
// C 布局结构体定义表（全局共享）- 动态数组实现
// 注意：cstruct 定义是编译期只读的类型元数据，所有线程共享
// ============================================================================

#define CSTRUCT_DEF_INITIAL_CAPACITY 256
#define CSTRUCT_DEF_GROW_FACTOR 2

// 全局共享的 cstruct 定义表 - 只读访问，无需锁保护
static ObjCStructDef** cstruct_def_table = NULL;
static int cstruct_def_count = 0;
static int cstruct_def_capacity = 0;

// 确保表容量足够
static int cstruct_def_ensure_capacity(int needed) {
    if (needed <= cstruct_def_capacity) {
        return 1;  // 容量足够
    }
    
    // 计算新容量
    int new_capacity = cstruct_def_capacity == 0 ? 
        CSTRUCT_DEF_INITIAL_CAPACITY : cstruct_def_capacity;
    while (new_capacity < needed) {
        new_capacity *= CSTRUCT_DEF_GROW_FACTOR;
    }
    
    // 重新分配内存
    ObjCStructDef** new_table = (ObjCStructDef**)realloc(
        cstruct_def_table, new_capacity * sizeof(ObjCStructDef*));
    
    if (!new_capacity) {
        return 0;  // 内存分配失败
    }
    
    cstruct_def_table = new_table;
    cstruct_def_capacity = new_capacity;
    return 1;
}

// ============================================================================
// 字段名哈希表实现（O(1) 查找）
// ============================================================================

#define FIELD_HASH_INITIAL_CAPACITY 16
#define FIELD_HASH_LOAD_FACTOR 0.75

// 初始化字段哈希表
static void cstruct_def_init_hash_table(ObjCStructDef* def) {
    def->field_hash_capacity = FIELD_HASH_INITIAL_CAPACITY;
    def->field_hash_table = (CStructFieldHashEntry**)calloc(
        def->field_hash_capacity, sizeof(CStructFieldHashEntry*));
}

// 扩容哈希表
static void cstruct_def_resize_hash_table(ObjCStructDef* def) {
    int old_capacity = def->field_hash_capacity;
    CStructFieldHashEntry** old_table = def->field_hash_table;
    
    // 计算新容量
    int new_capacity = old_capacity * 2;
    if (new_capacity <= 0) new_capacity = FIELD_HASH_INITIAL_CAPACITY;
    
    // 创建新表
    CStructFieldHashEntry** new_table = (CStructFieldHashEntry**)calloc(
        new_capacity, sizeof(CStructFieldHashEntry*));
    
    // 重新哈希所有条目
    for (int i = 0; i < old_capacity; i++) {
        CStructFieldHashEntry* entry = old_table[i];
        while (entry) {
            CStructFieldHashEntry* next = entry->next;
            
            // 计算新位置
            uint32_t hash = leno_fnv1a(entry->name);
            int index = hash & (new_capacity - 1);
            
            // 插入新表
            entry->next = new_table[index];
            new_table[index] = entry;
            
            entry = next;
        }
    }
    
    // 释放旧表
    free(old_table);
    
    def->field_hash_capacity = new_capacity;
    def->field_hash_table = new_table;
}

// 向哈希表添加字段
static void cstruct_def_hash_add_field(ObjCStructDef* def, const char* name, int field_index) {
    // 检查是否需要扩容
    int entry_count = 0;
    for (int i = 0; i < def->field_hash_capacity; i++) {
        CStructFieldHashEntry* entry = def->field_hash_table[i];
        while (entry) {
            entry_count++;
            entry = entry->next;
        }
    }
    
    if ((double)entry_count / def->field_hash_capacity > FIELD_HASH_LOAD_FACTOR) {
        cstruct_def_resize_hash_table(def);
    }
    
    // 创建新条目
    CStructFieldHashEntry* entry = (CStructFieldHashEntry*)malloc(sizeof(CStructFieldHashEntry));
    entry->name = strdup(name);
    entry->field_index = field_index;
    
    // 计算哈希位置
    uint32_t hash = leno_fnv1a(name);
    int index = hash & (def->field_hash_capacity - 1);
    
    // 插入链表头部
    entry->next = def->field_hash_table[index];
    def->field_hash_table[index] = entry;
}

// 从哈希表查找字段
static int cstruct_def_hash_find_field(ObjCStructDef* def, const char* name) {
    if (!def->field_hash_table || def->field_hash_capacity == 0) {
        return -1;
    }
    
    uint32_t hash = leno_fnv1a(name);
    int index = hash & (def->field_hash_capacity - 1);
    
    CStructFieldHashEntry* entry = def->field_hash_table[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            return entry->field_index;
        }
        entry = entry->next;
    }
    
    return -1;
}

// ============================================================================
// C 布局结构体定义操作
// ============================================================================

ObjCStructDef* cstruct_def_new(const char* name, int field_count, int total_size, int alignment) {
    ObjCStructDef* def = (ObjCStructDef*)gc_alloc(sizeof(ObjCStructDef), OBJ_CSTRUCT_DEF);
    def->name = strdup(name);
    def->field_count = field_count;
    def->fields = (CStructFieldInfo*)calloc(field_count, sizeof(CStructFieldInfo));
    def->total_size = total_size;
    def->alignment = alignment;
    // 初始化字段哈希表（O(1) 查找）
    def->field_hash_table = NULL;
    def->field_hash_capacity = 0;
    cstruct_def_init_hash_table(def);

    // 追踪 fields 数组和哈希表的内存
    gc_track_memory((Object*)def, 0,
        (size_t)field_count * sizeof(CStructFieldInfo) +
        (size_t)def->field_hash_capacity * sizeof(CStructFieldHashEntry*));

    return def;
}

void cstruct_def_set_field(ObjCStructDef* def, int index, const char* name, TypeKind type, int offset, int size, int array_dim, const char* struct_name, TypeKind element_type) {
    if (index < 0 || index >= def->field_count) return;

    def->fields[index].name = strdup(name);
    def->fields[index].type = type;
    def->fields[index].offset = offset;
    def->fields[index].size = size;
    def->fields[index].array_dim = array_dim;
    def->fields[index].struct_name = struct_name ? strdup(struct_name) : NULL;
    def->fields[index].element_type = element_type;
    
    // 添加到哈希表（O(1) 查找）
    cstruct_def_hash_add_field(def, name, index);
}

CStructFieldInfo* cstruct_def_find_field(ObjCStructDef* def, const char* name) {
    // 先尝试哈希表查找（O(1)）
    int index = cstruct_def_hash_find_field(def, name);
    if (index >= 0 && index < def->field_count) {
        return &def->fields[index];
    }
    // 哈希表未找到，回退到线性扫描（保险）
    for (int i = 0; i < def->field_count; i++) {
        if (strcmp(def->fields[i].name, name) == 0) {
            return &def->fields[i];
        }
    }
    return NULL;
}

// 获取字段索引
int cstruct_get_field_index(ObjCStructDef* def, const char* name) {
    // 先尝试哈希表查找（O(1)）
    int index = cstruct_def_hash_find_field(def, name);
    if (index >= 0) {
        return index;
    }
    // 哈希表未找到，回退到线性扫描（保险）
    for (int i = 0; i < def->field_count; i++) {
        if (strcmp(def->fields[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

// 注册 C 布局结构体定义
void cstruct_def_register(ObjCStructDef* def) {
    // 确保容量足够（需要 count + 1 个位置）
    if (!cstruct_def_ensure_capacity(cstruct_def_count + 1)) {
        error_add_at(ERR_RUNTIME, 0, 0, "C 布局结构体定义表内存分配失败");
        return;
    }

    // 检查是否已存在同名结构体
    for (int i = 0; i < cstruct_def_count; i++) {
        if (strcmp(cstruct_def_table[i]->name, def->name) == 0) {
            // 覆盖旧定义：将旧定义的资源指针置 NULL，防止 gc_free_all 时 double-free
            ObjCStructDef* old_def = cstruct_def_table[i];
            old_def->name = NULL;
            old_def->fields = NULL;
            old_def->field_count = 0;
            old_def->field_hash_table = NULL;
            old_def->field_hash_capacity = 0;

            cstruct_def_table[i] = def;
            return;
        }
    }

    cstruct_def_table[cstruct_def_count++] = def;
}

// 查找 C 布局结构体定义
ObjCStructDef* cstruct_def_find(const char* name) {
    for (int i = 0; i < cstruct_def_count; i++) {
        if (strcmp(cstruct_def_table[i]->name, name) == 0) {
            return cstruct_def_table[i];
        }
    }
    return NULL;
}

// ============================================================================
// C 布局结构体实例操作
// ============================================================================

ObjCStruct* cstruct_new(ObjCStructDef* def) {
    ObjCStruct* obj = (ObjCStruct*)gc_alloc(sizeof(ObjCStruct), OBJ_CSTRUCT);
    if (!obj) return NULL;
    
    obj->def = def;
    obj->data = (uint8_t*)calloc(1, def->total_size);  // 零初始化
    if (!obj->data) {
        // calloc 失败，清理已分配的 obj
        return NULL;
    }
    obj->owns_memory = 1;

    // 追踪 data 缓冲区的内存
    gc_track_memory((Object*)obj, 0, def->total_size);

    return obj;
}

ObjCStruct* cstruct_from_ptr(ObjCStructDef* def, void* ptr) {
    ObjCStruct* obj = (ObjCStruct*)gc_alloc(sizeof(ObjCStruct), OBJ_CSTRUCT);
    obj->def = def;
    obj->data = (uint8_t*)ptr;
    obj->owns_memory = 0;  // 不拥有内存，不需要 free
    return obj;
}

// 获取字段值的辅助函数
Value cstruct_get_field_value(ObjCStruct* obj, int field_index) {
    if (field_index < 0 || field_index >= obj->def->field_count) {
        return val_null();
    }

    CStructFieldInfo* field = &obj->def->fields[field_index];
    uint8_t* field_addr = obj->data + field->offset;

    switch (field->type) {
        case TYPE_I8:
            return val_num((double)(*(int8_t*)field_addr));
        case TYPE_U8:
            return val_num((double)(*(uint8_t*)field_addr));
        case TYPE_I16:
            return val_num((double)(*(int16_t*)field_addr));
        case TYPE_U16:
            return val_num((double)(*(uint16_t*)field_addr));
        case TYPE_I32:
            return val_num((double)(*(int32_t*)field_addr));
        case TYPE_U32:
            return val_num((double)(*(uint32_t*)field_addr));
        case TYPE_I64:
            return val_int_safe(*(int64_t*)field_addr);
        case TYPE_U64:
            return val_int_safe((int64_t)(*(uint64_t*)field_addr));
        case TYPE_F32:
            return val_num((double)(*(float*)field_addr));
        case TYPE_F64:
            return val_num((*(double*)field_addr));
        case TYPE_BOOL:
            return val_bool(*(uint8_t*)field_addr);
        case TYPE_C_INT:
            return val_num((double)(*(int*)field_addr));
        case TYPE_C_UINT:
            return val_num((double)(*(unsigned int*)field_addr));
        case TYPE_C_LONG:
            return val_num((double)(*(long*)field_addr));
        case TYPE_C_ULONG:
            return val_num((double)(*(unsigned long*)field_addr));
        case TYPE_C_LONGLONG:
            return val_int_safe(*(long long*)field_addr);
        case TYPE_C_ULONGLONG:
            return val_int_safe((int64_t)(*(unsigned long long*)field_addr));
        case TYPE_C_SIZE:
            return val_int_safe((int64_t)(*(size_t*)field_addr));
        case TYPE_C_SSIZE:
            return val_int_safe((int64_t)(*(ssize_t*)field_addr));
        case TYPE_PTR:
        case TYPE_PTR_GENERIC: {
            void* ptr = *(void**)field_addr;
            if (ptr) {
                ObjFFIPointer* ffi_ptr = (ObjFFIPointer*)gc_alloc(sizeof(ObjFFIPointer), OBJ_FFI_POINTER);
                ffi_ptr->ptr = ptr;
                ffi_ptr->size = 0;
                ffi_ptr->owned = 0;
                ffi_ptr->freed = 0;
                // 传递元素类型信息
                ffi_ptr->element_type = (field->type == TYPE_PTR_GENERIC) ? field->element_type : TYPE_PTR;
                return val_obj((Object*)ffi_ptr);
            }
            return val_null();
        }
        case TYPE_STR16: {
            // str16 类型：将 UTF-16 数组转换为 UTF-8 字符串（跨平台）
            if (field->array_dim <= 0) {
                return val_null();
            }
            
            wchar_t* wstr = (wchar_t*)field_addr;
            // 确保以 null 结尾
            int max_len = field->array_dim;
            int len = 0;
            while (len < max_len && wstr[len] != L'\0') len++;
            
            // 创建临时 wchar_t 数组（确保 null 结尾）
            wchar_t* temp = (wchar_t*)malloc((len + 1) * sizeof(wchar_t));
            if (!temp) return val_null();
            memcpy(temp, wstr, len * sizeof(wchar_t));
            temp[len] = L'\0';
            
            char* utf8 = utf16_to_utf8(temp);
            free(temp);
            
            if (utf8) {
                ObjString* str = str_new(utf8, strlen(utf8));
                free(utf8);
                return val_obj((Object*)str);
            }
            return val_null();
        }
        default:
            return val_null();
    }
}

// 设置字段值的辅助函数
void cstruct_set_field_value(ObjCStruct* obj, int field_index, Value value) {
    if (field_index < 0 || field_index >= obj->def->field_count) {
        return;
    }

    CStructFieldInfo* field = &obj->def->fields[field_index];
    uint8_t* field_addr = obj->data + field->offset;

    switch (field->type) {
        case TYPE_I8:
            *(int8_t*)field_addr = (int8_t)val_as_num(value);
            break;
        case TYPE_U8:
            *(uint8_t*)field_addr = (uint8_t)val_as_num(value);
            break;
        case TYPE_I16:
            *(int16_t*)field_addr = (int16_t)val_as_num(value);
            break;
        case TYPE_U16:
            *(uint16_t*)field_addr = (uint16_t)val_as_num(value);
            break;
        case TYPE_I32:
            *(int32_t*)field_addr = (int32_t)val_as_num(value);
            break;
        case TYPE_U32:
            *(uint32_t*)field_addr = (uint32_t)val_as_num(value);
            break;
        case TYPE_I64:
            *(int64_t*)field_addr = (int64_t)val_as_num(value);
            break;
        case TYPE_U64:
            *(uint64_t*)field_addr = (uint64_t)val_as_num(value);
            break;
        case TYPE_F32:
            *(float*)field_addr = (float)val_as_num(value);
            break;
        case TYPE_F64:
            *(double*)field_addr = (double)val_as_num(value);
            break;
        case TYPE_BOOL:
            *(uint8_t*)field_addr = val_as_num(value) != 0 ? 1 : 0;
            break;
        case TYPE_C_INT:
            *(int*)field_addr = (int)val_as_num(value);
            break;
        case TYPE_C_UINT:
            *(unsigned int*)field_addr = (unsigned int)val_as_num(value);
            break;
        case TYPE_C_LONG:
            *(long*)field_addr = (long)val_as_num(value);
            break;
        case TYPE_C_ULONG:
            *(unsigned long*)field_addr = (unsigned long)val_as_num(value);
            break;
        case TYPE_C_LONGLONG:
            *(long long*)field_addr = (long long)val_as_num(value);
            break;
        case TYPE_C_ULONGLONG:
            *(unsigned long long*)field_addr = (unsigned long long)val_as_num(value);
            break;
        case TYPE_C_SIZE:
            *(size_t*)field_addr = (size_t)val_as_num(value);
            break;
        case TYPE_C_SSIZE:
            *(ssize_t*)field_addr = (ssize_t)val_as_num(value);
            break;
        case TYPE_PTR:
        case TYPE_PTR_GENERIC:
            if (val_is_obj(value) && val_as_obj(value)->type == OBJ_FFI_POINTER) {
                *(void**)field_addr = ((ObjFFIPointer*)val_as_obj(value))->ptr;
            } else if (val_is_obj(value) && val_as_obj(value)->type == OBJ_FFI_CALLBACK) {
                *(void**)field_addr = ((ObjFFICallback*)val_as_obj(value))->trampoline;
            }
            break;
        case TYPE_STR16: {
            // str16 类型：将 UTF-8 字符串转换为 UTF-16 数组（跨平台）
            if (field->array_dim <= 0) {
                break;
            }
            if (!val_is_obj(value) || val_as_obj(value)->type != OBJ_STRING) {
                break;
            }
            
            ObjString* str = (ObjString*)val_as_obj(value);
            wchar_t* wstr = utf8_to_utf16(str->chars);
            if (wstr) {
                size_t wlen = wcslen(wstr);
                size_t max_len = field->array_dim;
                // 复制 UTF-16 字符到字段（确保不越界，并 null 结尾）
                wchar_t* dest = (wchar_t*)field_addr;
                if (wlen >= max_len) {
                    wlen = max_len - 1; // 留一个位置给 null 终止符
                }
                memcpy(dest, wstr, wlen * sizeof(wchar_t));
                dest[wlen] = L'\0';
                free(wstr);
            }
            break;
        }
        default:
            break;
    }
}

// 标记所有 C 布局结构体定义（供 GC 使用）
void cstruct_def_mark_all(void) {
    extern void gc_mark_object(Object* obj);
    for (int i = 0; i < cstruct_def_count; i++) {
        gc_mark_object((Object*)cstruct_def_table[i]);
    }
}

// ============================================================================
// cstruct 方法表（运行时 - 使用通用 MethodTable）
// ============================================================================

#define CSTRUCT_METHOD_TABLE_INITIAL_CAPACITY 16

static THREAD_LOCAL MethodTable cstructMethodTable = {NULL, 0, 0};

// 注册 cstruct 方法（带参数类型）
void cstruct_register_method_with_params(const char* name, ObjNative* method, int arity,
                                         int min_arity, int max_arity,
                                         TypeKind return_type, TypeKind return_element_type, TypeKind* param_types) {
    method_table_register_with_params(&cstructMethodTable, "cstruct", name, method, arity, min_arity, max_arity, return_type, return_element_type, param_types);
}

// 查找 cstruct 方法
ObjNative* cstruct_find_method(const char* name) {
    return method_table_find(&cstructMethodTable, name);
}

// 查找 cstruct 方法的元信息（用于 LSP 悬停）
CStructMethodEntry cstruct_find_method_meta(const char* name) {
    return method_table_find_meta(&cstructMethodTable, name);
}

// 标记 cstruct 方法表中的所有方法（供 GC 使用）
void cstruct_mark_methods(void) {
    method_table_mark(&cstructMethodTable);
}

void cstruct_init_methods(void) {
    method_table_init_methods(&cstructMethodTable, CSTRUCT_METHOD_TABLE_INITIAL_CAPACITY);
}

// ============================================================================
// C 布局结构体数组操作（批量操作）
// ============================================================================

// 创建 C 布局结构体数组
ObjCStructArray* cstruct_array_new(ObjCStructDef* def, int count) {
    if (count <= 0) return NULL;
    
    ObjCStructArray* array = (ObjCStructArray*)gc_alloc(sizeof(ObjCStructArray), OBJ_CSTRUCT_ARRAY);
    array->def = def;
    array->count = count;
    array->element_size = def->total_size;
    
    // 分配连续内存
    size_t total_size = (size_t)array->element_size * count;
    array->data = (uint8_t*)calloc(1, total_size);
    if (!array->data) {
        // 内存分配失败
        return NULL;
    }

    // 追踪 data 缓冲区的内存
    gc_track_memory((Object*)array, 0, total_size);

    return array;
}

// 从 cstruct 数组获取指定索引的元素（返回临时 cstruct 实例，不拥有内存）
ObjCStruct* cstruct_array_get(ObjCStructArray* array, int index) {
    if (!array || index < 0 || index >= array->count) {
        return NULL;
    }
    
    // 创建临时 cstruct 实例（不拥有内存）
    ObjCStruct* obj = (ObjCStruct*)gc_alloc(sizeof(ObjCStruct), OBJ_CSTRUCT);
    obj->def = array->def;
    obj->data = array->data + (index * array->element_size);
    obj->owns_memory = 0;  // 不拥有内存，由数组管理
    
    return obj;
}

// 释放 C 布局结构体数组
void cstruct_array_free(ObjCStructArray* array) {
    if (!array) return;
    
    if (array->data) {
        free(array->data);
        array->data = NULL;
    }
    array->count = 0;
    array->element_size = 0;
}
