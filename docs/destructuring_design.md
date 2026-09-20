# Leno 解构声明语法实现文档

## 一、已实现：数组/字典解构（Phase 1）

### 1.1 数组解构（按位置）

```
var[T1, T2, ..., Tn](a, b, ..., n) = expr
```

- `[T1, ..., Tn]`：解构形状，方括号内是每个槽位的期望类型
- `(a, b, ..., n)`：绑定目标，按索引顺序对应
- `expr`：数据源，运行时必须是 Array

### 1.2 字典解构（按键名）

```
var{"k1": T1, "k2": T2, ...}(a, b, ...) = expr
```

- `{"k1": T1, ...}`：解构形状，花括号内是键名:类型对
- `(a, b, ...)`：绑定目标，按花括号内顺序对应
- `expr`：数据源，运行时必须是 Dict

### 1.3 已实现规则

1. `[...]` 内的类型数量必须与 `(...)` 内的变量名数量一致
2. `var` / `const` 关键字是必须的（声明新变量）
3. 不支持嵌套解构（`var[int, [int, int]](a, b, c) = expr` 不合法）
4. 不支持剩余元素 `...rest`（预留扩展）
5. 槽位类型支持：`int`、`float`、`string`、`bool`、`any`、`var`、alias 类型

### 1.4 已实现类型检查规则（any 槽位优先）

| 源类型 (init) | 槽位类型 | 行为 | 示例 |
|---|---|---|---|
| `Array[int]` | `int` | ✓ 兼容 | `var[int](a) = [1, 2]` |
| `Array[int]` | `float` | ✓ int→float 提升 | `var[float](a) = [1, 2]` |
| `Array[int]` | `string` | ✗ 不兼容 | `var[string](a) = [1, 2]` |
| `Array[int]` | `any` | ✓ 跳过检查，不转换 | `var[any](a) = [1, 2]` |
| `Array[any]` | `any` | ✓ 跳过检查，不转换 | `var[any, any](a, b) = [1, "x"]` |
| `Array[any]` | `int` | ✗ 源 any 不能转具体类型 | `var[int](a) = [1, "x"]` |
| `int` (非Array) | 任何 | ✗ 不是数组 | `var[int](a) = 42` |
| `TYPE_INFER` | `any` | ✗ 无法确认是 Array | `var[any](a) = no_return_func()` |

同样适用于字典解构（`Dict[string, T]` + `{"key": any}` 等）。

### 1.5 已实现 Codegen

```
var[int, float, string](a, b, c) = arr
```

生成序列（实际实现，省略长度检查，靠 OP_INDEX 越界报错）：

```
gen_expr(arr)                     // 压入数组
OP_DUP                            // 复制引用
OP_CONST 0                        // 索引 0
OP_INDEX                          // arr[0]
OP_CAST_INT                       // 转为 int
OP_SET_LOCAL_POP a                // 存入 a

OP_DUP                            // 复制引用
OP_CONST 1
OP_INDEX                          // arr[1]
OP_CAST_FLOAT                     // 转为 float
OP_SET_LOCAL_POP b

OP_DUP
OP_CONST 2
OP_INDEX                          // arr[2]
OP_CAST_STRING
OP_SET_LOCAL_POP c

OP_POP                            // 弹出原始数组
```

any 槽位不生成 CAST 指令（直接取值）。

---

## 二、已实现：函数多返回值（Phase 2）

### 2.0 设计动机

当前函数返回多值的唯一方式是包装成 `Array[T]` 或 `Dict[string, T]`：

```
// 现有方式：返回 Array，所有元素必须同类型
func compute(): Array[int] {
    return [100, 200]    // 只能返回同类型
}

// 问题：返回不同类型时变成 Array[any]，丢失类型安全
func get_info(): Array[any] {
    return [42, "hello", 3.14]   // 类型信息丢失
}
```

**目标**：函数直接返回多个不同类型的值，编译时保证类型安全：

```
// 新方式：多返回值，每个值有独立的类型
func get_info(): [int, string, float] {
    return 42, "hello", 3.14
}

var[int, string, float](id, name, score) = get_info()
// id=42(int)  name="hello"(string)  score=3.14(float)
```

### 2.1 语法规格

#### 2.1.1 多返回值类型声明（按位置）

```
func name(params): [T1, T2, ..., Tn] {
    return expr1, expr2, ..., exprn
}
```

- `[T1, ..., Tn]`：多返回值类型列表，方括号内是每个返回值的类型
- `return expr1, ..., exprn`：返回多个值，逗号分隔，按位置对应

