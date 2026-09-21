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

// ⚠ 改这个版本号之前先看 docs/待办_单一事实来源与重复实现收敛.md 第七节
//   「缓存格式与版本号登记表」：.lenb / .lenomc / .lenosymc / .lenb.deps 四处要一起评估，
//   并写明"为什么升 / 为什么不升"；改前先 git fetch（2026-09-16 撞过车：两个会话都用 v23
//   但格式不同，数值相同、格式不同 ⇒ 靠版本号区分不开）。
#define LENO_BIN_MAGIC      0x424E454C  // "LENB" little-endian
// v3.0.1（2026-09-21）：S2/2b-2 的 `module_slot16` 操作数**重新落地到寄存器式**——
//   移植时丢了 codegen 侧的发射与 VM 侧的窺探/跳过，于是 `OP_STRUCT_INIT` 的操作数布局
//   与"带 3 字节"的口径不一致（旧寄存器构建产物少 3 字节，新 VM 的 `p += 3` 会多跳
//   3 字节 ⇒ 后续指令全部错位）。布局变了就必须整体失效重编译：bump 本版本号 +
//   LENO_MODCACHE_VERSION（.lenomc 里也含模块字节码）；.lenosymc 只存符号表、不含字节码
//   ⇒ 不动；.lenb.deps 靠 exe 指纹 fail-closed ⇒ 不动。
// v2.7.3（2026-09-16）：S2/2b-2 —— OP_STRUCT_INIT 追加 3 字节操作数（mod_space 1B + mod_slot 2B：
//   导入模块在 globals 里的槽位）。**操作数布局变了**：旧构建按 5+2N+arg 推进、新字节码多 3 字节
//   （反之新构建读旧字节码会多读 3 字节）⇒ 两侧都错位，必须整体失效重编译（.lenb / entry_*.lenb）。
// v2.7.2（2026-09-16）：Phase 1 —— 扫描阶段的 enum 成员求值改由真解析器执行（删掉复刻求值器）。
//   对"旧扫描器失败、解析器成功"的形态（浮点截断、超大整数饱和），旧产物里烙的是自动递增值
//   ⇒ .lenb / entry_*.lenb 必须整体失效重编译。
// v2.7.1（2026-09-16）：枚举成员求值语义修正 —— 扫描器补 `not`、除零/取模零改为与解析器同结论
//   ⇒ 修正前编译出的 .lenb / entry_*.lenb 里可能烙着错的常量值，必须整体失效重编译。
#define LENO_BIN_VERSION    0x00030001  // v3.0.1 - OP_STRUCT_INIT 恢复 3 字节
                                        //   module_slot16 操作数（见上面 v3.0.1 条目）
                                        // v3.0.0 - 寄存器式字节码：定长 4 字节指令，
                                        //   OpCode 枚举完全重写，旧 .lenb 全部失效
                                        //   读回来即悬空；owned 的还会被下个进程 free ⇒ 堆破坏）；
                                        //   空 chunk 的表示由 5×u32(20B) 改为 4×u32+u8(17B)，与
                                        //   deserialize_chunk_data 的读取逐字节对齐。
                                        //   两者都改变了对既有 .lenb 的读法（旧文件含其一即被错读/
                                        //   还原出悬空指针），故必须 bump：让旧 .lenb / entry_*.lenb
                                        //   整体失效重编译。
                                        // v2.6.0 - 新增 OP_CMPJMP_LI_INT（local vs 立即数比较+跳转）
                                        //   追加在枚举末尾，既有 opcode 编号全部不变；
                                        //   但仍要 bump：新字节码含旧构建不认识的 opcode
                                        //   注意：改变 opcode 集合/编号/操作数编码后必须 bump 本版本号，
                                        //   否则旧构建会按 magic+version 校验通过、直接加载含未知 opcode 的
                                        //   entry_*.lenb 并跳转发散（0xC0000005）；反之亦然
#define LENO_BIN_EXT        ".lenb"

