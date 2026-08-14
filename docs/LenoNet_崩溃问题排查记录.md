# LenoNet HttpClient 崩溃问题排查记录

## 概述

在开发 LenoNet（libcurl 绑定）过程中，`HttpClient` 多次请求时发生间歇性崩溃。
经过一整天排查（GC → FFI trampoline → 堆损坏），最终通过 gdb 定位到 **STATUS_HEAP_CORRUPTION (0xC0000374)**，
根因是**性能优化引入的 `vm_stack_push_fast` 越界写入破坏了堆元数据**。

> **✅ 崩溃已修复**：`op_type_specialized.inc` 中所有 `vm_stack_push_fast` 已回退为安全的 `vm_stack_push`。`vm_stack_push_fast` 在 `leno_vm.h` 中保留但不再被使用。

---

## 崩溃现象

1. `net.get()` 单次请求正常（执行时间短，未触发边界条件）
2. `HttpClient` 多次请求时崩溃（长时间持有句柄，跨模块调用链更深）
3. 崩溃不发生在执行期间，而是**程序结束时 `gc_free_all()` 调用 `free()` 触发堆损坏检测**
4. 用 `-g -O0` 编译的 debug 版也会崩溃，排除编译优化直接导致

---

## 排查过程

### 阶段1: 怀疑 GC 回收 ObjFFICallback

**假设**: `ObjFFICallback` 被 GC 回收，`ffi_callback_free_resources()` 释放了 trampoline 内存，libcurl 调用野指针。

**验证**:
- 在 `ffi_call_impl` 中永久禁用 GC → 仍然崩溃
- 在 `gc.c` 的 `mark_roots` 中检查模块全局变量标记 → 逻辑正确
- 调用帧链上的 `frame->module` 正确指向 `net.leno` 模块

**结论**: GC 回收不是直接原因（但 GC 禁用策略仍有问题，需后续修复）

### 阶段2: 怀疑 FFI 参数传递

**假设**: `setopt_ptr(handle, Opt.WRITEFUNCTION, g_writeCb)` 中 `ObjFFICallback` 作为 `any` 参数传递时类型处理有误。

**验证**:
- 在 `ffi_call_impl` 参数遍历中加 C 层调试 → 崩溃在到达 `ffi_call_impl` 之前
- 在 `OP_MODULE_CALL` 入口加调试 → 崩溃在到达 `OP_MODULE_CALL` 之前
- 在 Leno 层加 `print` → 最后输出停在 `Opt.WRITEFUNCTION=20011` 评估之后

**结论**: 崩溃不在 FFI 调用本身，而在 VM 执行期间

### 阶段3: gdb 堆栈回溯（关键突破）

用 gdb 运行带调试符号的 exe:

```
gdb -batch -ex run -ex bt --args build/leno_debug.exe crash_test10.leno
```

**堆栈回溯**:
```
Thread 1 received signal SIGTRAP, Trace/breakpoint trap.
0x00007ffad87ef413 in ntdll!RtlIsZeroMemory
#0  ntdll!RtlIsZeroMemory
#1  ntdll!RtlpNtSetValueKey
#2  ntdll!RtlpNtSetValueKey
#3  ntdll!RtlpNtSetValueKey
#4  ntdll!memset
#5  ntdll!RtlFreeHeap
#6  ucrtbase!_free_base
#7  gc_free_all () at src\gc.c:1625      ← free(gc.vm->stack)
#8  lenolang_run () at src\main.c:215
```

**错误码**: `c0000374` = **STATUS_HEAP_CORRUPTION**

**结论**: 堆内存在 VM 执行期间被越界写入破坏，程序结束时 `free()` 检测到堆元数据损坏。

### 阶段4: 定位堆损坏根因

查看 `性能优化记录.md`，发现:

1. **`vm_stack_push_fast` 不检查栈溢出**（`leno_vm.h:507`）:
   ```c
   static inline void vm_stack_push_fast(VM* vm, Value v) {
       vm->stack[vm->sp++] = v;  // 无边界检查！
   }
   ```

2. **`op_type_specialized.inc` 大量使用 `vm_stack_push_fast`**:
   - `OP_ADD_INT`, `OP_SUB_INT`, `OP_MUL_INT` 等 ~30 个指令
   - 这些指令在 FFI 回调执行 Leno 字节码时会被触发

