# FFI 模块优化方案

> 基于对 `src/module/ffi/ffi.c`、`leno_ffi.h`、`leno_ffi_win64.c`、`ffi_clib.h` 源码的全面审查

## 当前架构概览

FFI 模块核心路径：
1. **库加载** (`ffi.load`) → `LoadLibraryW`/`dlopen`，返回 `ObjFFILibrary`
2. **函数调用** (`ffi.call_*` / `clib`) → `ffi_call_impl` → ~~每次执行 `GetProcAddress`/`dlsym`~~ 先查函数地址缓存 → `ffi_call` → 平台分派 (`ffi_call_win64`/`ffi_call_sysv`/`ffi_call_aapcs`)
3. **参数转换** → int/bigint 窄化（已合并去重）、float 截断、string→char*、cstruct→ptr
4. **返回值展开** → C 窄类型自动升级为 Leno 宽类型（零摩擦）
5. **回调** → JIT trampoline 汇编 → `callback_dispatch` → 线程检测 → 直接执行 / 跨线程编组

## 优化项清单

| 优先级 | 编号 | 项目 | 难度 | 影响 | 状态 |
|--------|------|------|------|------|------|
| P0 | 1 | 符号地址缓存 | 中 | 热路径性能 | ✅ 已完成 |
| P0 | 2 | 混合参数回退路径浮点 bug | 中 | 正确性 | ✅ 已完成 |
| P1 | 3 | 回调数量上限 128 → 256 | 低 | 功能限制 | ✅ 已完成 |
| P1 | 4 | write_bytes/read_bytes 批量读写 | 低 | 易用性 | ✅ 已完成 |
| P1 | 5 | ffi.dlsym 显式符号解析 | 低 | 功能补充 | ✅ 已完成 |
| P1 | 10 | JIT 内联 ffi 定宽读写（`read_byte`/`int8`/`int16`/`uint16`/`int`/`uint` + `write_*`） | 中 | 热路径性能（像素直写 ~-34%） | ✅ 已完成 |
| P2 | 6 | 显式字节序函数 | 低 | 跨平台 | ✅ 已完成 |
| P2 | 7 | cstruct offset_of 方法 | 低 | 易用性 | ✅ 已完成 |
| P3 | 8 | 代码去重：窄化公共函数 | 低 | 可维护性 | ✅ 已完成 |
| P3 | 9 | ffi.call 标记 deprecated | 低 | 代码清理 | ✅ 已完成 |

---

## P0-1：符号地址缓存 ✅

### 问题

`ffi_call_impl`（`ffi.c:534-538`）每次调用都执行 `GetProcAddress`/`dlsym`。`ObjFFILibrary` 结构体（`ffi.c:108-117`）只有 `path`、`freed`、`handle` 三个字段，无缓存。

### 实现

在 `ObjFFILibrary` 中新增函数地址缓存表（线性数组，32 槽位）：

```c
#define FFI_FUNC_CACHE_SIZE 32
#define FFI_FUNC_NAME_MAX 63

typedef struct {
    char name[FFI_FUNC_NAME_MAX + 1];
    void* addr;
    int valid;
} FFIFuncCacheEntry;
```

在 `ObjFFILibrary` 中新增 `func_cache[FFI_FUNC_CACHE_SIZE]` 和 `func_cache_count`。

`ffi_cache_lookup(lib, name)` 线性扫描缓存表查找已缓存的函数地址。
`ffi_cache_insert(lib, name, addr)` 找空槽插入；全满则覆盖第一个（简化 LRU）。

`ffi_call_impl` 和 `ffi_dlsym_func` 都先查缓存，未命中再调 OS 并填入缓存。

### 效果

- 首次调用：与当前相同（查符号 + 缓存）
- 后续调用：直接命中缓存，跳过 `GetProcAddress`/`dlsym`
- 热路径循环中调用同一函数（如音频回调）提升显著

---

## P0-2：混合参数回退路径浮点 bug ✅（2026-10-01 彻底根治）

### 问题（历史）

