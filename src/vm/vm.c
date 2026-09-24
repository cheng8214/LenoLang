#include "../include/leno_vm_runtime.h"
#include "../include/string_table.h"
#include "../include/native.h"
#include "../module/ffi/ffi_clib.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <limits.h>

// ============================================================================
// 跳转表优化配置（Computed Goto）
// 1. GCC/Clang：自动启用跳转表（VM_USE_JUMPTABLE = 1）
// 2. MSVC：自动回退到 switch 模式（VM_USE_JUMPTABLE = 0）
// 3. 强制切换：编译时添加 -DVM_FORCE_SWITCH 或 -DVM_FORCE_JUMPTABLE
//
#if defined(__GNUC__) || defined(__clang__)
    #define VM_USE_JUMPTABLE 1
    #define unlikely(x) __builtin_expect(!!(x), 0)
    #define likely(x)   __builtin_expect(!!(x), 1)
#else
    #define VM_USE_JUMPTABLE 0
    #define unlikely(x) (x)
    #define likely(x)   (x)
#endif

// 手动覆盖（用于测试）
#if defined(VM_FORCE_JUMPTABLE)
    #undef VM_USE_JUMPTABLE
    #define VM_USE_JUMPTABLE 1
#endif
#if defined(VM_FORCE_SWITCH)
    #undef VM_USE_JUMPTABLE
    #define VM_USE_JUMPTABLE 0
#endif

// 主线程使用全局 VM 以获得最佳性能
VM vm = {0};
int vm_initialized = 0;

// ============================================================================
// 单文件打包（-p --onefile）释放出的**资源目录**（含结尾分隔符；未解包时为空串）
// ----------------------------------------------------------------------------
// ⚠ 存储**必须定义在 core**（`sources_core.txt`）里，理由：
//   · 读它的是 `src\module\dirs\dirs.c`（`dirs.res_dir()`，也在 core）；
//   · 写它的是 `src\vm_main.c`（解包后调用 `vm_set_res_dir()`），
//     而 vm_main.c **只属于 VM-only 构建**（`sources_vm.txt`，见 build_vm.bat）。
//   ⇒ 若把它定义在 vm_main.c，带编译器的 `lenoreg.exe`（core + compiler，不含 vm_main.c）
//     一旦让 dirs.c 引用它就会 **undefined reference 链接失败**。
// ============================================================================
static char g_res_dir[MAX_PATH_LEN] = {0};

const char* vm_res_dir(void) { return g_res_dir; }

// 供 VM 运行时（vm_main.c）在解包完成后写入；传 NULL / 空串表示"本次未解包"。
void vm_set_res_dir(const char* dir) {
    if (!dir || !dir[0]) { g_res_dir[0] = '\0'; return; }
    snprintf(g_res_dir, sizeof(g_res_dir), "%s", dir);
}

// 主程序 VM 全局变量（效率第一）
// 子线程使用独立的局部 VM，不共享此全局变量


// ============================================================================
// INC/DEC 操作码公共辅助函数
// ============================================================================

// 执行增减计算（delta: +1 表示 INC, -1 表示 DEC）
// 返回新的 Value，保持原操作数类型
static inline Value do_inc_dec(Value val, int delta, int* error) {
    *error = 0;

    if (!val_is_num(val)) {
        *error = 1;
        return val_null();
    }

    // 如果值是 BigInt，直接使用 BigInt 运算
    if (val_is_bigint(val)) {
        ObjBigInt* big_val = val_as_bigint(val);
        ObjBigInt* big_delta = bigint_from_int64(delta);
        Value result = bigint_add(big_val, big_delta);
        return result;
    }

    // 获取旧值
    int64_t old_int = (int64_t)val_as_num(val);

    // 计算新值
    int64_t new_int = old_int + delta;

    // 检查 int64 溢出
    if ((delta > 0 && old_int > INT64_MAX - delta) ||
        (delta < 0 && old_int < INT64_MIN - delta)) {
        // int64 溢出，使用 BigInt
        ObjBigInt* big_val = bigint_from_int64(old_int);
        ObjBigInt* big_delta = bigint_from_int64(delta);
        return bigint_add(big_val, big_delta);
    }

    // 检查是否超出 int32 范围
    if (new_int < INT32_MIN || new_int > INT32_MAX) {
        return val_bigint_from_int64(new_int);
    }

    // 保持操作数的类型
    if (val_is_float(val)) {
        return val_float((double)new_int);
    } else if (val_is_bigint(val)) {
        // 原来是 BigInt，保持为 BigInt（但值已经在 int32 范围内）
        return val_bigint_from_int64(new_int);
    } else {
        return val_int((int)new_int);
    }
}

// 注意：bigint_to_double 和 value_to_double 现在定义在 leno_value.h 中作为 static inline
// 这样所有模块都可以内联使用这些函数，避免函数调用开销

// 将 Value 转换为数组/字符串索引（int）
// 返回 1 表示成功，0 表示失败（非数字类型）
static inline int value_to_index(Value val, int* index) {
    if (val_is_int(val)) {
        *index = val_as_int(val);
        return 1;
    } else if (val_is_float(val)) {
        *index = (int)val_as_num(val);
        return 1;
    } else if (val_is_bigint(val)) {
        // 使用 value_to_double 处理超出 int64 范围的 BigInt
        *index = (int)value_to_double(val);
        return 1;
    }
    return 0;
}

// ============================================================================
// 类型特化指令 BigInt 运算通用辅助函数
// ============================================================================

// 检查两个操作数是否需要使用 BigInt 运算（当任一操作数超出 int64 范围时）
static inline int need_bigint_arith(Value va, Value vb) {
    if (val_is_bigint(va) && !bigint_fits_in_int64(val_as_bigint(va))) return 1;
    if (val_is_bigint(vb) && !bigint_fits_in_int64(val_as_bigint(vb))) return 1;
    return 0;
}

// 获取操作数的 BigInt 指针（如果是 int 则创建临时 BigInt）
static inline ObjBigInt* get_bigint_operand(Value v) {
    if (val_is_bigint(v)) {
        return val_as_bigint(v);
    }
    return bigint_from_int64(val_as_int(v));
}

// 根据 int64 结果推送 Value（自动选择 int 或 BigInt）
// 注意：此函数不处理 int64 溢出，调用前需要确保不会溢出
// 定义在 vminc/vm_helpers.inc 中，以便在 vm_run_with_vm 的 #define vm (*vm_ptr) 上下文中正确展开

// 比较两个 Value（支持 BigInt 超出 int64 范围的情况）
// 返回值: -1(a<b), 0(a==b), 1(a>b)
static inline int compare_values(Value va, Value vb) {
    // 如果都不是超大 BigInt，使用 int64 比较
    if (bigint_fits_in_int64(get_bigint_operand(va)) && 
        bigint_fits_in_int64(get_bigint_operand(vb))) {
        int64_t a = val_is_bigint(va) ? bigint_to_int64(val_as_bigint(va)) : val_as_int(va);
        int64_t b = val_is_bigint(vb) ? bigint_to_int64(val_as_bigint(vb)) : val_as_int(vb);
        return (a < b) ? -1 : (a > b) ? 1 : 0;
    }
    // 使用 BigInt 比较
    return bigint_compare(get_bigint_operand(va), get_bigint_operand(vb));
}

// 比较 Value 和 int64 立即数
static inline int compare_value_imm(Value va, int64_t imm) {
    if (val_is_bigint(va) && !bigint_fits_in_int64(val_as_bigint(va))) {
        // 超大 BigInt 与立即数比较
        ObjBigInt* bigint_imm = bigint_from_int64(imm);
        int result = bigint_compare(val_as_bigint(va), bigint_imm);
        return result;
    }
    int64_t a = val_is_bigint(va) ? bigint_to_int64(val_as_bigint(va)) : val_as_int(va);
    return (a < imm) ? -1 : (a > imm) ? 1 : 0;
}