#### 2.1.2 多返回值类型声明（按键名，可选）

```
func name(params): {"k1": T1, "k2": T2, ...} {
    return expr1, expr2, ...
}
```

- `{"k1": T1, ...}`：带键名的多返回值类型，键名仅用于文档和错误提示
- `return expr1, ...`：仍按位置对应（键名不改变返回顺序）

#### 2.1.3 接收多返回值

复用解构声明语法：

```
// 按位置接收
var[int, string, float](a, b, c) = get_info()

// 按键名接收（键名需与函数声明匹配）
var{"id": int, "name": string, "score": float}(a, b, c) = get_info()

// 部分接收（只取前 N 个）
var[int, string](a, b) = get_info()

// any 槽位接收（不转换）
var[any, any, any](a, b, c) = get_info()
```

#### 2.1.4 与单返回值的兼容

- `func f(): int { return 42 }` — 单返回值，现有语法不变
- `func f(): [int] { return 42 }` — 单返回值的多返回值语法（等价，但风格不推荐）
- 普通函数调用 `var x = f()` 仍只取第一个返回值（如果有多个，其余被丢弃）

### 2.2 语法层（Parser）

#### 2.2.1 多返回值类型解析

在 `parse_func_stmt`（`parser_func.c`）中，解析返回类型时：

```
parse_func_stmt:
    ...
    consume RPAREN  // 参数列表结束
    
    if (current == TOK_COLON):
        consume COLON
        if (current == TOK_LBRACKET):
            → parse_multi_return_type(p)    // ★ 新增
        elif (current == TOK_LBRACE):
            → parse_multi_return_type(p)    // ★ 新增（带键名）
        else:
            return_type = parse_type(p)     // 原有：单返回类型
```

`parse_multi_return_type` 流程：

```
parse_multi_return_type(p):
    is_named = (token == TOK_LBRACE)
    consume LBRACKET or LBRACE
    
    count = 0
    loop:
        if is_named:
            parse STRING → key
            expect COLON
        parse_type → ret_type[count]
        count++
        if COMMA: continue
        else: break
    consume RBRACKET or RBRACE
    
    // 创建 TYPE_MULTI_RET 类型
    type = type_new(TYPE_MULTI_RET)
    type->param_types = ret_type[]   // 复用 param_types 字段存储返回类型列表
    type->param_count = count        // 复用 param_count 存储返回值数量
    type->struct_name = keys[]       // 如果有键名，存储为 JSON 字符串（可选）
    return type
```

#### 2.2.2 return 多值解析

在 `parse_return_stmt`（`parser_stmt_other.c`）中：

```
parse_return_stmt(p):
    consume RETURN
    
    if (current is not SEMI/RBRACE):
        expr = parse_expression(p)
        
        // ★ 新增：检测多值返回
        if (current == TOK_COMMA):
            exprs[0] = expr
            count = 1
            while (match COMMA):
                exprs[count] = parse_expression(p)
                count++
            // 创建 AST_RETURN_MULTI 节点
            ast = ast_new(AST_RETURN_MULTI, line)
            ast->u.ret_multi.exprs = exprs
            ast->u.ret_multi.count = count
        else:
            // 原有：单值返回
            ast = ast_new(AST_RETURN, line)
            ast->u.ret = expr
    else:
        // 原有：无返回值
        ast = ast_new(AST_RETURN, line)
        ast->u.ret = NULL
```

#### 2.2.3 AST 扩展

新增枚举值：

```c
AST_RETURN_MULTI,   // 多值返回: return a, b, c
```

新增 union 字段：

```c
struct {
    Ast** exprs;     // 返回值表达式数组
    int count;       // 返回值数量
} ret_multi;
```

### 2.3 类型系统（TypeInfo）

#### 2.3.1 新增 TypeKind

```c
TYPE_MULTI_RET,     // 多返回值类型 [T1, T2, ...] 或 {"k": T1, ...}
```

#### 2.3.2 TypeInfo 复用现有字段

`TYPE_MULTI_RET` 类型的 TypeInfo 复用现有字段：
- `param_types[]`：存储返回值类型数组（`param_types[0]` 是第一个返回值类型）
- `param_count`：返回值数量
- 不使用 `return_type`、`element_type`、`key_type`、`value_type`

#### 2.3.3 新增辅助函数

```c
// 创建多返回值类型
TypeInfo* type_multi_ret(TypeInfo** ret_types, int count);

// 判断是否是多返回值类型
int is_multi_ret_type(TypeInfo* type);
```

