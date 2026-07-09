#ifndef LENO_SERIALIZE_H
#define LENO_SERIALIZE_H

#include "leno_vm.h"
#include <stdio.h>
#include <stdint.h>

// ============================================================================
// 二进制文件格式 (.lenb)
// ============================================================================
//
// 文件布局:
//   Header (20 bytes)
//     Magic:      "LENB" (4 bytes)
//     Version:    uint32
//     Flags:      uint32 (保留)
//     SrcHash:    uint64 (源文件 FNV-1a 哈希，用于缓存失效)
//
//   ScopeData
//     global_var_count:  uint32
//     global_func_count: uint32
//     symbols[]:         SymbolEntry[]
//
//   Chunk (递归格式)
//     filename_len:  uint32
//     filename:      UTF-8 bytes
//     local_count:   uint32
//     const_count:   uint32
//     constants[]:   ConstantEntry[]
//     code_len:      uint32
//     code:          uint8[]
//     lines[]:       uint16[] (行号表，与 code 等长)
//
// ConstantEntry:
//   type_tag (1 byte) + payload
//     0x00: NULL       (无载荷)
//     0x01: TRUE       (无载荷)
//     0x02: FALSE      (无载荷)
//     0x03: INT        -> int32 (4 bytes)
//     0x04: FLOAT      -> double (8 bytes, IEEE 754)
//     0x05: STRING     -> uint32 len + UTF-8 bytes
//     0x06: FUNCTION   -> FunctionData (递归)
//     0x07: BIGINT     -> uint32 limb_count + uint8 is_negative + uint32[] limbs
//     0x08: RANGE      -> int32 start + int32 end + uint8 inclusive
//     0x09: ARRAY      -> uint32 count + ConstantEntry[]
//     0x0A: DICT       -> uint32 count + (ConstantEntry key + ConstantEntry value)[]
//     0x0B: ENUM_DEF   -> name + members[]
//     0x0C: STRUCT_DEF -> name + fields[] + methods[]
//     0x0D: CSTRUCT_DEF -> name + fields[] + total_size + alignment
//     0x0E: FACE_DEF   -> name + method_sigs[]
//     0x0F: MODULE     -> name + source_path + globals[] + exports{}
//
// FunctionData:
//   name_len:       uint32
//   name:           UTF-8 bytes
//   arity:          uint32
//   upvalue_count:  uint32
//   local_count:    uint32
//   has_try:        uint8
//   param_count:    uint32
//   param_types:    uint8[] (TypeKind 枚举值)
//   chunk:          Chunk (递归)
//
// SymbolEntry:
//   kind:           uint8 (SymKind)
//   name_len:       uint32
//   name:           UTF-8 bytes
//   index:          int32
//   is_captured:    uint8
//   type_kind:      uint8 (TypeKind，仅用于需要类型信息的符号)
//   has_dict_keys:  uint8
//   dict_key_count: uint32 (如果 has_dict_keys)
//   dict_keys[]:    (uint32 len + UTF-8 bytes)[]
//
// ============================================================================

#define LENO_BIN_MAGIC      0x424E454C  // "LENB" little-endian
#define LENO_BIN_VERSION    0x00020100  // v2.1.0 - 序列化模块补充 use_reexport 字段
#define LENO_BIN_EXT        ".lenb"

// 模块编译缓存格式（.lenomc）—— 跨运行的模块编译产物缓存
#define LENO_MODCACHE_MAGIC    0x434D4E4C  // "LNMC" little-endian
#define LENO_MODCACHE_VERSION  0x00000001
#define LENO_MODCACHE_EXT      ".lenomc"

// 常量类型标签
#define CONST_TAG_NULL       0x00
#define CONST_TAG_TRUE       0x01
#define CONST_TAG_FALSE      0x02
#define CONST_TAG_INT        0x03
#define CONST_TAG_FLOAT      0x04
#define CONST_TAG_STRING     0x05
#define CONST_TAG_FUNCTION   0x06
#define CONST_TAG_BIGINT     0x07
#define CONST_TAG_RANGE      0x08
#define CONST_TAG_ARRAY      0x09
#define CONST_TAG_DICT       0x0A
#define CONST_TAG_ENUM_DEF   0x0B
#define CONST_TAG_STRUCT_DEF 0x0C
#define CONST_TAG_CSTRUCT_DEF 0x0D
#define CONST_TAG_FACE_DEF   0x0E
#define CONST_TAG_MODULE    0x0F
#define CONST_TAG_FFI_PTR   0x10
#define CONST_TAG_CLOSURE   0x11
#define CONST_TAG_FFI_LIB   0x12
#define CONST_TAG_MODULE_REF 0x13

// 序列化结果
typedef enum {
    SERIALIZE_OK = 0,
    SERIALIZE_ERR_FILE,
    SERIALIZE_ERR_WRITE,
    SERIALIZE_ERR_READ,
    SERIALIZE_ERR_MAGIC,
    SERIALIZE_ERR_VERSION,
    SERIALIZE_ERR_FORMAT,
    SERIALIZE_ERR_MEMORY,
    SERIALIZE_ERR_TYPE,
} SerializeResult;

// 反序列化上下文
typedef struct {
    uint8_t* data;
    size_t size;
    size_t pos;
} DeserializeCtx;

// ============================================================================
// 序列化 API
// ============================================================================

// 将 Chunk + Scope 序列化写入文件
SerializeResult chunk_serialize(const char* path, Chunk* chunk, Scope* global_scope);

// 从文件反序列化 Chunk + Scope
SerializeResult chunk_deserialize(const char* path, Chunk* out_chunk, Scope** out_scope);

// 检查文件是否是 .lenb 二进制文件
int serialize_is_binary_file(const char* path);

// 获取 .lenb 文件路径（将 .leno 替换为 .lenb，或追加 .lenb）
// 返回值需要调用者 free
char* serialize_get_bin_path(const char* source_path);

// 检查 .lenb 缓存是否有效（存在且比源文件新）
int serialize_cache_is_valid(const char* source_path, const char* bin_path);

// 计算源代码的 FNV-1a 哈希
uint64_t serialize_source_hash(const char* source, size_t len);

// ============================================================================
// 模块编译缓存 API（跨运行缓存 import 的 .leno 模块编译产物）
// ============================================================================

// 计算模块缓存文件路径：<cache_dir>/<fnv1a(full_path)>.lenomc
// 返回值需调用者 free
char* module_cache_path_for(const char* full_path, const char* cache_dir);

// 序列化单个模块到缓存文件（含依赖信息收集）
// source: 模块源代码（用于计算 src_hash 做失效判定）
SerializeResult module_cache_serialize(const char* cache_path,
                                       ObjModule* mod,
                                       const char* source);

// 从缓存文件反序列化单个模块
// full_path: 模块绝对规范化路径（用于加入 loaded_modules 与依赖匹配）
// 返回模块对象，失败（缓存不存在/失效/损坏）返回 NULL
ObjModule* module_cache_deserialize(const char* cache_path,
                                     const char* full_path);

#endif // LENO_SERIALIZE_H
