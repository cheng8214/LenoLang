/* Leno FFI Clib - 供 VM OP_CLIB_CALL 使用的公开接口 */
#ifndef LENO_FFI_CLIB_H
#define LENO_FFI_CLIB_H

#include "leno_ffi.h"
#include "../../include/leno_value.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * VM 内部调用 FFI 函数（供 OP_CLIB_CALL 使用）
 * 参数布局：args[0]=lib, args[1]=func_name_str, args[2..]=user_args
 * ret_type 为 FFIType 枚举值
 * arg_types 为用户参数的类型（TypeKind 枚举值），用于 str16 自动转换；
 *   传 NULL 则不进行类型感知转换（兼容旧路径）
 * 返回调用结果
 */
Value ffi_clib_call(int argc, Value* args, int ret_type, const int* arg_types);

#ifdef __cplusplus
}
#endif

#endif /* LENO_FFI_CLIB_H */