3. **FFI 回调 `callback_dispatch_direct` 用 `vm_stack_push`（安全版）压入参数**:
   ```c
   // ffi.c:2354 - 用安全版
   vm_stack_push(vm_ptr, leno_arg);
   ```
   但回调函数体内部执行的字节码使用 `vm_stack_push_fast`（不安全版）

4. **`op_type_specialized.inc` 的快速栈操作在 2026-05-28 曾因 `更多排序.leno` 测试失败而部分回退**，但类型特化指令中的使用被保留

**根因链**:
```
FFI 回调 (libcurl write_callback)
  → callback_dispatch_direct
    → vm_call_value → 执行 Leno 字节码
      → op_type_specialized.inc 中的 vm_stack_push_fast
        → VM 栈已满（FFI 回调复用主线程 VM 栈，栈空间不足）
          → 越界写入 vm->stack[vm->sp++] 
            → 破坏相邻堆内存的元数据
              → gc_free_all() 时 free() 检测到 STATUS_HEAP_CORRUPTION
```

---

## 涉及的文件和代码

### 根因文件

| 文件 | 行号 | 问题 |
|------|------|------|
| `src/include/leno_vm.h` | 507-509 | `vm_stack_push_fast` 不检查栈溢出 |
| `src/vm/vminc/op_type_specialized.inc` | 全文件 | ~30 个指令使用 `vm_stack_push_fast` |
| `src/module/ffi/ffi.c` | 2264-2400 | `callback_dispatch_direct` 在主线程 VM 栈上执行回调 |

### 性能优化记录中的警告

`docs/性能优化记录.md` 第 40 行:
> ❌ **部分快速栈操作** (`vm_stack_pop_fast`, `vm_stack_push_fast`, `vm_stack_peek_fast`) - 2026-05-28 回退
> 原因: `更多排序.leno` 测试失败，这些指令中的快速栈操作在复杂场景下导致栈边界错误
> **例外**: `op_type_specialized.inc` 中的快速栈操作已恢复，因其栈操作确定且由编译器生成，相对安全

**这个"例外"正是崩溃的原因** — FFI 回调场景下栈空间可能不足，"编译器生成的确定栈操作"在栈满时仍然越界。

---

## 修复方案

> **✅ 已采用方案 A + C**：`op_type_specialized.inc` 中的快速栈操作已全部回退为安全版。

### 方案 A: 回退 `op_type_specialized.inc` 中的快速栈操作（推荐，安全）

将 `op_type_specialized.inc` 中所有 `vm_stack_push_fast` → `vm_stack_push`，
`vm_stack_pop_fast` → `vm_stack_pop`。

**代价**: 性能从 203ms 回退到 ~210ms（约 3% 性能损失）
**收益**: 彻底消除栈越界风险

### 方案 B: 在 FFI 回调中增加栈空间预留

在 `callback_dispatch_direct` 调用 `vm_call_value` 之前，检查并确保 VM 栈有足够空间:
```c
// 确保至少有 256 个 Value 的栈空间（回调可能触发深层调用）
if (vm_ptr->sp + 256 > vm_ptr->stack_capacity) {
    int new_capacity = vm_grow_capacity(vm_ptr->stack_capacity + 256);
    Value* new_stack = (Value*)realloc(vm_ptr->stack, new_capacity * sizeof(Value));
    if (new_stack) {
        vm_ptr->stack = new_stack;
        vm_ptr->stack_capacity = new_capacity;
    }
}
```

**代价**: 不影响性能，但需要验证所有 FFI 回调场景
**风险**: 如果预留空间不够，仍可能越界

### 方案 C: 给 `vm_stack_push_fast` 加 debug 断言

在 debug 构建中加边界检查:
```c
static inline void vm_stack_push_fast(VM* vm, Value v) {
    assert(vm->sp < vm->stack_capacity && "vm_stack_push_fast: stack overflow!");
    vm->stack[vm->sp++] = v;
}
```

**代价**: 无性能损失（仅 debug 构建生效）
**收益**: 在开发阶段捕获越界，但 release 构建仍有风险

### 推荐组合: A + C