#### 2.3.4 type_free / type_copy / type_equals / type_to_string 扩展

在 `type.c` 中为 `TYPE_MULTI_RET` 添加分支：
- `type_free`：释放 `param_types[]`
- `type_copy`：深拷贝 `param_types[]`
- `type_equals`：比较 `param_count` 和每个 `param_types[i]`
- `type_to_string`：输出 `[T1, T2, ...]`

### 2.4 语义层（Semantic）

#### 2.4.1 return 多值类型检查

在 `visit_stmt.inc` 的 `case AST_RETURN:` 后新增 `case AST_RETURN_MULTI:`：

```
case AST_RETURN_MULTI:
    // 1. 访问每个返回值表达式
    for i in 0..count:
        visit(s, exprs[i])
    
    // 2. 获取函数声明的返回类型
    expected = s->current_func->return_type
    
    // 3. 如果函数声明是多返回值类型
    if expected->kind == TYPE_MULTI_RET:
        // 检查返回值数量匹配
        if count != expected->param_count:
            error("多返回值数量不匹配：期望 %d 个，实际 %d 个")
        
        // 逐个检查类型兼容性
        for i in 0..count:
            actual = infer_expr_type(exprs[i])
            if !type_is_compatible(expected->param_types[i], actual):
                error("返回值 %d 类型不匹配：期望 %s，实际 %s")
    
    // 4. 如果函数声明是单返回值类型但 return 了多个值
    elif expected && expected->kind != TYPE_MULTI_RET:
        error("函数声明返回单个值，但 return 语句返回了 %d 个值")
    
    // 5. 如果函数无返回类型注解（TYPE_INFER）
    else:
        // 不报错，允许无注解的函数使用多返回值
        // 但调用方解构时会被拦截（TYPE_INFER 不允许解构）
```

#### 2.4.2 多返回值函数调用的类型推断

在 `semantic_type.c` 的 `case AST_CALL:` 中，当从函数定义获取返回类型时：

```
// 现有逻辑（semantic_type.c 第 1099-1108 行）:
if (func_def->u.func.return_type) {
    ret = type_copy(func_def->u.func.return_type);
} else {
    ret = type_new(TYPE_ANY);
}

// ★ 新增：如果是多返回值类型，直接返回 TYPE_MULTI_RET
// （调用方通过 is_multi_ret_type 判断是否走多值解构路径）
```

无需修改——`type_copy` 会正确复制 `TYPE_MULTI_RET` 类型。

#### 2.4.3 解构声明接收多返回值的类型检查

在 `visit_var.inc` 的 `case AST_DESTRUCT_DECL:` 中，现有逻辑已处理 `init_type->kind` 的各种情况。
新增对 `TYPE_MULTI_RET` 的处理：

```
// 在数组解构和字典解构的判断之前，新增多返回值解构分支
if (init_type->kind == TYPE_MULTI_RET) {
    // 多返回值解构：按位置匹配
    int ret_count = init_type->param_count;
    
    for (int i = 0; i < slot_count; i++) {
        TypeInfo* slot_type = slot_types[i];
        if (!slot_type) continue;
        
        // any 槽位：总是兼容，跳过检查
        if (slot_kind == TYPE_ANY || slot_kind == TYPE_INFER) continue;
        
        // 具体类型槽位：检查与对应返回值类型的兼容性
        if (i >= ret_count) {
            error("解构槽位 %d 超出返回值数量 %d", i, ret_count);
        } else if (!type_is_compatible(slot_type, init_type->param_types[i])) {
            error("解构槽位 %d 类型 %s 与返回值类型 %s 不兼容",
                  i, slot_type, init_type->param_types[i]);
        }
    }
}
```

### 2.5 代码生成层（Codegen）

#### 2.5.1 多值 return 字节码生成

在 `codegen_stmt.c` 新增 `gen_return_multi(CodeGen* gen, Ast* ast)`：

```
gen_return_multi(gen, ast):
    // 1. 求值所有返回值表达式，依次压栈
    for i in 0..count:
        gen_expr(exprs[i])
    
    // 2. 发射 OP_RETURN_MULTI 指令
    OP_RETURN_MULTI count
```

字节码序列：

```
// return 42, "hello", 3.14
gen_expr(42)          // 压入 42
gen_expr("hello")     // 压入 "hello"
gen_expr(3.14)        // 压入 3.14
OP_RETURN_MULTI 3     // 返回 3 个值
```

#### 2.5.2 多返回值函数调用的字节码生成

