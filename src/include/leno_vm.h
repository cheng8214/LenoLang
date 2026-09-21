#ifndef LENO_VM_H
#define LENO_VM_H

#include <stdbool.h>
#include "leno_types.h"
#include "leno_value.h"
#include "platform_thread.h"

// ============================================================================
// 寄存器式字节码操作码（定长 4 字节，Lua 5.x 风格编码）
// ============================================================================
// 编码形态：
//   iABC  : op8 A8 B8 C8        — 三寄存器运算，A = 目标寄存器
//   iABx  : op8 A8 Bx16        — 寄存器 + 常量/全局/upvalue 索引
//   iAsBx : op8 A8 sBx16       — 寄存器 + 有符号立即数
//   iAsJ  : op8 + sJ24         — 跳转（24 位有符号偏移，以字节为单位）
//
// 寄存器文件 = CallFrame.locals：
//   R0..arity-1 = 参数，声明变量紧随，临时寄存器往后
//   不再使用 vm.stack 做表达式求值
// ============================================================================

typedef enum {
    // --- 空操作 / 杂项 ---
    OP_NOP,             // iABC  空操作

    // --- 装载 / 移动 ---
    OP_LOADK,           // iABx  R[A] = K[Bx]            常量表
    OP_LOADI,           // iAsBx R[A] = sBx              整数立即数
    OP_LOADF,           // iAsBx R[A] = (double)sBx      浮点立即数
    OP_LOADNIL,         // iABC  R[A] = null
    OP_LOADTRUE,        // iABC  R[A] = true
    OP_LOADFALSE,       // iABC  R[A] = false
    OP_MOV,             // iABC  R[A] = R[B]

    // --- 全局变量 ---
    OP_GETGLOBAL,       // iABx  R[A] = globals[Bx]
    OP_SETGLOBAL,       // iABx  globals[Bx] = R[A]
    OP_DEFGLOBAL,       // iABx  globals[Bx] = R[A]（定义）
    OP_GETGLOBALFUNC,   // iABx  R[A] = global_funcs[Bx]
    OP_DEFGLOBALFUNC,   // iABx  global_funcs[Bx] = R[A]

    // --- upvalue / 闭包 ---
    OP_GETUPVAL,        // iABx  R[A] = upvalues[Bx]
    OP_SETUPVAL,        // iABx  upvalues[Bx] = R[A]
    OP_CLOSE,           // iABx  关闭 R[A..A+Bx-1] 的 upvalue
    OP_CLOSURE,         // iABx  R[A] = closure(func_const[Bx])

    // --- 算术运算（通用） ---
    OP_ADD,             // iABC  R[A] = R[B] + R[C]
    OP_SUB,             // iABC  R[A] = R[B] - R[C]
    OP_MUL,             // iABC  R[A] = R[B] * R[C]
    OP_DIV,             // iABC  R[A] = R[B] / R[C]
    OP_MOD,             // iABC  R[A] = R[B] % R[C]
    OP_NEG,             // iABC  R[A] = -R[B]            一元取负
    OP_NOT,             // iABC  R[A] = !R[B]            逻辑取反

    // --- 算术运算 int 特化（溢出自动提升 bigint） ---
    OP_ADD_INT,         // iABC  R[A] = R[B] + R[C] (int 快速路径)
    OP_SUB_INT,         // iABC
    OP_MUL_INT,         // iABC
    OP_NEG_INT,         // iABC  R[A] = -R[B] (int 快速路径)

    // --- 算术运算 float 特化 ---
    OP_ADD_F,           // iABC  float 快速路径
    OP_SUB_F,           // iABC
    OP_MUL_F,           // iABC
    OP_DIV_F,           // iABC
    OP_NEG_F,           // iABC

    // --- 比较运算（结果为 bool） ---
    OP_EQ,              // iABC  R[A] = (R[B] == R[C])
    OP_LT,              // iABC  R[A] = (R[B] <  R[C])
    OP_GT,              // iABC  R[A] = (R[B] >  R[C])
    OP_LE,              // iABC  R[A] = (R[B] <= R[C])
    OP_GE,              // iABC  R[A] = (R[B] >= R[C])
    OP_NEQ,             // iABC  R[A] = (R[B] != R[C])

    // --- 比较 int 特化 ---
    OP_LT_INT,          // iABC
    OP_GT_INT,          // iABC
    OP_LE_INT,          // iABC
    OP_GE_INT,          // iABC
    OP_EQ_INT,          // iABC

    // --- 位运算 ---
    OP_BITAND,          // iABC  R[A] = R[B] & R[C]
    OP_BITOR,           // iABC  R[A] = R[B] | R[C]
    OP_BITXOR,          // iABC  R[A] = R[B] ^ R[C]
    OP_BITNOT,          // iABC  R[A] = ~R[B]
    OP_SHL,             // iABC  R[A] = R[B] << R[C]
    OP_SHR,             // iABC  R[A] = R[B] >> R[C]（算术右移）
    OP_USHR,            // iABC  R[A] = R[B] >>> R[C]（逻辑右移）

    // --- 类型转换 ---
    OP_CAST_FLOAT,      // iABC  R[A] = (float)R[B]
    OP_CAST_INT,        // iABC  R[A] = (int)R[B]
    OP_CAST_STRING,     // iABC  R[A] = (string)R[B]
    OP_IS_NULL,         // iABC  R[A] = (R[B] == null)

    // --- 字符串拼接 ---
    OP_STRCAT,          // iABC  R[A] = str(R[B]) .. str(R[C])

    // --- 跳转 ---
    OP_JMP,             // iAsJ  pc += sJ
    OP_JMP_IF_FALSE,    // iAsBx if !R[A] then pc += sBx
    OP_JMP_IF_TRUE,     // iAsBx if R[A] then pc += sBx
    OP_TEST,            // iABC  if !(R[B] op C) then pc++  (C=0:not, C=1:is)

    // --- 自增自减 ---
    OP_INC,             // iABC  R[A] = R[B] + 1
    OP_DEC,             // iABC  R[A] = R[B] - 1

    // --- 调用 / 返回 ---
    // OP_CALL: R[A] = callee, R[A+1..A+B-1] = 实参, B = nargs+1, C = nresults+1
    OP_CALL,            // iABC
    // OP_CALL_NATIVE: R[A] = 结果, Bx = 常量表中函数名索引, C 通过 EXTRAARG 编码（或固定）
    // 简化：CALL_NATIVE 用 iABC，A=结果寄存器，B=函数名常量索引(低8位)，C=nargs
    OP_CALL_NATIVE,     // iABC  R[A] = native(K[B>>0])(R[A+1..A+C-1])
    // OP_RETURN: 返回 R[A..A+B-2], B = nresults+1
    OP_RETURN,          // iABC
    OP_RETURN_MULTI,    // iABC  B = 存放个数的寄存器
    OP_TAIL_CALL,       // iABC  尾调用：复用当前帧

    // --- 数组 ---
    OP_NEWARRAY,        // iABC  R[A] = new array(R[A+1..A+C-1]), C = count
    OP_ARRAY_GET,       // iABC  R[A] = R[B][R[C]]
    OP_ARRAY_SET,       // iABC  R[B][R[C]] = R[A]
    OP_ARRAY_APPEND,    // iABC  append R[A] to R[B], R[A] = new len（表达式用）
    OP_LEN,             // iABC  R[A] = len(R[B])

    // --- 字典 ---
    OP_NEWDICT,         // iABC  R[A] = new dict, C = 初始容量
    OP_DICT_GET,        // iABC  R[A] = R[B][R[C]]
    OP_DICT_SET,        // iABC  R[B][R[C]] = R[A]
    OP_DICT_GET_KEY,    // iABC  R[A] = key(R[B], R[C])  for 迭代

    // --- 通用索引 ---
    OP_INDEX,           // iABC  R[A] = R[B][R[C]]（数组或字典）
    OP_INDEX_SET,       // iABC  R[B][R[C]] = R[A]
    OP_SLICE,           // iABC  R[A] = R[B][R[A+1]:R[A+2]]  切片

    // --- 迭代 ---
    OP_ITER_GET,        // iABC  R[A] = iterate(R[B], R[C])
    OP_ITER_GET_VALUE,  // iABC  R[A] = iterate_value(R[B], R[C])

    // --- 范围 ---
    OP_RANGE,           // iABC  R[A] = Range(R[B], R[C])

    // --- in 操作符 ---
    OP_IN,              // iABC  R[A] = (R[B] in R[C])

    // --- 异常处理 ---
    OP_TRY,             // iABx  设置 catch_ip = chunk->code + Bx
    OP_CATCH,           // iABx  清除 catch_ip，设置异常变量 R[A]
    OP_FINALLY,         // iABx  设置 finally_ip
    OP_END_TRY,         // iABC  清除 try 上下文
    OP_THROW,           // iABC  throw R[A]

    // --- 模块 ---
    OP_LOAD_NATIVE_MODULE, // iABx  加载原生模块
    OP_MODULE_CALL,     // iABC  R[A] = module.method(R[A+1..])
    OP_GET_MODULE_CONST, // iABx  R[A] = module_const[Bx]
    OP_GET_MODULE_VAR,  // iABC  R[A] = module[B].var[C]
    OP_SET_MODULE_VAR,  // iABC  module[B].var[C] = R[A]
    OP_GET_MODULE_FUNC, // iABC
    OP_DEFINE_MODULE_FUNC, // iABC
    OP_INIT_LENOMODULE, // iABx  初始化 .leno 模块

    // --- struct ---
    OP_STRUCT_DEF,      // iABx  定义结构体
    OP_STRUCT_INIT,     // iABC  R[A] = new struct(R[A+1..A+C-1])
    OP_GET_FIELD,       // iABC  R[A] = R[B].field(C)
    OP_SET_FIELD,       // iABC  R[B].field(C) = R[A]
    OP_GET_FIELD_ADDR,  // iABC  R[A] = &R[B].field(C)（cstruct）
    OP_GET_METHOD,      // iABC  R[A] = R[B].method(C)
    OP_SET_PTR_ELEM_TYPE, // iABC  R[A] 是 FFI 指针时设置 element_type = (TypeKind)B
    OP_SET_DECLARED_FACE, // iABx  R[A] 是 struct 实例时设置 declared_face = K[Bx]

    // --- enum / face / cstruct ---
    OP_ENUM_DEF,        // iABx
    OP_FACE_DEF,        // iABx
    OP_CSTRUCT_DEF,     // iABx
    OP_GET_CSTRUCT_DEF, // iABx

    // --- 类型检查 / 转换 ---
    OP_TYPE_CHECK,      // iABx  R[A] = (R[A] is type(Bx))
    OP_AS_CAST,         // iABx  R[A] = R[A] as type(Bx)

    // --- 协程 ---
    OP_AWAIT,           // iABC  R[A] = await R[B]
    OP_ASYNC_CALL,      // iABC  R[A] = async call R[B]

    // --- FFI ---
    OP_CLIB_CALL,       // iABC  R[A] = clib(R[B], R[C]...)
    OP_CFUNC_CALLBACK,  // iABx  R[A] = cfunc_callback(K[Bx])
    OP_U8_TO_F64,       // iABC  R[A] = (double)(uint8)R[B]

    // --- 泛型 / 析构 ---
    OP_PUSH_TYPE_ARGS,  // iABx  推送类型参数
    OP_DTOR_LOCAL,      // iABC  析构 R[A]..R[A+C-1]

    // --- switch ---
    OP_SWITCH_LOOKUP,   // iABx  switch 查找

    // --- for 数值循环（寄存器块：R[A]=start, R[A+1]=end, R[A+2]=step, R[A+3]=var）---
    // 两条指令均为 iABC + 紧随 2 字节 sBx（与 CLOSURE 一样，是"指令 + 附加数据"）：
    //   OP_FOR_PREP A B C : R[A+3] = R[A]；初始条件不满足则 ip += sBx（跳过循环体）
    //   OP_FOR_LOOP A B C : R[A+3] += R[A+2]；仍满足条件则 ip += sBx（负值 = 回跳）
    //   B = inclusive（1 = 包含结束值）
    OP_FOR_PREP,
    OP_FOR_LOOP,

    // --- 扩展指令（寄存器号 > 255 或常量索引 > 65535 时使用） ---
    OP_EXTRAARG,        // iAx   24 位无符号扩展值
    OP_EXTEND,          // iABC  前缀：扩展紧随指令的 A/B/C 高 8 位

    // --- 立即数快速路径（追加在末尾，避免改动既有 opcode 编号）-------------
    // 右操作数是小整数字面量时，省掉「求右值 → LOADI → *_INT」两条指令。
    // 立即数用 C 的 8 位**有符号**表示（-128..127），覆盖 `n-1` / `i+1` 这类最常见写法。
    OP_ADD_INT_IMM,     // iABC   R[A] = R[B] + (int8_t)C
    OP_SUB_INT_IMM,     // iABC   R[A] = R[B] - (int8_t)C
    // 立即数比较（`n <= 1` 这类）：省掉「求右值 → LOADI」一条指令。
    // 与 OP_*_INT 同口径：左值为 null ⇒ 结果 false（立即数本身不会为 null）。
    OP_LT_INT_IMM,      // iABC   R[A] = R[B] <  (int8_t)C
    OP_GT_INT_IMM,      // iABC   R[A] = R[B] >  (int8_t)C
    OP_LE_INT_IMM,      // iABC   R[A] = R[B] <= (int8_t)C
    OP_GE_INT_IMM,      // iABC   R[A] = R[B] >= (int8_t)C
    // 全局函数直呼：省掉「GETGLOBALFUNC 取函数值 → CALL」两条里的取函数。
    //   A = 结果寄存器（实参在 R[A+1 .. A+B]），B = 实参个数，C = 全局函数槽位（8 位）。
    //   只支持槽位/实参数 ≤ 255 的调用点（codegen 不满足时不发这条指令，走老路径）。
    OP_CALL_GLOBAL_FUNC,

    // 「比较 + 条件跳转」融合（T10-①，对齐栈式的 OP_CMPJMP_*）：
    //   省掉一条 OP_JMP_IF_FALSE 的**派发**（条件判断在热循环里每次都要付）。
    //   编码：byte1 = A = 左操作数寄存器
    //         byte2 = B = 右操作数（寄存器号；C 的最高位为 1 时是 int8 立即数）
    //         byte3 = C = 0x80 位表示"立即数形式"，其余位保留
    //         紧随**第二个 4 字节字**：跳转偏移（16 位，存 offset + 32768，与 iAsBx 同约定）
    //   语义与 `OP_LT_INT[_IMM] + OP_JMP_IF_FALSE` 逐条等价：**比较为假则跳转**，
    //   且 int 快路径与 *_INT 系列完全一致（只排除 null，不做类型检查 —— 这是栈式口径，
    //   test_generic_face_impl 依赖它）。
    //   ⚠ codegen 只在"两侧静态类型都是 int、左操作数是普通局部变量/参数"时才发这条，
    //     与 *_INT / *_INT_IMM 的发射条件一致；不满足时仍走原来的两条指令。
    OP_CMPJMP_LT,       // (R[A] <  rhs) 为假 ⇒ 跳
    OP_CMPJMP_LE,       // (R[A] <= rhs) 为假 ⇒ 跳
    OP_CMPJMP_GT,       // (R[A] >  rhs) 为假 ⇒ 跳
    OP_CMPJMP_GE,       // (R[A] >= rhs) 为假 ⇒ 跳

    OP_OPCODE_COUNT,    // 用于跳转表大小
} OpCode;

