// ============================================================================
// bytes —— 二进制缓冲的定宽读写（小端为主，另给大端；越界一律抛错）
// ============================================================================
// 为什么有这个模块（2026-11-28）
// ----------------------------------------------------------------------------
// `_u8/_u16/_u32` 这类 helper 此前在多个模块里各抄一份：
//   LenoZip/lib/zip_read.leno   (Array[int] bs, o)   读
//   LenoZip/lib/zip_write.leno  (Array[int] out, v)  **写**（同名不同义）
//   LenoXLSX/lib/xls_read.leno  (Array[int] b, o)    读
//   android/tools/*.leno        (string s, o)        读（走 strings.byte）
// 抄漏一处不会报错，只表现为"某个文件解析失败/某列数字不对"。
//
// 更麻烦的是边界口径三套并存：
//   · 手抄版        ⇒ 越界**静默返回 0**（脏数据当 0 继续算，错位不报错）
//   · strings.byte  ⇒ 越界返回 **null**（进 `|` 就炸，且报错离现场很远）
//   · 数组下标 b[o] ⇒ 越界**抛错**
// 本模块统一成**抛错**（与数组下标同口径）；要容错就先 `bytes.check(x, o, n)` 判一次，
// 不要指望"读坏了给个 0"——那正是最难查的一类解析 bug。
//
// 载体两种都吃（运行期按类型分派，不需要调用方转换）：
//   · string     —— 推荐。`files.read` / `ffi.read_bytes` / `ole.stream` 直接就是它，
//                   **不用再 to_bytes()**（22KB 的 .xls 转一次就要 60µs）
//   · Array[int] —— zip/xls 这类"边解析边 push 输出"的代码本来就是这个形状，不必改
//
// 性能口径（实测，同一台机）
//   · string 载体：一次 native 调用读 4 字节 ≈ 比 `strings.byte` 连调 4 次快 3~4 倍
//   · Array[int] 载体：与"手写内联下标"基本持平（数组下标本来就被 VM 内联了）
//   ⇒ 热循环里 Array[int] 的读**可以继续内联**，本模块的价值在"少抄 + 口径统一 +
//     string 载体不用整块 to_bytes"。
// ============================================================================

#include "include/native.h"
#include "include/leno_value.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ==================== 载体访问 ====================

// 字节缓冲长度；不是 string / Array 时返回 -1（已抛错）
static int bytes_len_of(Value v) {
    if (val_is_string(v)) return ((ObjString*)val_as_obj(v))->len;
    if (val_is_obj(v) && val_as_obj(v)->type == OBJ_ARRAY) {
        return ((ObjArray*)val_as_obj(v))->count;
    }
    return -1;
}

// 取 [off, off+n) 的 n 个字节到 out。失败（元素不是合法字节）返回 0，已抛错。
//   调用方保证范围已判过 ⇒ 这里不做边界判断（省掉每次读的两三次比较）。
static int bytes_fetch(Value v, int off, int n, unsigned char* out) {
    if (val_is_string(v)) {
        ObjString* s = (ObjString*)val_as_obj(v);
        memcpy(out, s->chars + off, (size_t)n);
        return 1;
    }
    ObjArray* a = (ObjArray*)val_as_obj(v);
    for (int i = 0; i < n; i++) {
        Value e = a->elements[off + i];
        if (!val_is_int(e)) {
            native_throw_error("bytes: 字节数组的元素必须是 int（0~255）");
            return 0;
        }
        int64_t x = val_as_int(e);
        if (x < 0 || x > 255) {
            native_throw_error("bytes: 字节值越界（Array[int] 里的每个元素必须是 0~255）");
            return 0;
        }
        out[i] = (unsigned char)x;
    }
    return 1;
}

// ==================== 读 ====================

typedef enum {
    BK_U8, BK_U16, BK_U32, BK_U64,
    BK_I16, BK_I32, BK_I64,
    BK_F32, BK_F64
} BytesKind;

