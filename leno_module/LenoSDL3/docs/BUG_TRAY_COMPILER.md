# BUG: Enum 成员值 >= 2^31 时 u32 FFI 传参溢出

## 发现位置
`leno_module/LenoSDL3/lib/sdl_tray.leno` — 系统托盘封装开发时发现。

## 现象
`TrayEntryFlag.DISABLED (0x80000000) | TrayEntryFlag.BUTTON (0x00000001)` 的结果传给
SDL3 的 `SDL_InsertTrayEntryAt(menu, pos, label, u32 flags)` 时：
```
[运行时错误] 参数 4 值 -2147483647 超出 u32 范围
```

## 根因
以下路径导致值从 64 位无符号被截断为 32 位有符号：

1. **`src/semantic/visitinc/visit_enum.inc:29`**
   ```c
   sym->enum_values[i] = (int)ast->u.enum_def.member_values[i];
   ```
   `ast->u.enum_def.member_values[i]` 是 `int64_t`（64 位），`(int)` 截断为 32 位有符号。
   `0x80000000`(2147483648) → `-2147483648`

2. **`src/include/leno_vm.h:289`** — `Symbol.enum_values` 是 `int*`（32 位有符号）

3. **`src/include/module_symbol_table.h:57`** — `ModuleEnumSymbol.member_values` 是 `int*`（32 位有符号）

4. **`src/module_symbol_table/inc/sym_table_cache.inc:265`** — 序列化时用 `int32_t` 写盘

5. **`src/module_symbol_table/inc/scan/scan_enum.inc:17`** — 快速扫描时用 `int[]` 解析 enum 值

后经 FFI 的 u32 范围检查（`ffi.c:592`）时，负值被拦截报错。

## 影响范围
所有 `>= 0x80000000`（32 位最高位置1）的 enum 成员值，做 `|` 运算后作为 `u32` 传给 clib 时崩溃。

**受影响**：`TrayEntryFlag.DISABLED (0x80000000)`、`TrayEntryFlag.CHECKED (0x40000000)` 组合使用场景。

**不受影响**：WindowFlag（值都在 $2^{31}$ 以内，没有最高位置1的）。

## 修复方向
将所有存储 enum 成员值的位置从 `int`/`int*` 改为 `int64_t`/`int64_t*`：

| 文件 | 行号 | 当前 | 改为 |
|------|------|------|------|
| `src/include/leno_vm.h` | 289 | `int* enum_values` | `int64_t* enum_values` |
| `src/include/module_symbol_table.h` | 57 | `int* member_values` | `int64_t* member_values` |
| `src/include/module_symbol_table.h` | 165 | `int* member_values` (参数) | `int64_t* member_values` |
| `src/semantic/visitinc/visit_enum.inc` | 26 | `(int*)malloc(sizeof(int)*n)` | `(int64_t*)malloc(sizeof(int64_t)*n)` |
| `src/semantic/visitinc/visit_enum.inc` | 29 | `(int)ast->u.enum_def...` | 去掉 `(int)` 截断 |
| `src/semantic/visitinc/visit_module.inc` | 461 | `(int*)malloc(sizeof(int)*n)` | `(int64_t*)malloc(sizeof(int64_t)*n)` |
| `src/semantic/visitinc/visit_module.inc` | 1533 | `int member_value` | `int64_t member_value` |
| `src/semantic/semantic_visit_ast.c` | 156 | `(int*)malloc(sizeof(int)*n)` | `(int64_t*)malloc(sizeof(int64_t)*n)` |
| `src/module_symbol_table/inc/sym_table_add.inc` | 162 | `int* member_values` | `int64_t* member_values` |
| `src/module_symbol_table/inc/sym_table_add.inc` | 181 | `(int*)malloc(sizeof(int)*n)` | `(int64_t*)malloc(sizeof(int64_t)*n)` |
| `src/module_symbol_table/inc/sym_table_cache.inc` | 7 | `LENOSYMC_VERSION 0x05` | `0x06`（enum 值格式 int32→int64，旧缓存自动失效） |
| `src/module_symbol_table/inc/sym_table_cache.inc` | 265-266 | `int32_t mv; fwrite(&mv,4,...)` | `int64_t mv; fwrite(&mv,8,...)` |
| `src/module_symbol_table/inc/sym_table_cache.inc` | 526,529-530 | `calloc(sizeof(int))` + `int32_t read` | `calloc(sizeof(int64_t))` + `int64_t read` |
| `src/module_symbol_table/inc/scan/scan_enum.inc` | 17,45 | `int[]` + `int val` | `int64_t[]` + `int64_t val` |