1. 回退 `op_type_specialized.inc` 中的快速栈操作（方案 A）
2. 同时在 debug 构建中加断言（方案 C）防止未来重复引入
3. 更新 `性能优化记录.md` 标记 `op_type_specialized.inc` 快速栈操作为 **❌ 已回退**

---

## 开发 LenoNet 过程中发现的所有问题

开发 `lib/curl_core.leno` 和 `lib/net.leno` 时，发现以下编译器和语言层面的问题。

---

### 问题1: ~~`ffi_call_impl` 的 GC 禁用策略不当~~ （已修复）

**严重程度**: 高
**类型**: GC 安全性
**状态**: ✅ 已修复。`ffi.c` 中已移除所有 `gc_set_enabled`/`gc_clear_deferred` 调用，不再永久禁用 GC。

**描述**: `ffi_call_impl` 中调用 `gc_set_enabled(0)` 永久禁用 GC，不恢复。
程序运行时间越长，内存占用越大，最终可能 OOM。

同时 `gc_clear_deferred()` 清除了延迟 GC 标志，但返回值转换中 `gc_alloc` 新对象时
GC 已被禁用，这些新对象不会被及时回收。

**应改为**: 保存/恢复 GC 状态:
```c
int saved_gc = gc_get_enabled();
gc_set_enabled(0);
// ... FFI 调用 ...
gc_set_enabled(saved_gc);
```

---

### 问题2: ~~GC `mark_roots` 可能遗漏非活跃模块的全局变量~~ （已修复）

**严重程度**: 高
**类型**: GC 安全性
**状态**: ✅ 已修复。`mark_roots` 中新增第 13.5 步 `loaded_modules_mark_all()`，遍历所有已加载模块并标记其全局变量，不再依赖 `current_module_frame` 链。

**描述**: 模块初始化完成后 `module_frame_exit()` 弹出模块帧，
`current_module_frame` 链不再包含该模块。

虽然调用帧的 `frame->module` 指向正确模块（GC 通过 `OBJ_MODULE` 的
`gc_scan_references` 标记全局变量），但如果某些调用路径下
`frame->module` 为 NULL 或指向其他模块，全局变量可能不被标记。

**影响**: 模块级全局变量（如 `g_writeCb`）持有的 `ObjFFICallback` 可能被 GC 回收，
释放 trampoline 内存，libcurl 后续调用回调时访问野指针 → Segfault。

**建议修复**: 在 GC `mark_roots` 中遍历所有已加载的 `ObjModule` 对象，
不仅依赖 `current_module_frame` 链和 `frame->module`。

---

### 问题3: `cfunc` 类型不能跨模块 `use` 导入

**严重程度**: 中
**类型**: 语言限制 / 编译器
**状态**: ✅ 已修复

**描述**: `use` 语句已支持导入 `cfunc`（C 回调签名）。完整实现了 `cfunc` 的跨模块 `use` 导入，
包括符号表存储、模块扫描器、use 语句查找、缓存序列化/反序列化全链路。

**修复内容**:
1. 在 `module_symbol_table.h` 中定义 `ModuleCfuncSymbol` 结构体和 `cfuncs` 数组
2. 在 `sym_table_create.inc` 中添加 `cfunc` 的初始化和销毁
3. 在 `sym_table_add.inc` 中添加 `cfunc` 的 `add`/`find`/`count` 函数
4. 在 `scan_pass1.inc` 中扫描 `export cfunc` 和裸 `cfunc` 声明，解析参数和返回类型（支持 `Ptr[T]` 语法）
5. 在 `visit_module.inc` 的 `AST_USE` 中添加 `cfunc` 查找逻辑，从 `ModuleCfuncSymbol` 重建 `TypeInfo*` 签名
6. 在 `sym_table_cache.inc` 中添加 `cfunc` 的序列化/反序列化（版本号升至 `v14`）

**测试**: `assert/test_cfunc_cross_module/` 验证跨模块 `use cfunc` 导入。

---

### 问题4: `clib` 不支持变参函数声明

**严重程度**: 高
**类型**: 语言限制
**状态**: ⚠️ 未修复（设计限制）。clib 要求固定参数数量和类型，无法支持 C 的 `...` 变参。当前变通方案：用 `ffi.call_int`/`ffi.call_ptr` 等动态调用。