// ============================================================================
// 寄存器式 4 字节指令编码/解码宏
// ============================================================================
// 指令布局（大端，与旧 emit 一致）：
//   字节0: opcode (8 位)
//   字节1: A (8 位)
//   字节2-3: B/C 或 Bx 或 sBx 或 sJ
//
// iABC :  op8 A8 B8 C8        B = byte2, C = byte3
// iABx :  op8 A8 Bx16         Bx = (byte2<<8)|byte3
// iAsBx:  op8 A8 sBx16        sBx = Bx - 32768 (偏移使 0 = -32768)
// iAsJ :  op8 + sJ24          sJ = (byte1<<16)|(byte2<<8)|byte3, 符号扩展
// iAx  :  op8 + Ax24          Ax = (byte1<<16)|(byte2<<8)|byte3

// 前向声明：reg_encode_* 内联函数在 Chunk 完整定义之前就需要 Chunk* 与 chunk_write
typedef struct Chunk Chunk;
void chunk_write(Chunk* chunk, uint8_t byte, int line);

// 诊断：寄存器号超过 8 位上限（编码会静默截断 ⇒ 读到别的槽位）。
// 定义在 debug.c。见 reg_encode_iABC 里的说明。
void codegen_reg_overflow_warn(OpCode op, int a, int b, int c, int line);

