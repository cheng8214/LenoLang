# Leno C Layout Struct 设计文档

> 版本: 1.0  
> 日期: 2026-05-16  
> 状态: 设计阶段

---

## 1. 概述

### 1.1 背景

当前 Leno 的 FFI 模块需要手动计算内存偏移来操作 C 结构体：

```leno
// 当前方式：手动计算偏移，容易出错
var ptr = ffi.malloc(16)
ffi.write_int(ptr, 0, 10)       // offset 0: int x
ffi.write_int(ptr, 4, 20)       // offset 4: int y
ffi.write_double(ptr, 8, 3.14)  // offset 8: double z (需对齐到 8)
ffi.call(lib, "process", ptr)
ffi.free(ptr)
```

这种方式存在以下问题：
- 手动计算偏移容易出错
- 不了解 C 对齐规则的开发者难以正确使用
- 跨平台时对齐规则不同，代码难以维护
- 嵌套结构体的偏移计算非常复杂

### 1.2 目标

通过独立的 `cstruct` 关键字，定义与 C ABI 兼容的结构体，自动计算内存布局，实现与 C 结构体的二进制兼容。

```leno
// 目标方式：自动计算偏移，安全可靠
cstruct Vec3 {
    f32 x
    f32 y
    f32 z
}

var v = Vec3.malloc()
v.x = 1.0
v.y = 2.0
v.z = 3.0
ffi.call(lib, "draw", v.to_ptr())
v.free()
```

### 1.3 设计原则

1. **二进制兼容** — 内存布局与目标平台的 C 编译器完全一致
2. **跨平台** — 正确处理 ILP32 / LP64 / LLP64 数据模型差异
3. **零开销抽象** — 字段访问直接映射为内存读写，无额外开销
4. **类型安全** — 仅允许 C 兼容的基本类型作为字段
5. **独立实现** — `cstruct` 与普通 `struct` 完全独立，不互相干扰

---

## 2. C 数据模型与跨平台类型

### 2.1 三种主流数据模型

| 类型 | ILP32 (32位) | LP64 (64位 Unix/macOS) | LLP64 (64位 Windows) |
|------|-------------|----------------------|---------------------|
| `char` | 1 | 1 | 1 |
| `short` | 2 | 2 | 2 |
| `int` | 4 | 4 | 4 |
| `long` | 4 | **8** | **4** |
| `long long` | 8 | 8 | 8 |
| `float` | 4 | 4 | 4 |
| `double` | 8 | 8 | 8 |
| `void*` | 4 | **8** | **8** |
| `size_t` | 4 | **8** | **8** |
| `intptr_t` | 4 | **8** | **8** |

**关键差异：**
- `long` 在 Linux/macOS 是 8 字节，在 Windows 是 4 字节
- 指针在 32 位是 4 字节，64 位是 8 字节

### 2.2 Leno C 类型关键字

为了避免跨平台歧义，Leno 提供明确的、大小固定的类型关键字：

| Leno 类型 | C 等价类型 | 大小 (字节) | 对齐 (字节) | 说明 |
|-----------|-----------|------------|------------|------|
| `i8` | `int8_t` | 1 | 1 | 有符号 8 位整数 |
| `u8` | `uint8_t` | 1 | 1 | 无符号 8 位整数 |
| `i16` | `int16_t` | 2 | 2 | 有符号 16 位整数 |
| `u16` | `uint16_t` | 2 | 2 | 无符号 16 位整数 |
| `i32` | `int32_t` | 4 | 4 | 有符号 32 位整数 |
| `u32` | `uint32_t` | 4 | 4 | 无符号 32 位整数 |
| `i64` | `int64_t` | 8 | 8 | 有符号 64 位整数 |
| `u64` | `uint64_t` | 8 | 8 | 无符号 64 位整数 |
| `f32` | `float` | 4 | 4 | 32 位浮点数 |
| `f64` | `double` | 8 | 8 | 64 位浮点数 |
| `bool` | `bool` (C99) | 1 | 1 | 布尔值 |
| `Ptr` | `void*` | 平台相关 | 平台相关 | 指针 (4 或 8) |
| `c_int` | `int` | 4 | 4 | C 默认 int |
| `c_uint` | `unsigned int` | 4 | 4 | C 默认 unsigned int |
| `c_long` | `long` | 平台相关 | 平台相关 | C long (4 或 8) |
| `c_ulong` | `unsigned long` | 平台相关 | 平台相关 | C unsigned long |
| `c_longlong` | `long long` | 8 | 8 | C long long |
| `c_ulonglong` | `unsigned long long` | 8 | 8 | C unsigned long long |
| `c_size` | `size_t` | 平台相关 | 平台相关 | C size_t |
| `c_ssize` | `ssize_t` | 平台相关 | 平台相关 | C ssize_t |