static int kind_width(BytesKind k) {
    switch (k) {
    case BK_U8:  return 1;
    case BK_U16: case BK_I16: return 2;
    case BK_U32: case BK_I32: case BK_F32: return 4;
    default:     return 8;   // U64 / I64 / F64
    }
}

// 三种失败都**抛错**（不返回默认值）：载体类型错、偏移不是 int、范围越界
static Value bytes_read_impl(Value* args, BytesKind kind, int big_endian, const char* who) {
    const int width = kind_width(kind);

    int len = bytes_len_of(args[0]);
    if (len < 0) {
        native_throw_error("bytes: 第一个参数必须是 string 或 Array[int]（字节缓冲）");
        return val_null();
    }
    if (!val_is_int(args[1])) {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s: 偏移必须是 int", who);
        native_throw_error(msg);
        return val_null();
    }

    int64_t off = val_as_int(args[1]);
    if (off < 0 || off + (int64_t)width > (int64_t)len) {
        char msg[192];
        snprintf(msg, sizeof(msg), "%s: 偏移越界（offset=%lld, 需要 %d 字节, 缓冲长度 %d）",
                 who, (long long)off, width, len);
        native_throw_error(msg);
        return val_null();
    }

    unsigned char b[8];
    if (!bytes_fetch(args[0], (int)off, width, b)) return val_null();

    // 先拼成"逻辑位模式"（与主机字节序无关）：小端时 b[0] 是最低位
    uint64_t u = 0;
    if (big_endian) {
        for (int i = 0; i < width; i++) u = (u << 8) | (uint64_t)b[i];
    } else {
        for (int i = width - 1; i >= 0; i--) u = (u << 8) | (uint64_t)b[i];
    }

    switch (kind) {
    case BK_U8:  return val_int((int64_t)u);
    case BK_U16: return val_int((int64_t)u);
    case BK_U32: return val_int((int64_t)u);
    // u64 可能超出 int48 ⇒ 交给 val_int_safe（超出走装箱表示，不静默截断）
    case BK_U64: return val_int_safe((int64_t)u);
    case BK_I16: return val_int((int64_t)(int16_t)(uint16_t)u);
    case BK_I32: return val_int((int64_t)(int32_t)(uint32_t)u);
    case BK_I64: return val_int_safe((int64_t)u);
    case BK_F32: {
        uint32_t bits = (uint32_t)u;
        float f;
        memcpy(&f, &bits, sizeof(f));
        return val_float((double)f);
    }
    case BK_F64: {
        uint64_t bits = u;
        double d;
        memcpy(&d, &bits, sizeof(d));
        return val_float(d);
    }
    }
    return val_null();
}

#define BYTES_READER(fn, kind, be, label)                     \
    static Value fn(int argc, Value* args) {                  \
        (void)argc;                                           \
        return bytes_read_impl(args, kind, be, label);         \
    }

// ⚠ 名字里不能直接用 `u8/u16/u32/u64/i16/...`：这些是**语言保留字**（C 布局类型名），
//   写成 `bytes.u32(...)` 会直接报"保留字不能用作方法名"。所以读一律 `read_*`，
//   写一律 `write_*`（与 ffi 的 `read_bytes/write_bytes` 同一套动词）。
BYTES_READER(bytes_u8,    BK_U8,  0, "bytes.read_u8")
BYTES_READER(bytes_u16,   BK_U16, 0, "bytes.read_u16")
BYTES_READER(bytes_u32,   BK_U32, 0, "bytes.read_u32")
BYTES_READER(bytes_u64,   BK_U64, 0, "bytes.read_u64")
BYTES_READER(bytes_i16,   BK_I16, 0, "bytes.read_i16")
BYTES_READER(bytes_i32,   BK_I32, 0, "bytes.read_i32")
BYTES_READER(bytes_i64,   BK_I64, 0, "bytes.read_i64")
BYTES_READER(bytes_f32,   BK_F32, 0, "bytes.read_f32")
BYTES_READER(bytes_f64,   BK_F64, 0, "bytes.read_f64")
BYTES_READER(bytes_u16be, BK_U16, 1, "bytes.read_u16be")
BYTES_READER(bytes_u32be, BK_U32, 1, "bytes.read_u32be")
BYTES_READER(bytes_u64be, BK_U64, 1, "bytes.read_u64be")