// 编码：把 4 字节写入 chunk（大端）
//   ⚠ A/B/C 都是 8 位：任何寄存器号 ≥ 256 都会被 &0xFF 静默截断成别的槽位号
//   ——"读出来是 null / 写坏了别人"这类怪现象的真凶。这里加一道编译期告警，
//   免得再靠猜（函数寄存器高水位 = func->local_count，超过 256 就已经不可寻址）。
static inline void reg_encode_iABC(Chunk* chunk, OpCode op, int a, int b, int c, int line) {
    if (a > 255 || b > 255 || c > 255) {
        codegen_reg_overflow_warn(op, a, b, c, line);
    }
    chunk_write(chunk, (uint8_t)op, line);
    chunk_write(chunk, (uint8_t)(a & 0xFF), line);
    chunk_write(chunk, (uint8_t)(b & 0xFF), line);
    chunk_write(chunk, (uint8_t)(c & 0xFF), line);
}

static inline void reg_encode_iABx(Chunk* chunk, OpCode op, int a, int bx, int line) {
    if (a > 255) {
        codegen_reg_overflow_warn(op, a, bx, -1, line);
    }
    chunk_write(chunk, (uint8_t)op, line);
    chunk_write(chunk, (uint8_t)(a & 0xFF), line);
    chunk_write(chunk, (uint8_t)((bx >> 8) & 0xFF), line);
    chunk_write(chunk, (uint8_t)(bx & 0xFF), line);
}