**描述**: C 语言的变参函数（如 `curl_easy_setopt`、`curl_easy_getinfo`、`printf` 等）
无法在 `clib` 声明中使用，因为 `clib` 要求每个函数有固定的参数数量和类型。

`curl_easy_setopt(CURL*, CURLoption, ...)` 是变参函数，不同选项需要不同类型的参数
（有时传 `int`，有时传 `string`，有时传函数指针 `Ptr`）。

**复现**:
```leno
clib curl {
    // 以下声明会导致类型检查错误：
    i32 curl_easy_setopt(Ptr handle, i32 option, i32 param)  // 只能声明一种类型
    // 无法同时声明 string 和 ptr 版本
}
```

**当前绕过**: 用 `ffi.call_int(lib(), "curl_easy_setopt", handle, option, value)` 代替
`clib` 声明，因为 `ffi.call_*` 不做编译期类型检查，运行时自动转换。

**建议修复**: 在 `clib` 中支持 `vararg` 标记，如
`i32 curl_easy_setopt(Ptr handle, i32 option, ...)`，
或允许同名函数重载（不同参数类型）。

---

### 问题5: ~~`enum` 不支持位运算表达式作为成员值~~ （已修复）

**严重程度**: 低
**类型**: 语言限制
**状态**: ✅ 已修复。Parser 层新增 `eval_const_expr`，支持 `+` `-` `*` `/` `%` `|` `&` `^` `<<` `>>` `~` `!` 和括号分组的编译期常量表达式。

**描述**: `enum` 成员的显式值必须是整数常量，不支持 `~0`（位取反）或 `0 | 1`（位或）等表达式。

**复现**:
```leno
export enum Auth {
    ANY = ~0      // 错误: enum 成员显式值必须是整数常量
    ANYSAFE = ~2   // 错误
}
```

**当前绕过**: 使用计算后的具体数值：`ANY = 0xFFFFFFFF`、`ANYSAFE = 0xFFFFFFFD`

**建议修复**: 支持常量表达式作为 enum 成员值，至少支持位运算（`~`、`|`、`&`）。

---

### 问题6: ~~`clib` 声明的 `str8` 参数不接受 Leno `string` 类型~~ （已修复）

**严重程度**: 中
**类型**: 语言限制
**状态**: ✅ 已修复。`visit_expr.inc` 中已添加 `TYPE_STRING ↔ TYPE_STR8/TYPE_STR16` 双向兼容检查，`string` 可直接传给 `str8` 参数。

**描述**: 当 `clib` 声明中函数参数为 `str8` 类型时，传入 Leno 的 `string` 类型会报错：
```
clib 函数 'curl_slist_append' 参数 3 类型不能使用 'string'，请使用 'str8' 或 'ptr'
```

虽然 `string` 和 `str8` 在运行时都是 `const char*`，但编译器在 clib 调用时做了严格类型检查。

**当前绕过**: 从 `clib` 声明中移除有此问题的函数，改用
`ffi.call_ptr(lib(), "curl_slist_append", list, s)` 调用。

**建议修复**: 允许 `string` 类型自动转换为 `str8`（零摩擦类型转换）。

---

### 问题7: ~~`double` 不是有效的 Leno 类型~~ （已修复）

**严重程度**: 低
**类型**: 错误提示不友好
**状态**: ✅ 已修复。类型未定义错误中，对 `double` 追加提示："Leno 中使用 float 代替 double，Leno 的 float 是 64 位双精度浮点数"。

**描述**: Leno 语言中浮点数类型只有 `float`（对应 C 的 `double`，64 位双精度），没有 `double` 关键字。
初学者容易混淆。

**错误信息**: `未定义的类型 'double'`

**建议**: 错误提示中增加：
"Leno 中使用 `float` 类型代替 `double`（Leno 的 float 是 64 位双精度浮点数）"

---

### 问题8: `setopt_ptr` 参数类型必须用 `any` 而不能用 `var`/`Ptr`

**严重程度**: 中
**类型**: 编译器语义检查
**状态**: ⚠️ 未修复（设计限制）。`Ptr` 类型不接受 `ObjFFICallback`，`var` 声明有语义问题。当前变通：参数声明为 `any`。

