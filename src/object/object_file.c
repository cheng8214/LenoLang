#include "include/lenolang.h"
#include "include/native.h"
#include "include/platform_thread.h"
#include "include/method_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// 文件方法注册表（使用通用 MethodTable）
// ============================================================================

#define FILE_METHOD_TABLE_INITIAL_CAPACITY 16

static THREAD_LOCAL MethodTable fileMethodTable = {NULL, 0, 0};

// 注册文件方法（带参数类型）
void file_register_method(const char* name, ObjNative* method,
                                  TypeKind return_type, TypeKind return_element_type,
                                  NativeParamSpec params) {
    method_table_register_method(&fileMethodTable, "File", name, method, return_type, return_element_type, params);
}

// 获取文件方法的参数类型

// 查找文件方法（O(1)）
ObjNative* file_find_method(const char* name) {
    return method_table_find(&fileMethodTable, name);
}

// 查找文件方法的元信息（用于编译期类型检查）

void file_init_methods(void) {
    method_table_init_methods(&fileMethodTable, FILE_METHOD_TABLE_INITIAL_CAPACITY);
}

// 标记所有文件方法对象（供 GC 使用）
void file_mark_methods(void) {
    method_table_mark(&fileMethodTable);
}