// ============================================================================
// OP_SWITCH_LOOKUP 的查找逻辑 —— **语义唯一来源**（§8.61）
//   在按类型分组排好序的 case 值数组里查 switch_val，返回匹配下标；
//   -1 = 未匹配（走 default）。类型不匹配按原实现的语义**直接放弃查找**
//   （`break` 出整个循环，而不是继续探测）。
// 所有调用方都必须调它：各写一套二分查找迟早会分叉，
// 而分叉表现为"某些值悄悄跳到错误分支"。
// arr_val 传 Value（而不是 ObjArray*），使声明只依赖 Value —— 头文件零耦合，
// 且与原实现的 OBJ_ARRAY 检查天然合一。
// ============================================================================
int switch_lookup_index(Value switch_val, Value arr_val, int case_count) {
    if (case_count <= 0) return -1;
    if (!val_is_obj(arr_val) || val_as_obj(arr_val)->type != OBJ_ARRAY) return -1;

    ObjArray* arr = (ObjArray*)val_as_obj(arr_val);
    int matched_index = -1;

    // 根据类型选择比较策略
    if (val_is_int(switch_val)) {
        // 整数二分查找
        int64_t target = val_as_int(switch_val);
        int lo = 0, hi = case_count - 1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            Value cv = arr->elements[mid];
            int64_t cmp_val;
            if (val_is_int(cv)) {
                cmp_val = val_as_int(cv);
            } else if (val_is_bigint(cv)) {
                int c = compare_values(switch_val, cv);
                if (c == 0) { matched_index = mid; break; }
                if (c < 0) { hi = mid - 1; }
                else { lo = mid + 1; }
                continue;
            } else {
                break;  // 类型不匹配，回退
            }
            if (cmp_val == target) { matched_index = mid; break; }
            else if (cmp_val < target) { lo = mid + 1; }
            else { hi = mid - 1; }
        }
    } else if (val_is_bigint(switch_val)) {
        // BigInt 二分查找
        int lo = 0, hi = case_count - 1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            Value cv = arr->elements[mid];
            if (val_is_int(cv) || val_is_bigint(cv)) {
                int c = compare_values(switch_val, cv);
                if (c == 0) { matched_index = mid; break; }
                if (c < 0) { hi = mid - 1; }
                else { lo = mid + 1; }
            } else {
                break;
            }
        }
    } else if (val_is_float(switch_val)) {
        // 浮点数二分查找
        double target = val_as_double(switch_val);
        int lo = 0, hi = case_count - 1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            Value cv = arr->elements[mid];
            if (val_is_float(cv)) {
                double cmp_val = val_as_double(cv);
                if (cmp_val == target) { matched_index = mid; break; }
                else if (cmp_val < target) { lo = mid + 1; }
                else { hi = mid - 1; }
            } else {
                break;
            }
        }
    } else if (val_is_string(switch_val)) {
        // 字符串二分查找（按字典序）
        ObjString* target = (ObjString*)val_as_obj(switch_val);
        int lo = 0, hi = case_count - 1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            Value cv = arr->elements[mid];
            if (val_is_string(cv)) {
                ObjString* cs = (ObjString*)val_as_obj(cv);
                // 先比 hash（快速路径）
                if (cs->hash == target->hash && cs->len == target->len) {
                    if (memcmp(cs->chars, target->chars, cs->len) == 0) {
                        matched_index = mid;
                        break;
                    }
                }
                // hash 不等，按字典序比较
                int min_len = cs->len < target->len ? cs->len : target->len;
                int cmp = memcmp(cs->chars, target->chars, min_len);
                if (cmp == 0) {
                    cmp = cs->len - target->len;
                }
                if (cmp == 0) { matched_index = mid; break; }
                else if (cmp < 0) { lo = mid + 1; }
                else { hi = mid - 1; }
            } else {
                break;
            }
        }
    }

    return matched_index;
}

// ============================================================================
// OP_STRING_ADD 的语义唯一来源（§8.64）
//   字符串拼接：两侧都是 ObjString 时走 str_concat（正确处理内嵌 NUL），
//   否则把两侧都转成字符串（value_to_string）再拼。
// 所有调用方都必须调它 —— 各写一套拼接迟早分叉，
// 而分叉表现是"某些值拼出来不一样"。
// 纯 Value→Value：不碰 VM 状态、不报错（任何值都能转成字符串）。
// ============================================================================
Value string_add(Value a, Value b) {
    // 快速路径：两个操作数都是 ObjString，直接使用 str_concat（正确处理 null 字节）
    if (val_is_obj(a) && val_as_obj(a)->type == OBJ_STRING &&
        val_is_obj(b) && val_as_obj(b)->type == OBJ_STRING) {
        ObjString* result = str_concat((ObjString*)val_as_obj(a), (ObjString*)val_as_obj(b));
        return val_obj((Object*)result);
    }

    // 将两个值都转换为字符串
    char* str_a = value_to_string(a);
    char* str_b = value_to_string(b);

    // 计算新字符串长度（转换后的字符串不含 null 字节，strlen 安全）
    int len_a = (int)strlen(str_a);
    int len_b = (int)strlen(str_b);
    int total_len = len_a + len_b;

    // 创建新字符串
    ObjString* result = str_alloc(total_len);
    memcpy(result->chars, str_a, len_a);
    memcpy(result->chars + len_a, str_b, len_b);
    result->chars[total_len] = '\0';
    result->hash = hash_string(result->chars, total_len);

    // 释放临时字符串
    free(str_a);
    free(str_b);

    return val_obj((Object*)result);
}