### 2.3 类型分类

**固定大小类型（跨平台一致）：**
```
i8, u8, i16, u16, i32, u32, i64, u64, f32, f64, bool
```

**平台相关类型（大小随平台变化）：**
```
Ptr, c_long, c_ulong, c_longlong, c_ulonglong, c_size, c_ssize
```

**嵌套类型：**
```
其他 cstruct
```

### 2.4 平台相关类型的运行时值

```leno
// 编译期常量 — 反映当前平台的类型大小
c_int.size       // 4
c_long.size      // 8 (Linux/macOS) 或 4 (Windows)
Ptr.size         // 8 (64位) 或 4 (32位)
c_size.size      // 8 (64位) 或 4 (32位)
```

---

## 3. 对齐规则

### 3.1 基本对齐规则

C 结构体的对齐遵循以下规则：

1. **每个字段的对齐要求** = 该字段类型的大小（`sizeof`）
2. **字段的偏移量** = 向上取整到其对齐要求的倍数
3. **结构体的总大小** = 向上取整到其最大字段对齐要求的倍数
4. **结构体的对齐要求** = 其最大字段的对齐要求

### 3.2 对齐计算示例

```leno
cstruct Example1 {
    i8 a         // offset 0, size 1
    // padding 3 (对齐到 i32 的 4 字节边界)
    i32 b        // offset 4, size 4
    // padding 4 (对齐到 f64 的 8 字节边界)
    f64 c        // offset 8, size 8
}
// 总大小: 16, 对齐: 8
```

内存布局：
```
offset:  0  1  2  3  4  5  6  7  8  9  10  11  12  13  14  15
         [a ][pad  ][    b    ][pad          ][       c        ]
```

```leno
cstruct Example2 {
    i8 a         // offset 0, size 1
    i8 b         // offset 1, size 1
    i8 c         // offset 2, size 1
    i8 d         // offset 3, size 1
    i32 e        // offset 4, size 4 (已对齐)
}
// 总大小: 8, 对齐: 4
```

```leno
cstruct Example3 {
    Ptr p        // offset 0, size 8 (64位)
    i32 x        // offset 8, size 4
    // padding 4 (对齐到 Ptr 的 8 字节边界)
    Ptr q        // offset 16, size 8
}
// 总大小: 24, 对齐: 8
```

### 3.3 嵌套结构体对齐

```leno
cstruct Inner {
    f64 x        // offset 0, size 8
    i32 y        // offset 8, size 4
    // padding 4
}
// Inner.size = 16, Inner.align = 8

cstruct Outer {
    i8 a         // offset 0, size 1
    // padding 7 (对齐到 Inner 的 8 字节边界)
    Inner b      // offset 8, size 16
    i32 c        // offset 24, size 4
    // padding 4 (总大小对齐到 8)
}
// 总大小: 32, 对齐: 8
```

### 3.4 数组字段对齐

```leno
cstruct WithArray {
    i32 count    // offset 0, size 4
    // padding 4 (对齐到 f64 的 8 字节边界)
    f64 data[3]  // offset 8, size 24 (3 × 8)
}
// 总大小: 32, 对齐: 8
```

**数组字段的对齐要求 = 数组元素类型的对齐要求。**

---

## 4. 语法设计

### 4.1 定义语法

```leno
cstruct StructName {
    type1 field1
    type2 field2
    type3 field3 = default_value    // 可选默认值
    type4 field4[10]                 // 固定大小数组
    OtherCStruct nested              // 嵌套 cstruct
}
```

### 4.2 字段类型限制

**允许的类型：**