// 模块编译缓存格式（.lenomc）—— 跨运行的模块编译产物缓存
// ⚠ 版本号登记表：docs/待办_单一事实来源与重复实现收敛.md 第七节（与 .lenb 同源改动要一起评估）
#define LENO_MODCACHE_MAGIC    0x434D4E4C  // "LNMC" little-endian
// v14：模块符号表记录 `async` 标记（ModuleFuncSymbol.is_async / ModuleStructMethod.is_async）。
//      旧缓存没有这两位 ⇒ 跨模块 async 会被当成普通函数（同步调用、返回值不是 Future），
//      所以必须**作废旧缓存**。
// v13：ObjFunction 增加 is_async（运行期判定"调用即建协程"用）。旧缓存里的函数对象缺这个
//      字段 ⇒ async 函数的**间接调用**（`var f = w; f()`、当参数传、绑定方法）会退回同步执行、
//      静默错值，所以必须**作废旧缓存**。
#define LENO_MODCACHE_VERSION  0x0000000E  // 上一版 v13 / v12 - 同 LENO_BIN_VERSION v3.0.1（OP_STRUCT_INIT
                                           //       多 3 字节 module_slot16 操作数，模块字节码里
                                           //       同样存在，旧构建按旧长度推进会错位）
                                           // v11 - 寄存器式字节码，与 LENO_BIN_VERSION v3.0.0 同步
                                           //       指纹）。模块字节码里烙着 native 方法签名 /
                                           //       模块常量 / 实例方法表 / 求值语义等**没有源文件**
                                           //       的编译期输入，只看 src_hash+dep_hash 会漏整类
                                           //       失效（实测：静默错误代码）。**不**跟升
                                           //       LENO_BIN_VERSION：.lenb 的字节布局未改，
                                           //       字段只加在 .lenomc 自己的 header 上。
                                           // v9 - 同 LENO_BIN_VERSION v2.7.3（OP_STRUCT_INIT 追加
                                          //      mod_space + mod_slot 操作数 —— 模块字节码里同样存在，
                                          //      旧构建按旧长度推进会错位）。
                                          // v8 - 同 LENO_BIN_VERSION v2.7.2（Phase 1：扫描阶段 enum
                                          //      求值改由真解析器执行）—— 模块产物里可能烙着错值。
                                          // v7 - 同 LENO_BIN_VERSION v2.7.1（求值语义修正）。
                                          // v6 - 不再序列化 ObjFFIPointer（原始地址跨进程无意义：
                                          //      owned 的还会被下个进程 free ⇒ 堆破坏）。旧缓存里
                                          //      可能存着这种指针，必须整体失效重编译。
                                          //      同一处改动同时把 LENO_BIN_VERSION 升到 v2.7.0。
                                          // v5 - 与 LENO_BIN_VERSION v2.6.0 同步（新增 OP_CMPJMP_LI_INT）
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

// 序列化到内存：返回 malloc 的缓冲（调用方负责 free），不落盘
// 返回值 SERIALIZE_OK 时 *out_data / *out_size 有效
SerializeResult chunk_serialize_to_memory(Chunk* chunk, Scope* global_scope,
                                          uint8_t** out_data, size_t* out_size);

// 从内存缓冲反序列化：直接在 data 上解析，不 free 调用方缓冲
SerializeResult chunk_deserialize_from_memory(uint8_t* data, size_t size,
                                              Chunk* out_chunk, Scope** out_scope);

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

// ============================================================================
// 模块源快照 API —— 「这段字节码/符号表当初由哪一版源码编出来，那份源码变了没有」的
// 唯一实现。三处缓存产物（.lenomc / .lenosymc / entry_*.lenb.deps）都只允许调这三个，
// 不要再写第四份比对（见 docs/待办_单一事实来源与重复实现收敛.md 的 Phase 3 / S8）。
// 哈希口径：文本模式读入（Windows 下 CRLF→LF）+ FNV-1a；大小取 stat 的磁盘字节数。
// ============================================================================

// ① 取「当前源文件」的快照。out_size 可传 NULL（跳过 stat）；
//    out_hash 传 NULL 时只取大小。返回 0=成功，-1=读不到
int module_source_snapshot_now(const char* src_path, uint64_t* out_size, uint64_t* out_hash);

// ② 取「已编译版本」的快照：读该模块在 cache_dir 下 .lenomc 的 header。
//    返回 0=成功（out_size/out_hash 被填），-1=拿不到（缓存不存在/损坏/版本不符）
int module_cache_read_source_snapshot(const char* cache_dir, const char* src_path,
                                      uint64_t* out_size, uint64_t* out_hash);

// ③ 判定某个快照是否仍成立：先比大小（check_size=0 时跳过，给只存了哈希的格式用），
//    再比内容哈希；源文件读不到一律判「不成立」（fail-closed）。
//    返回 1=成立，0=不成立
int module_source_snapshot_matches(const char* src_path, uint64_t size, uint64_t hash,
                                   int check_size);

// ④ 取「**运行中的可执行文件**」的指纹（size + mtime + 内容 FNV-1a）。
//    返回 >0 = 指纹；0 = 取不到（调用方必须按 fail-closed 处理：把缓存判为失效）。
//
//    为什么需要它（§8.112，2026-09-17）：字节码里烙着**编译期决策**（类型、opcode、名字解析），
//    其中一部分来自**原生模块注册表**（native 方法签名 / 模块常量 / 实例方法表）—— 这些东西
//    **没有源文件**，改一处 C 代码不会让任何 .leno 源快照变化 ⇒ 三个 source snapshot 全都
//    「成立」⇒ 缓存被判有效却已过期 ✗。实测事故：`times.ms()` 的注册由 TYPE_INT 改成
//    TYPE_FLOAT 后，`examples/性能测试/光线追踪对比.leno` 仍用旧缓存的"int 签名"编译
//    ⇒ `t2 - t1` 按整数相减 ⇒ 打印出 10^9 量级的"毫秒"（静默错误代码）。
//
//    做法：直接把**当前 exe 自身**当输入 —— ABI、注册表、编译器语义任何一处改动都必须
//    重新构建 ⇒ 指纹必变 ⇒ 一次覆盖**整类**问题（不必逐张表去枚举，也就不会漏表）。
//    方向是 fail-closed：指纹取不到时调用方须判缓存失效（宁可重编译，不要跑旧码）。
uint64_t cache_runtime_binary_fingerprint(void);

#endif // LENO_SERIALIZE_H