// ============================================================================
// OP_AS_CAST 的安全类型转换 —— **语义唯一来源**（§8.83）
//   从 vm/vminc/op_as_cast.inc 整段抽出（解释器那一坨 TypeKind switch）。
//   与 type_check_value 同一做法：操作数不再用 READ_BYTE/READ_SHORT 就地消费，
//   而是由调用方把 elem_type / name_val 传进来（名字常量要查 chunk->constants，
//   由调用方查好再传 —— 与 §8.61/§8.65 同一套路）。
//   ⚠ 行为必须与抽取前**逐字一致**（含各类型不匹配时压 null 的语义），
//     特别是：**TYPE_ENUM 不在本 switch 里**（抽取前它是落到 default ⇒ matches=0），
//     所以调用方对 TYPE_ENUM 仍按 1 字节操作数读，不能想当然当成"带名字常量"。
//   匹配则返回（可能已转换的）value，不匹配返回 null —— 不报错、但可能分配
//   （字符串转换 / 整数转 FFI 指针）；分配只会置延迟回收标志、不会就地回收（§8.36），
//   所以调用中途不会被 GC 打断。
// ============================================================================
Value vm_as_cast(Value value, TypeKind expected_type, TypeKind elem_type, Value name_val) {
    int matches = 0;

    switch (expected_type) {
        case TYPE_INT: {
            if (val_is_int(value)) {
                matches = 1;
            } else if (val_is_bigint(value)) {
                // bigint 在 int32 范围内 → 转为 int，否则保持原值
                ObjBigInt* bi = val_as_bigint(value);
                if (bigint_fits_in_int64(bi)) {
                    int64_t i64 = bigint_to_int64(bi);
                    if (i64 >= INT32_MIN && i64 <= INT32_MAX) {
                        value = val_int((int)i64);
                    }
                }
                matches = 1;
            } else if (val_is_float(value)) {
                // float 转换为 int（截断小数）
                value = val_num((int64_t)val_as_double(value));
                matches = 1;
            } else if (val_is_bool(value)) {
                // bool 转 int（true→1, false→0）
                value = val_int(val_as_bool(value) ? 1 : 0);
                matches = 1;
            } else {
                matches = 0;
            }
            break;
        }
        case TYPE_FLOAT: {
            if (val_is_float(value)) {
                matches = 1;
            } else if (val_is_int(value)) {
                // int 转换为 float
                value = val_float((double)val_as_int(value));
                matches = 1;
            } else if (val_is_bigint(value)) {
                // bigint 转换为 float
                value = val_float(bigint_to_double(val_as_bigint(value)));
                matches = 1;
            } else if (val_is_bool(value)) {
                // bool 转换为 float（true→1.0, false→0.0）
                value = val_float(val_as_bool(value) ? 1.0 : 0.0);
                matches = 1;
            } else {
                matches = 0;
            }
            break;
        }
        case TYPE_STRING:
            if (val_is_obj(value) && val_as_obj(value)->type == OBJ_STRING) {
                matches = 1;
            } else if (!val_is_null(value)) {
                // 与 as int/as float 一致：非字符串类型自动转换
                const char* str = val_to_string(value);
                value = val_obj((struct Object*)str_copy(str, (int)strlen(str)));
                matches = 1;
            } else {
                matches = 0;
            }
            break;
        case TYPE_BOOL:
            matches = val_is_bool(value);
            break;

        case TYPE_ARRAY: {
            TypeKind expected_elem_type = elem_type;
            if (!val_is_obj(value) || val_as_obj(value)->type != OBJ_ARRAY) {
                matches = 0;
                break;
            }
            if (expected_elem_type != TYPE_ANY) {
                ObjArray* arr = (ObjArray*)val_as_obj(value);
                matches = 1;
                for (int i = 0; i < arr->count; i++) {
                    Value elem = arr->elements[i];
                    int elem_matches = 0;
                    switch (expected_elem_type) {
                        case TYPE_INT:
                            elem_matches = val_is_int(elem) || val_is_bigint(elem);
                            break;
                        case TYPE_FLOAT:
                            elem_matches = val_is_float(elem);
                            break;
                        case TYPE_STRING:
                            elem_matches = (val_is_obj(elem) && val_as_obj(elem)->type == OBJ_STRING);
                            break;
                        case TYPE_BOOL:
                            elem_matches = val_is_bool(elem);
                            break;
                        default:
                            elem_matches = 1;
                            break;
                    }
                    if (!elem_matches) {
                        matches = 0;
                        break;
                    }
                }
            } else {
                matches = 1;
            }
            break;
        }

        case TYPE_DICT:
            matches = (val_is_obj(value) && val_as_obj(value)->type == OBJ_DICT);
            break;
        case TYPE_STRUCT: {
            if (!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING) {
                matches = 0;
                break;
            }
            const char* struct_name = ((ObjString*)val_as_obj(name_val))->chars;
            if (val_is_obj(value) && val_as_obj(value)->type == OBJ_STRUCT) {
                ObjStruct* obj = (ObjStruct*)val_as_obj(value);
                if (obj->def && obj->def->name) {
                    matches = (strcmp(obj->def->name, struct_name) == 0);
                } else {
                    matches = 0;
                }
            } else {
                matches = 0;
            }
            break;
        }

        case TYPE_FACE: {
            if (!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING) {
                matches = 0;
                break;
            }
            const char* face_name = ((ObjString*)val_as_obj(name_val))->chars;
            if (val_is_obj(value) && val_as_obj(value)->type == OBJ_STRUCT) {
                ObjStruct* obj = (ObjStruct*)val_as_obj(value);
                if (!obj->def) {
                    matches = 0;
                    break;
                }
                ObjFaceDef* fdef = face_def_find(face_name);
                if (fdef) {
                    matches = struct_implements_face(obj->def, fdef);
                } else {
                    matches = 0;
                }
            }
            break;
        }

        case TYPE_FILE:
            matches = (val_is_obj(value) && val_as_obj(value)->type == OBJ_FILE);
            break;
        case TYPE_SOCKET:
            matches = (val_is_obj(value) && val_as_obj(value)->type == OBJ_SOCKET);
            break;
        case TYPE_CHANNEL:
            matches = (val_is_obj(value) && val_as_obj(value)->type == OBJ_CHANNEL);
            break;
        case TYPE_THREAD:
            matches = (val_is_obj(value) && val_as_obj(value)->type == OBJ_THREAD);
            break;
        case TYPE_PTR:
            if (val_is_obj(value) &&
                (val_as_obj(value)->type == OBJ_FFI_POINTER ||
                 val_as_obj(value)->type == OBJ_FFI_LIBRARY ||
                 val_as_obj(value)->type == OBJ_FFI_CALLBACK)) {
                matches = 1;
            } else if (val_is_int(value)) {
                // 整数转指针（如 -1 表示 SQLITE_TRANSIENT）
                int64_t addr = (int64_t)val_as_int(value);
                ObjFFIPointer* ffi_ptr = (ObjFFIPointer*)gc_alloc(sizeof(ObjFFIPointer), OBJ_FFI_POINTER);
                if (ffi_ptr) {
                    ffi_ptr->ptr = (void*)addr;
                    ffi_ptr->size = 0;
                    ffi_ptr->owned = 0;
                    ffi_ptr->freed = 0;
                    ffi_ptr->element_type = TYPE_PTR;
                    value = val_obj((Object*)ffi_ptr);
                    matches = 1;
                } else {
                    matches = 0;
                }
            } else if (val_is_float(value)) {
                int64_t addr = (int64_t)val_as_double(value);
                ObjFFIPointer* ffi_ptr = (ObjFFIPointer*)gc_alloc(sizeof(ObjFFIPointer), OBJ_FFI_POINTER);
                if (ffi_ptr) {
                    ffi_ptr->ptr = (void*)addr;
                    ffi_ptr->size = 0;
                    ffi_ptr->owned = 0;
                    ffi_ptr->freed = 0;
                    ffi_ptr->element_type = TYPE_PTR;
                    value = val_obj((Object*)ffi_ptr);
                    matches = 1;
                } else {
                    matches = 0;
                }
            } else {
                matches = 0;
            }
            break;
        case TYPE_PTR_GENERIC: {
            TypeKind elem_kind = elem_type;
            matches = (val_is_obj(value) &&
                      (val_as_obj(value)->type == OBJ_FFI_POINTER ||
                       val_as_obj(value)->type == OBJ_FFI_LIBRARY ||
                       val_as_obj(value)->type == OBJ_FFI_CALLBACK));
            // 如果匹配，将元素类型记录到 FFI 指针对象中
            if (matches && val_as_obj(value)->type == OBJ_FFI_POINTER) {
                ObjFFIPointer* ffi_ptr = (ObjFFIPointer*)val_as_obj(value);
                if (ffi_ptr->element_type == TYPE_ANY || ffi_ptr->element_type == TYPE_PTR) {
                    ffi_ptr->element_type = elem_kind;
                }
            }
            break;
        }
        case TYPE_NULL:
            matches = val_is_null(value);
            break;
        case TYPE_ANY:
            matches = 1;
            break;

        /* 整数截断类型：as i8/u8/i16/u16/i32/u32/i64/u64 — 显式截断高位，始终成功 */
        case TYPE_I8: {
            if (val_is_int(value)) {
                value = val_int((int)(int8_t)val_as_int(value));
                matches = 1;
            } else if (val_is_bigint(value)) {
                value = val_int((int)(int8_t)bigint_to_int64(val_as_bigint(value)));
                matches = 1;
            } else if (val_is_float(value)) {
                value = val_int((int)(int8_t)(int64_t)val_as_double(value));
                matches = 1;
            } else if (val_is_bool(value)) {
                value = val_int(val_as_bool(value) ? 1 : 0);
                matches = 1;
            } else {
                matches = 0;
            }
            break;
        }
        case TYPE_U8: {
            if (val_is_int(value)) {
                value = val_int((int)(uint8_t)val_as_int(value));
                matches = 1;
            } else if (val_is_bigint(value)) {
                value = val_int((int)(uint8_t)bigint_to_int64(val_as_bigint(value)));
                matches = 1;
            } else if (val_is_float(value)) {
                value = val_int((int)(uint8_t)(int64_t)val_as_double(value));
                matches = 1;
            } else if (val_is_bool(value)) {
                value = val_int(val_as_bool(value) ? 1 : 0);
                matches = 1;
            } else {
                matches = 0;
            }
            break;
        }
        case TYPE_I16: {
            if (val_is_int(value)) {
                value = val_int((int)(int16_t)val_as_int(value));
                matches = 1;
            } else if (val_is_bigint(value)) {
                value = val_int((int)(int16_t)bigint_to_int64(val_as_bigint(value)));
                matches = 1;
            } else if (val_is_float(value)) {
                value = val_int((int)(int16_t)(int64_t)val_as_double(value));
                matches = 1;
            } else if (val_is_bool(value)) {
                value = val_int(val_as_bool(value) ? 1 : 0);
                matches = 1;
            } else {
                matches = 0;
            }
            break;
        }
        case TYPE_U16: {
            if (val_is_int(value)) {
                value = val_int((int)(uint16_t)val_as_int(value));
                matches = 1;
            } else if (val_is_bigint(value)) {
                value = val_int((int)(uint16_t)bigint_to_int64(val_as_bigint(value)));
                matches = 1;
            } else if (val_is_float(value)) {
                value = val_int((int)(uint16_t)(int64_t)val_as_double(value));
                matches = 1;
            } else if (val_is_bool(value)) {
                value = val_int(val_as_bool(value) ? 1 : 0);
                matches = 1;
            } else {
                matches = 0;
            }
            break;
        }
        case TYPE_I32: {
            if (val_is_int(value)) {
                value = val_int((int)(int32_t)val_as_int(value));
                matches = 1;
            } else if (val_is_bigint(value)) {
                value = val_int((int)(int32_t)bigint_to_int64(val_as_bigint(value)));
                matches = 1;
            } else if (val_is_float(value)) {
                value = val_int((int)(int32_t)(int64_t)val_as_double(value));
                matches = 1;
            } else if (val_is_bool(value)) {
                value = val_int(val_as_bool(value) ? 1 : 0);
                matches = 1;
            } else {
                matches = 0;
            }
            break;
        }
        case TYPE_U32: {
            if (val_is_int(value)) {
                value = val_int((int)(uint32_t)val_as_int(value));
                matches = 1;
            } else if (val_is_bigint(value)) {
                value = val_int((int)(uint32_t)bigint_to_int64(val_as_bigint(value)));
                matches = 1;
            } else if (val_is_float(value)) {
                value = val_int((int)(uint32_t)(int64_t)val_as_double(value));
                matches = 1;
            } else if (val_is_bool(value)) {
                value = val_int(val_as_bool(value) ? 1 : 0);
                matches = 1;
            } else {
                matches = 0;
            }
            break;
        }
        case TYPE_I64: {
            if (val_is_int(value)) {
                matches = 1;
            } else if (val_is_bigint(value)) {
                value = val_int((int)bigint_to_int64(val_as_bigint(value)));
                matches = 1;
            } else if (val_is_float(value)) {
                value = val_int((int)(int64_t)val_as_double(value));
                matches = 1;
            } else if (val_is_bool(value)) {
                value = val_int(val_as_bool(value) ? 1 : 0);
                matches = 1;
            } else {
                matches = 0;
            }
            break;
        }
        case TYPE_U64: {
            if (val_is_int(value)) {
                matches = 1;
            } else if (val_is_bigint(value)) {
                value = val_int((int)(int64_t)(uint64_t)bigint_to_int64(val_as_bigint(value)));
                matches = 1;
            } else if (val_is_float(value)) {
                value = val_int((int)(uint64_t)val_as_double(value));
                matches = 1;
            } else if (val_is_bool(value)) {
                value = val_int(val_as_bool(value) ? 1 : 0);
                matches = 1;
            } else {
                matches = 0;
            }
            break;
        }
        /* 浮点窄化类型 */
        case TYPE_F32: {
            if (val_is_float(value)) {
                /* float→f32: 始终可转换（可能损失精度） */
                value = val_float((double)(float)val_as_double(value));
                matches = 1;
            } else if (val_is_int(value)) {
                value = val_float((double)(float)val_as_int(value));
                matches = 1;
            } else if (val_is_bigint(value)) {
                value = val_float((double)(float)bigint_to_double(val_as_bigint(value)));
                matches = 1;
            } else if (val_is_bool(value)) {
                value = val_float(val_as_bool(value) ? 1.0 : 0.0);
                matches = 1;
            } else {
                matches = 0;
            }
            break;
        }
        case TYPE_F64: {
            if (val_is_float(value)) {
                matches = 1;
            } else if (val_is_int(value)) {
                value = val_float((double)val_as_int(value));
                matches = 1;
            } else if (val_is_bigint(value)) {
                value = val_float(bigint_to_double(val_as_bigint(value)));
                matches = 1;
            } else if (val_is_bool(value)) {
                value = val_float(val_as_bool(value) ? 1.0 : 0.0);
                matches = 1;
            } else {
                matches = 0;
            }
            break;
        }
        /* cstruct 类型 */
        case TYPE_CSTRUCT: {
            if (!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING) {
                matches = 0;
                break;
            }
            const char* cs_name = ((ObjString*)val_as_obj(name_val))->chars;
            if (val_is_obj(value) && val_as_obj(value)->type == OBJ_CSTRUCT) {
                ObjCStruct* obj = (ObjCStruct*)val_as_obj(value);
                if (obj->def && obj->def->name) {
                    matches = (strcmp(obj->def->name, cs_name) == 0);
                } else {
                    matches = 0;
                }
            } else {
                matches = 0;
            }
            break;
        }
        default:
            matches = 0;
            break;
    }

    return matches ? value : val_null();
}

