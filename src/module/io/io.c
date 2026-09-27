#include "include/lenolang.h"
#include "include/native.h"
#include "include/platform_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ==================== 线程安全输出锁 ====================

// 全局输出锁，确保多线程环境下输出不会交错
static PlatformMutex g_output_mutex;
static volatile int g_output_mutex_initialized = 0;
static PlatformMutex g_init_mutex;  // 用于保护初始化

// 初始化输出锁（线程安全）
static void io_init_output_mutex(void) {
    if (!g_output_mutex_initialized) {
        platform_mutex_init(&g_init_mutex);
        platform_mutex_lock(&g_init_mutex);
        if (!g_output_mutex_initialized) {
            platform_mutex_init(&g_output_mutex);
            g_output_mutex_initialized = 1;
        }
        platform_mutex_unlock(&g_init_mutex);
    }
}

// ==================== 核心实现层（纯逻辑，无参数检查）====================

// 前向声明
static void print_value_internal(Value value);
static void print_value_quoted(Value value);
static void print_array_internal(ObjArray* arr);
static void print_dict_internal(ObjDict* dict);

// 外部声明：tombstone 哨兵（线程局部存储，用于标识已删除的字典条目）
extern _Thread_local ObjString* tombstone;

// 打印值（用于普通输出，字符串不加引号）
static void print_value_internal(Value value) {
    switch (val_get_type(value)) {
        case VAL_NULL:
            printf("null");
            break;
        case VAL_BOOL:
            printf(val_as_bool(value) ? "true" : "false");
            break;
        case VAL_INT:
            printf("%lld", (long long)val_as_int(value));
            break;
        case VAL_FLOAT: {
            char fbuf[64];
            snprintf(fbuf, sizeof(fbuf), "%.17g", val_as_num(value));
            if (!strchr(fbuf, '.') && !strchr(fbuf, 'e') && !strchr(fbuf, 'E')) {
                size_t len = strlen(fbuf);
                if (len + 2 < sizeof(fbuf)) {
                    fbuf[len] = '.';
                    fbuf[len + 1] = '0';
                    fbuf[len + 2] = '\0';
                }
            }
            printf("%s", fbuf);
            break;
        }
        case VAL_OBJ: {
            Object* obj = val_as_obj(value);
            switch (obj->type) {
                case OBJ_STRING: {
                    ObjString* s = (ObjString*)obj;
                    fwrite(s->chars, 1, s->len, stdout);
                    break;
                }
                case OBJ_ARRAY:
                    print_array_internal((ObjArray*)obj);
                    break;
                case OBJ_DICT:
                    print_dict_internal((ObjDict*)obj);
                    break;
                case OBJ_BIGINT: {
                    ObjBigInt* bigint = (ObjBigInt*)obj;
                    char* str = bigint_to_string(bigint);
                    if (str) {
                        printf("%s", str);
                        free(str);
                    }
                    break;
                }
                case OBJ_STRUCT: {
                    ObjStruct* struct_obj = (ObjStruct*)obj;
                    ObjStructDef* struct_def = struct_obj->def;
                    printf("%s{", struct_def->name);
                    for (int i = 0; i < struct_def->field_count; i++) {
                        if (i > 0) printf(", ");
                        printf("%s=", struct_def->fields[i].name);
                        print_value_quoted(struct_obj->field_values[i]);
                    }
                    printf("}");
                    break;
                }
                case OBJ_ENUM_DEF: {
                    ObjEnumDef* enum_def = (ObjEnumDef*)obj;
                    printf("{");
                    for (int i = 0; i < enum_def->member_count; i++) {
                        if (i > 0) printf(", ");
                        printf("%s: %lld", enum_def->members[i].name, (long long)enum_def->members[i].value);
                    }
                    printf("}");
                    break;
                }
                case OBJ_FFI_POINTER: {
                    void* ptr = ((ObjFFIPointer*)obj)->ptr;
                    printf("<ptr %p>", ptr);
                    break;
                }
                case OBJ_FFI_CALLBACK: {
                    void* trampoline = ((ObjFFICallback*)obj)->trampoline;
                    printf("<callback %p>", trampoline);
                    break;
                }
                case OBJ_FILE: {
                    printf("<file>");
                    break;
                }
                case OBJ_CSTRUCT_DEF: {
                    ObjCStructDef* def = (ObjCStructDef*)obj;
                    printf("cstruct %s{", def->name);
                    for (int i = 0; i < def->field_count; i++) {
                        if (i > 0) printf(", ");
                        printf("%s %s", type_kind_to_string(def->fields[i].type), def->fields[i].name);
                    }
                    printf("}");
                    break;
                }
                case OBJ_CSTRUCT: {
                    ObjCStruct* cst = (ObjCStruct*)obj;
                    ObjCStructDef* def = cst->def;
                    printf("cstruct %s{", def->name);
                    for (int i = 0; i < def->field_count; i++) {
                        if (i > 0) printf(", ");
                        printf("%s=", def->fields[i].name);
                        Value field_val = cstruct_get_field_value(cst, i);
                        print_value_quoted(field_val);
                    }
                    printf("}");
                    break;
                }
                case OBJ_SOCKET: {
                    ObjSocket* sock = (ObjSocket*)obj;
                    printf("<Socket fd=%d>", (int)sock->fd);
                    break;
                }
                default:
                    printf("<object>");
                    break;
            }
            break;
        }
    }
}

