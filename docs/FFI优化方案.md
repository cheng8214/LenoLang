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

## P0-2：混合参数回退路径浮点 bug ✅

### 问题

`leno_ffi_win64.c:405-436` 的"路径 4 回退策略"中，当参数超过 4 个且含浮点时，把 `double` 的位模式当 `int64_t` 传递。代码注释自己承认（第 409-412 行）：

```
⚠ 警告：此回退路径对浮点参数不保证正确！
int64 位模式通过整数寄存器/栈传递，但被调函数期望
浮点参数在 XMM 寄存器中，两者不匹配。
```

### 实现

在 `ffi_call_impl` 中、调用 `ffi_call` 之前新增检查逻辑：

- 统计参数中 `FFI_TYPE_FLOAT` 和 `FFI_TYPE_DOUBLE` 的数量
- 判断是否进入 broken 路径：
  - `nargs > 5 && total_float > 0` → 出错
  - `nargs == 5 && double_count > 0` → 出错
  - `nargs == 5 && float_count > 0 && float_count < 4` → 出错（路径 3.5 仅处理 4f32+1ptr）
- 出错时 `native_throw_error` 给出明确错误和解决方案

同时更新 `leno_ffi_win64.c` 路径 4 的警告消息，提示上层应已拦截。

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