| 类别 | 类型 |
|------|------|
| 固定大小整数 | `i8`, `u8`, `i16`, `u16`, `i32`, `u32`, `i64`, `u64` |
| 浮点数 | `f32`, `f64` |
| 布尔 | `bool` |
| 指针 | `Ptr` |
| C 平台类型 | `c_int`, `c_uint`, `c_long`, `c_ulong`, `c_longlong`, `c_ulonglong`, `c_size`, `c_ssize` |
| 嵌套 cstruct | 其他 `cstruct` |
| 固定数组 | `type[N]`（仅限上述类型） |

**禁止的类型：**

| 类型 | 原因 |
|------|------|
| `int` | 大小不明确，应使用 `i32` 或 `c_int` |
| `float` | 大小不明确，应使用 `f32` |
| `string` | C 中无等价类型，应使用 `Ptr` + `ffi.read_string()` |
| `Array` | 动态数组，C 中无等价类型 |
| `Dict` | C 中无等价类型 |
| 普通 `struct` | 非 C 布局，内存不兼容 |
| `enum` | C enum 大小由编译器决定，不安全 |
| `null` | 无意义 |
| 闭包/函数 | C 中无等价类型 |

### 4.3 与普通 struct 的对比

```leno
// 普通 struct — 应用逻辑，GC 管理
struct Point {
    float x
    float y
    string name = "origin"
    func distance(): float {
        return maths.sqrt(x * x + y * y)
    }
}

// cstruct — FFI 互操作，手动内存管理
cstruct CPoint {
    f64 x
    f64 y
}
```

| 特性 | 普通 `struct` | `cstruct` |
|------|--------------|-----------|
| 字段类型 | 任意 Leno 类型 | 仅 C 兼容类型 |
| 方法 | ✅ 支持 | ❌ 不支持 |
| 默认值 | ✅ 任意表达式 | ✅ 仅常量 |
| 内存管理 | GC 自动 | 手动 (alloc/free) |
| 内存布局 | Leno 内部 | C ABI 对齐 |
| GC 追踪 | ✅ | ❌ |
| 用途 | 应用逻辑 | FFI 互操作 |
| 作为函数参数 | 按引用传递 | 可按指针传递 |

---

## 5. API 设计

### 5.1 编译期常量

```leno
// 结构体大小和对齐信息
Vec3.size        // 12 (3 × f64 = 24, 但 3 × f32 = 12)
Vec3.align       // 4 (f32 的对齐)

// 平台相关类型大小
Ptr.size         // 8 (64位) 或 4 (32位)
c_long.size      // 8 (LP64) 或 4 (LLP64/ILP32)
```

### 5.2 实例方法

```leno
// 分配内存
var v = Vec3.malloc()
// 等同于: ffi.malloc(Vec3.size)
// 返回 Vec3 实例（内部持有 Ptr）

// 从已有指针创建视图（零拷贝）
var v = Vec3.from_ptr(ptr)
// 不分配新内存，直接操作 ptr 指向的内存
// ⚠️ 调用者负责 ptr 的生命周期

// 获取底层指针
var p: Ptr = v.to_ptr()
// 返回内部指针，用于传递给 C 函数

// 释放内存
v.free()
// 等同于: ffi.free(v.to_ptr())
// ⚠️ 仅释放 alloc() 分配的内存
// ⚠️ from_ptr() 创建的实例不应调用 free()

// 获取字段偏移量（调试用）
Vec3.offsetof("x")   // 0
Vec3.offsetof("y")   // 4
Vec3.offsetof("z")   // 8
```

### 5.3 字段访问

```leno
var v = Vec3.malloc()

// 写入字段（自动计算偏移和类型）
v.x = 1.0     // 内部: ffi.write_float(ptr, 0, 1.0)
v.y = 2.0     // 内部: ffi.write_float(ptr, 4, 2.0)
v.z = 3.0     // 内部: ffi.write_float(ptr, 8, 3.0)

// 读取字段
print(v.x)    // 内部: ffi.read_float(ptr, 0)
print(v.y)    // 内部: ffi.read_float(ptr, 4)
print(v.z)    // 内部: ffi.read_float(ptr, 8)
```

### 5.4 数组字段

```leno
cstruct Matrix4 {
    f32 m[16]   // 4×4 矩阵
}

var mat = Matrix4.malloc()
mat.m[0] = 1.0   // 第一个元素
mat.m[15] = 1.0  // 对角线最后一个元素

// 获取数组指针
var m_ptr: Ptr = mat.m.to_ptr()
ffi.call(lib, "mat4_multiply", m_ptr)
```