> ✅ **已修复（2026-07-25）**：上述全部 13 处已改为 `int64_t`。`EnumName.member` 编译期取值改为 `int64_t`，经 `(double)` 转 `AST_NUM`（与 lexer 对 hex 字面量 ≤ 2^53 的处理一致），codegen 中 `val_int_safe` 自动处理 int48→bigint 提升。缓存版本升级 v5→v6，旧 `.lenosymc` 自动失效重扫。验证：`TrayEntryFlag.DISABLED | BUTTON` = 2147483649，经 ffi `u32` 传参（kernel32 SetLastError/GetLastError 往返）通过。

**修复后需要删除 `.lenocache` 缓存目录**重新生成符号表缓存。

---

# BUG: struct 字段是值类型，不能与 null 比较

## 发现位置
`leno_module/LenoSDL3/lib/sdl_tray.leno` — `Tray.getMenu()` 懒初始化。

## 现象
```leno
export struct Tray {
    TrayMenu _menu    // struct 类型字段
    // ...
    func getMenu(): TrayMenu {
        if _menu == null {   // 值类型字段永远不为 null，始终为 false
            // 初始化代码从未执行
        }
        return _menu
    }
}
```
`if _menu == null` 始终为 `false`，懒初始化不生效，`_menu._ptr` 保持默认值 null。

## 根因
Leno 中 struct 字段是**值类型**：`new Tray()` 时嵌套 struct 字段被**递归立即分配**
（`object_struct.c:struct_instance_new_depth` 中 TYPE_STRUCT 分支），所以默认不为 null。
编译器允许 `== null` 比较（`type_is_compatible` 中 `null 可赋值给任何类型`），
但默认状态下结果恒为 false，懒初始化分支永不执行。

注：显式 `_menu = null` 后 `== null` 才会为 true（运行时确认为 true）。

## 影响范围
所有对 struct 类型字段做 `== null` / `!= null` 判断的懒初始化模式。

## 修复方向
两种可行方案：
1. **显式 boolean 标志**（当前 workaround）：增加 `bool _menuInited` 字段跟踪初始化状态
2. **编译器层面**：struct 字段 `== null` 时给出编译警告或错误

目前 `sdl_tray.leno` 使用方案 1。

> ✅ **方案 2 已实现（2026-07-25）**：新增 `WARN_STRUCT_EQ_NULL` 警告类型，
> 在语义分析 `AST_BINOP` 中检测 `struct 类型 == null` / `!= null`（一侧 `TYPE_STRUCT`，
> 另一侧 `AST_NULL`），提示"struct 值类型默认被立即分配，仅在显式赋 null 后才成立"。
> 警告不阻断编译，Ptr/string 等非 struct 类型的 `== null` 不受影响。

---

# BUG: struct 嵌套字段赋值不支持（值类型写穿限制）

## 发现位置
`leno_module/LenoSDL3/lib/sdl_tray.leno`。

## 现象
```leno
_rootMenu._ptr = some_ptr    // 编译错误：字段类型不匹配
_rootMenu._isSub = false     // 编译错误：字段类型不匹配
```
编译器将 `_rootMenu._ptr` 解析为对 `_rootMenu` 的赋值而非对 `_rootMenu` 字段 `_ptr` 的赋值。

## 根因
Leno 不支持通过 `.` 链式赋值修改嵌套在 struct 内部的 struct 类型字段的子字段。

## 修复方向
workaround：使用临时变量：
```leno
TrayMenu m = new TrayMenu()
m._ptr = some_ptr
m._isSub = false
_rootMenu = m
```

---

# 注意事项: 托盘回调需要 drain 事件

托盘菜单的点击回调通过 SDL 内部事件处理触发。
事件循环必须 drain 事件队列，否则回调不会执行：

```leno
var ev = createEvent()
while running {
    ev.waitTimeout(500)
    while ev.has { ev.poll() }   // ★ 必须 drain
}
```

`waitTimeout` 只取一个事件，后续事件（包括触发回调的事件）堆积在队列中不会被处理。

---

# 托盘开发中发现的编译器/语言问题
