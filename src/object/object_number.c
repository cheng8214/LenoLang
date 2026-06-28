#include "include/lenolang.h"
#include "include/native.h"
#include "include/platform_thread.h"
#include "include/method_table.h"
#include <string.h>

// ============================================================================
// 数字方法注册表（使用通用 MethodTable）
// ============================================================================

#define NUMBER_METHOD_TABLE_INITIAL_CAPACITY 16

static THREAD_LOCAL MethodTable numberMethodTable = {NULL, 0, 0};

void number_register_method_with_params(const char* name, ObjNative* method, int arity, int min_arity, int max_arity,
                                         TypeKind return_type, TypeKind return_element_type, TypeKind* param_types) {
    method_table_register_with_params(&numberMethodTable, "number", name, method, arity, min_arity, max_arity, return_type, return_element_type, param_types);
}

// 查找数字方法（O(1)）
ObjNative* number_find_method(const char* name) {
    return method_table_find(&numberMethodTable, name);
}

void number_init_methods(void) {
    method_table_init_methods(&numberMethodTable, NUMBER_METHOD_TABLE_INITIAL_CAPACITY);
}

void number_mark_methods(void) {
    method_table_mark(&numberMethodTable);
}