// ============================================================================
// OP_TYPE_CHECK 的类型判定 —— **语义唯一来源**（§8.65）
//   从 vm/vminc/op_type_check.inc 整段抽出（解释器那一坨 TypeKind switch）。
//   区别只有两点：操作数不再用 READ_BYTE/READ_SHORT 就地消费，而是由调用方
//   把 elem_type / name_val 传进来 —— 因为**名字常量要查 chunk->constants**，
//   而调用方查好再传（与 §8.61 的 switch case 数组同一做法）。
//   纯判定：不分配、不报错。
// ============================================================================
int type_check_value(Value value, TypeKind expected_type, TypeKind elem_type, Value name_val) {
    int matches = 0;

    switch (expected_type) {
        // --- 简单类型：仅检查值类型 ---
        case TYPE_INT:
            matches = val_is_int(value) || val_is_bigint(value);
            break;
        case TYPE_FLOAT:
            matches = val_is_float(value);
            break;
        case TYPE_STRING:
            matches = (val_is_obj(value) && val_as_obj(value)->type == OBJ_STRING);
            break;
        case TYPE_BOOL:
            matches = val_is_bool(value);
            break;

        // --- 数组类型: elem_type 用于检查每个元素的类型 ---
        case TYPE_ARRAY: {
            if (!val_is_obj(value) || val_as_obj(value)->type != OBJ_ARRAY) {
                matches = 0;
                break;
            }
            // TYPE_ANY 表示不检查元素类型，只确认是数组即可
            if (elem_type != TYPE_ANY) {
                ObjArray* arr = (ObjArray*)val_as_obj(value);
                matches = 1;
                for (int i = 0; i < arr->count; i++) {
                    Value elem = arr->elements[i];
                    int elem_matches = 0;
                    switch (elem_type) {
                        case TYPE_INT:
                            elem_matches = val_is_int(elem) || val_is_bigint(elem);
                            break;
                        case TYPE_FLOAT:
                            elem_matches = val_is_float(elem);
                            break;
                        case TYPE_STRING:
                            elem_matches = (val_is_obj(elem) && val_as_obj(elem)->type == OBJ_STRING);
                            break;
                        case TYPE_BOOL:
                            elem_matches = val_is_bool(elem);
                            break;
                        default:
                            elem_matches = 1;
                            break;
                    }
                    if (!elem_matches) {
                        matches = 0;
                        break;
                    }
                }
            } else {
                matches = 1;
            }
            break;
        }

        // --- 字典/文件/指针/空值/任意: 仅检查值类型 ---
        case TYPE_DICT:
            matches = (val_is_obj(value) && val_as_obj(value)->type == OBJ_DICT);
            break;

        case TYPE_STRUCT: {
            if (!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING) {
                matches = 0;
                break;
            }
            const char* struct_name = ((ObjString*)val_as_obj(name_val))->chars;
            if (val_is_obj(value) && val_as_obj(value)->type == OBJ_STRUCT) {
                ObjStruct* obj = (ObjStruct*)val_as_obj(value);
                if (obj->def && obj->def->name) {
                    matches = (strcmp(obj->def->name, struct_name) == 0);
                } else {
                    matches = 0;
                }
            } else {
                matches = 0;
            }
            break;
        }

        case TYPE_FACE: {
            if (!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING) {
                matches = 0;
                break;
            }
            const char* face_name = ((ObjString*)val_as_obj(name_val))->chars;
            if (val_is_obj(value) && val_as_obj(value)->type == OBJ_STRUCT) {
                ObjStruct* obj = (ObjStruct*)val_as_obj(value);
                if (!obj->def) {
                    matches = 0;
                    break;
                }
                ObjFaceDef* fdef = face_def_find(face_name);
                if (fdef) {
                    matches = struct_implements_face(obj->def, fdef);
                } else {
                    matches = 0;
                }
            }
            break;
        }

        case TYPE_ENUM: {
            if (!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING) {
                matches = 0;
                break;
            }
            const char* enum_name = ((ObjString*)val_as_obj(name_val))->chars;
            if (val_is_obj(value) && val_as_obj(value)->type == OBJ_ENUM_DEF) {
                ObjEnumDef* edef = (ObjEnumDef*)val_as_obj(value);
                if (edef->name) {
                    matches = (strcmp(edef->name, enum_name) == 0);
                } else {
                    matches = 0;
                }
            } else {
                matches = 0;
            }
            break;
        }

        case TYPE_FILE:
            matches = (val_is_obj(value) && val_as_obj(value)->type == OBJ_FILE);
            break;
        case TYPE_SOCKET:
            matches = (val_is_obj(value) && val_as_obj(value)->type == OBJ_SOCKET);
            break;
        case TYPE_CHANNEL:
            matches = (val_is_obj(value) && val_as_obj(value)->type == OBJ_CHANNEL);
            break;
        case TYPE_THREAD:
            matches = (val_is_obj(value) && val_as_obj(value)->type == OBJ_THREAD);
            break;
        case TYPE_PTR:
            matches = (val_is_obj(value) &&
                      (val_as_obj(value)->type == OBJ_FFI_POINTER ||
                       val_as_obj(value)->type == OBJ_FFI_LIBRARY ||
                       val_as_obj(value)->type == OBJ_FFI_CALLBACK));
            break;
        case TYPE_NULL:
            matches = val_is_null(value);
            break;
        case TYPE_FUNCTION:
            matches = (val_is_obj(value) &&
                      (val_as_obj(value)->type == OBJ_CLOSURE ||
                       val_as_obj(value)->type == OBJ_NATIVE ||
                       val_as_obj(value)->type == OBJ_FFI_CALLBACK));
            break;
        case TYPE_ANY:
            matches = 1;
            break;
        default:
            matches = 0;
            break;
    }

    return matches;
}