// ==================== 写（追加到 Array[int]） ====================

static Value bytes_put_impl(Value* args, int width, int big_endian, const char* who) {
    Value outv = args[0];
    if (!(val_is_obj(outv) && val_as_obj(outv)->type == OBJ_ARRAY)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s: 第一个参数必须是 Array[int]（要写入的缓冲）", who);
        native_throw_error(msg);
        return val_null();
    }
    // const 深保护的运行期那一半：只读对象拒绝原地修改（与数组 add 同一个宏口径）
    if (obj_is_immutable(outv)) {
        native_throw_error(immutable_msg(outv,
            "const 数组不可修改（const 是**整体不可变**：需要可变数组请声明为 var）"));
        return val_null();
    }
    // 值必须是 int（对外只有 int 一种整数：超过 int48 的值运行期是装箱表示，
    //   但语言层面仍是 int ⇒ 这里两种表示都收，报错文案只说 int）
    int64_t sv = 0;
    if (val_is_int(args[1])) {
        sv = val_as_int(args[1]);
    } else if (val_is_bigint(args[1])) {
        // 超过 64 位的值只取低 64 位（按位截断，与 C 的 (uint64_t) 强转同口径）
        sv = bigint_to_int64((ObjBigInt*)val_as_obj(args[1]));
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s: 值必须是 int", who);
        native_throw_error(msg);
        return val_null();
    }

    uint64_t v = (uint64_t)sv;
    ObjArray* arr = (ObjArray*)val_as_obj(outv);
    for (int i = 0; i < width; i++) {
        int shift = big_endian ? (width - 1 - i) * 8 : i * 8;
        unsigned char byte = (unsigned char)((v >> shift) & 0xFF);
        if (arr->count >= arr->capacity) {
            if (!arr_grow(arr)) {
                native_throw_error("bytes: 数组扩容失败（内存不足或元素数达上限）");
                return val_null();
            }
        }
        // int 是立即值、不指向堆 ⇒ 不需要写屏障
        arr->elements[arr->count++] = val_int((int64_t)byte);
    }
    return val_null();
}

#define BYTES_WRITER(fn, width, be, label)                    \
    static Value fn(int argc, Value* args) {                  \
        (void)argc;                                           \
        return bytes_put_impl(args, width, be, label);         \
    }

BYTES_WRITER(bytes_put_u8,     1, 0, "bytes.write_u8")
BYTES_WRITER(bytes_put_u16,    2, 0, "bytes.write_u16")
BYTES_WRITER(bytes_put_u32,    4, 0, "bytes.write_u32")
BYTES_WRITER(bytes_put_u64,    8, 0, "bytes.write_u64")
BYTES_WRITER(bytes_put_u16be,  2, 1, "bytes.write_u16be")
BYTES_WRITER(bytes_put_u32be,  4, 1, "bytes.write_u32be")
BYTES_WRITER(bytes_put_u64be,  8, 1, "bytes.write_u64be")

// ==================== 辅助 ====================

// bytes.len(x)：字节长度。string 用**字节数**（不是字符数），Array[int] 用元素数
static Value bytes_len_func(int argc, Value* args) {
    (void)argc;
    int n = bytes_len_of(args[0]);
    if (n < 0) {
        native_throw_error("bytes.len: 参数必须是 string 或 Array[int]（字节缓冲）");
        return val_null();
    }
    return val_int((int64_t)n);
}

// bytes.check(x, o, n)：`o` 起 `n` 个字节是否都落在缓冲内（不抛错的窄口）。
//   用途是"先判再读"：解析器遇到**可能截断**的记录时，一次判定比 try/catch 便宜得多。
static Value bytes_check_func(int argc, Value* args) {
    (void)argc;
    int len = bytes_len_of(args[0]);
    if (len < 0) {
        native_throw_error("bytes.check: 第一个参数必须是 string 或 Array[int]（字节缓冲）");
        return val_null();
    }
    if (!val_is_int(args[1]) || !val_is_int(args[2])) {
        native_throw_error("bytes.check: 偏移与长度必须是 int");
        return val_null();
    }
    int64_t off = val_as_int(args[1]);
    int64_t cnt = val_as_int(args[2]);
    if (off < 0 || cnt < 0) return val_bool(0);
    // 写成减法避免 off + cnt 溢出
    return val_bool(cnt <= (int64_t)len - off ? 1 : 0);
}

