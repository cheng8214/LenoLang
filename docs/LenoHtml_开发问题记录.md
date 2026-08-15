# LenoHtml 开发问题记录

## 概述

在开发纯 Leno 实现的 HTML 解析器（`leno_module/LenoHtml/lib/html.leno`）过程中，发现以下 LenoC 语言层面的 bug 和限制。

---

## Bug 1: 模块级全局变量在函数内访问不稳定（严重）

**严重程度**: 高
**类型**: 运行时崩溃
**状态**: ⚠️ 未修复（已绕过）

**描述**: 模块级 `var` 全局变量（如 `var _g_pos = 0`、`var _g_html = ""`）在模块函数中被赋值后，跨函数调用时值不稳定，导致程序崩溃。

**复现**:
```leno
// 模块文件 lib/test.leno
var _g_pos = 0
var _g_html = ""

func _inner() {
    // 读取 _g_pos 时可能崩溃或返回错误值
    while _g_pos < _g_html.len() {
        ...
    }
}

export func parse(string html) {
    _g_html = html
    _g_pos = 0
    _inner()  // 崩溃在这里
}
```

**影响**: 无法使用模块级全局变量在多个函数间共享状态（如解析器的位置指针和输入字符串）。

**绕过方案**: 使用 `Array` 参数传递状态（如 `Array state = [pos, len]`），通过 `state[0]` 和 `state[1]` 读写，避免使用模块级全局变量。

**推测原因**: 可能是模块级全局变量在函数调用时的 GC 标记或寄存器分配有问题，导致变量值在函数返回后被覆盖或丢失。

---

## Bug 2: 函数参数不能使用 `var` 类型

**严重程度**: 中
**类型**: 编译器限制
**状态**: ⚠️ 设计限制

**描述**: 函数参数声明为 `var` 类型时编译报错："参数 'xxx' 不能使用 var 类型，请改用 any"。

**复现**:
```leno
// 报错
func foo(var state) { ... }

// 正确
func foo(any state) { ... }
```

**影响**: 无法在函数签名中表达"可变容器"语义，只能用 `any` 替代，但这意味着调用方需要在函数内部做类型收窄才能访问字段。

---

## Bug 3: Dict 索引返回 `any` 类型，不能直接赋值给具体类型变量

**严重程度**: 中
**类型**: 类型系统限制
**状态**: ⚠️ 设计限制

**描述**: `Dict` 索引访问（如 `dict["key"]`）返回 `any` 类型，不能直接赋值给 `int`、`string` 等具体类型变量，也不能直接调用方法。

**复现**:
```leno
Dict d = {pos: 0, len: 10}
int pos = d["pos"]  // 报错：any 类型需要显式转换
int pos = _int(d["pos"])  // 报错：_int() 用于字符串转 int

// 正确方式：类型收窄
var v = d["pos"]
if v is int {
    int pos = v
}
```

**影响**: 使用 Dict 传递状态非常繁琐，每次读取都需要类型收窄。这是选择 Array 传递状态的原因之一（Array 索引也返回 `any`，但可以通过 `is int` 收窄）。

**建议**: 支持泛型 Dict（如 `Dict[string, int]`），或在 Dict 索引时自动类型推断。

---

## Bug 4: `var` 声明的 Dict 字面量类型推断为 `Dict[string, string]`，无法存储异构类型

**严重程度**: 中
**类型**: 类型推断限制
**状态**: ⚠️ 设计限制

**描述**: `var node = {tag: "div", attrs: {}, children: [], text: ""}` 这种异构 Dict 字面量在赋值时，如果某个字段值是 `Array` 或 `bool`，会报"字典值类型不匹配"。

**复现**:
```leno
// 报错：Dict 字面量中 classes: [] 和 hasAttr: false 的类型与 string 不匹配
Dict r = {tag: "", id: "", classes: [], attrName: "", attrValue: "", hasAttr: false}

// 正确方式：用 var 声明
var r = {tag: "", id: "", classes: [], attrName: "", attrValue: "", hasAttr: false}
```

**影响**: 必须用 `var` 声明异构 Dict，无法在函数签名中指定具体 Dict 类型。

---

## Bug 5: `Array` 返回类型与 `Array[any]` 不兼容

**严重程度**: 中
**类型**: 类型系统限制
**状态**: ⚠️ 设计限制

