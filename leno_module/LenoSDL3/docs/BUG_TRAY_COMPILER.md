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

# 托盘开发中发现的编译器/语言问题
