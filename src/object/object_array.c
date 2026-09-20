#include "include/lenolang.h"
#include "include/native.h"
#include "include/platform_thread.h"
#include "include/method_table.h"
#include <stdlib.h>
#include <string.h>

// ============================================================================
// 数组对象 Free List（小数组复用，减少 malloc 调用）
// ============================================================================
#define ARRAY_FREE_LIST_MAX_CAP  64    // 最大缓存容量
#define ARRAY_FREE_LIST_MAX_COUNT 32   // 最多缓存 32 个数组

typedef struct ArrayFreeSlot {
    ObjArray* array;
    int capacity;
    struct ArrayFreeSlot* next;
} ArrayFreeSlot;

static THREAD_LOCAL ArrayFreeSlot* array_free_list = NULL;
static THREAD_LOCAL int array_free_count = 0;

// 数组操作
ObjArray* arr_new(int capacity) {
    // 小容量数组优先从 free_list 获取
    if (capacity <= ARRAY_FREE_LIST_MAX_CAP && array_free_list) {
        ArrayFreeSlot** prev = &array_free_list;
        ArrayFreeSlot* slot = array_free_list;
        while (slot) {
            if (slot->capacity == capacity) {
                *prev = slot->next;
                ObjArray* arr = slot->array;
                free(slot);
                array_free_count--;
                
                // 清零元素并重新注册到 GC
                for (int i = 0; i < capacity; i++) {
                    arr->elements[i] = val_null();
                }
                arr->count = 0;
                arr->capacity = capacity;
                arr->type_info = NULL;
                
                // 重新链接到 GC 年轻代
                arr->header.type = OBJ_ARRAY;
                arr->header.marked = 1;
                arr->header.flags = 0;
                arr->header.generation = GEN_YOUNG;
                arr->header.survived = 0;
                arr->header.size = sizeof(ObjArray) + capacity * sizeof(Value);
                arr->header.next = gc.young_heap;
                gc.young_heap = (Object*)arr;
                gc.young_allocated += arr->header.size;
                
                return arr;
            }
            prev = &slot->next;
            slot = slot->next;
        }
    }
    
    // 正常分配
    ObjArray* arr = (ObjArray*)gc_alloc(sizeof(ObjArray), OBJ_ARRAY);
    if (!arr) return NULL;
    
    arr->elements = (Value*)malloc(capacity * sizeof(Value));
    if (!arr->elements) {
        native_throw_error("数组内存分配失败");
        return NULL;
    }
    
    // 追踪 elements 数组的内存占用，确保 GC 正确计算内存使用量
    gc_track_memory((Object*)arr, 0, capacity * sizeof(Value));
    
    arr->count = 0;
    arr->capacity = capacity;
    arr->type_info = NULL;  // 运行时类型信息初始为 NULL
    
    // 初始化为 null
    for (int i = 0; i < capacity; i++) {
        arr->elements[i] = val_null();
    }
    
    return arr;
}

// GC sweep 时调用：尝试回收小数组到 free_list
// 返回 1 表示已回收（调用者不应 free），返回 0 表示正常释放
int arr_try_recycle(ObjArray* arr) {
    if (!arr || arr->capacity > ARRAY_FREE_LIST_MAX_CAP) return 0;
    if (array_free_count >= ARRAY_FREE_LIST_MAX_COUNT) return 0;
    
    // 先分配 slot，失败则回退到正常 free 路径
    ArrayFreeSlot* slot = (ArrayFreeSlot*)malloc(sizeof(ArrayFreeSlot));
    if (!slot) return 0;
    
    // 清理数组内容但保留 elements 缓冲区
    for (int i = 0; i < arr->count; i++) {
        arr->elements[i] = val_null();
    }
    arr->count = 0;
    
    // 释放 type_info
    if (arr->type_info) {
        type_free(arr->type_info);
        arr->type_info = NULL;
    }
    
    // 加入 free_list
    slot->array = arr;
    slot->capacity = arr->capacity;
    slot->next = array_free_list;
    array_free_list = slot;
    array_free_count++;
    
    return 1;  // 已回收，元素缓冲区和对象结构体均保留
}

// 注意：arr_read, arr_write 已在 leno_value.h 中内联

// 数组扩容 - 使用简单的2倍增长策略
// 返回1成功, 0失败
int arr_grow(ObjArray* arr) {
    if (!arr) return 0;

    int new_capacity = arr->capacity < 8 ? 8 : arr->capacity * 2;

    size_t old_size = (size_t)arr->capacity * sizeof(Value);
    size_t new_size = (size_t)new_capacity * sizeof(Value);

    Value* new_elements = (Value*)realloc(arr->elements, new_size);
    if (!new_elements) {
        native_throw_error("数组内存分配失败");
        return 0;
    }

    gc_track_memory((Object*)arr, old_size, new_size);

    // 初始化新分配的空间
    for (int i = arr->capacity; i < new_capacity; i++) {
        new_elements[i] = val_null();
    }

    arr->elements = new_elements;
    arr->capacity = new_capacity;
    return 1;
}

// ============================================================================
// 数组方法注册表（使用通用 MethodTable）
// ============================================================================

static THREAD_LOCAL MethodTable arrayMethodTable = {NULL, 0, 0};

#define ARRAY_METHOD_TABLE_INITIAL_CAPACITY 32

// 注册数组方法（带参数类型）
void array_register_method_with_params(const char* name, ObjNative* method, int arity, int min_arity, int max_arity,
                                        TypeKind return_type, TypeKind return_element_type, TypeKind* param_types) {
    method_table_register_with_params(&arrayMethodTable, "Array", name, method, arity, min_arity, max_arity, return_type, return_element_type, param_types);
}

// 获取数组方法的参数类型
TypeKind array_get_method_param_type(const char* method_name, int param_index) {
    return method_table_get_param_type(&arrayMethodTable, method_name, param_index);
}

// 查找数组方法（O(1)）
ObjNative* array_find_method(const char* name) {
    return method_table_find(&arrayMethodTable, name);
}

// 查找数组方法的元信息（用于编译期类型检查）
ArrayMethodEntry array_find_method_meta(const char* name) {
    return method_table_find_meta(&arrayMethodTable, name);
}

// 初始化数组方法表
void array_init_methods(void) {
    method_table_init_methods(&arrayMethodTable, ARRAY_METHOD_TABLE_INITIAL_CAPACITY);
}

// 标记所有数组方法对象（供 GC 使用）
void array_mark_methods(void) {
    method_table_mark(&arrayMethodTable);
}