**描述**: 函数返回类型声明为 `Array`，但返回 `var current = [root]`（被推断为 `Array[any]`）时，报"返回类型不匹配"。

**复现**:
```leno
// 报错
export func select(any root, string selector): Array {
    var current = [root]  // 被推断为 Array[any]
    ...
    return current  // 类型不匹配
}

// 正确方式：返回类型改为 any
export func select(any root, string selector): any {
    ...
    return current
}
```

**影响**: 调用方需要对返回值做 `is Array` 类型收窄才能调用 `.len()` 或索引访问。

**提示**: LenoC 的数组类型是不变的（invariant），`Array[any]` 不能赋给 `Array`。

---

## Bug 6: `for 0 to N to i` 语法不被支持

**严重程度**: 低
**类型**: 语法限制
**状态**: ⚠️ 设计限制

**描述**: LenoC 的数字 for 循环语法是 `for 0:N to i`（用冒号），不是 `for 0 to N to i`。

**复现**:
```leno
// 报错：for 循环体必须用大括号 {} 包裹
for 0 to N to i { ... }

// 正确
for 0:N to i { ... }
```

---

## Bug 7: `!` 运算符不支持逻辑非

**严重程度**: 低
**类型**: 语法限制
**状态**: ⚠️ 设计限制

**描述**: LenoC 不支持 `!` 作为逻辑非运算符，必须使用 `not` 关键字。

**复现**:
```leno
// 报错：意外的字符 '!'
if !condition { ... }

// 正确
if not condition { ... }
```

---

## Bug 8: 函数参数声明顺序是 `type name` 而非 `name: type`

**严重程度**: 低
**类型**: 语法限制
**状态**: ⚠️ 设计限制

**描述**: LenoC 函数参数声明使用 `type name` 顺序（类似 C/Go），不是 `name: type`（类似 Python/Kotlin）。

**复现**:
```leno
// 报错：期望参数名
func foo(x: int, y: string) { ... }

// 正确
func foo(int x, string y) { ... }
```

---

## Bug 9: 字符串不能用 `>=` 比较单字符

**严重程度**: 低
**类型**: 运行时错误
**状态**: ⚠️ 已绕过

**描述**: 在模块函数中，对字符串做 `c >= "a"` 这样的比较时，可能触发"操作数必须是数字或字符串"错误。

**绕过方案**: 使用 `.byte(index)` 获取 ASCII 值（int），用 int 比较代替字符串比较。

```leno
// 可能出错
string c = _str(html[i])
if c >= "a" and c <= "z" { ... }

// 正确方式
int b = html.byte(i)
if b >= 97 and b <= 122 { ... }
```

---

## 测试结果

全部 18 个测试用例通过（`html_test.leno`），覆盖：
- 基本解析、多层嵌套
- 标签选择器、ID 选择器、class 选择器、组合选择器
- 后代选择器、直接子选择器
- 属性选择器
- allText、getLinks、children、attr
- 自闭标签、注释、script/style 标签
- 多 class 选择器、selectOne 返回 null

### 已知小问题（逻辑层面，非语言 bug）

1. **allText 重复收集**: `allText` 同时收集了父节点的 `text` 字段和文本子节点的 `text`，导致重复。应只收集子节点的 text 或只收集父节点的 text。
2. **void 标签属性解析**: `<img src='pic.jpg' alt='picture'>` 的属性可能未正确解析（因为 void 标签直接返回，没有继续解析后续属性）。

---

## 总结

| 问题 | 严重程度 | 类型 | 状态 |
|------|----------|------|------|
| 模块级全局变量跨函数不稳定 | 高 | 运行时崩溃 | ⚠️ 已绕过（用 Array 传状态） |
| 函数参数不能用 var | 中 | 编译器限制 | ⚠️ 用 any 替代 |
| Dict 索引返回 any | 中 | 类型系统限制 | ⚠️ 用 is 收窄 |
| Dict 字面量异构类型推断 | 中 | 类型推断限制 | ⚠️ 用 var 声明 |
| Array 返回类型不兼容 | 中 | 类型系统限制 | ⚠️ 返回 any |
| for 循环语法 | 低 | 语法限制 | ⚠️ 用 0:N 语法 |
| ! 不支持 | 低 | 语法限制 | ⚠️ 用 not |
| 参数顺序 | 低 | 语法限制 | ⚠️ 用 type name |
| 字符串比较 | 低 | 运行时错误 | ⚠️ 用 byte() 比较 |