### 5.5 嵌套结构体

```leno
cstruct Vec2 {
    f32 x
    f32 y
}

cstruct Rect {
    Vec2 origin     // 嵌套 C struct
    Vec2 size
}

var r = Rect.malloc()
r.origin.x = 10.0
r.origin.y = 20.0
r.size.x = 100.0
r.size.y = 200.0

// 获取嵌套结构体的指针
var origin_ptr: Ptr = r.origin.to_ptr()
// 指向 Rect 内部 offset 0 的位置
```

---

## 6. 完整使用示例

### 6.1 Windows RECT 结构体

```c
// C 定义
typedef struct tagRECT {
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
} RECT;
```

```leno
// Leno 定义
cstruct Rect {
    i32 left       // Windows LONG = 32位有符号整数（LLP64 下 long = 4 字节）
    i32 top
    i32 right
    i32 bottom
}
// Rect.size = 16, Rect.align = 4
// ⚠️ 注意：这里用 i32 而非 c_long，因为 Windows API 的 LONG 始终是 32 位
// 如果是跨平台的通用 C long 字段，应使用 c_long 类型

// 使用
var r = Rect.malloc()
r.left = 0
r.top = 0
r.right = 800
r.bottom = 600

var user32 = ffi.load("user32.dll")
ffi.call(user32, "GetWindowRect", hwnd, r.to_ptr())
print("窗口位置: ({r.left}, {r.top}) - ({r.right}, {r.bottom})")
r.free()
```

### 6.2 嵌套结构体 — 链表节点

```c
// C 定义
typedef struct Node {
    int value;
    struct Node* next;
} Node;
```

```leno
// Leno 定义
cstruct Node {
    i32 value
    Ptr next       // struct Node* → Ptr
}
// 64位: Node.size = 16, Node.align = 8
// 32位: Node.size = 8,  Node.align = 4

// 创建链表
var n1 = Node.malloc()
n1.value = 1
n1.next = ffi.nullptr()

var n2 = Node.malloc()
n2.value = 2
n2.next = n1.to_ptr()

var n3 = Node.malloc()
n3.value = 3
n3.next = n2.to_ptr()

// 遍历链表
var current: Ptr = n3.to_ptr()
while current != ffi.nullptr() {
    var node = Node.from_ptr(current)
    print(node.value)
    current = node.next
}

// ⚠️ 注意：释放顺序（反向）
n1.free()
n2.free()
n3.free()
```

### 6.3 包含数组的结构体

```c
// C 定义
typedef struct {
    int count;
    double values[10];
} DataBuffer;
```

```leno
// Leno 定义
cstruct DataBuffer {
    i32 count
    // padding 4 (对齐到 f64 的 8 字节边界)
    f64 values[10]
}
// DataBuffer.size = 88 (4 + 4pad + 80), DataBuffer.align = 8

var buf = DataBuffer.malloc()
buf.count = 3
buf.values[0] = 1.1
buf.values[1] = 2.2
buf.values[2] = 3.3

ffi.call(lib, "process_buffer", buf.to_ptr())
buf.free()
```

### 6.4 跨平台 sockaddr_in

```c
// C 定义
struct sockaddr_in {
    sa_family_t sin_family;
    in_port_t sin_port;
    struct in_addr sin_addr;
    char sin_zero[8];
};
```

```leno
// Leno 定义
cstruct SockAddrIn {
    u16 family       // sa_family_t
    u16 port         // in_port_t (网络字节序)
    u32 addr         // struct in_addr (s_addr)
    u8 zero[8]       // sin_zero[8]
}
// SockAddrIn.size = 16, SockAddrIn.align = 2

var addr = SockAddrIn.malloc()
addr.family = 2        // AF_INET
addr.port = 8080       // ⚠️ 需要手动处理字节序
addr.addr = 0x7F000001 // 127.0.0.1
ffi.call(lib, "connect", sockfd, addr.to_ptr(), SockAddrIn.size)
addr.free()
```

### 6.5 与现有 win_reg 模块对比