static inline void reg_encode_iAsBx(Chunk* chunk, OpCode op, int a, int sbx, int line) {
    int bx = sbx + 32768;  // 偏移使 0 表示 -32768
    reg_encode_iABx(chunk, op, a, bx, line);
}

static inline void reg_encode_iAsJ(Chunk* chunk, OpCode op, int sj, int line) {
    uint32_t j = (uint32_t)(sj & 0xFFFFFF);  // 24 位
    chunk_write(chunk, (uint8_t)op, line);
    chunk_write(chunk, (uint8_t)((j >> 16) & 0xFF), line);
    chunk_write(chunk, (uint8_t)((j >> 8) & 0xFF), line);
    chunk_write(chunk, (uint8_t)(j & 0xFF), line);
}

static inline void reg_encode_iAx(Chunk* chunk, OpCode op, int ax, int line) {
    uint32_t v = (uint32_t)(ax & 0xFFFFFF);
    chunk_write(chunk, (uint8_t)op, line);
    chunk_write(chunk, (uint8_t)((v >> 16) & 0xFF), line);
    chunk_write(chunk, (uint8_t)((v >> 8) & 0xFF), line);
    chunk_write(chunk, (uint8_t)(v & 0xFF), line);
}

// 解码宏（VM 侧使用）
#define REG_A(byte1)            (byte1)
#define REG_B(byte2)            (byte2)
#define REG_C(byte3)            (byte3)
#define REG_Bx(byte2, byte3)    (((int)(byte2) << 8) | (int)(byte3))
#define REG_sBx(byte2, byte3)   (REG_Bx(byte2, byte3) - 32768)
static inline int REG_sJ(int byte1, int byte2, int byte3) {
    uint32_t raw = ((uint32_t)(byte1 & 0xFF) << 16) | ((uint32_t)(byte2 & 0xFF) << 8) | (uint32_t)(byte3 & 0xFF);
    if (raw & 0x800000) return (int)(raw | 0xFF000000);  // 符号扩展
    return (int)raw;
}
#define REG_Ax(byte1, byte2, byte3) (((int)(byte1) << 16) | ((int)(byte2) << 8) | (int)(byte3))

// ============================================================================
// 字节码块
// ============================================================================

// OP_SWITCH_LOOKUP 的查找逻辑（定义在 vm.c）—— **语义唯一来源**，
// 所有调用方共用（见 vm.c 的说明与文档 §8.61）。
// 在 case 值数组（Value，必须是 OBJ_ARRAY）里二分查找 switch_val；
// 返回匹配下标，-1 = 未匹配（走 default）。case_count <= 0 或非数组 → -1。
int switch_lookup_index(Value switch_val, Value arr_val, int case_count);

// OP_STRING_ADD 的语义唯一来源（定义在 vm.c）—— 所有调用方共用（§8.64）。
// 字符串拼接：两侧都是 ObjString 时走 str_concat（正确处理内嵌 NUL），
// 否则两侧各自转字符串再拼。纯 Value→Value，不依赖 VM 状态、不报错。
Value string_add(Value a, Value b);

// OP_TYPE_CHECK 的类型判定（定义在 vm.c）—— **语义唯一来源**（§8.65）。
//   expected_type: 期望类型（TypeKind）
//   elem_type    : 元素类型字节（TYPE_ANY = 不检查元素；仅数组用）
//   name_val     : TYPE_STRUCT / TYPE_FACE / TYPE_ENUM 的名字字符串常量（其余类型忽略）
// 返回 1 = 匹配。纯判定：不分配、不报错。
int type_check_value(Value value, TypeKind expected_type, TypeKind elem_type, Value name_val);

// OP_AS_CAST 的安全类型转换（定义在 vm.c）—— **语义唯一来源**（§8.83）。
// 解释器（vm/vminc/op_as_cast.inc）与原生模块都调它：
//   value        : 被转换的值
//   expected_type: 期望类型（TypeKind）
//   elem_type    : 元素类型字节（TYPE_ANY = 不检查元素；仅数组 / 泛型指针用）
//   name_val     : TYPE_STRUCT / TYPE_FACE / TYPE_CSTRUCT 的名字字符串常量
//                  （其余类型忽略；ENUM **不在**本函数处理的类型里 —— 与解释器一致）
// 返回：匹配则为（可能已转换的）值，不匹配为 null。不报错；可能分配
//       （字符串转换 / 整数转 FFI 指针）。
Value vm_as_cast(Value value, TypeKind expected_type, TypeKind elem_type, Value name_val);

// struct 方法表查找（定义在 vm.c）—— 规则唯一来源（§8.66）。
// 按名字线性比对、跳过 ctor/dtor；找到返回 1 并回填 out_closure（预创建闭包，可为 NULL）
// 与 out_func。GC 安全的 push-pop / 报错文本留在调用方（各调用方各自处理）。
int struct_method_lookup(ObjStructDef* def, ObjString* method_name,
                         ObjClosure** out_closure, ObjFunction** out_func);

typedef struct Chunk {
    uint8_t* code;
    int len;
    int code_capacity;
    Value* constants;
    int const_cnt;
    int const_capacity;
    int* lines;
    char* filename;  // 源代码文件名（用于错误报告）
    int local_count; // 主代码块的局部变量数量（用于闭包捕获）
} Chunk;

// Chunk API
void chunk_init(Chunk* chunk);
void chunk_free(Chunk* chunk);
int chunk_add_const(Chunk* chunk, Value value);
void chunk_write(Chunk* chunk, uint8_t byte, int line);

