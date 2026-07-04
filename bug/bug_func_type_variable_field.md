# Bug: 函数类型不支持变量声明、struct 字段、匿名闭包传参

## 概述

Leno 已支持 `func(T):U` 函数类型（见 `examples/func/func类型/test_func_type.leno`），但仅限于**返回值类型**和**参数类型**两个位置。以下三种场景不生效：

1. **变量类型声明**：`func():void v = sayHello` → 语法错误
2. **struct 字段类型**：`func(int,int):int op` → 语法错误
3. **匿名闭包传参**：`apply(func(x):int { ... }, 1)` → 语法错误

## 复现

### 场景 1：变量类型声明

```leno
func sayHello() { print("hi") }
func():void fv = sayHello    // ❌ 期望函数名
fv()
```

**错误**：`[语法错误] 期望函数名`

### 场景 2：struct 字段

```leno
struct Calculator {
    func(int, int):int op     // ❌ 期望方法名

    func calc(int a, int b): int {
        return op(a, b)
    }
}
```

**错误**：`[语法错误] 期望方法名`



## 已验证可用的场景（对比）

| 场景 | 示例 | 状态 |
|------|------|------|
| 返回值类型 | `func foo(): func():int { ... }` | ✅ |
| 参数类型（单参） | `func bar(func(int):int fn)` | ✅ |
| 参数类型（多参） | `func baz(func(int,int):int op)` | ✅ |
| 参数类型（void） | `func run(func():void fn)` | ✅ |
| 闭包返回函数 | `func makeCounter(): func():int` | ✅ |
| 命名函数作实参 | `bar(triple)` | ✅ |
| **变量声明** | `func():void v = sayHello` | ✅ | 648671a8 |
| **struct 字段** | `struct S { func():void cb }` | ✅ | 648671a8 |
| **匿名闭包实参** | `bar(func(int x):int {...}, 1)` | ✅ | 已有

## 影响

- **SDL3 Timer**：无法将 `var cb` 声明为 `func():void`，只能用 `var`（any 类型），丢失编译期类型检查
- **回调存储**：struct 无法声明带类型的回调字段
- **链式调用**：匿名闭包不能直接在调用点传入高阶函数

## 修复方案

`648671a8` 涉及 4 个编译文件 + 2 个测试：

### parser.c — func 关键字预读
```c
case TOK_FUNC: {
    Lexer saved = p->lex;
    lexer_next(&p->lex);
    int is_func_type = (p->lex.current.type == TOK_LPAREN);
    p->lex = saved;
    if (is_func_type) {
        stmt = parse_var_decl_internal(p);  // 函数类型变量声明
    } else {
        stmt = parse_func_stmt(p);          // 函数定义
    }
    break;
}
```
`func` 后面跟 `(` 时识别为函数类型（`func():void v = ...`），走变量声明解析。

### parser_func.c — parse_base_type 优先匹配函数类型
在解析 `TOK_IDENT` 之前，先检查前面是否已解析出函数类型关键字 `func`，避免把 `func():void` 拆成"自定义 struct func" + "未知的 ()"。

### semantic/semantic_type.c — 函数类型变量/字段推断
`infer_expr_type` 中新增对 `AST_FUNC_DEF` 的处理路径，允许函数类型的变量和 struct 字段正确参与类型推断。

### codegen/codegen_expr.c — 函数类型字段代码生成
struct 字段支持生成函数类型（`TYPE_FUNCTION`）的初始化代码。

## 环境

- 提交: 3ddff11f → 648671a8
- 测试: `examples/func/func类型/test_func_type.leno`
- 新增: `examples/func/func类型/test_func_type_var_field.leno`

## 状态

- [x] 已修复 — `648671a8`