在 `gen_destruct_decl` 中，现有逻辑是：
1. `gen_expr(init)` — 求值 init（压入数组/字典）
2. 循环 `OP_DUP + OP_CONST + OP_INDEX + OP_CAST + OP_SET_LOCAL_POP`

对于多返回值，需要新增路径：

```
gen_destruct_decl(gen, ast):
    ...
    gen_expr(init)    // 求值 init → 如果是函数调用，栈上是多个返回值
    
    // ★ 检测 init 是否是多返回值函数调用
    if (is_multi_ret_call(init)):
        // 多返回值已在栈上，直接逐个取值
        for i in 0..slot_count:
            // 栈布局: [ret0][ret1][ret2]...
            // 需要从底部取第 i 个值
            OP_GET_RET_VALUE i    // ★ 新指令：取第 i 个返回值
            // 或用 OP_GET_LOCAL 临时槽位方式
            
            // 类型转换
            if slot_type == TYPE_FLOAT: OP_CAST_FLOAT
            elif slot_type == TYPE_INT: OP_CAST_INT
            elif slot_type == TYPE_STRING: OP_CAST_STRING
            
            OP_SET_LOCAL_POP name[i]
        
        // 清理栈上剩余返回值
        OP_POP_N remaining_count
    else:
        // 原有逻辑：数组/字典解构
        ...
```

**替代方案（更简洁，推荐）**：将多返回值先存入临时局部变量槽位，再走索引取值：

```
// 方案 B：临时槽位 + 索引取值（复用现有 OP_GET_LOCAL）
gen_expr(init)    // 函数调用后栈上是 N 个返回值

// 将 N 个返回值存入临时槽位（逆序弹出）
for i = N-1 downto 0:
    OP_SET_LOCAL_POP temp_slot[i]

// 逐槽位从临时槽位加载 + 转换 + 存储
for i in 0..slot_count:
    OP_GET_LOCAL temp_slot[i]
    // 类型转换...
    OP_SET_LOCAL_POP name[i]

// 无需清理（临时槽位在函数结束时自动回收）
```

方案 B 的优势：**不新增 VM 指令**，完全复用 `OP_GET_LOCAL` / `OP_SET_LOCAL_POP`。

#### 2.5.3 尾调用优化

多返回值的 `return f()` 不能使用 `OP_TAIL_CALL`（因为尾调用假设单返回值）。
在 `is_tail_call` 检查中排除多返回值场景。

### 2.6 VM 层

#### 2.6.1 新增 OP_RETURN_MULTI 指令

```c
OP_RETURN_MULTI,    // 多值返回: OP_RETURN_MULTI count(1)
```

**字节格式**：`OP_RETURN_MULTI` (1 byte) + `count` (1 byte)

**VM 实现**：

```c
OPCODE(OP_RETURN_MULTI) {
    frame = &vm.frames[vm.frame_cnt - 1];
    int ret_count = READ_BYTE();  // 读取返回值数量
    
    // GC 安全点检查（同 OP_RETURN）
    ...
    
    // 从栈上弹出 count 个返回值（保存到临时数组）
    Value results[MAX_MULTI_RET];
    for (int i = ret_count - 1; i >= 0; i--) {
        results[i] = vm_stack_pop_fast(&vm);
    }
    
    // 关闭 upvalues（同 OP_RETURN）
    ...
    
    // 恢复栈到调用前状态
    vm.frame_cnt--;
    vm.sp = frame->stack_base;
    
    // 将多个返回值压回栈
    for (int i = 0; i < ret_count; i++) {
        vm_stack_push(&vm, results[i]);
    }
    
    // 释放 locals
    ...
    
    frame = &vm.frames[vm.frame_cnt - 1];
    DISPATCH;
}
```

**关键点**：与 `OP_RETURN` 的区别在于——`OP_RETURN` 弹出一个值后 `vm.sp = stack_base` 再压入一个值；
`OP_RETURN_MULTI` 弹出 N 个值后 `vm.sp = stack_base` 再压入 N 个值。
调用方栈上留下 N 个值。

#### 2.6.2 栈布局变化

**单返回值（现有）**：
```
调用前:  [...][arg1][arg2][callee]
                    ^stack_base
返回后:  [...][result]
                    ^stack_base
```

**多返回值（新增）**：
```
调用前:  [...][arg1][arg2][callee]
                    ^stack_base
返回后:  [...][result0][result1][result2]
                    ^stack_base
```

调用方通过临时槽位方案将多个返回值逐个存入局部变量。

