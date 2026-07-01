# Bug: struct 前向引用在函数作用域内失效

## 现象

函数定义在 struct 之前时，函数体内无法访问 struct 字段：

```leno
// ❌ 报错："无法确定字段索引，struct 类型可能未定义"
func makePixel(int r, int g, int b): Pixel {
    var p = new Pixel()
    p.r = r     // ← 这里报错
    p.g = g
    p.b = b
    return p
}

struct Pixel { int r; int g; int b }

main() {
    var p = makePixel(255, 128, 64)
    print(p.r)   // 这里正常
}
```

如果 struct 定义在函数之前则正常（`makePixel` 后，`struct Pixel` 前）。

## 根因分析

编译器已有**块级预扫描**（`src/semantic/visitinc/visit_block.inc`），会在访问块内语句前先注册所有 struct 名称和字段到全局 `struct_def` 表和**块作用域符号表**。

但函数体有**独立的作用域层级**（`semantic_visit_func.c:395` 创建 `func_scope`，parent = 当前作用域）。当函数体语义分析走到 `visit_field_access.inc` 处理 `p.r` 时：

```c
// visit_field_access.inc:65-68
struct_sym = scope_resolve(s->current, "p");       // 找到变量 p
if (struct_sym->struct_field_count == 0 && obj_type->struct_name) {
    struct_sym = scope_resolve(s->current, "Pixel"); // ← 从函数作用域查 Pixel
}
```

`scope_resolve(s->current, "Pixel")` 从函数作用域开始，按 parent 链向上查找。预扫描注册的 Pixel 在**块作用域**（pre-scan 时的 `s->current`），但函数作用域的 parent 链是否包含块作用域尚不确定——**此处 scope_resolve 可能无法穿透到预扫描注册的位置**。

### 相关代码路径

| 文件 | 行号 | 作用 |
|------|------|------|
| `src/semantic/visitinc/visit_block.inc` | 1-52 | 块级预扫描，注册 struct 名称+字段到作用域 |
| `src/semantic/visitinc/visit_field_access.inc` | 63-74 | 字段访问时 resolve struct 符号 |
| `src/semantic/semantic_visit_func.c` | 394-397 | 创建函数作用域（`scope_new`） |
| `src/scope.c` | 237 | `scope_resolve` 向上遍历 parent 链 |

## 修复方向（待实现）

两个可行方案：

### 方案 A：函数作用域字段注入（推荐）

在 `visit_func_impl` 中，函数作用域创建后（397行之后），扫描返回类型和参数类型中的 struct 引用，将父作用域中预注册的 struct 字段信息**注入到函数作用域**。

```c
// 伪代码 - 在 scope_new 之后
for each struct type in (return_type + param_types) {
    Symbol* ps = scope_resolve(func_scope->parent, struct_name);
    if (ps && ps->struct_field_count > 0) {
        Symbol* fs = scope_define(func_scope, struct_name, SYM_LOCAL);
        copy_struct_fields(ps, fs);
    }
}
```

### 方案 B：完善 scope_resolve 的 parent 链

确保 `scope_new(s->current, 1)` 的 parent 正确指向了预扫描注册所在的块作用域，使 `scope_resolve` 的递归查找能穿透所有层级。

## 测试用例

`leno_module/LenoSDL3/examples/test_forward_ref.leno`

## 修复后预期

```leno
// ✅ 修复后应正常工作
func makePixel(int r, int g, int b): Pixel {
    var p = new Pixel()
    p.r = r     // OK
    p.g = g
    p.b = b
    return p
}
struct Pixel { int r; int g; int b }
```

## 记录日期

2026-07-01