```leno
// ===== 之前：手动偏移 =====
var info = ffi.malloc(12)
ffi.write_int(info, 0, 42)       // offset 0
ffi.write_int(info, 4, 100)      // offset 4
ffi.write_float(info, 8, 3.14)   // offset 8
ffi.call(lib, "process", info)
ffi.free(info)

// ===== 之后：cstruct =====
cstruct Info {
    i32 id
    i32 value
    f32 weight
}

var info = Info.malloc()
info.id = 42
info.value = 100
info.weight = 3.14
ffi.call(lib, "process", info.to_ptr())
info.free()
```

---

## 7. 编译器实现

### 7.1 编译期布局计算

在编译期（语义分析阶段），对每个 `cstruct` 计算布局信息：

```c
typedef struct {
    int field_count;
    int total_size;
    int alignment;
    struct {
        char name[64];
        int offset;
        int size;
        int alignment;
        TypeKind type;
    } fields[MAX_C_LAYOUT_FIELDS];
} CLayoutInfo;
```

**布局计算算法：**

```
function calculate_layout(struct_def):
    offset = 0
    max_align = 1
    
    for each field in struct_def.fields:
        field_size = get_type_size(field.type)
        field_align = get_type_align(field.type)
        
        // 对齐偏移量
        offset = align_up(offset, field_align)
        
        field.offset = offset
        max_align = max(max_align, field_align)
        offset += field_size
    
    // 对齐总大小
    total_size = align_up(offset, max_align)
    alignment = max_align
    
    return { total_size, alignment, fields }
```

### 7.2 字段访问的字节码生成

字段访问编译为直接的内存读写操作：

```
// 读取: v.x
OP_LOAD_C_FIELD  v, field_index
// 内部: ptr = v.to_ptr(); result = ffi.read_type(ptr + field.offset)

// 写入: v.x = value
OP_STORE_C_FIELD  v, field_index, value
// 内部: ptr = v.to_ptr(); ffi.write_type(ptr + field.offset, value)
```

### 7.3 类型检查

在语义分析阶段，对 `cstruct` 的字段类型进行严格检查：

```
function validate_cstruct_field(field_type):
    if field_type is i8/u8/i16/u16/i32/u32/i64/u64/f32/f64/bool:
        return VALID
    if field_type is Ptr/c_int/c_uint/c_long/c_ulong/c_longlong/c_ulonglong/c_size/c_ssize:
        return VALID
    if field_type is fixed_array AND element_type is valid C type:
        return VALID
    if field_type is another cstruct:
        return VALID
    return ERROR("cstruct 不允许使用 {field_type} 类型")
```

### 7.4 运行时表示

`cstruct` 实例在运行时的表示：

```c
typedef struct ObjCLayoutStruct {
    Object obj;          // 对象头
    Ptr data_ptr;        // 指向 C 内存的指针
    int is_owner;        // 1 = alloc() 创建（需要 free），0 = from_ptr() 创建
    CLayoutInfo* layout; // 布局信息（编译期生成）
} ObjCLayoutStruct;
```

**注意：** `ObjCLayoutStruct` 本身由 GC 管理，但它持有的 `data_ptr` 指向的 C 内存需要手动释放。

---

## 8. GC 与内存安全

### 8.1 GC 行为

- `cstruct` 实例（`ObjCLayoutStruct`）由 GC 管理
- 但实例持有的 C 内存（`data_ptr`）**不受 GC 管理**
- 当实例被 GC 回收时，如果 `is_owner == 1`，自动调用 `ffi.free(data_ptr)`

### 8.2 所有权规则

```leno
// malloc() — 拥有内存所有权
var v = Vec3.malloc()
// is_owner = 1, GC 回收时自动 free
v.free()  // 也可以手动提前释放

// from_ptr() — 不拥有内存所有权
var v = Vec3.from_ptr(ptr)
// is_owner = 0, GC 回收时不 free
// ⚠️ 调用者负责 ptr 的生命周期
```

### 8.3 安全规则

| 规则 | 说明 |
|------|------|
| `alloc()` 后必须 `free()` | 或依赖 GC 自动释放 |
| `from_ptr()` 不要 `free()` | 指针不属于你 |
| 不要使用已 `free()` 的实例 | use-after-free，未定义行为 |
| 不要跨线程共享 | 当前单线程，无问题 |
| 嵌套 struct 共享内存 | `from_ptr()` 创建的嵌套视图与父实例共享内存 |

### 8.4 实现注意事项