#### 2.6.3 OP_CALL 无需修改

`OP_CALL` 调用函数后，VM 的 `frame` 切换到被调用函数。
被调用函数执行 `OP_RETURN_MULTI` 后，栈上留下多个值。
`OP_CALL` 本身不需要知道返回值数量——调用方 codegen 知道。

#### 2.6.4 普通调用场景的兼容

如果一个多返回值函数被普通调用（非解构接收）：
```
var x = get_info()   // 只取第一个返回值
```

Codegen 在 `gen_expr(AST_CALL)` 后压入所有返回值，但 `gen_var_decl` 只取栈顶一个值。
**需要在 codegen 中处理**：如果是多返回值函数调用且非解构接收，在 `OP_CALL` 后发射 `OP_POP_N (count-1)` 弹出多余的返回值。

或者更简单：**`OP_RETURN_MULTI` 时检查调用方是否期望多值**。
但这需要 `OP_CALL` 携带"期望返回值数量"参数，增加复杂度。

**推荐方案**：在 codegen 层处理。`gen_call` 之后，如果调用方不是解构声明，插入 `OP_POP_N` 弹出多余值。

### 2.7 错误处理

#### 2.7.1 编译期错误

| 场景 | 错误信息 |
|------|----------|
| `return a, b` 但函数声明 `: int`（单返回值） | "函数声明返回单个值，但 return 语句返回了 2 个值" |
| `return a` 但函数声明 `: [int, string]`（多返回值） | "函数声明返回 2 个值，但 return 语句只返回了 1 个值" |
| `return a, b` 数量 ≠ `: [int, string, float]` 数量 | "多返回值数量不匹配：期望 3 个，实际 2 个" |
| 返回值类型不匹配 | "返回值 1 类型不匹配：期望 string，实际 int" |
| 解构槽位超出返回值数量 | "解构槽位 3 超出返回值数量 2" |
| 多返回值函数用于表达式（非解构） | 警告："多返回值函数在表达式中调用，只取第一个返回值" |

#### 2.7.2 运行期错误

| 场景 | 错误来源 | 行为 |
|------|----------|------|
| 返回值数量不足解构槽位 | codegen 检查 | 编译期拦截 |
| 类型转换失败 | `OP_CAST_INT` 等 | 报错 "类型转换 int: 操作数必须是数字" |

### 2.8 实现步骤

| 步骤 | 文件 | 内容 |
|------|------|------|
| 1 | `leno_types.h` | 新增 `TYPE_MULTI_RET` 枚举值 |
| 2 | `leno_ast.h` | 新增 `AST_RETURN_MULTI` 枚举值和 `ret_multi` union 字段 |
| 3 | `type.c` | 新增 `type_multi_ret()`；`type_free/copy/equals/to_string` 添加 `TYPE_MULTI_RET` 分支 |
| 4 | `parser_func.c` | 新增 `parse_multi_return_type()`；在 `parse_func_stmt` 中检测 `:` 后的 `[` / `{` |
| 5 | `parser_stmt_other.c` | `parse_return_stmt` 中检测逗号分隔的多值返回 |
| 6 | `visit_stmt.inc` | 新增 `case AST_RETURN_MULTI:` 类型检查 |
| 7 | `visit_var.inc` | `case AST_DESTRUCT_DECL:` 中新增 `TYPE_MULTI_RET` 分支 |
| 8 | `semantic_type.c` | `AST_CALL` 类型推断返回 `TYPE_MULTI_RET`（已由 `type_copy` 自然支持） |
| 9 | `codegen_stmt.c` | 新增 `gen_return_multi()`；`gen_destruct_decl` 中新增多返回值路径 |
| 10 | `codegen_expr.c` | `gen_call` 后处理多余返回值（非解构场景） |
| 11 | `leno_vm.h` | 新增 `OP_RETURN_MULTI` 操作码 |
| 12 | `op_call.inc` | 实现 `OPCODE(OP_RETURN_MULTI)` |
| 13 | `codegen_inline.c` | `patch_ast_indices` 和 `body_has_unsupported` 添加 `AST_RETURN_MULTI` |
| 14 | `ast.c` | `ast_free` 添加 `AST_RETURN_MULTI` 释放 |
| 15 | `leno_value.h` | `ObjFunction` 新增 `ret_count` 字段（可选，用于运行时检查） |

### 2.9 复杂度评估