老实现是"按参数类型组合枚举函数指针 + 全 int64 位模式回退"：当参数超过 4 个且含浮点时，
回退路径把 `double` 的位模式当 `int64_t` 传递 —— 而被调函数按 ABI 在 **XMM** 里等它 ⇒
参数整体错位。代码注释自己承认（当时的 `leno_ffi_win64.c` 第 409-412 行）：

```
⚠ 警告：此回退路径对浮点参数不保证正确！
int64 位模式通过整数寄存器/栈传递，但被调函数期望
浮点参数在 XMM 寄存器中，两者不匹配。
```

### 当时的实现（**已废弃**）

在 `ffi_call_impl` 中、调用 `ffi_call` 之前加检查，命中就 `native_throw_error`：

- `nargs > 5 && total_float > 0` → 出错
- `nargs == 5 && double_count > 0` → 出错
- `nargs == 5 && float_count > 0 && float_count < 4` → 出错（路径 3.5 仅处理 4f32+1ptr）

**它只是"别再静默算错"，并不是修好** —— 用户会被要求改代码来迁就实现
（拆成多次调用 / 打包进 cstruct / 调整参数顺序）✗；而且那段拦截没有平台条件，
**连 Linux 也一起拦了**（SysV 有 XMM0-7，本不该受限 ✗）。

### 现在的实现（2026-10-01）

三平台统一改成 **"按 ABI 精确分类 + 汇编调用桩"**：枚举表、`path 3.5`、位模式回退
**整体删除**，上层那段拦截也随之删除 ✓

| 平台 | 桩 | 分配模型 | 影子空间 |
|---|---|---|---|
| Win64 | `leno_win64_call` | **第 i 个参数占第 i 号槽**（RCX:XMM0 / RDX:XMM1 / R8:XMM2 / R9:XMM3） | 32B，且必须**先压实参再 `sub rsp,32`** |
| SysV x64 | `leno_sysv_call` | 整数/浮点**各自计数**（6 + 8），AL=用掉的向量寄存器数（变参） | 无 |
| AAPCS64 | `leno_aapcs_call` | 整数/浮点**各自计数**（8 + 8） | 无 |

⇒ 任意整数/浮点混合组合都正确。同时收口了两个同源既有缺陷：

- **f32 返回值恒为 0**：前端读 `result.f`，后端却写 `result.d`（同一联合体）✗
- **"全浮点参数 + 整数返回"**：用 `double (*)(...)` 指针调用 ⇒ 从浮点寄存器取返回值，
  而真值在整数返回寄存器 ⇒ 拿到残留浮点垃圾（实测：`i64 f(f64×6)` 稳定返回
  0/6 这类无意义值，给被调方加一行日志就换个数）✗

回归用例：`assert/test_ffi_abi.leno`（38 例 ABI 边界 + 压力，**跨平台**，见该文件头 ✓）

---

## P1-3：回调数量上限 ✅

### 实现

`MAX_FFI_CALLBACKS` 从 128 增大到 256。不引入动态分配，保持简单。

---

## P1-4：write_bytes / read_bytes 批量读写 ✅

### 问题

当前只有逐类型读写（`write_int`、`write_double` 等），处理二进制协议时用户只能循环 + `write_byte`，每次都有 VM→C 调用开销。

### 实现

- `ffi.write_bytes(ptr, offset, str)` — 一次 `memcpy` 写入字符串原始字节（不含 `'\0'`）
- `ffi.read_bytes(ptr, offset, length)` — 一次 `memcpy` 读取并返回字符串

均带 `CHECK_BOUNDS` 边界检查。

---

## P1-5：ffi.dlsym 显式符号解析 ✅

### 问题

用户无法单独解析符号（检查函数是否存在），只能通过 `ffi.call_*` 间接调用。

### 实现

- `ffi.dlsym(lib, name)` → 返回 `Ptr`（函数地址）或 `null`
- 内部也使用函数地址缓存

---

## P1-10：JIT 内联 ffi 定宽读写 ✅

### 问题