// ============================================================================
// struct 方法表查找 —— **规则唯一来源**（§8.66）
//   从 vm/vminc/op_struct.inc 的 OP_GET_METHOD（struct 分支）抽出**查找规则**：
//   按名字线性比对方法表，**跳过 ctor/dtor**（不允许显式调用）。
//   只抽"规则"，不抽周边：inline cache 更新、GC 安全的 push/pop、报错文本都留在原处
//   —— 那些是调用方的实现细节（各自的缓存与报错通道）。
//   找到 → 返回 1，并按需回填 out_closure（预创建闭包，可能为 NULL）/ out_func。
// ============================================================================
int struct_method_lookup(ObjStructDef* def, ObjString* method_name,
                         ObjClosure** out_closure, ObjFunction** out_func) {
    if (!def || !method_name) return 0;
    ObjClosure* closure = NULL;
    ObjFunction* func = NULL;
    for (int i = 0; i < def->method_count; i++) {
        if (strcmp(def->methods[i].name, method_name->chars) == 0) {
            // 跳过构造/析构函数（不允许显式调用）
            if (def->has_ctor && i == def->ctor_index) continue;
            if (def->has_dtor && i == def->dtor_index) continue;
            closure = def->methods[i].closure;
            func = def->methods[i].func;
            break;
        }
    }
    if (!func) return 0;
    if (out_closure) *out_closure = closure;
    if (out_func) *out_func = func;
    return 1;
}

// ============================================================================
// 辅助函数实现 - 从各 .c 文件合并（需要在 OPCODE 之前定义）
// ============================================================================
// 注意：按依赖顺序包含，被依赖的在前
// 使用 #define vm (*current_exec_vm) 确保子线程使用正确的 VM

THREAD_LOCAL VM* current_exec_vm = NULL;

// 异常处理（最基础，被其他函数依赖）
#define vm (*current_exec_vm)
#include "vminc/vm_exception.inc"
#undef vm

// 算术辅助宏（int48 溢出提升 bigint、安全加减乘等）
#include "vminc/vm_helpers.inc"

// 基础工具函数（依赖 exception）
#define vm (*current_exec_vm)
#include "vminc/vm_utils.inc"
#undef vm

// 字节码块操作（独立，不使用 vm）
#include "vminc/vm_chunk.inc"

// 内联缓存（独立）
#define vm (*current_exec_vm)
#include "vminc/vm_ic.inc"
#undef vm

// Upvalue 管理（独立）
#define vm (*current_exec_vm)
#include "vminc/vm_upvalue.inc"
#undef vm

// 函数调用（依赖 exception）
#define vm (*current_exec_vm)
#include "vminc/vm_call.inc"
#undef vm

// ============================================================================
// 比较语义（寄存器式 VM 的唯一来源，与栈式 op_compare.inc 保持一致）
// ============================================================================

// 相等比较（OP_EQ / OP_NEQ）：
//   int/float/bigint 数值相等；string 比内容；array 逐元素；null/bool 按值；
//   其余对象按引用；类型不同 → 不相等。
static inline int value_eq_stdlib(Value a, Value b) {
    if (val_is_int(a) && val_is_int(b)) return val_as_int(a) == val_as_int(b);
    if (val_is_float(a) || val_is_float(b)) {
        if (!(val_is_num(a) || val_is_bigint(a)) || !(val_is_num(b) || val_is_bigint(b))) return 0;
        return val_as_num_ex(a) == val_as_num_ex(b);
    }
    if (val_is_bigint(a) || val_is_bigint(b)) {
        return bigint_compare(promote_to_bigint(a), promote_to_bigint(b)) == 0;
    }
    if (val_is_null(a) && val_is_null(b)) return 1;
    if (val_is_bool(a) && val_is_bool(b)) return val_as_bool(a) == val_as_bool(b);
    if (val_is_obj(a) && val_is_obj(b)) {
        Object* oa = val_as_obj(a);
        Object* ob = val_as_obj(b);
        if (oa->type != ob->type) return 0;
        if (oa->type == OBJ_STRING) {
            ObjString* sa = (ObjString*)oa;
            ObjString* sb = (ObjString*)ob;
            return sa->len == sb->len && memcmp(sa->chars, sb->chars, (size_t)sa->len) == 0;
        }
        if (oa->type == OBJ_ARRAY) {
            ObjArray* aa = (ObjArray*)oa;
            ObjArray* ab = (ObjArray*)ob;
            if (aa->count != ab->count) return 0;
            for (int i = 0; i < aa->count; i++) {
                if (!value_eq_stdlib(aa->elements[i], ab->elements[i])) return 0;
            }
            return 1;
        }
        return oa == ob;
    }
    return 0;
}

// 三向比较（OP_LT / OP_GT / OP_LE / OP_GE）
// 返回 1 = 可比（*out = -1/0/1）；0 = 不可比较（调用方报错）
static inline int value_compare_stdlib(Value a, Value b, int* out) {
    if (val_is_int(a) && val_is_int(b)) {
        int64_t x = val_as_int(a), y = val_as_int(b);
        *out = (x < y) ? -1 : (x > y ? 1 : 0);
        return 1;
    }
    if ((val_is_num(a) || val_is_bigint(a)) && (val_is_num(b) || val_is_bigint(b))) {
        if (val_is_float(a) || val_is_float(b)) {
            double x = val_as_num_ex(a), y = val_as_num_ex(b);
            *out = (x < y) ? -1 : (x > y ? 1 : 0);
            return 1;
        }
        *out = bigint_compare(promote_to_bigint(a), promote_to_bigint(b));
        return 1;
    }
    if (val_is_obj(a) && val_is_obj(b) &&
        val_as_obj(a)->type == OBJ_STRING && val_as_obj(b)->type == OBJ_STRING) {
        ObjString* sa = (ObjString*)val_as_obj(a);
        ObjString* sb = (ObjString*)val_as_obj(b);
        int min_len = sa->len < sb->len ? sa->len : sb->len;
        int c = memcmp(sa->chars, sb->chars, (size_t)min_len);
        if (c == 0) c = sa->len - sb->len;
        *out = (c < 0) ? -1 : (c > 0 ? 1 : 0);
        return 1;
    }
    return 0;
}

// ============================================================================
// cstruct 字段读取（唯一实现，供 OP_GET_FIELD / OP_INDEX / OP_GET_METHOD 复用）
//   数值字段 → 直接取值；
//   数组字段 → ObjCStructArrayView（后续按整数索引读写元素）；
//   嵌套 cstruct → 指向父内存的**非拥有**视图（`r.top_left.x = 1` 要能写回去）；
//   str16 数组 → 交给 cstruct_get_field_value 做 UTF-16 → UTF-8 转换。
// 之前三个地方各自只调 cstruct_get_field_value ⇒ 数组/嵌套字段一律得 null ✗
// ============================================================================
static inline Value vm_cstruct_field_read(ObjCStruct* co, int field_idx) {
    if (!co || !co->def || field_idx < 0 || field_idx >= co->def->field_count) return val_null();
    CStructFieldInfo* f = &co->def->fields[field_idx];

    if (f->type == TYPE_STR16 && f->array_dim > 0) {
        return cstruct_get_field_value(co, field_idx);
    }
    if (f->array_dim > 0) {
        ObjCStructArrayView* view = (ObjCStructArrayView*)gc_alloc(
            sizeof(ObjCStructArrayView), OBJ_CSTRUCT_ARRAY_VIEW);
        if (!view) return val_null();
        view->cstruct = co;
        view->field_index = field_idx;
        view->element_type = f->type;
        view->element_size = f->size;
        view->array_dim = f->array_dim;
        return val_obj((Object*)view);
    }
    if (f->type == TYPE_CSTRUCT) {
        const char* nested_name = f->struct_name ? f->struct_name : f->name;
        ObjCStructDef* nested_def = nested_name ? cstruct_def_find(nested_name) : NULL;
        if (!nested_def) return val_null();
        ObjCStruct* child = (ObjCStruct*)gc_alloc(sizeof(ObjCStruct), OBJ_CSTRUCT);
        if (!child) return val_null();
        child->def = nested_def;
        child->data = co->data + f->offset;
        child->owns_memory = 0;
        return val_obj((Object*)child);
    }
    return cstruct_get_field_value(co, field_idx);
}

// ============================================================================
// 按 C 布局类型从**裸地址**读一个标量值（cstruct 数组字段视图的元素读取用）
// ============================================================================
static inline Value vm_c_layout_read_scalar(const uint8_t* addr, TypeKind t) {
    if (!addr) return val_null();
    switch (t) {
        case TYPE_I8:   return val_num((double)(*(const int8_t*)addr));
        case TYPE_U8:   return val_num((double)(*(const uint8_t*)addr));
        case TYPE_I16:  return val_num((double)(*(const int16_t*)addr));
        case TYPE_U16:  return val_num((double)(*(const uint16_t*)addr));
        case TYPE_I32:  return val_num((double)(*(const int32_t*)addr));
        case TYPE_U32:  return val_num((double)(*(const uint32_t*)addr));
        case TYPE_I64:  return val_int_safe(*(const int64_t*)addr);
        case TYPE_U64:  return val_int_safe((int64_t)(*(const uint64_t*)addr));
        case TYPE_F32:  return val_num((double)(*(const float*)addr));
        case TYPE_F64:  return val_num(*(const double*)addr);
        case TYPE_BOOL: return val_bool(*(const uint8_t*)addr);
        case TYPE_PTR:
        case TYPE_PTR_GENERIC:
        case TYPE_STR8: {
            void* p = *(void* const*)addr;
            return p ? val_int_safe((int64_t)(intptr_t)p) : val_null();
        }
        default: return val_null();
    }
}