// ============================================================================
// 调用栈帧
// ============================================================================

// 内联 locals 数组大小 - 小函数直接使用栈上空间，避免 malloc/free
// 经验值：大多数函数的局部变量不超过 8 个
// 性能修复：8 太小——7 参函数（如 sphereHit 16 槽、trace 18 槽）每次调用
// 都会 malloc/free，光追测试中 1600 万次调用的堆分配开销是输给 Python 的主因
// （见 docs/函数调用性能优化_调用帧与多返回值解构.md）。32 覆盖典型函数，
// CallFrame 增大 ~190B，对帧池内存和缓存压力的影响可忽略
#define INLINE_LOCALS_MAX 32

typedef struct CallFrame {
    Chunk* chunk;
    uint8_t* ip;
    int stack_base;      // 栈基址索引（用于操作数栈）
    int slot_count;      // locals数组大小（包括参数和局部变量）
    ObjClosure* closure;
    Value* locals;       // 独立的局部变量数组（可能指向 inline_locals）
    int local_count;     // locals数组大小（与slot_count相同）
    // 内联 locals 数组 - 小函数直接使用，避免内存分配
    Value inline_locals[INLINE_LOCALS_MAX];
    int locals_is_dynamic;  // 1 = 使用 malloc 分配，0 = 使用 inline_locals
    // 异常处理
    uint8_t* catch_ip;   // catch 块的指令指针
    uint8_t* finally_ip; // finally 块的指令指针
    uint8_t* prev_catch_ip;   // 保存之前的 catch_ip（用于嵌套 try-catch）
    uint8_t* prev_finally_ip; // 保存之前的 finally_ip（用于嵌套 try-catch）
    // ★ catch 体所属 try 的 finally 块入口（OP_CATCH 预备，最后由 OP_END_TRY 清除）：
    //   `try{...}catch e{ throw ... }finally{...}` 里 catch 体内再次 throw 时，
    //   必须先执行**本层** finally 再向外传播（finally_ip 此刻已被 END_TRY 还原成外层）
    uint8_t* catch_finally_ip;
    int in_finally;      // 标记是否在 finally 块中（防止无限循环）
    Value try_return_value;  // 保存 try 块中的 return 值
    int has_try_return;      // 标记是否有 try return 值需要处理
    // 模块信息
    void* module;        // 所属模块（如果是模块函数）
    // 优化标志
    int has_captures;    // 1 = 该帧捕获了 upvalue，0 = 无捕获（优化 close_upvalues）
    // --- 寄存器式返回目标（call_reg 设置；栈式 call() 走 ret_frame = -1）---
    //  ret_frame >= 0：结果写回 vm.frames[ret_frame].locals[ret_reg ...]（寄存器式 OP_CALL）
    //  ret_frame <  0：宿主调用（vm_call_value），结果压回 vm.stack[stack_base]
    //  用「帧索引 + 寄存器号」而不是裸指针：调用期间 vm.frames 可能 realloc，
    //  或调用方帧的 inline_locals 被搬移，裸指针会失效（写回时踩坏内存）。
    int ret_frame;
    int ret_reg;         // 结果起始寄存器号（调用方）
    int ret_want;        // 期望结果个数；-1 = 1 个；0 = 丢弃
} CallFrame;

// ============================================================================
// 帧复用：try 字段有效性守护宏
//
// 性能优化（结构短板 B）：call() 仅在 function->has_try 时才初始化帧的 7 个
// try 字段（catch_ip/finally_ip/prev_catch_ip/prev_finally_ip/in_finally/
// try_return_value/has_try_return，~50B/次），无 try 的函数（绝大多数）跳过。
// 帧槽（vm.frames[]）被复用，跳过初始化时这些字段是上一次调用的脏值，
// 读取前必须用本宏守护，否则脏的 catch_ip/finally_ip 会导致异常误捕获、
// OP_RETURN 误跳 finally、GC 扫描脏 try_return_value 而崩溃。
//
// 字段有效的两种情况：
// - 主帧 / 线程帧：vm_load / vm_init 显式初始化（closure 或 function 为 NULL）
// - 函数帧：function->has_try == 1 时 call() 初始化
// ============================================================================
#define FRAME_TRY_VALID(frame) \
    ((frame)->closure == NULL || (frame)->closure->function == NULL || \
     (frame)->closure->function->has_try)

// ============================================================================
// 模块帧 - 模块执行时的上下文
// ============================================================================

typedef struct ModuleFrame {
    ObjModule* module;    // 所属模块
    Value* globals;       // 模块全局变量数组
    int global_count;     // 全局变量数量
    int global_capacity;  // 全局变量数组容量
    struct ModuleFrame* parent;  // 父模块帧（用于嵌套模块）
} ModuleFrame;

// ============================================================================
// 作用域和符号表（前向声明）
// ============================================================================

typedef struct Symbol Symbol;
typedef struct Scope Scope;