**防止 Double-Free：**
```c
// 在 ObjCLayoutStruct 中维护状态
typedef struct ObjCLayoutStruct {
    Object obj;
    Ptr data_ptr;
    int is_owner;
    int is_freed;       // 新增：标记是否已释放
    CLayoutInfo* layout;
} ObjCLayoutStruct;

// free() 实现
void c_layout_struct_free(ObjCLayoutStruct* s) {
    if (s->is_freed) return;  // 防止重复释放
    if (s->is_owner && s->data_ptr) {
        ffi.free(s->data_ptr);
    }
    s->is_freed = 1;
    s->data_ptr = NULL;
}

// GC finalizer
void c_layout_struct_finalize(ObjCLayoutStruct* s) {
    if (!s->is_freed && s->is_owner) {
        c_layout_struct_free(s);
    }
}
```

**字段访问安全检查：**
```c
// 读取字段前检查是否已释放
Value c_layout_read_field(ObjCLayoutStruct* s, int field_index) {
    if (s->is_freed || s->data_ptr == NULL) {
        runtime_error("访问已释放的 C struct");
        return val_null();
    }
    // ... 正常读取
}
```

---

## 9. 新增 Token 与语法扩展

### 9.1 新增 Token

```
TOK_CSTRUCT          // cstruct
TOK_I8, TOK_U8       // i8, u8
TOK_I16, TOK_U16     // i16, u16
TOK_I32, TOK_U32     // i32, u32
TOK_I64, TOK_U64     // i64, u64
TOK_F32, TOK_F64     // f32, f64
TOK_C_INT            // c_int
TOK_C_UINT           // c_uint
TOK_C_LONG           // c_long
TOK_C_ULONG          // c_ulong
TOK_C_LONGLONG       // c_longlong
TOK_C_ULONGLONG      // c_ulonglong
TOK_C_SIZE           // c_size
TOK_C_SSIZE          // c_ssize
```

### 9.2 新增 OpCode

```
OP_C_STRUCT_ALLOC       // 分配 C struct 内存
OP_C_STRUCT_FROM_PTR    // 从指针创建视图
OP_C_STRUCT_TO_PTR      // 获取底层指针
OP_C_STRUCT_FREE        // 释放内存
OP_LOAD_C_FIELD         // 读取 C struct 字段
OP_STORE_C_FIELD        // 写入 C struct 字段
OP_C_STRUCT_SIZEOF      // 获取结构体大小
OP_C_STRUCT_ALIGNOF     // 获取结构体对齐
OP_C_STRUCT_OFFSETOF    // 获取字段偏移量
```

### 9.3 新增 TypeKind

```c
typedef enum {
    // ... 现有类型 ...
    TYPE_I8, TYPE_U8,
    TYPE_I16, TYPE_U16,
    TYPE_I32, TYPE_U32,
    TYPE_I64, TYPE_U64,
    TYPE_F32, TYPE_F64,
    TYPE_C_INT, TYPE_C_UINT,
    TYPE_C_LONG, TYPE_C_ULONG,
    TYPE_C_LONGLONG, TYPE_C_ULONGLONG,
    TYPE_C_SIZE, TYPE_C_SSIZE,
    TYPE_CSTRUCT,         // cstruct
} TypeKind;
```

---

## 10. 实现计划

### 阶段 1：基础类型支持（2 天）

- [ ] 添加新 Token（`i8`, `u8`, ..., `f32`, `f64`, `c_int`, ...）
- [ ] 添加新 TypeKind
- [ ] 实现类型大小和对齐查询函数
- [ ] 实现跨平台类型大小检测

### 阶段 2：布局计算（2 天）

- [ ] 解析 `cstruct` 关键字
- [ ] 实现编译期布局计算算法
- [ ] 字段类型验证
- [ ] 嵌套 cstruct 支持

### 阶段 3：运行时支持（2 天）

- [ ] `ObjCLayoutStruct` 运行时表示
- [ ] 实现 `alloc()` / `from_ptr()` / `to_ptr()` / `free()`
- [ ] 新增 OpCode 和 VM 实现
- [ ] GC 集成（is_owner 自动释放）

### 阶段 4：字段访问（1 天）

- [ ] `OP_LOAD_C_FIELD` / `OP_STORE_C_FIELD` 实现
- [ ] 数组字段支持
- [ ] 嵌套字段路径访问（`r.origin.x`）

### 阶段 5：测试与文档（1 天）

