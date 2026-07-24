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
| `src/module_symbol_table/inc/sym_table_cache.inc` | 265-266 | `int32_t mv; fwrite(&mv,4,...)` | `int64_t mv; fwrite(&mv,8,...)` |
| `src/module_symbol_table/inc/sym_table_cache.inc` | 526,529-530 | `calloc(sizeof(int))` + `int32_t read` | `calloc(sizeof(int64_t))` + `int64_t read` |
| `src/module_symbol_table/inc/scan/scan_enum.inc` | 17,45 | `int[]` + `int val` | `int64_t[]` + `int64_t val` |

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
Leno 中 struct 字段是**值类型**（内嵌在父 struct 的内存中），不是指针/引用。
值类型永远不为 null，编译器允许 `== null` 比较但不产生正确结果。

## 影响范围
所有对 struct 类型字段做 `== null` / `!= null` 判断的懒初始化模式。

## 修复方向
两种可行方案：
1. **显式 boolean 标志**（当前 workaround）：增加 `bool _menuInited` 字段跟踪初始化状态
2. **编译器层面**：struct 字段 `== null` 时给出编译警告或错误

目前 `sdl_tray.leno` 使用方案 1。

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
| `src/module_symbol_table/inc/sym_table_cache.inc` | 265-266 | `int32_t mv; fwrite(&mv,4,...)` | `int64_t mv; fwrite(&mv,8,...)` |
| `src/module_symbol_table/inc/sym_table_cache.inc` | 526,529-530 | `calloc(sizeof(int))` + `int32_t read` | `calloc(sizeof(int64_t))` + `int64_t read` |
| `src/module_symbol_table/inc/scan/scan_enum.inc` | 17,45 | `int[]` + `int val` | `int64_t[]` + `int64_t val` |

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
Leno 中 struct 字段是**值类型**（内嵌在父 struct 的内存中），不是指针/引用。
值类型永远不为 null，编译器允许 `== null` 比较但不产生正确结果。

## 影响范围
所有对 struct 类型字段做 `== null` / `!= null` 判断的懒初始化模式。

## 修复方向
两种可行方案：
1. **显式 boolean 标志**（当前 workaround）：增加 `bool _menuInited` 字段跟踪初始化状态
2. **编译器层面**：struct 字段 `== null` 时给出编译警告或错误

目前 `sdl_tray.leno` 使用方案 1。

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