struct Symbol {
    SymKind kind;
    char* name;
    int index;
    int is_captured;
    int is_in_loop;           // 变量是否定义在循环作用域中
    int is_initialized;       // 变量是否已初始化
    int is_const;             // 是否为 const 声明（不可重新赋值）
    Scope* scope;
    Symbol* next;
    TypeInfo* type;
    // 类型定义符号的"来源声明"（仅当该符号由某条 struct/cstruct/enum/face 声明注册时非空）。
    // 用途：把「预注册占位 + 主阶段复用同一条声明」与「同名同种类的第二条声明」区分开 ——
    // 前者必须静默复用，后者必须报重复定义（见 visit_type_def.inc / visit_ffi.inc /
    // visit_enum.inc 的复用路径）。由 use 导入等非声明途径建立的符号该字段为 NULL，
    // 沿用旧的静默复用行为，避免把跨模块导入的同名类型误判为重复定义。
    const void* type_decl_ast;
    // 字典键集合（仅当类型为 dict 时使用）
    char** dict_keys;
    int dict_key_count;
    int dict_key_capacity;
    // struct 字段信息（仅当类型为 struct 时使用）
    char** struct_field_names;
    TypeInfo** struct_field_types;
    int struct_field_count;
    char* struct_type_name;     // struct 类型名称（如 "Point"）
    int* struct_field_null_default;  // 字段默认值是否为 null（用于空指针检查）
    // 泛型 struct 类型参数（仅当 struct 有泛型参数时使用）
    char** struct_type_params;       // 类型参数名数组（如 ["T", "U"]）
    int struct_type_param_count;     // 类型参数数量
    // cstruct 布局信息（仅当类型为 cstruct 时使用）
    int cstruct_size;           // cstruct 总大小
    int cstruct_alignment;      // cstruct 对齐要求
    int* cstruct_field_offsets; // cstruct 字段偏移量数组
    bool cstruct_is_packed;     // cstruct 是否 packed（取消所有字段间 padding）
    int  cstruct_explicit_align; // cstruct 显式对齐要求（0 = 未指定）
    // clib 函数签名信息（仅当类型为 clib 时使用）
    char** clib_func_names;         // 函数名称数组
    TypeInfo** clib_func_return_types; // 函数返回类型数组
    int* clib_func_param_counts;    // 函数参数数量数组
    TypeInfo*** clib_func_param_types; // 函数参数类型数组（二维）
    int clib_func_count;            // 函数数量
    // cfunc 回调签名信息（仅当类型为 cfunc 时使用）
    TypeInfo** cfunc_param_types;   // 参数类型数组
    int cfunc_param_count;          // 参数数量
    TypeInfo* cfunc_return_type;    // 返回类型
    // enum 成员信息（仅当类型为 enum 时使用，use 导入的 enum）
    char** enum_value_names;        // 成员名称数组
    int64_t* enum_values;           // 成员值数组（int64_t，支持 >= 2^31 的位标志值）
    int enum_value_count;           // 成员数量
    // 函数别名信息（仅当 kind == SYM_FUNC_ALIAS 时使用）
    char* alias_module_name;        // 源模块名（如 "core"）
    char* alias_func_name;          // 源函数名（如 "loadTexture"）
};

struct Scope {
    Scope* parent;
    Symbol** syms;            // 动态数组：符号表
    int sym_cnt;
    int sym_capacity;         // 符号表容量
    int is_func;
    int is_loop;              // 作用域是否是循环作用域
    int depth;
    int local_count;
    int param_count;
    int global_var_index;
    int global_func_index;
    
    // 作用域树结构
    Scope** children;         // 子作用域数组
    int child_count;          // 子作用域数量
    int child_capacity;       // 子作用域数组容量
    int id;                   // 作用域唯一标识（用于调试）
};

// 作用域 API
Scope* scope_new(Scope* parent, int is_func);
void scope_free(Scope* scope);
Symbol* scope_define(Scope* s, const char* name, SymKind kind);
Symbol* scope_resolve(Scope* s, const char* name);
Symbol* scope_resolve_local(Scope* s, const char* name);

// 字典键管理 API
void symbol_add_dict_key(Symbol* sym, const char* key);

// 释放"语义阶段临时造的虚拟符号"（不是作用域拥有的符号！）。
// 只给 visit_struct_init.inc 的"模块限定 new a.X()"分支用：那里 calloc 了一份 Symbol 并复制了
// 字段/泛型信息，用完必须整份释放。作用域里的符号请勿用这个函数（由 scope_free 统一管）。
void symbol_free_virtual(Symbol* sym);
int symbol_has_dict_key(Symbol* sym, const char* key);

// ============================================================================
// 作用域树 API
// ============================================================================

// 获取根作用域
Scope* scope_get_root(Scope* s);

// 获取第 n 个子作用域
Scope* scope_get_child(Scope* s, int index);

// 遍历作用域树（前序遍历）
void scope_traverse_preorder(Scope* s, void (*callback)(Scope* scope, void* userdata), void* userdata);

// 遍历作用域树（后序遍历）
void scope_traverse_postorder(Scope* s, void (*callback)(Scope* scope, void* userdata), void* userdata);

// 在作用域树中查找符号（广度优先）
Symbol* scope_resolve_tree_bfs(Scope* root, const char* name);

// 获取作用域的完整路径（用于调试）
char* scope_get_path(Scope* s);

// 打印作用域树（用于调试）
void scope_print_tree(Scope* s, int indent);

// 从父作用域中移除子作用域（不释放子作用域）
// 用于临时作用域，避免递归释放时重复释放
void scope_detach_child(Scope* parent, Scope* child);

// ============================================================================
// 内联缓存 (Inline Caching) - 优化属性访问和方法调用
// ============================================================================
//
// 原理：缓存 receiver 类型到方法的映射，避免每次查找
// 效果：对频繁访问同一类型对象的属性/方法，性能提升 2-5 倍
//

typedef struct {
    int valid;              // 缓存是否有效
    ObjType receiver_type;  // receiver 的对象类型
    uint32_t name_hash;     // 方法名的哈希值（防止不同方法碰撞到同一缓存槽）
    ObjNative* method;      // 缓存的方法指针
} InlineCacheEntry;

// 每个 OP_GET_PROPERTY 指令使用一个缓存槽
// 简单实现：使用固定大小的缓存表，以指令地址的哈希值为索引
#define IC_CACHE_SIZE 4096  // 缓存表大小（必须是 2 的幂），增大以减少冲突

// ============================================================================
// 内联缓存：全局 Native 函数 (OP_GET_NATIVE)
// 缓存 name_hash → ObjNative* 的映射，避免每次调用都线性扫描 native 函数表
// ============================================================================
typedef struct {
    int valid;
    uint32_t name_hash;     // native 函数名的 FNV-1a 哈希
    ObjNative* fn;          // 缓存的 native 函数对象
} InlineNativeCacheEntry;

#define IC_NATIVE_CACHE_SIZE 2048  // 必须是 2 的幂