// ============================================================================
// 按 C 布局类型往**裸地址**写一个标量值
// ============================================================================
static inline void vm_c_layout_write_scalar(uint8_t* addr, TypeKind t, Value v) {
    if (!addr) return;
    double d = val_is_int(v) ? (double)val_as_int(v)
             : (val_is_float(v) ? val_as_double(v)
             : (val_is_bigint(v) ? bigint_to_double(val_as_bigint(v)) : 0.0));
    switch (t) {
        case TYPE_I8: case TYPE_U8:     *(uint8_t*)addr  = (uint8_t)(int64_t)(val_is_int(v) ? val_as_int(v) : d); break;
        case TYPE_I16: case TYPE_U16:   *(uint16_t*)addr = (uint16_t)(int64_t)(val_is_int(v) ? val_as_int(v) : d); break;
        case TYPE_I32: case TYPE_U32:   *(uint32_t*)addr = (uint32_t)(int64_t)(val_is_int(v) ? val_as_int(v) : d); break;
        case TYPE_I64: case TYPE_U64:   *(uint64_t*)addr = (uint64_t)(val_is_int(v) ? val_as_int(v) : (int64_t)d); break;
        case TYPE_F32:                  *(float*)addr    = (float)d; break;
        case TYPE_F64:                  *(double*)addr   = d; break;
        case TYPE_BOOL:                 *(uint8_t*)addr  = val_is_truthy(v) ? 1 : 0; break;
        case TYPE_PTR: case TYPE_PTR_GENERIC: case TYPE_STR8: {
            void* p = NULL;
            if (val_is_int(v)) p = (void*)(intptr_t)val_as_int(v);
            *(void**)addr = p;
            break;
        }
        default: break;
    }
}

// ============================================================================
// bigint → int64：**64 位回绕**语义（不是饱和）
// ----------------------------------------------------------------------------
// `bigint_to_int64` 对超范围值做**饱和**（2^63 → INT64_MAX、0xFFFFFFFFFFFFFFFF → INT64_MAX），
// 而 Leno 里"静态 int"就是把 64 位字当整数用（`hex64(int v)` 的 `v >> 60`、掩码
// 常量 `0xffffffffffffffff`）—— 这些场合需要 2^63 → INT64_MIN、全 1 → -1。
// 位运算 / int 特化算术统一走这个取值口。
// ============================================================================
static inline int64_t vm_bigint_as_i64(Value v) {
    ObjBigInt* b = val_as_bigint(v);
    if (!b) return val_as_int(v);
    if (b->limb_count > 2) return bigint_to_int64(b);   // 超出 64 位：交给原实现（饱和）
    uint64_t x = 0;
    for (int i = b->limb_count - 1; i >= 0; i--) {
        x = (x << 32) | (uint64_t)b->limbs[i];          // BASE_BITS = 32
    }
    if (b->is_negative) x = (uint64_t)0 - x;
    return (int64_t)x;
}

// ============================================================================
// 泛型类型名推断（与栈式 vm_call.inc 的两支口径一致）
//   用于 instanceof 泛型替换：`new Holder[K](...)` 里 K 的实际类型
//   要靠**运行时值**反推（int / float / string / bool / struct X）
// ============================================================================
static inline const char* vm_value_to_generic_type_name(Value value) {
#ifdef _MSC_VER
    static __declspec(thread) char buf[64];
#else
    static __thread char buf[64];
#endif
    if (val_is_int(value) || val_is_bigint(value)) return "int";
    if (val_is_float(value)) return "float";
    if (val_is_bool(value)) return "bool";
    if (val_is_obj(value)) {
        ObjType ot = val_as_obj(value)->type;
        if (ot == OBJ_STRING) return "string";
        if (ot == OBJ_STRUCT) {
            ObjStruct* arg_s = (ObjStruct*)val_as_obj(value);
            if (arg_s->def && arg_s->def->name) {
                snprintf(buf, sizeof(buf), "struct %s", arg_s->def->name);
                return buf;
            }
            return "struct";
        }
        if (ot == OBJ_ARRAY) return "array";
    }
    return NULL;
}

static inline const char* vm_typeinfo_to_generic_type_name(TypeInfo* type) {
#ifdef _MSC_VER
    static __declspec(thread) char buf[64];
#else
    static __thread char buf[64];
#endif
    if (!type) return NULL;
    switch (type->kind) {
        case TYPE_INT:    return "int";
        case TYPE_FLOAT:  return "float";
        case TYPE_STRING: return "string";
        case TYPE_BOOL:   return "bool";
        case TYPE_STRUCT:
            if (type->struct_name) {
                snprintf(buf, sizeof(buf), "struct %s", type->struct_name);
                return buf;
            }
            return "struct";
        default: return NULL;
    }
}

int vm_run(void) {
    // 主线程直接使用全局 vm，零开销
    current_exec_vm = &vm;

    // vm_run.inc 中直接使用 vm 变量（全局）
    #include "vminc/vm_run.inc"
}

// 使用指定的 VM 执行字节码（供子线程使用）
// 注意：vm_run.inc 中的代码使用 vm.xxx 访问 VM 成员，
// 并且所有栈操作函数都使用 &vm 作为第一个参数
// 所以我们需要将全局 vm 变量替换为 vm_ptr 指向的 VM
int vm_run_with_vm(VM* vm_ptr) {
    if (!vm_ptr) return -1;

    current_exec_vm = vm_ptr;

    // 使用宏将 vm_run.inc 中的 vm 替换为 (*vm_ptr)
    // 这样 vm.xxx 会变成 (*vm_ptr).xxx，即 vm_ptr->xxx
    // 而 &vm 会变成 &(*vm_ptr)，即 vm_ptr
    #define vm (*vm_ptr)
    
    // 执行字节码
    int result;
    #include "vminc/vm_run.inc"
    #undef vm
    
    return result;
}

/* callee 是否属于「**native 类**」—— 即 `call_value` 里**不压帧**、直接把结果 push 回
 * VM 栈（**从不写 `vm.last_return_value`**）的那两支：
 *   · `OBJ_NATIVE`：模块函数 / 原生函数值（vm_call.inc 的 native 分支）；
 *   · `OBJ_BOUND_METHOD` 且 `closure == NULL`：native 绑定方法（`T.malloc()` / `c.free()`）。 */
static int call_callee_is_native_like(Value callee) {
    if (!val_is_obj(callee)) return 0;
    Object* o = val_as_obj(callee);
    if (o->type == OBJ_NATIVE) return 1;
    if (o->type == OBJ_BOUND_METHOD) return ((ObjBoundMethod*)o)->closure == NULL;
    return 0;
}

int vm_call_value(Value callee, int arg_count, int line) {
    VM* vm_ptr = current_exec_vm ? current_exec_vm : &vm;
    int saved_frame_cnt = vm_ptr->frame_cnt;
    // 嵌套调用（回调里再 vm_call_value，如 FFI 回调 → 脚本 → 回调）时，
    // 内层返回会把 stop_frame_cnt 清零，导致外层的停止条件失效、一直跑到
    // 字节码结束。改为保存/恢复，而不是无条件清零。
    int saved_stop_frame_cnt = vm_ptr->stop_frame_cnt;
    int saved_sp = vm_ptr->sp;   /* 调用前：实参(arg_count) + callee 已在栈上 */

    current_exec_vm = vm_ptr;
    #define vm (*current_exec_vm)
    if (!call_value(callee, arg_count, line)) {
        #undef vm
        return 0;
    }
    /* ---- native 类 callee：`call_value` 已**同步完成**整个调用、**不压帧** ----
     * （vm_call.inc:268-270 与 :328-330 都是 `vm.sp -= arg_count+1;
     *   vm_stack_push(&vm, result); return 1;` —— **从不写 `vm.last_return_value`**。）
     * ⇒ 此时若照常进 `vm_run_with_vm`，那个解释器循环会**继续执行调用方帧的字节码**
     * （栈上还多着一个结果槽）⇒ **静默错**，实测三种形态：
     *   · 结果被冲成陈值（`T.malloc()` 返回 NULL_VAL 或 int48 50 ⇒ 随后"在 null 上设置字段"）；
     *   · 调用方局部量/控制流多跑一遍（热循环 acc 只加到 50，应 101）；
     *   · 执行到栈布局不符的 OP_CALL（"只能调用函数（不是对象类型）"）。
     * 修法：① 从**基准位**取出结果并发布成单返回（native 恒 1 个返回值）——
     * 这样「成功 ⇒ `last_return_value` 有效」这条契约对**所有** callee 类别成立；
     * ② **跳过** `vm_run_with_vm`（native 调用不产生帧，没有属于它的东西要跑；
     * native 内部若回调脚本，那些嵌套调用由各自的 vm_call_value 收尾）。
     * 这一处修掉即修**所有**调用方：arrays.c 的 arr.map/filter 回调、ffi.c 的 FFI
     * 回调派发，以及任何复用 `vm_call_value` 的宿主。 */
    if (call_callee_is_native_like(callee)) {
        int base = saved_sp - arg_count - 1;   /* call_value 把 arg_count+1 个槽换成 1 个结果 */
        Value res = (base >= 0 && vm_ptr->sp > base) ? vm_ptr->stack[base] : val_null();
        vm_ptr->last_return_value     = res;
        vm_ptr->last_return_values[0] = res;
        vm_ptr->last_return_count     = 1;
        vm_ptr->stop_frame_cnt        = saved_stop_frame_cnt;
        #undef vm
        return 1;
    }
    vm.stop_frame_cnt = saved_frame_cnt;
    #undef vm

    int r = vm_run_with_vm(vm_ptr);

    vm_ptr->stop_frame_cnt = saved_stop_frame_cnt;
    return (r == 0) ? 1 : 0;
}

