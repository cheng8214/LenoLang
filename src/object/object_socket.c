/* ============================================================================
 * Socket 方法注册表（使用通用 MethodTable）
 * 支持 sock.send()、sock.recv()、sock.close() 等实例方法调用
 * ============================================================================ */

#include "include/lenolang.h"
#include "include/native.h"
#include "include/platform_thread.h"
#include "include/method_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOCKET_METHOD_TABLE_INITIAL_CAPACITY 32

static THREAD_LOCAL MethodTable socketMethodTable = {NULL, 0, 0};

/* 注册 Socket 方法（带参数类型信息，供编译期和运行时使用） */
void socket_register_method_with_params(const char* name, ObjNative* method, int arity,
                                        int min_arity, int max_arity,
                                        TypeKind return_type, TypeKind return_element_type, TypeKind* param_types) {
    method_table_register_with_params(&socketMethodTable, "Socket", name, method, arity, min_arity, max_arity, return_type, return_element_type, param_types);
}

/* 查找 Socket 方法（运行时 O(1) 查找） */
ObjNative* socket_find_method(const char* name) {
    return method_table_find(&socketMethodTable, name);
}

/* 初始化 Socket 方法表 */
void socket_init_methods(void) {
    method_table_init_methods(&socketMethodTable, SOCKET_METHOD_TABLE_INITIAL_CAPACITY);
}

/* 标记所有 Socket 方法对象（供 GC 使用） */
void socket_mark_methods(void) {
    method_table_mark(&socketMethodTable);
}