// ============================================================================
// 内联缓存：模块方法调用 (OP_MODULE_CALL)
// 缓存 hash(module, method) → ModuleMethodMeta* 的映射
// 避免每次调用都做哈希表查找
// 注意：ModuleMethodMeta 在 native.h 中定义，此处用 void* 避免循环依赖
// ============================================================================
typedef struct {
    int valid;
    uint32_t combined_hash;         // hash(module_name, method_name) 组合哈希
    void* meta;                     // 实际类型为 ModuleMethodMeta*（在 native.h 中定义）
} InlineModuleCallCacheEntry;

#define IC_MODULE_CACHE_SIZE 2048   // 必须是 2 的幂

// ============================================================================
// 内联缓存：struct 方法查找 (OP_GET_METHOD) 与 struct 定义查找 (OP_STRUCT_INIT)
// 缓存 (struct def 指针, 方法名哈希) → 方法闭包，避免每次调用线性 strcmp 遍历方法表
// 安全性：ObjStructDef 由 struct_def_mark_all() 作为 GC 根标记（gc.c），
//         def->methods[i].closure 经 def 可达，缓存裸指针不会悬空；
//         Leno GC 为非移动式（晋升仅翻转 generation 标志），指针地址稳定
// ============================================================================
typedef struct {
    int valid;
    ObjStructDef* def;       // receiver 的 struct 定义（精确匹配，含继承语义下的区分）
    uint32_t name_hash;      // 方法名的 FNV-1a 哈希
    uint32_t gen;            // 写入缓存时的 struct 定义表代数（防同名 def 被覆盖后拿到旧方法）
    ObjClosure* closure;     // 缓存的方法闭包（仅缓存预创建闭包，upvalue 方法不缓存）
} InlineMethodCacheEntry;

#define IC_METHOD_CACHE_SIZE 2048  // 必须是 2 的幂

typedef struct {
    int valid;
    uint32_t name_hash;      // struct 名的 FNV-1a 哈希
    uint32_t gen;            // 写入缓存时的 struct 定义表代数（防同名 def 被覆盖后拿到旧 def）
    ObjModule* owner;        // 该次查找解析出的"声明模块"（S2/2b-2）；缓存键必须连它一起比，
                             // 否则同名不同 owner 的两个调用点会互相污染（各自要取自己那份）
    ObjStructDef* def;       // 缓存的 struct 定义
} InlineStructDefCacheEntry;

#define IC_STRUCTDEF_CACHE_SIZE 1024  // 必须是 2 的幂（分配点远少于方法调用点）

// ============================================================================
// 虚拟机
// ============================================================================

// vm_call_value（native 回调脚本函数）路径下可读取的最大返回值个数
// 与 OP_RETURN_MULTI 的 results[16] 上限保持一致
#define VM_MAX_RETURNS 16

typedef struct VM {
    Chunk* chunk;
    uint8_t* ip;
    Value* stack;
    int sp;
    int stack_capacity;
    CallFrame* frames;
    int frame_cnt;
    int frame_capacity;
    Scope* global_scope;
    Upvalue* open_upvalues;
    Value* globals;           // 动态数组：全局变量
    int global_count;
    int global_capacity;      // 全局变量数组容量
    Value* global_funcs;      // 动态数组：全局函数
    int global_func_count;
    int global_func_capacity; // 全局函数数组容量
    // 模块系统
    ModuleFrame* current_module_frame;  // 当前模块帧
    // 异常处理
    Value exception;          // 当前异常值
    int has_exception;        // 是否有未处理的异常
    uint8_t* catch_ip;        // catch 块的指令指针
    int catch_frame;          // catch 块的帧索引
    int pending_exception;    // finally 后需要传播的异常
    int exception_line;       // 异常发生时的行号
    int current_line;         // 当前执行行号（供原生函数使用）
    // 内联缓存
    InlineCacheEntry ic_cache[IC_CACHE_SIZE];           // 属性访问缓存表 (OP_GET_PROPERTY)
    InlineNativeCacheEntry ic_native_cache[IC_NATIVE_CACHE_SIZE];   // 全局 Native 函数缓存 (OP_GET_NATIVE)
    InlineModuleCallCacheEntry ic_module_cache[IC_MODULE_CACHE_SIZE]; // 模块方法缓存 (OP_MODULE_CALL)
    InlineMethodCacheEntry ic_method_cache[IC_METHOD_CACHE_SIZE];   // struct 方法缓存 (OP_GET_METHOD)
    InlineStructDefCacheEntry ic_structdef_cache[IC_STRUCTDEF_CACHE_SIZE]; // struct 定义缓存 (OP_STRUCT_INIT)
    int ic_hits;               // 缓存命中次数（统计用）
    int ic_misses;             // 缓存未命中次数（统计用）
    // 协程系统
    EventLoop* event_loop;     // 事件循环
    ObjCoroutine* current_coroutine;
    ObjCoroutine* all_coroutines;
    int next_coroutine_id;         // 协程 ID 计数器（coroutine_new 递增；asyncs.current() 读）
                                   //   放 VM 上而不是文件级 static：各线程的 VM 都是
                                   //   memset(&vm, 0, sizeof(VM)) 出来的，天然线程安全
    Value last_return_value;
    // 多返回值：vm_call_value（native 回调脚本函数）路径下，OP_RETURN_MULTI 的
    // 全部返回值保存在这里，last_return_count 记录个数（单返回值时恒为 1）。
    // 从前只保留 results[0] 到 last_return_value，其余返回值全部丢失。
    // 注意：这里刻意不往 VM 栈压值——压栈会改变所有 vm_call_value 调用方
    // （arrays.map / FFI 回调等）看到的栈布局，存在栈漂移风险。
    Value last_return_values[VM_MAX_RETURNS];
    int last_return_count;
    // 线程系统
    ObjThread** active_threads;  // 活动线程列表
    int active_thread_count;
    int active_thread_capacity;
    int stop_frame_cnt;          // 回调执行时：帧数降到此值时停止
    // 泛型类型参数传递：OP_PUSH_TYPE_ARGS → OP_CALL 间的桥接
    char** pending_type_args;    // 待处理的泛型类型参数
    int pending_type_arg_count;  // 待处理数量
    int gc_return_counter;       // OP_RETURN 的 GC 检查计数器（放在 VM 结构体中避免 THREAD_LOCAL 开销）
    int gc_force_request;        // 测试钩子：LENO_GC_FORCE_EVERY 到点后在下一个安全点强制回收一次（§8.35）
} VM;