**描述**: `setopt_ptr` 函数需要接受不同类型的指针参数（`ObjFFIPointer`、`ObjFFICallback`、`null`）。
当参数声明为 `Ptr` 时，传入 `ObjFFICallback` 会报类型错误。
当参数声明为 `var` 时，编译器报语义错误。

**当前绕过**: 参数类型改为 `any`:
```leno
export func setopt_ptr(Ptr handle, int option, any value): int {
    return ffi.call_int(lib(), "curl_easy_setopt", handle, option, value)
}
```

**建议修复**: `Ptr` 类型应能接受 `ObjFFICallback`（因为 callback 本质上也是函数指针），
或在类型系统中增加联合类型支持。

---

### 问题9: ~~字符串方法命名与常见语言不同~~ （已修复错误提示）

**严重程度**: 低
**类型**: 设计差异
**状态**: ✅ 已修复错误提示。当 `string` 类型调用 `contains` 或 `includes` 时，提示"是否想用 'has'？（如 s.has(sub)）"。

**描述**: Leno 字符串包含检查方法为 `.has(sub)`，而不是 Python/Java/JavaScript 中常见的
`.contains(sub)` 或 `.includes(sub)`。

**建议**: 在错误提示中建议：
"'string' 类型没有方法 'contains'，是否想用 'has'？"

---

### 问题10: ~~`CURLINFO_SIZE_DOWNLOAD` 信息码可能不正确~~ （已修复）

**严重程度**: 中
**类型**: 待确认
**状态**: ✅ 已修复。`SIZE_DOWNLOAD = 0x300008`（`CURLINFO_DOUBLE + 8`）正确，并新增 `SIZE_DOWNLOAD_T = 0x600008`（`CURLINFO_OFF_T + 8`）。

**描述**: 在 `basic.leno` 测试中，HTTP GET 请求成功返回了完整响应体，
但 `downloadSize` 始终显示为 0 bytes。

**可能原因**: `CURLINFO_SIZE_DOWNLOAD` 的信息码 `0x200010` 可能不正确，
实际可能是 `CURLINFO_SSL_ENGINES` 而非 `SIZE_DOWNLOAD`。

**当前绕过**: 使用 `resp.body.len()` 获取响应体大小。

---

### 问题11: ~~`Opt.ACCEPT_ENCODING` 设为空字符串 `""` 的行为~~ （已确认正常）

**严重程度**: 低
**类型**: 文档缺失
**状态**: ✅ 已确认正常。`setopt_str` 传空字符串 `""` 正确传递给 libcurl，不会当作 null 处理。LenoNet 测试全部通过。

**描述**: 设置 `curl_easy_setopt(CURLOPT_ACCEPT_ENCODING, "")` 时，
libcurl 会自动添加 `Accept-Encoding: gzip, deflate` header 并自动解压响应。
但 Leno 中 `core.setopt_str(handle, Opt.ACCEPT_ENCODING, "")` 是否正确传递空字符串
需要验证（空字符串可能被当作 null 处理）。

---

## 时间线

| 时间 | 事件 |
|------|------|
| 2026-05-27 | 引入 `vm_stack_push_fast` 等快速栈操作 |
| 2026-05-28 | 快速栈操作导致 `更多排序.leno` 失败，部分回退；`op_type_specialized.inc` 保留 |
| 2026-08-10 | 批量移除 opcode frame 更新，导致性能下降，全部回退 |
| 2026-08-14 | 开发 LenoNet，HttpClient 多次请求触发 `op_type_specialized.inc` 中的 `vm_stack_push_fast` 越界，导致 STATUS_HEAP_CORRUPTION |

---

## 总结

| 项 | 内容 |
|----|------|
| **崩溃类型** | STATUS_HEAP_CORRUPTION (0xC0000374) |
| **触发条件** | FFI 回调期间执行 Leno 字节码，VM 栈溢出 |
| **根因** | `op_type_specialized.inc` 中 `vm_stack_push_fast` 不检查栈溢出 |
| **性能优化关联** | 2026-05-27 引入的快速栈操作优化 |
| **修复方案** | 回退 `op_type_specialized.inc` 中的快速栈操作 + 加 debug 断言 |
| **性能代价** | ~3% (203ms → ~210ms) |