- [ ] 基本类型测试
- [ ] 嵌套 struct 测试
- [ ] 跨平台验证（Windows/Linux）
- [ ] 更新入门指南

**总计：约 8 天**

---

## 11. 未来扩展

### 11.1 可能的扩展功能

| 功能 | 说明 | 优先级 |
|------|------|--------|
| `#[packed]` | 取消对齐，紧凑排列 | 中（硬件寄存器映射需要） |
| 位域 | `u32 flags : 8` | 低 |
| 联合体 `union` | C union 支持 | 中 |
| 函数指针字段 | `Ptr` 已可替代 | 低 |
| 字节序转换 | 网络字节序处理 | 中 |
| `to_bytes()` / `from_bytes()` | 序列化/反序列化 | 中 |
| `#[c_enum]` | C 枚举支持 | 中 |

### 11.2 字节序转换辅助

许多网络协议和二进制格式需要处理字节序问题：

```leno
// 方案1：内置字节序转换函数
var port_be = htons(8080)    // host to network (16-bit)
var addr_be = htonl(0x7F000001)  // host to network (32-bit)
var port = ntohs(port_be)    // network to host

// 方案2：类型后缀（编译期处理）
var port: u16.be = 8080      // 大端存储
var value: u32.le = 0x12345678  // 小端存储

// 方案3：字段级字节序标注
cstruct SockAddrIn {
    u16 family
    u16.be port       // 大端存储
    u32.be addr       // 大端存储
    u8 zero[8]
}
```

### 11.3 C 枚举支持

许多 C API 使用枚举作为字段类型：

```c
// C 定义
typedef enum {
    COLOR_RED = 0,
    COLOR_GREEN = 1,
    COLOR_BLUE = 2
} Color;

struct Pixel {
    Color color;
    int x, y;
};
```

```leno
// 方案1：映射到固定大小整数
cstruct Pixel {
    i32 color       // C enum 通常是 int 大小
    i32 x
    i32 y
}

// 方案2：专用 c_enum 类型（未来扩展）
#[c_enum]
enum Color {
    RED = 0
    GREEN = 1
    BLUE = 2
}

cstruct Pixel2 {
    Color color     // 编译为 i32
    i32 x
    i32 y
}
```

### 11.4 `packed` 属性

某些硬件寄存器映射需要紧凑布局，取消 padding：

```leno
cstruct packed HardwareReg {
    u8 control      // offset 0
    u8 status       // offset 1 (无 padding)
    u16 data        // offset 2 (无对齐要求)
}
// 总大小: 4 (而非普通布局的 6)
```

### 11.5 与字节码模块的关系

`cstruct` 的布局信息可以嵌入字节码模块（.lc），确保跨编译一致性。

---

## 附录 A：平台相关类型大小速查

### Windows x64 (LLP64)

```
i8=1  u8=1  i16=2  u16=2  i32=4  u32=4  i64=8  u64=8
f32=4  f64=8  bool=1
Ptr=8  c_int=4  c_uint=4  c_long=4  c_ulong=4
c_longlong=8  c_ulonglong=8  c_size=8  c_ssize=8
```

### Linux x64 (LP64)

```
i8=1  u8=1  i16=2  u16=2  i32=4  u32=4  i64=8  u64=8
f32=4  f64=8  bool=1
Ptr=8  c_int=4  c_uint=4  c_long=8  c_ulong=8
c_longlong=8  c_ulonglong=8  c_size=8  c_ssize=8
```

### Linux x86 (ILP32)

```
i8=1  u8=1  i16=2  u16=2  i32=4  u32=4  i64=8  u64=8
f32=4  f64=8  bool=1
Ptr=4  c_int=4  c_uint=4  c_long=4  c_ulong=4
c_longlong=8  c_ulonglong=8  c_size=4  c_ssize=4
```

---

## 附录 B：对齐计算参考

```
align_up(offset, alignment) = (offset + alignment - 1) & ~(alignment - 1)
```

示例：
```
align_up(5, 8) = (5 + 7) & ~7 = 12 & 0xFFFFFFF8 = 8
align_up(8, 8) = (8 + 7) & ~7 = 15 & 0xFFFFFFF8 = 8
align_up(13, 4) = (13 + 3) & ~3 = 16 & 0xFFFFFFFC = 16
```