// 打印值（用于数组/字典内部输出，字符串加引号）
static void print_value_quoted(Value value) {
    switch (val_get_type(value)) {
        case VAL_NULL:
            printf("null");
            break;
        case VAL_BOOL:
            printf(val_as_bool(value) ? "true" : "false");
            break;
        case VAL_INT:
            printf("%lld", (long long)val_as_int(value));
            break;
        case VAL_FLOAT: {
            char fbuf[64];
            snprintf(fbuf, sizeof(fbuf), "%.17g", val_as_num(value));
            if (!strchr(fbuf, '.') && !strchr(fbuf, 'e') && !strchr(fbuf, 'E')) {
                size_t len = strlen(fbuf);
                if (len + 2 < sizeof(fbuf)) {
                    fbuf[len] = '.';
                    fbuf[len + 1] = '0';
                    fbuf[len + 2] = '\0';
                }
            }
            printf("%s", fbuf);
            break;
        }
        case VAL_OBJ: {
            Object* obj = val_as_obj(value);
            switch (obj->type) {
                case OBJ_STRING: {
                    ObjString* s = (ObjString*)obj;
                    printf("\"");
                    fwrite(s->chars, 1, s->len, stdout);
                    printf("\"");
                    break;
                }
                case OBJ_ARRAY:
                    print_array_internal((ObjArray*)obj);
                    break;
                case OBJ_DICT:
                    print_dict_internal((ObjDict*)obj);
                    break;
                case OBJ_BIGINT: {
                    ObjBigInt* bigint = (ObjBigInt*)obj;
                    char* str = bigint_to_string(bigint);
                    if (str) {
                        printf("%s", str);
                        free(str);
                    }
                    break;
                }
                case OBJ_STRUCT: {
                    ObjStruct* struct_obj = (ObjStruct*)obj;
                    ObjStructDef* struct_def = struct_obj->def;
                    printf("%s{", struct_def->name);
                    for (int i = 0; i < struct_def->field_count; i++) {
                        if (i > 0) printf(", ");
                        printf("%s=", struct_def->fields[i].name);
                        print_value_quoted(struct_obj->field_values[i]);
                    }
                    printf("}");
                    break;
                }
                case OBJ_FFI_POINTER: {
                    // FFI 指针显示为 <ptr 0x...>，方便调试
                    void* ptr = ((ObjFFIPointer*)obj)->ptr;
                    printf("<ptr %p>", ptr);
                    break;
                }
                case OBJ_FFI_CALLBACK: {
                    void* trampoline = ((ObjFFICallback*)obj)->trampoline;
                    printf("<callback %p>", trampoline);
                    break;
                }
                case OBJ_FILE: {
                    printf("<file>");
                    break;
                }
                case OBJ_SOCKET: {
                    ObjSocket* sock = (ObjSocket*)obj;
                    printf("<Socket fd=%d>", (int)sock->fd);
                    break;
                }
                default:
                    printf("<object>");
                    break;
            }
            break;
        }
    }
}

static void print_array_internal(ObjArray* arr) {
    printf("[");
    for (int i = 0; i < arr->count; i++) {
        print_value_quoted(arr->elements[i]);
        if (i < arr->count - 1) {
            printf(", ");
        }
    }
    printf("]");
}

static void print_dict_internal(ObjDict* dict) {
    printf("{");
    int printed = 0;

    // 优先使用插入顺序数组
    for (int i = 0; i < dict->order_count; i++) {
        Value order_key = dict->order[i];
        if (val_is_null(order_key)) continue;
        // 检查键是否仍然存在（未被删除）
        Value value = dict_get(dict, order_key);
        if (!dict_has(dict, order_key)) continue;

        // 跳过整数键（这些应该在数组部分处理）
        if (val_is_int(order_key)) continue;
        // 跳过纯数字字符串键
        if (val_is_obj(order_key) && val_as_obj(order_key)->type == OBJ_STRING) {
            ObjString* ks = (ObjString*)val_as_obj(order_key);
            int is_numeric = 1;
            for (int j = 0; j < ks->len; j++) {
                if (ks->chars[j] < '0' || ks->chars[j] > '9') {
                    is_numeric = 0;
                    break;
                }
            }
            if (is_numeric) continue;
        }

        if (printed > 0) {
            printf(", ");
        }
        // 输出键名
        if (val_is_obj(order_key) && val_as_obj(order_key)->type == OBJ_STRING) {
            ObjString* ks = (ObjString*)val_as_obj(order_key);
            fwrite(ks->chars, 1, ks->len, stdout);
            printf(": ");
        } else {
            ObjString* ks = dict_key_to_string(order_key);
            if (ks) {
                fwrite(ks->chars, 1, ks->len, stdout);
            }
            printf(": ");
        }
        print_value_quoted(value);
        printed++;
    }

    // 处理数组部分的数字键（按数字顺序）
    for (int i = 0; i < dict->asize; i++) {
        if (!val_is_null(dict->array[i])) {
            if (printed > 0) {
                printf(", ");
            }
            printf("%d: ", i);
            print_value_quoted(dict->array[i]);
            printed++;
        }
    }

    printf("}");
}