`ffi.read_byte` / `ffi.write_byte` 这类「ptr + offset 定宽访存」在像素直写、缓冲区循环里
每像素调用十几次，但在 JIT 里走的是 `OP_MODULE_CALL` 通用 callout（~20ns/次：逐个装箱成
Value 数组 + native 调用 + 异常检查），而它们实际只做一次 1/2/4 字节 memcpy。

### 实现

JIT 后端（`src/jit/backend/x86_64.c` 的 `ffi_inline_specs[]` 表 + `x86_inc/ops_return.inc`
的 `OP_MODULE_CALL` 分支）把定宽读写做成**表驱动内联**，详细设计见
`docs/JIT实现与调试记录.md` §2.6：命中后直接生成 load/store，前置检查
（int48 偏移 → NaN-boxed 对象 → `OBJ_FFI_POINTER` → `!NULL/!freed` → owned 边界）
任一不过就 bailout 交解释器，报错文本与语义不变。

已内联 12 个：`read_byte`、`read_int8`、`read_int16`、`read_uint16`、`read_int`、`read_uint`、
`write_byte`、`write_int8`、`write_int16`、`write_uint16`、`write_int`、`write_uint`。

有意不内联：64 位系列（值域越 int48，需 bigint 堆分配）、`float`/`double`（NaN-box 构造）、
返回对象/字符串/bool 的方法（`read_ptr`/`read_at`/`read_string`/`offset`/`read_bool`）、`copy4`。

### 效果

像素直写风格基准（3,000,000 次迭代 = 15,000,000 次 ffi 调用）：**334ms → 221ms，约 -34%**
（每次调用省下 ~7.5ns）。测试 `assert/test_ffi_inline_widths.leno` 在 JIT 与 `LENO_NO_JIT=1`
两种模式均通过；`read_int`/`write_int` 的机器码经 dump 差分确认未改动。

---

## P2-6：显式字节序函数 ✅

### 问题

`write_*`/`read_*` 都是主机字节序，跨平台二进制交换需要手动 `write_byte` 拼装。

### 实现

新增 8 个显式字节序函数：

| 函数 | 说明 |
|------|------|
| `ffi.write_le_i16(ptr, off, val)` | 小端 16 位写入 |
| `ffi.write_be_i16(ptr, off, val)` | 大端 16 位写入 |
| `ffi.write_le_i32(ptr, off, val)` | 小端 32 位写入 |
| `ffi.write_be_i32(ptr, off, val)` | 大端 32 位写入 |
| `ffi.read_le_i16(ptr, off)` | 小端 16 位读取 |
| `ffi.read_be_i16(ptr, off)` | 大端 16 位读取 |
| `ffi.read_le_i32(ptr, off)` | 小端 32 位读取 |
| `ffi.read_be_i32(ptr, off)` | 大端 32 位读取 |

---

## P2-7：cstruct offset_of 方法 ✅

### 问题

用户无法编程式获取字段偏移，只能看 `debug()` 打印输出。

### 实现

在 `cstructs.c` 中新增 `offset_of(field_name)` 方法：
- 接受字段名字符串参数
- receiver 可以是 `cstruct` 定义或实例
- 返回 `int` 偏移量
- 字段不存在时抛出明确错误

---

## P3-8：代码去重 ✅

### 问题

`ffi_call_impl` 中 int 和 bigint 的窄化逻辑（`ffi.c:573-704`）几乎完全重复，约 130 行。

### 实现

合并为统一分支：

```c
if (val_is_int(arg) || val_is_bigint(arg)) {
    int64_t ival = val_is_int(arg) ? (int64_t)val_as_int(arg) : bigint_to_int64(val_as_bigint(arg));
    // 统一的 switch (param_tk) { ... }
}
```

减少了约 60 行重复代码，并统一了 bigint 的 `TYPE_U64` 负值检查（原来缺失）。

---

## P3-9：ffi.call 标记 deprecated ✅

### 问题

`ffi.call` 和 `ffi.call_int` 做的事情完全一样（都是 `TYPE_I32`），`ffi.call` 是多余的。

### 实现

在 `ffi_call_func` 中添加 `fprintf(stderr)` deprecation 警告，引导用户使用 `ffi.call_int()` 等明确返回类型的函数。