// ==================== 初始化 ====================

void bytes_init_module(void) {
    TypeKind read_params[] = { TYPE_ANY, TYPE_INT };            // x, offset
    TypeKind put_params[]  = { TYPE_ANY, TYPE_INT };            // out, value
    TypeKind len_params[]  = { TYPE_ANY };                      // x
    TypeKind check_params[] = { TYPE_ANY, TYPE_INT, TYPE_INT }; // x, offset, n

    // 小端读（与 zip/biff 一致）
    native_register_module_method("bytes", "read_u8",  bytes_u8,  &NATIVE_T_INT, NATIVE_FIXED(read_params));
    native_register_module_method("bytes", "read_u16", bytes_u16, &NATIVE_T_INT, NATIVE_FIXED(read_params));
    native_register_module_method("bytes", "read_u32", bytes_u32, &NATIVE_T_INT, NATIVE_FIXED(read_params));
    native_register_module_method("bytes", "read_u64", bytes_u64, &NATIVE_T_INT, NATIVE_FIXED(read_params));
    native_register_module_method("bytes", "read_i16", bytes_i16, &NATIVE_T_INT, NATIVE_FIXED(read_params));
    native_register_module_method("bytes", "read_i32", bytes_i32, &NATIVE_T_INT, NATIVE_FIXED(read_params));
    native_register_module_method("bytes", "read_i64", bytes_i64, &NATIVE_T_INT, NATIVE_FIXED(read_params));
    native_register_module_method("bytes", "read_f32", bytes_f32, &NATIVE_T_FLOAT, NATIVE_FIXED(read_params));
    native_register_module_method("bytes", "read_f64", bytes_f64, &NATIVE_T_FLOAT, NATIVE_FIXED(read_params));

    // 大端读（网络字节序 / PNG / 部分字体格式）
    native_register_module_method("bytes", "read_u16be", bytes_u16be, &NATIVE_T_INT, NATIVE_FIXED(read_params));
    native_register_module_method("bytes", "read_u32be", bytes_u32be, &NATIVE_T_INT, NATIVE_FIXED(read_params));
    native_register_module_method("bytes", "read_u64be", bytes_u64be, &NATIVE_T_INT, NATIVE_FIXED(read_params));

    // 写：追加到 Array[int]（对应 zip_write 那份 helper）
    native_register_module_method("bytes", "write_u8",    bytes_put_u8,    &NATIVE_T_NULL, NATIVE_FIXED(put_params));
    native_register_module_method("bytes", "write_u16",   bytes_put_u16,   &NATIVE_T_NULL, NATIVE_FIXED(put_params));
    native_register_module_method("bytes", "write_u32",   bytes_put_u32,   &NATIVE_T_NULL, NATIVE_FIXED(put_params));
    native_register_module_method("bytes", "write_u64",   bytes_put_u64,   &NATIVE_T_NULL, NATIVE_FIXED(put_params));
    native_register_module_method("bytes", "write_u16be", bytes_put_u16be, &NATIVE_T_NULL, NATIVE_FIXED(put_params));
    native_register_module_method("bytes", "write_u32be", bytes_put_u32be, &NATIVE_T_NULL, NATIVE_FIXED(put_params));
    native_register_module_method("bytes", "write_u64be", bytes_put_u64be, &NATIVE_T_NULL, NATIVE_FIXED(put_params));

    // 辅助
    native_register_module_method("bytes", "len",   bytes_len_func,   &NATIVE_T_INT,  NATIVE_FIXED(len_params));
    native_register_module_method("bytes", "check", bytes_check_func, &NATIVE_T_BOOL, NATIVE_FIXED(check_params));
}