// 核心：打印值到 stdout（带空格分隔，线程安全）
static void print_values_core(int argCount, Value* args, int addNewline) {
    // 确保输出锁已初始化
    io_init_output_mutex();

    // 加锁，确保完整的一行输出不会被其他线程打断
    platform_mutex_lock(&g_output_mutex);

    for (int i = 0; i < argCount; i++) {
        print_value_internal(args[i]);
        if (i < argCount - 1) {
            printf(" ");
        }
    }
    if (addNewline) {
        printf("\n");
    }
    fflush(stdout);  // 确保立即输出

    // 解锁
    platform_mutex_unlock(&g_output_mutex);
}

// 核心：读取用户输入
static ObjString* read_input_core(void) {
    char buffer[BUFFER_XLARGE];
    if (fgets(buffer, sizeof(buffer), stdin) == NULL) {
        return str_copy("", 0);
    }

    // 去掉换行符
    size_t len = strlen(buffer);
    if (len > 0 && buffer[len - 1] == '\n') {
        buffer[len - 1] = '\0';
        len--;
    }

    return str_copy(buffer, (int)len);
}

// ==================== 全局函数适配器层 ====================

// print(...) - 全局打印函数，自动换行
static Value native_print(int argCount, Value* args) {
    print_values_core(argCount, args, 1);
    return val_null();
}

// printf(...) - 全局打印函数，不换行
static Value native_printf(int argCount, Value* args) {
    print_values_core(argCount, args, 0);
    return val_null();
}

// input(prompt) - 全局输入函数
static Value native_input(int argCount, Value* args) {
    if (argCount > 1) {
        // 运行时错误
        return val_null();
    }

    if (argCount > 0) {
        print_value_internal(args[0]);
    }

    return val_obj((Object*)read_input_core());
}

// ==================== 初始化 ====================

void io_init_globals(void) {
    // 注册全局 print 函数（返回 null/void，可变参数，0~∞）
    // ⚠ 原来写的是 TYPE_ANY —— 与**本行注释**（"返回 null/void"）不一致：实现就是 `return val_null()`
    //   ⇒ 标 `TYPE_NULL`（v3.2.6）。标 any 的后果是 `var x = print(...)` 推出 any 而不是 null。
    vm_register_native("print", native_print, -1, 0, -1, TYPE_NULL, TYPE_UNKNOWN, NULL);

    // 注册全局 printf 函数（返回 null/void，可变参数，0~∞）
    vm_register_native("printf", native_printf, -1, 0, -1, TYPE_NULL, TYPE_UNKNOWN, NULL);

    // 注册全局 input 函数（返回 string，0 或 1 个参数）
    // 参数类型 string（v3.2.8）：唯一那个可选参数是**提示文案** —— 此前是 any（`input(123)` 也能编过）。
    //   ⚠ `arity == -1`（可变参数）时 vm_register_native 的 param_types 实参是**整份忽略**的
    //   ⇒ 必须注册之后再声明（`native_set_builtin_vararg_params`，与模块通道同一套做法）。
    vm_register_native("input", native_input, -1, 0, 1, TYPE_STRING, TYPE_UNKNOWN, NULL);
    TypeKind input_params[] = {TYPE_STRING};
    native_set_builtin_vararg_params("input", 1, input_params, TYPE_STRING);
}

// 初始化 io 模块（import io 时调用）
void io_init_module(void) {
    // 注册 io.print 方法（模块名，方法名，函数指针，参数数量，返回类型，返回元素类型，参数类型数组）
    // 返回类型 `null`（v3.2.6）：实现是 `return val_null()`（只打印、不产生值）⇒ 别再说它是 any。
    native_register_module_method_spec("io", "print", native_print, -1, 0, -1, &NATIVE_T_NULL, NULL);

    // 注册 io.printf 方法（模块名，方法名，函数指针，参数数量，返回类型，返回元素类型，参数类型数组）
    native_register_module_method_spec("io", "printf", native_printf, -1, 0, -1, &NATIVE_T_NULL, NULL);

    // 注册 io.input 方法（模块名，方法名，函数指针，参数数量，返回类型，返回元素类型，参数类型数组）
    // 参数类型 string（v3.2.8，同全局内置那处）：可选参数是提示文案 ⇒ 别再留成 any。
    //   ⚠ 可变参数方法的 param_types **由注册后的 `native_set_method_vararg_params` 声明**
    //     （注册时的实参对它整份忽略 —— 实例二十三的口径）。
    native_register_module_method_spec("io", "input", native_input, -1, 0, 1, &NATIVE_T_STRING, NULL);
    TypeKind input_params[] = {TYPE_STRING};
    native_set_method_vararg_params("io", "input", 1, input_params, TYPE_STRING);
}