// 主线程使用全局 VM（效率第一）
extern VM vm;
extern int vm_initialized;

// 当前执行线程使用的 VM 指针（子线程使用独立的 VM）
extern THREAD_LOCAL VM* current_exec_vm;

// VM API
void vm_init(void);
void vm_init_with_scope(Scope* global_scope);
void vm_load(Chunk* c);
int vm_run(void);
int vm_run_chunk(Chunk* chunk);
int vm_run_with_vm(VM* vm_ptr);
// 读取 main 函数返回值作为进程退出码（int → 退出码，其他 → 0）
int vm_get_exit_code(void);
void vm_reset_stack(void);
void vm_free(void);
int vm_call_value(Value callee, int arg_count, int line);
bool vm_get_global(ObjString* name, Value* out);

// ============================================================================
// 栈操作（static inline 供编译器内联优化）
// ============================================================================

// 动态扩容辅助函数（内部使用）
static inline int vm_grow_capacity(int old_capacity) {
    return old_capacity < 8 ? 8 : old_capacity * 2;
}

// 栈操作：压入值（带 VM* 参数）
static inline void vm_stack_push(VM* vm, Value v) {
    if (__builtin_expect(vm->sp >= vm->stack_capacity, 0)) {
        int new_capacity = vm_grow_capacity(vm->stack_capacity);
        Value* new_stack = (Value*)realloc(vm->stack, new_capacity * sizeof(Value));
        if (!new_stack) return;
        vm->stack = new_stack;
        vm->stack_capacity = new_capacity;
    }
    vm->stack[vm->sp++] = v;
}

// 栈操作：压入值（不检查溢出，用于性能关键路径）
// 警告：只在确定栈有空间时使用，否则会导致未定义行为
static inline void vm_stack_push_fast(VM* vm, Value v) {
    vm->stack[vm->sp++] = v;
}

// 栈操作：弹出值（带 VM* 参数）
static inline Value vm_stack_pop(VM* vm) {
    if (__builtin_expect(vm->sp <= 0, 0)) {
        fprintf(stderr, "\n[VM 栈下溢] sp=%d, frame_cnt=%d\n", vm->sp, vm->frame_cnt);
        return val_null();
    }
    return vm->stack[--vm->sp];
}

// 栈操作：弹出值（不检查下溢，用于性能关键路径）
// 警告：只在确定栈有值时使用，否则会导致未定义行为
static inline Value vm_stack_pop_fast(VM* vm) {
    return vm->stack[--vm->sp];
}

// 栈操作：查看栈顶值（带 VM* 参数）
static inline Value vm_stack_peek(VM* vm, int distance) {
    if (__builtin_expect(vm->sp - 1 - distance < 0, 0)) {
        return val_null();
    }
    return vm->stack[vm->sp - 1 - distance];
}

// 栈操作：查看栈顶值（不检查边界，用于性能关键路径）
// 警告：只在确定栈有足够值时使用，否则会导致未定义行为
static inline Value vm_stack_peek_fast(VM* vm, int distance) {
    return vm->stack[vm->sp - 1 - distance];
}

// Native 函数注册
// min_arity/max_arity: 当 arity == -1（可变参数）时，指定最小/最大允许参数个数；其他情况传 -1
void vm_register_native(const char* name, NativeFn function, int arity, int min_arity, int max_arity, TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);
TypeKind vm_get_native_return_type(const char* name);

// IO 模块初始化
void io_init_globals(void);

// ============================================================================
// 模块系统 API
// ============================================================================

// 创建模块对象
ObjModule* module_new(const char* name);

// 创建模块帧
ModuleFrame* module_frame_new(ObjModule* module);

// 进入模块帧
void module_frame_enter(ModuleFrame* frame);

// 退出模块帧
void module_frame_exit(void);

// 当前模块帧所属的模块（不在模块 init 中时返回 NULL）
// 供 OP_DEFINE_* 记录类型定义的"声明来源"（跨模块同名检测，见 docs 的 S2）
ObjModule* module_frame_current_module(void);

// 获取当前模块帧中的变量
Value module_get_var(int index);

// 设置当前模块帧中的变量
void module_set_var(int index, Value value);

// 确保模块变量数组容量
int module_ensure_var_capacity(int slot);

// 模块函数定义
void module_define_func(int index, Value func);

// 获取模块函数
Value module_get_func(int index);

// ============================================================================
// 调试功能（VM 独立运行时不需要，通过 LENO_VM_ONLY 条件编译排除）
// ============================================================================

#ifndef LENO_VM_ONLY
int disassembleInstruction(Chunk* chunk, int offset);
void disassembleChunk(Chunk* chunk, const char* name);
void debugPrintStack(Value* stack, int sp);
#endif // LENO_VM_ONLY

// ============================================================================
// 协程系统 API
// ============================================================================

// 获取当前正在执行的协程（NULL 表示主线程）
ObjCoroutine* vm_current_coroutine(void);

// 检查当前是否在 async 函数中
int vm_in_async_context(void);

// 挂起当前协程，等待 Future
void vm_suspend_coroutine(ObjFuture* future);

// 恢复协程执行
void vm_resume_coroutine(ObjCoroutine* co, Value result);

// 使用指定的 VM 执行协程（供子线程使用）
int vm_run_coroutine_with_vm(ObjCoroutine* co, VM* vm_ptr);

// 包装函数：主线程使用全局 vm
int vm_run_coroutine(ObjCoroutine* co);

#endif // LENO_VM_H