| 维度 | 评估 |
|------|------|
| 类型系统 | 新增 1 个 `TYPE_MULTI_RET`，复用 `param_types/param_count` 字段 |
| AST | 新增 1 个 `AST_RETURN_MULTI`，1 个 union 字段 |
| Parser | ~60 行（`parse_multi_return_type` + return 多值检测） |
| Semantic | ~50 行（return 类型检查 + 解构 `TYPE_MULTI_RET` 分支） |
| Codegen | ~60 行（`gen_return_multi` + `gen_destruct_decl` 多返回值路径 + `gen_call` 弹出多余值） |
| VM | ~30 行（`OP_RETURN_MULTI` 实现） |
| 总计 | ~200 行新代码 |

**引入 1 个新类型、1 个新 AST 节点、1 个新 VM 操作码。不修改现有类型系统逻辑。**

---

## 三、完整测试用例规划

### 3.1 已实现测试（Phase 1）

- `test_destruct.leno` — 数组/字典解构、类型转换、const、alias、any 槽位、部分解构
- `test_destruct_sdl3_sim.leno` — SDL3 场景模拟（多元素数组、函数返回值、循环内解构）
- `destruct_type_check_test.leno` — 编译时类型检查（期望编译失败的 9 个测试）
- `test_destruct_export.leno` — 跨模块导出解构
- `test_destruct_mod_var.leno` — 模块变量解构

### 3.2 已实现测试（Phase 2）

```
// 基本多返回值
func get_user(): [int, string, float] {
    return 42, "Alice", 95.5
}
var[int, string, float](id, name, score) = get_user()

// 带键名的多返回值
func parse_url(): {"host": string, "port": int, "path": string} {
    return "localhost", 8080, "/api"
}
var{"host": string, "port": int, "path": string}(host, port, path) = parse_url()

// 部分接收
var[int, string](id, name) = get_user()   // 只取前 2 个

// any 槽位接收
var[any, any, any](a, b, c) = get_user()

// int → float 类型提升
func get_dims(): [int, int] {
    return 100, 200
}
var[float, float](w, h) = get_dims()   // int 自动提升为 float

// 循环内多返回值解构
func get_frame_data(int frame): [float, float, float] {
    return frame * 1.0, frame * 2.0, frame * 3.0
}
for 5 to i {
    var[float, float, float](d1, d2, d3) = get_frame_data(i)
}

// 普通调用（只取第一个返回值）
var x = get_user()   // x = 42，其余返回值被丢弃
```

### 3.3 编译时类型检查测试（Phase 2）

```
// 返回值数量不匹配
func f(): [int, string] { return 42 }              // ✗ 期望 2 个，实际 1 个
func g(): [int, string] { return 42, "x", 3.14 }   // ✗ 期望 2 个，实际 3 个

// 返回值类型不匹配
func f(): [int, string] { return 42, 100 }          // ✗ 返回值 1 类型不匹配：期望 string，实际 int

// 解构槽位超出返回值数量
func f(): [int, string] { return 42, "x" }
var[int, string, float](a, b, c) = f()              // ✗ 解构槽位 2 超出返回值数量 2

// 解构槽位类型不匹配
func f(): [int, string] { return 42, "x" }
var[string, int](a, b) = f()                        // ✗ 槽位 0 类型 string 与返回值类型 int 不兼容
```

---

## 四、设计决策记录

### 4.1 为什么不引入 tuple 类型？

多返回值不需要一等公民的 tuple 类型：
- tuple 作为值在栈上传递，需要内存分配
- 多返回值只在"函数返回 → 解构接收"这一瞬间存在，不需要持久化
- 直接在栈上留多个值，零分配，最高效

如果用户需要持久化多返回值，可以显式包装：
```
var result = [get_user()]   // 包装成数组
```

### 4.2 为什么用 `[T1, T2]` 而不是 `(T1, T2)`？

- `(T1, T2)` 与函数参数列表语法冲突
- `[T1, T2]` 与现有解构形状语法一致（`var[T1, T2](a, b) = ...`）
- `{"k": T}` 是字典格式的自然扩展，与字典解构形状一致

### 4.3 为什么 return 用逗号而不是数组？

```
// 选择：return 42, "hello", 3.14
// 而非：return [42, "hello", 3.14]
```

- `return [42, "hello", 3.14]` 会创建 Array 对象（堆分配），且类型为 `Array[any]`
- `return 42, "hello", 3.14` 直接在栈上留 3 个值，零分配，类型安全

### 4.4 为什么不用 OP_RETURN 携带数量？

