# Bug: 单独编译与模块导入的前向引用行为不一致

## 复现

`sdl_font.leno` 中，struct 方法（第 89 行开始）调用了后面定义的 `ttfInit()`（第 261 行）和 `packColor()`（第 286 行）：

```
第 85 行: export struct Font {
第 89 行:     func load(...): bool {
第 89 行:         if not ttfInit() { ... }   // ❌ 单独编译报错
第 106 行:         return ... packColor(...)  // ❌ 单独编译报错
            ...
           }
           ...

第 261 行: export func ttfInit(): bool { ... }
第 286 行: export func packColor(int r, int g, int b): int { ... }
```

**单独编译** `build/leno.exe sdl_font.leno` → 报"未定义函数"

```
[未定义函数] sdl_font.leno 第 89 行: 未定义的函数: ttfInit
[未定义函数] sdl_font.leno 第 106 行: 未定义的函数: packColor
```

**作为模块被 import** → 不报错

## 原因

两条编译路径处理函数声明的方式不同：

| 路径 | 函数注册时机 | 前向引用 |
|------|------------|---------|
| 单独编译 | 按 AST 顺序逐个处理，struct 方法先于后面函数 | ❌ 报错 |
| 模块导入 | `module_symbol_table_scan()` 预扫描所有 `export func` | ✅ 不报错 |

## 影响

- 同一文件单独编译失败但 import 成功，开发者体验不一致
- 只有模块文件（纯提供函数/类型、没有 main()）容易踩坑

## 修复方向

单独编译路径也增加预扫描，在处理 AST_BLOCK 之前先遍历一遍收集所有函数定义：

1. 在 `visit(s, block)` 之前，遍历 block 收集所有 `AST_FUNC_DEF`
2. 用 `func_table_add()` 提前注册到 func_table
3. 之后 struct 方法访问函数时就能通过 func_table 找到

或者更简单的：单独编译时复用模块导入路径的符号表预扫描逻辑。

## 临时规避

把被 struct 方法引用的函数定义移到 struct 前面。