int vm_run_coroutine_with_vm(ObjCoroutine* co, VM* vm_ptr) {
    // 直接使用传入的 VM 指针，不再使用宏
    if (!vm_ptr) {
        fprintf(stderr, "[ERROR] vm_ptr is NULL!\n");
        fflush(stderr);
        return -1;
    }

    // 设置当前执行 VM，确保全局变量访问正确
    extern THREAD_LOCAL VM* current_exec_vm;
    VM* saved_vm = current_exec_vm;
    current_exec_vm = vm_ptr;

    // 保存当前的 current_coroutine，防止嵌套调用时覆盖
    ObjCoroutine* saved_current = vm_ptr->current_coroutine;
    
    // 检查协程状态
    if (co->state == COROUTINE_COMPLETED || co->state == COROUTINE_FAILED) {
        vm_ptr->current_coroutine = saved_current;
        return 0;
    }
    
    // 如果协程是NEW状态，需要初始化执行环境
    if (co->state == COROUTINE_NEW) {
        co->state = COROUTINE_RUNNING;

        // 确保 current_coroutine 被设置
        vm_ptr->current_coroutine = co;

        // 创建初始调用帧
        if (vm_ptr->frame_cnt >= vm_ptr->frame_capacity) {
            if (!vm_grow_frames(vm_ptr)) {
                vm_ptr->current_coroutine = saved_current;
                return -1;
            }
        }
        
        CallFrame* frame = &vm_ptr->frames[vm_ptr->frame_cnt++];
        ObjFunction* func = co->closure->function;
        int local_count = func->local_count;
        
        // 分配locals数组
        if (local_count <= INLINE_LOCALS_MAX) {
            frame->locals = frame->inline_locals;
            frame->locals_is_dynamic = 0;
            frame->locals_capacity = INLINE_LOCALS_MAX;
        } else {
            frame->locals = (Value*)malloc(local_count * sizeof(Value));
            frame->locals_is_dynamic = 1;
            frame->locals_capacity = local_count;
        }
        
        // 初始化所有 locals 为 null
        for (int i = 0; i < local_count; i++) {
            frame->locals[i] = val_null();
        }
        
        // 设置初始参数到 locals（参数对应前几个 locals）
        if (co->initial_args && co->initial_arg_count > 0) {
            int param_count = func->arity;  // 函数定义的参数数量
            int args_to_copy = (co->initial_arg_count < param_count) ? co->initial_arg_count : param_count;
            for (int i = 0; i < args_to_copy; i++) {
                frame->locals[i] = co->initial_args[i];
            }
        }
        
        frame->closure = co->closure;
        frame->chunk = func->chunk;
        frame->ip = func->chunk->code;
        frame->stack_base = vm_ptr->sp;
        frame->slot_count = local_count;
        frame->local_count = local_count;
        frame->catch_ip = NULL;
        frame->finally_ip = NULL;
        frame->catch_finally_ip = NULL;
        frame->prev_catch_ip = NULL;
        frame->prev_finally_ip = NULL;
        frame->in_finally = 0;
        frame->try_return_value = val_null();
        frame->has_try_return = 0;
        frame->has_captures = 0;   // 由 OP_CLOSURE 在真建了指向本帧窗口的 upvalue 时置 1
        
        // 保存创建此协程 frame 之前的 frame_cnt（其他协程/主程序的）= 协程首帧索引
        int saved_frame_cnt = vm_ptr->frame_cnt - 1;  // 减去新创建的 frame
        // ★ saved_frame_cnt 的语义统一为"**协程首帧的下标**"（不是"帧数上界"）：
        //   · OP_RETURN / REG_FINISH_RETURN 用它判"协程顶层函数返回"（frame_cnt <= 它）；
        //   · OP_AWAIT 用它做 co_start（保存[首帧, frame_cnt)整段）；
        //   · 异常展开用它做**协程边界**（不许跨到调用方去，见 vm_exception.inc）。
        //   此前这里存的是"含本协程帧的 frame_cnt"、恢复路径存的是"装回后总数"，
        //   两处语义不一致 ⇒ 嵌套挂起时会漏存帧、协程异常还会被调用方接走（F1）。
        co->saved_frame_cnt = saved_frame_cnt;
        
        // 执行协程直到完成或挂起
        // 使用传入的 VM 执行（子线程使用自己的 VM）
        int result = vm_run_with_vm(vm_ptr);
        
        // 恢复之前的 frame_cnt（移除本协程添加的 frame）
        if (co->state == COROUTINE_SUSPENDED) {
            // 挂起时 saved_frames 拥有动态 locals，不释放 vm.frames 中的
            vm_ptr->frame_cnt = saved_frame_cnt;
        } else {
            // 完成或失败时，清理残留帧的动态 locals
            for (int i = saved_frame_cnt; i < vm_ptr->frame_cnt; i++) {
                CallFrame* f = &vm_ptr->frames[i];
                if (f->locals_is_dynamic && f->locals) {
                    free(f->locals);
                    f->locals = NULL;
                }
            }
            vm_ptr->frame_cnt = saved_frame_cnt;
        }
        
        // 检查协程是否完成
        // 如果 result == 0 且状态不是 SUSPENDED（即没有被再次挂起），则协程已完成
        if (result == 0 && co->state != COROUTINE_SUSPENDED) {
            co->state = COROUTINE_COMPLETED;
            // 完成 task_future（这是 task 返回给调用者的 Future）
            if (co->task_future && !co->task_future->completed) {
                future_complete(co->task_future, co->result);
            }
            // 释放初始参数内存
            if (co->initial_args) {
                free(co->initial_args);
                co->initial_args = NULL;
                co->initial_arg_count = 0;
            }
        } else if (vm_ptr->has_exception) {
            co->state = COROUTINE_FAILED;
            co->result = vm_ptr->exception;
            // GC 写屏障：协程可能已晋升老年代，exception 可能是年轻代对象
            gc_write_barrier((Object*)co, vm_ptr->exception);
            if (co->task_future && !co->task_future->completed) {
                // 如果有等待者，标记错误已传播
                if (co->task_future->waiter) {
                    co->error_propagated = 1;
                }
                future_fail(co->task_future, vm_ptr->exception);
            }
            // 释放初始参数内存
            if (co->initial_args) {
                free(co->initial_args);
                co->initial_args = NULL;
                co->initial_arg_count = 0;
            }
        }

        vm_ptr->current_coroutine = saved_current;
        return result;
    }
    
    // 如果协程是SUSPENDED或RUNNING状态，恢复执行
    // 注意：被Future完成的协程会被设为RUNNING状态，然后被加入就绪队列
    if (co->state == COROUTINE_SUSPENDED || co->state == COROUTINE_RUNNING) {
        co->state = COROUTINE_RUNNING;
        
        // 确保 current_coroutine 被设置
        vm_ptr->current_coroutine = co;
        
        // 保存当前的 frame_cnt（可能属于其他协程或主程序）
        int saved_frame_cnt = vm_ptr->frame_cnt;
        
        // 恢复所有保存的 frame 到 vm.frames
        if (co->saved_frames && co->saved_frame_count > 0) {
            // 确保有足够的空间
            while (vm_ptr->frame_cnt + co->saved_frame_count >= vm_ptr->frame_capacity) {
                if (!vm_grow_frames(vm_ptr)) {
                    vm_ptr->current_coroutine = saved_current;
                    return -1;
                }
            }
            // 将所有保存的 frame 拷贝到 vm.frames
            for (int i = 0; i < co->saved_frame_count; i++) {
                CallFrame* dst = &vm_ptr->frames[vm_ptr->frame_cnt + i];
                CallFrame* src = &co->saved_frames[i];
                memcpy(dst, src, sizeof(CallFrame));
                // inline_locals 帧的 locals 指针需重定向到 vm.frames 中的 inline_locals
                if (!dst->locals_is_dynamic && dst->local_count > 0) {
                    dst->locals = dst->inline_locals;
                }
                // 动态分配的 locals 指针直接拷贝即可，所有权转回 vm.frames
            }
            vm_ptr->frame_cnt += co->saved_frame_count;
            // 释放 saved_frames 数组本身（不释放动态 locals，所有权已转移到 vm.frames）
            free(co->saved_frames);
            co->saved_frames = NULL;
            co->saved_frame_count = 0;
        }
        
        // 更新协程的首帧索引（= 装回前的 frame_cnt，见上文语义说明），
        // 使 OP_RETURN 能判断协程顶层函数返回、异常展开知道边界在哪
        co->saved_frame_cnt = saved_frame_cnt;
        
        // 恢复栈指针到挂起时的位置，确保压入 Future 结果的位置正确
        vm_ptr->sp = co->saved_sp;
        
        // 如果协程正在等待一个已完成的 Future，推送结果到栈或抛出异常
        if (co->waiting_for && co->waiting_for->completed) {
            // 检查 Future 是否有错误
            if (!val_is_null(co->waiting_for->error)) {
                // Future 失败了，抛出异常
                vm_ptr->exception = co->waiting_for->error;
                vm_ptr->has_exception = 1;
                // 查找 catch/finally 并跳转
                // ★ 同样以**协程首帧**为边界：不许展开到调用方的帧去（F1）
                for (int i = vm_ptr->frame_cnt - 1; i >= co->saved_frame_cnt; i--) {
                    CallFrame* frame = &vm_ptr->frames[i];
                    if (frame->closure && frame->closure->function && !frame->closure->function->has_try) {
                        continue;
                    }
                    if (frame->catch_ip || frame->finally_ip) {
                        // 展开调用栈
                        for (int j = vm_ptr->frame_cnt - 1; j > i; j--) {
                            CallFrame* unwind_frame = &vm_ptr->frames[j];
                            if (unwind_frame->locals && unwind_frame->locals_is_dynamic) {
                                free(unwind_frame->locals);
                                unwind_frame->locals = NULL;
                            }
                        }
                        vm_ptr->frame_cnt = i + 1;
                        vm_ptr->sp = frame->stack_base;
                        if (frame->catch_ip) {
                            frame->ip = frame->catch_ip;
                            // 手动压入异常到栈
                            if (vm_ptr->sp >= vm_ptr->stack_capacity) {
                                int new_capacity = vm_ptr->stack_capacity < 8 ? 8 : vm_ptr->stack_capacity * 2;
                                Value* new_stack = (Value*)realloc(vm_ptr->stack, new_capacity * sizeof(Value));
                                if (new_stack) {
                                    vm_ptr->stack = new_stack;
                                    vm_ptr->stack_capacity = new_capacity;
                                }
                            }
                            vm_ptr->stack[vm_ptr->sp++] = vm_ptr->exception;
                        } else {
                            vm_ptr->pending_exception = 1;
                            frame->ip = frame->finally_ip;
                        }
                        co->waiting_for = NULL;
                        // 异常已处理，清除异常状态
                        vm_ptr->has_exception = 0;
                        goto resume_done;
                    }
                }
                // 没有找到异常处理
                if (co->saved_frames && co->saved_frame_count > 0) {
                    error_add_at(ERR_RUNTIME, co->saved_frames[0].chunk->lines[0], 0, "未捕获的异步异常");
                } else {
                    error_add_at(ERR_RUNTIME, 0, 0, "未捕获的异步异常");
                }
            } else {
                // ★ 寄存器式：把 Future 结果写回挂起时记录的**目标寄存器**。
                //   OP_AWAIT 已在顶层帧保存了 await_dst_reg，恢复后顶层帧就是
                //   发起 await 的那一帧（帧已按原样装回）。栈式在这里是压栈。
                if (co->await_dst_reg >= 0 && vm_ptr->frame_cnt > 0) {
                    CallFrame* af = &vm_ptr->frames[vm_ptr->frame_cnt - 1];
                    if (af->locals && co->await_dst_reg < af->local_count) {
                        af->locals[co->await_dst_reg] = co->waiting_for->result;
                        gc_write_barrier((Object*)af->closure, co->waiting_for->result);
                    }
                }
                co->await_dst_reg = -1;
            }
            co->waiting_for = NULL;
        }
        resume_done:;
        
        // 执行协程直到完成或再次挂起
        int result = vm_run_with_vm(vm_ptr);
        
        // 如果协程再次挂起，OP_AWAIT 已经保存了所有 frame
        // 如果协程完成，需要清理 saved_frames（如果有的话）
        // OP_AWAIT 会释放旧的 saved_frames 并重新分配，所以这里不需要处理
        
        // 恢复之前的 frame_cnt（移除本协程添加的 frame）
        // 注意：
        // - 协程挂起时: OP_AWAIT 已将帧保存到 saved_frames，动态 locals 所有权也转移了
        //   此时 vm.frames 中协程帧的 locals 指针不应被释放（所有权已转给 saved_frames）
        //   所以只需重置 frame_cnt，不释放 locals
        // - 协程完成时: OP_RETURN 已逐帧释放动态 locals 并减少 frame_cnt
        //   正常情况下 frame_cnt 已等于 saved_frame_cnt，无需清理
        // - 异常完成时: 可能有部分帧未正常 OP_RETURN，需清理这些帧的动态 locals
        //   但此时 saved_frames 应为空（恢复时已转出所有权）
        if (co->state == COROUTINE_SUSPENDED) {
            // 挂起时 saved_frames 拥有动态 locals，不释放 vm.frames 中的
            vm_ptr->frame_cnt = saved_frame_cnt;
        } else {
            // 完成或失败时，清理残留帧的动态 locals
            for (int i = saved_frame_cnt; i < vm_ptr->frame_cnt; i++) {
                CallFrame* f = &vm_ptr->frames[i];
                if (f->locals_is_dynamic && f->locals) {
                    free(f->locals);
                    f->locals = NULL;
                }
            }
            vm_ptr->frame_cnt = saved_frame_cnt;
        }
        
        // 检查协程是否完成
        // 如果 result == 0 且状态不是 SUSPENDED（即没有被再次挂起），则协程已完成
        if (result == 0 && co->state != COROUTINE_SUSPENDED) {
            co->state = COROUTINE_COMPLETED;
            // 完成 task_future（这是 task 返回给调用者的 Future）
            if (co->task_future && !co->task_future->completed) {
                future_complete(co->task_future, co->result);
            }
            // 释放初始参数内存
            if (co->initial_args) {
                free(co->initial_args);
                co->initial_args = NULL;
                co->initial_arg_count = 0;
            }
        } else if (vm_ptr->has_exception) {
            co->state = COROUTINE_FAILED;
            co->result = vm_ptr->exception;
            // GC 写屏障：协程可能已晋升老年代，exception 可能是年轻代对象
            gc_write_barrier((Object*)co, vm_ptr->exception);
            if (co->task_future && !co->task_future->completed) {
                // 如果有等待者，标记错误已传播
                if (co->task_future->waiter) {
                    co->error_propagated = 1;
                }
                future_fail(co->task_future, vm_ptr->exception);
            }
            // 释放初始参数内存
            if (co->initial_args) {
                free(co->initial_args);
                co->initial_args = NULL;
                co->initial_arg_count = 0;
            }
        }

        vm_ptr->current_coroutine = saved_current;
        current_exec_vm = saved_vm;  // 恢复之前的 VM
        return result;
    }
    
    vm_ptr->current_coroutine = saved_current;
    current_exec_vm = saved_vm;  // 恢复之前的 VM
    return 0;
}

// 包装函数：主线程使用全局 vm
int vm_run_coroutine(ObjCoroutine* co) {
    return vm_run_coroutine_with_vm(co, &vm);
}

// 获取当前协程
ObjCoroutine* vm_current_coroutine(void) {
    VM* target_vm = current_exec_vm ? current_exec_vm : &vm;
    return target_vm->current_coroutine;
}

int vm_in_async_context(void) {
    VM* target_vm = current_exec_vm ? current_exec_vm : &vm;
    return target_vm->current_coroutine != NULL;
}

// VM 初始化和栈操作（放在最后，因为它依赖前面的函数）
#include "vminc/vm_init.inc"