考虑过让 `OP_RETURN` 携带返回值数量参数，但这会改变 `OP_RETURN` 的字节格式（现有 1 byte → 2 bytes），
影响所有现有函数的性能（指令解码开销）。

新增独立的 `OP_RETURN_MULTI` 更干净：
- 现有单返回值函数不受影响
- 多返回值是显式选择，零性能退化

### 4.5 方括号 vs 花括号的语义区别

| 语法 | 含义 | 接收方式 |
|------|------|----------|
| `func f(): [int, string]` | 按位置的多返回值 | `var[int, string](a, b) = f()` |
| `func f(): {"code": int, "msg": string}` | 带键名的多返回值 | `var{"code": int, "msg": string}(a, b) = f()` |

两者运行时机制完全相同（栈上留多个值）。区别仅在于：
- 花括号格式提供键名，用于文档和错误提示
- 接收端键名需与声明匹配（编译时检查）

### 4.6 与现有 Array/Dict 返回值解构的共存

现有方式（返回 Array/Dict 后解构）和新方式（多返回值解构）完全独立、共存：

```
// 方式 A：返回 Array（现有，所有元素同类型）
func f(): Array[int] { return [1, 2, 3] }
var[int, int, int](a, b, c) = f()    // init_type = TYPE_ARRAY

// 方式 B：多返回值（新，每个值不同类型）
func f(): [int, string, float] { return 1, "x", 3.14 }
var[int, string, float](a, b, c) = f()   // init_type = TYPE_MULTI_RET
```

解构声明通过 `init_type->kind` 区分走哪条路径：
- `TYPE_ARRAY` → 现有数组解构路径
- `TYPE_DICT` → 现有字典解构路径
- `TYPE_MULTI_RET` → 新增多返回值解构路径

### 4.7 非解构上下文中的自动退化

当多返回值函数在**非解构上下文**中被调用时（算术运算、条件判断、函数参数、普通变量声明等），
自动取**第一个返回值**，丢弃其余返回值。

```
func get_info(): [int, string, float] {
    return 42, "hello", 3.14
}

var y = get_info() + 1        // y = 43（取第一个值 42，丢弃 "hello" 和 3.14）
var first = get_info()         // first = 42
if get_info() > 0 { ... }     // 42 > 0 = true

func add(int a, int b): int { return a + b }
var result = add(get_info(), 10)  // add(42, 10) = 52
```

**实现机制**：

1. **Codegen 层**：`gen_expr` 的 `AST_CALL` case 在 `gen_call` 返回后检查 `cached_type`。
   如果是 `TYPE_MULTI_RET` 且 `suppress_multi_pop` 未设置，则 emit `OP_POP` 弹出多余的返回值，
   只保留第一个返回值在栈顶。

2. **调用者控制**：三个调用者通过 `suppress_multi_pop` 标志控制弹出行为：
   - `gen_var_decl`：设置 `suppress_multi_pop=1`，由自己统一弹出多余值（保留第一个）
   - `gen_destruct_decl`：设置 `suppress_multi_pop=1`，保留所有返回值用于解构
   - `gen_expr_stmt`：设置 `suppress_multi_pop=1`，由自己弹出所有返回值

3. **类型系统层**：`type_is_compatible` 在 source 是 `TYPE_MULTI_RET` 时，
   自动退化为第一个元素类型进行比较，使多返回值函数可以直接作为单参数传递。

### 4.8 已知限制

| 限制 | 性质 | 说明 |
|------|------|------|
| 多返回值最多 16 个 | 设计限制 | `OP_RETURN_MULTI` 操作数为 1 byte，最大 255；实际限制为 16 以保持栈安全 |
| 多返回值函数不被内联 | 设计限制 | `OP_RETURN_MULTI` 需要真实调用帧操作栈，内联会绕过此机制 |
| 多返回值不能直接解构嵌套 | 设计限制 | 不支持 `var[int, [int, int]](a, b, c) = f()`，无嵌套解构 |
| 闭包返回多返回值时类型可能退化为 any | 实现限制 | 闭包类型推断不完整时，`cached_type` 可能为 `TYPE_ANY` 而非 `TYPE_MULTI_RET`，导致 codegen 无法弹出多余值。解构声明仍可工作（走 `TYPE_ARRAY` 路径），但非解构上下文中会产生栈上多余值 |

---

## 五、已实现：var[] 空形状自动推断（Phase 3）

### 5.0 设计动机

Phase 1/2 的解构声明要求用户在 `[...]` 内显式写出每个槽位的类型：

```leno
var[float, float, float, float](x, y, w, h) = getRect()
var[int, string, float](id, name, score) = getUser()
```

但函数的返回类型已经在声明中写过了，调用方再写一遍是冗余的。`var[]` 空形状语法允许省略类型标注，由编译器自动推断。

### 5.1 语法

```leno
// 空数组形状：var[](name1, name2, ...) = expr
// 等价于省略所有槽位类型，编译器从 init_type 自动推断

// 多返回值自动推断
var[](x, y, w, h) = getRect()     // 等价于 var[float, float, float, float](...)

// 数组解构自动推断
var[](a, b, c) = [1, 2, 3]        // 等价于 var[int, int, int](...)

// const 也支持
const[](cx, cy) = getRect()

// 部分解构（只取前 N 个）
var[](first, second) = getUser()  // 只取前 2 个返回值
```

### 5.2 不支持的场景

- **字典解构不支持空形状**：`var{}(a, b) = expr` 不合法。字典解构必须写键名，因为它语义上是按键名取值，不是按位置。
- **无法推断的源类型**：如果 init 的类型是 `TYPE_ANY` 或 `TYPE_INFER`（如无返回类型注解的函数），编译器报错："解构声明要求初始化值为具体类型的 Array 或多返回值函数"。

### 5.3 实现机制

#### 5.3.1 Parser 层

在 `parse_destruct_decl`（`parser_func.c`）中，当 `[` 后直接遇到 `]`（空形状）时：

1. 消费 `]`
2. 解析 `(name1, name2, ...)` 变量名列表
3. 根据变量名数量，为每个生成 `TYPE_INFER` 槽位
4. 解析 `= expr` 初始值

生成的 `AST_DESTRUCT_DECL` 节点与普通解构声明完全一致，只是所有 `slot_types[i]` 为 `TYPE_INFER`。

#### 5.3.2 Semantic 层

在 `visit_var.inc` 的 `AST_DESTRUCT_DECL` 类型检查中，当 `init_type` 为 `TYPE_MULTI_RET` 或 `TYPE_ARRAY` 且槽位为 `TYPE_INFER` 时：

1. 从 `init_type->param_types[i]`（多返回值）或 `init_type->element_type`（数组）获取类型
2. 回填 `ast->u.destruct_decl.slot_types[i]`（影响 codegen 的 CAST 生成）
3. 通过 `scope_resolve` 获取符号，回填 `sym->type`（影响后续类型检查）

回填后，槽位类型从 `TYPE_INFER` 变为具体类型（如 `TYPE_FLOAT`、`TYPE_INT`），后续 codegen 会根据回填后的类型生成对应的 CAST 指令。

#### 5.3.3 Codegen 层

**无需修改**。Codegen 在 `gen_destruct_decl` 中根据 `slot_type->kind` 生成 CAST 指令。semantic 层回填类型后，codegen 自动走正确路径。

### 5.4 类型推断链路

```
函数声明: func getRect(): [float, float, float, float]
    ↓ type_copy
模块符号表: ModuleFuncSymbol.return_type_info = TYPE_MULTI_RET{param_types[0..3] = float}
    ↓ module_symbol_table_find_func
调用方 infer_expr_type(): 返回 TYPE_MULTI_RET{param_types[0..3] = float}
    ↓ 回填
var[](x, y, w, h) → slot_types[0..3] = float, sym->type = float
    ↓ codegen
OP_GET_LOCAL + OP_CAST_FLOAT + OP_SET_LOCAL_POP（逐槽位）
```

### 5.5 测试

| 测试文件 | 内容 |
|----------|------|
| `assert/test_destruct_auto_infer.leno` | 基本语法：多返回值、混合类型、数组、float 数组、部分解构、const、旧语法兼容、bool 数组、循环内解构 |
| `assert/test_destruct_auto_infer_cross.leno` | 跨模块：多返回值、混合类型、float 多返回值、旧语法兼容 |
| `assert/destruct_auto_infer_mod.leno` | 跨模块测试模块 |

### 5.6 与现有语法的关系

`var[]` 是现有 `var[T1, T2, ...]` 的语法糖，完全向后兼容：

```leno
// 以下两种写法等价
var[](x, y, w, h) = getRect()
var[float, float, float, float](x, y, w, h) = getRect()

// 旧写法完全不受影响
var[int, string](a, b) = [1, "hello"]
var{"id": int, "name": string}(a, b) = getUser()
```

底层走同一套解析、类型检查、codegen 路径，只是空形状时槽位类型从 `TYPE_INFER` → semantic 阶段自动回填为实际类型。
