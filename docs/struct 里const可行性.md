# Leno struct 关联常量（Associated Constants）可行性分析

> 分析日期：2026-08-08
>
> 分析方法：基于源码全链路追踪，从 parser → AST → semantic → codegen → VM → LSP → 模块符号表，逐层验证

---

## 一、当前 struct 成员体系（源码确认）

### 1.1 AST 层

`src/include/leno_ast.h:262-279` —— `struct_def` 联合体：

```c
struct {
    char* name;                    // struct 名称
    char** field_names;           // 字段名称数组
    TypeInfo** field_types;       // 字段类型数组
    Ast** field_defaults;         // 字段默认值表达式数组
    int field_count;              // 字段数量
    Ast** methods;                // 方法定义数组（AST_FUNC_DEF）
    int method_count;             // 方法数量
    char** impl_names;            // impl 声明的 face 名称数组
    int impl_count;               // impl 声明数量
    // 泛型类型参数
    char** type_params;
    char** type_param_constraints;
    char** type_param_defaults;
    int type_param_count;
} struct_def;
```

**结论**：AST 层只有字段、方法、impl 三类成员，无 const 相关字段。

### 1.2 运行时对象层

`src/include/leno_value.h:558-575` —— `ObjStructDef`：

```c
typedef struct {
    Object header;
    char* name;
    StructFieldInfo* fields;
    int field_count;
    StructMethodInfo* methods;
    int method_count;
    char** impl_names;
    int impl_count;
    int type_param_count;
    char** type_param_names;
    char** type_param_constraints;
    int has_ctor;
    int ctor_index;
    int has_dtor;
    int dtor_index;
} ObjStructDef;
```

**结论**：运行时定义对象同样只有 fields/methods/impls，无 const 存储。

### 1.3 解析层

`src/parser/parser_func.c:1277-1643` —— `parse_struct_stmt()` 的 while 循环体：

```
while (current != TOK_RBRACE && current != TOK_EOF) {
    if (TOK_ERROR) → 跳过注释
    if (TOK_ASYNC) → 消费 async，期望 func（方法）
    if (TOK_FUNC)  → 预读判定方法定义 vs 函数类型字段
    // fall through → parse_type() → 字段类型 → 字段名 → 默认值
}
```

**关键发现**：循环中 **没有 `TOK_CONST` 分支**。如果用户在 struct 体内写 `const MAX = 256`，`TOK_CONST` 会落入 `parse_type()` 路径，由于 `const` 不是合法的类型关键字，将报错"期望字段类型"。

这证实了：**struct 内部当前完全不支持 const 声明**。

### 1.4 字段访问层

`src/vm/vminc/op_struct.inc:528-743`：

```c
OPCODE(OP_GET_FIELD) {
    uint8_t field_idx = READ_BYTE();   // 编译期确定的字段索引
    Value obj_val = vm_stack_pop();
    ObjStruct* obj = (ObjStruct*)val_as_obj(obj_val);
    Value field_value = struct_get_field(obj, field_idx);
    vm_stack_push(&vm, field_value);
}

OPCODE(OP_SET_FIELD) {
    uint8_t field_idx = READ_BYTE();
    Value value = vm_stack_pop();
    Value obj_val = vm_stack_pop();
    struct_set_field(obj, field_idx, value);  // 无任何不可变性检查
}
```

**关键发现**：`OP_SET_FIELD` **没有任何不可变性检查机制**。如果要实现实例级 const 字段，需要在这里加运行时检查，影响性能且复杂度高。

---

## 二、模块级 const 的现有机制（可复用基础）

### 2.1 解析

`src/parser/parser_func.c:643-755` —— `parse_var_decl_internal()`：

- 遇到 `TOK_CONST` 时设置 `is_const = 1`，消费 `const` 关键字
- 支持类型推断：`const PI = 3.14`（`TYPE_INFER`）
- 支持显式类型：`const int X = 42`
- **const 必须有初始值**，否则报错
- 生成 `AST_VAR_DECL` 节点，`ast->u.var_decl.is_const = is_const`

### 2.2 语义分析

`src/semantic/visitinc/visit_var.inc:383-387`：

```c
// 检查是否对 const 变量赋值
if (sym->is_const) {
    snprintf(msg, sizeof(msg), "不能对 const 变量 '%s' 重新赋值", ...);
    error_add(ERR_SEMANTIC, ast->line, msg);
}
```

### 2.3 跨模块传播

`src/module_symbol_table/inc/scan/scan_var.inc` 扫描 `export const X = ...` 时，能递归解析 `module.member` 形式的初始值，正确传播类型。

已有测试 `assert/test_export_const_type.leno` 验证了跨模块 `export const X = base.Y` 的类型传播。

### 2.4 类型级访问的语义路径（关键复用点）

`src/semantic/visitinc/visit_module.inc:1652-1855` —— `AST_MODULE_ACCESS` 处理：

当解析 `StructName.MEMBER` 时（`StructName` 不是模块别名，而是作用域中的变量/类型符号）：

1. **enum 类型**（行 1714-1765）：查找 `sym->enum_value_names` 数组，找到后将 `AST_MODULE_ACCESS` **直接转换为 `AST_NUM` 常量**。这正是关联常量应该复用的模式。

2. **struct/cstruct/face 类型**（行 1766-1783）：转换为 `AST_FIELD_ACCESS`，即当作"访问 struct 实例的字段"处理。

3. **普通变量**（行 1784-1794）：转换为 `AST_INDEX`（`obj["member"]`）。

**关键洞察**：enum 的 `TypeName.MEMBER` 访问路径已经是"编译期常量解析"的实现。struct 关联常量可以在 enum 路径旁边加一个分支，以相同方式将 `StructName.CONST_NAME` 解析为编译期常量值。

---

## 三、实际代码中的常量散落问题（痛点验证）

### 3.1 LenoSDL3 中的典型模式

**sdl_layout.leno**（布局常量与 HBox/VBox 概念强绑定，却在模块顶层）：

```leno
export const LAYOUT_START   = 0   // 靠起始边
export const LAYOUT_CENTER  = 1   // 居中
export const LAYOUT_END     = 2   // 靠结束边
export const LAYOUT_STRETCH = 3   // 撑满交叉轴

export struct HBox impl Widget {
    float x = 0.0, y = 0.0, w = 300.0, h = 40.0
    // ...
}
```

**sdl_font.leno**（字体样式常量与 Font 概念强绑定）：

```leno
export const STYLE_NORMAL        = 0x00
export const STYLE_BOLD          = 0x01
export const STYLE_ITALIC        = 0x02
export const STYLE_UNDERLINE     = 0x04
export const STYLE_STRIKETHROUGH = 0x08

export const HINTING_NORMAL = 0
export const HINTING_LIGHT  = 1
export const HINTING_MONO   = 2
export const HINTING_NONE   = 3

export const WRAP_ALIGN_LEFT   = 0
export const WRAP_ALIGN_CENTER = 1
export const WRAP_ALIGN_RIGHT  = 2
```

**sdl_splitter.leno**：

```leno
export const SPLIT_HORIZONTAL = 0
```

### 3.2 跨模块重新导出（SDL3.leno 的转发层）

`SDL3.leno` 作为聚合模块，大量转发子模块的常量：

```leno
// Splitter 方向常量
export const SPLIT_HORIZONTAL = spl.SPLIT_HORIZONTAL
export const SPLIT_VERTICAL   = spl.SPLIT_VERTICAL

// 字体样式常量
export const STYLE_NORMAL        = fnt.STYLE_NORMAL
export const STYLE_BOLD          = fnt.STYLE_BOLD
export const STYLE_ITALIC        = fnt.STYLE_ITALIC
export const STYLE_UNDERLINE     = fnt.STYLE_UNDERLINE
export const STYLE_STRIKETHROUGH = fnt.STYLE_STRIKETHROUGH

// 对齐常量
export const HINTING_NORMAL = fnt.HINTING_NORMAL
// ...
```

### 3.3 LenoWin32 中的同类模式

```leno
// w32_hotkey.leno
export const MOD_CONTROL = 0x0002
export const MOD_NOREPEAT = 0x4000

// w32_message.leno  
export const PM_NOREMOVE = 0

// Win32.leno（聚合模块重新导出）
export const HOTKEY_MOD_CONTROL = hotkey.MOD_CONTROL
export const HOTKEY_MOD_NOREPEAT = hotkey.MOD_NOREPEAT
export const KEY_ALL_ACCESS = 0xF003F
```

### 3.4 私有常量的"假作用域"

```leno
// w32_process.leno
const PROCESS_QUERY_INFORMATION = 0x0400    // 本应是 Process 结构的关联常量

// w32_clipboard.leno
const DIB_HEADER_SIZE = 40                  // 本应是 Clipboard 的关联常量

// w32_capture.leno
const DIB_RGB_COLORS = 0                    // 本应是 Capture 的关联常量
```

这些 `const` 用下划线前缀做"假作用域"，但实际暴露在模块顶层，LSP 输入 `Capture.` 看不到它们。

### 3.5 痛点量化

| 痛点 | 具体表现 | 影响程度 |
|------|---------|---------|
| **命名空间扁平** | `LAYOUT_*`、`STYLE_*`、`SPLIT_*`、`HINTING_*` 全部挤在模块顶层，靠前缀区分归属 | 中 |
| **LSP 补全断裂** | 输入 `HBox.` 只能看到字段和方法，看不到 `LAYOUT_START` 等关联常量 | 中 |
| **跨模块转发冗余** | `SDL3.leno` 有 20+ 行纯常量转发，纯噪音 | 中 |
| **封装缺失** | `PROCESS_QUERY_INFORMATION` 等"私有常量"暴露在模块作用域 | 低 |

---

## 四、方案对比：关联常量 vs 实例 const 字段

### 4.1 关联常量（static const，推荐）

```leno
struct HBox impl Widget {
    const LAYOUT_START   = 0    // 关联常量，通过 HBox.LAYOUT_START 访问
    const LAYOUT_CENTER  = 1
    const LAYOUT_END     = 2
    const LAYOUT_STRETCH = 3

    float x = 0.0, y = 0.0
    // ...字段和方法
}
```

### 4.2 实例 const 字段（C++ 风格，不推荐）

```leno
struct Point {
    const int MAX = 256    // 每个实例存一份，构造时设置后不可变
    int x
    int y
}
```

### 4.3 对比

| 维度 | 关联常量（static const） | 实例 const 字段（C++ 风格） |
|------|------------------------|--------------------------|
| **语义** | 属于类型，所有实例共享 | 属于实例，构造时设置后不可变 |
| **存储** | 编译期常量，不占实例内存 | 每个实例各存一份 |
| **AST 改动** | `struct_def` 加 `const_names`/`const_values`/`const_types` 三个数组 | `field_defaults` 加 `is_const` 标志 |
| **运行时改动** | `ObjStructDef` 加 const 元数据，访问走 `TypeName.CONST` 路径 | `OP_SET_FIELD` 加不可变检查，`ObjStruct` 的 `field_values` 需标记某些索引为 const |
| **NaN-boxing 影响** | 无（不改实例内存布局） | 有（需在 `field_values` 旁加 const 标志位） |
| **GC 影响** | 无（常量值是编译期确定） | 有（需追踪每个字段的 const 状态） |
| **实现复杂度** | 中等 | 高 |
| **收益** | 解决命名空间和 LSP 补全 | 额外提供不可变保证，但 Leno 无此痛点 |
| **Leno 实际需求** | LenoSDL3/LenoWin32 有大量散落常量 | 无实际代码受益 |

**结论**：关联常量是正确选择。实例 const 字段需要运行时不可变性追踪，而 Leno 的 `OP_SET_FIELD` 完全没有这套机制，复杂度远超收益。

---

## 五、关联常量的全链路改动评估

### 5.1 Parser 层

**文件**：`src/parser/parser_func.c` —— `parse_struct_stmt()`

**改动**：在 while 循环开头（`TOK_FUNC` 检查之前）加 `TOK_CONST` 分支。**直接复用全局 const 的预读逻辑**，支持两种语法形式：

```c
// 在 while 循环体内，TOK_FUNC 检查之前
if (p->lex.current.type == TOK_CONST) {
    lexer_next(&p->lex); // 消费 const

    // ★ 复用 parse_var_decl_internal 行 654-661 的预读逻辑 ★
    // const 后面可能是：
    //   形式1: NAME = value     → 类型推断（如 const MAX = 256）
    //   形式2: TYPE NAME = value → 显式类型（如 const int LIMIT = 100）
    TypeInfo* const_type = NULL;
    if (p->lex.current.type == TOK_IDENT) {
        Lexer saved_lex = p->lex;
        lexer_next(&p->lex);
        LenoTokenType peek = p->lex.current.type;
        p->lex = saved_lex;
        if (peek == TOK_EQ || peek == TOK_COMMA) {
            const_type = type_new(TYPE_INFER);  // 类型推断
        }
    }
    if (!const_type) {
        const_type = parse_type(p);  // 显式类型
    }

    // 解析常量名
    char* const_name = copy_string(p->lex.current.text, p->lex.current.len);
    lexer_next(&p->lex);
    // 期望 =
    if (!match(p, TOK_EQ)) { error_add(...); continue; }
    // 解析常量值表达式
    Ast* const_expr = parse_expression(p);
    // 存入 const_names / const_values / const_types 数组
    // ...
    continue;
}
```

**难度**：低。预读逻辑直接从 `parse_var_decl_internal` 复制，保证两种上下文行为完全一致。

### 5.2 AST 层

**文件**：`src/include/leno_ast.h` —— `struct_def` 联合体

**改动**：添加三个字段：

```c
char** const_names;       // 关联常量名数组
Ast** const_values;       // 关联常量值表达式数组
int const_count;          // 关联常量数量
```

**文件**：`src/ast.c` —— `ast_free` 的 `AST_STRUCT_DEF` 分支

**改动**：添加 const 数组的释放逻辑。

**难度**：低。照搬 field_names/field_defaults 的模式。

### 5.3 语义分析层

**文件**：`src/semantic/visitinc/visit_type_def.inc` —— struct 定义访问

**改动**：在处理字段和方法之后，遍历 `const_names`/`const_values`，对每个常量值表达式做 `visit()` 求值，将结果存入 `ObjStructDef` 的新增 const 元数据。

**文件**：`src/semantic/visitinc/visit_module.inc` —— `AST_MODULE_ACCESS` 处理

**改动**：在 enum 类型分支（行 1720-1765）旁边，加 struct 关联常量分支：

```c
// 在 is_enum_type 检查之后
if (is_struct_type) {
    // 检查是否是 struct 关联常量访问
    ObjStructDef* sdef = struct_def_find(var_sym->type->struct_name);
    if (sdef && sdef->const_count > 0) {
        for (int ci = 0; ci < sdef->const_count; ci++) {
            if (strcmp(sdef->const_names[ci], mname) == 0) {
                // 找到关联常量，转换为 AST_NUM / AST_STRING 等常量节点
                // 类似 enum 值的处理方式
                break;
            }
        }
    }
    // 如果不是关联常量，继续走原有 struct 字段访问逻辑
}
```

**难度**：中。需要确保 `StructName.CONST` 和 `StructName.field` 两条路径不冲突。但由于 struct 关联常量在编译期就解析为值，而 struct 字段访问走 `AST_FIELD_ACCESS`（需要实例），两者语义上不冲突。

### 5.4 Codegen 层

**文件**：`src/codegen/codegen_stmt.c:1585` —— `AST_STRUCT_DEF` codegen

**改动**：在 `OP_STRUCT_DEF` 指令的编码中追加 const 信息：

```
OP_STRUCT_DEF + name(2) + field_count(1) + method_count(1)
  + impl_count(1) + impl_names(...)
  + type_param_count(1) + type_params(...)
  + field_data...
  + method_data...
  + const_count(1) + const_data...   ← 新增
```

每个 const 数据：`name(2) + type(1) + value(2)`（与字段默认值的编码模式一致）。

**文件**：`src/vm/vminc/op_struct.inc` —— `OP_STRUCT_DEF` VM 实现

**改动**：在读取完字段和方法信息后，继续读取 const 信息，存入 `ObjStructDef`。

**难度**：中。需要同步更新 `debug.c` 的反汇编和 `serialize.c` 的序列化。

### 5.5 运行时对象层

**文件**：`src/include/leno_value.h` —— `ObjStructDef`

**改动**：

```c
typedef struct {
    // ... 现有字段 ...
    // 关联常量
    char** const_names;
    Value* const_values;
    int const_count;
} ObjStructDef;
```

**文件**：`src/object/object_struct.c` —— `struct_def_new()`

**改动**：初始化 const 相关字段为 NULL/0。

**文件**：`src/gc.c` —— GC 大小计算

**改动**：在 `OBJ_STRUCT_DEF` 分支中添加 const 数组的大小计算。

**文件**：`src/serialize/serialize.c` —— 序列化/反序列化

**改动**：添加 const 数组的序列化逻辑。

**难度**：低-中。纯机械式扩展。

### 5.6 LSP 补全层

**文件**：`leno_lsp/comp_symbols.c:590-668`

**改动**：在添加字段和方法之后，遍历 struct 的关联常量，添加为 `LSP_COMP_CONSTANT`：

```c
// 添加关联常量
if (sdef && sdef->const_count > 0) {
    for (int c = 0; c < sdef->const_count; c++) {
        char detail[256];
        snprintf(detail, sizeof(detail), "%s.%s", var_name, sdef->const_names[c]);
        comp_set_add(set, sdef->const_names[c], LSP_COMP_CONSTANT, PRIO_FIELD,
                     detail, NULL, NULL, NULL);
    }
}
```

**文件**：`src/module_symbol_table/inc/scan/scan_struct.inc`

**改动**：在 struct 体内扫描时识别 `const` 关键字，将关联常量加入 `ModuleStructSymbol`。

**文件**：`src/include/module_symbol_table.h` —— `ModuleStructSymbol`

**改动**：添加 `const_names`/`const_values`/`const_count` 字段。

**难度**：中。LSP 补全的收益是此特性的主要价值点之一。

### 5.7 其他需要更新的文件

| 文件 | 改动内容 | 难度 |
|------|---------|------|
| `src/optimize/optimize.c` | `AST_STRUCT_DEF` 的常量折叠需遍历 const_values | 低 |
| `src/debug.c` | `OP_STRUCT_DEF` 反汇编需读取 const 信息 | 低 |
| `src/semantic/semantic.c` | 早期注册 `ObjStructDef` 时需处理 const | 低 |

---

## 六、与 enum 的关系

Leno 的 enum 已提供 `TypeName.MEMBER` 风格的命名空间：

```leno
enum LayoutAlign { START = 0, CENTER = 1, END = 2, STRETCH = 3 }
// 访问: LayoutAlign.START
```

### 6.1 enum 的局限

- **只支持 int64_t 值**：`parse_enum_stmt` 中 `member_values` 类型为 `int64_t*`，显式值必须是整数常量
- **不支持字符串/浮点/数组等类型**
- **不支持表达式求值**：`member = value` 的 value 只能是字面量

### 6.2 关联常量的补充能力

```leno
struct Font {
    const DEFAULT_SIZE = 16.0           // 浮点常量
    const FAMILY_NAME  = "sans-serif"   // 字符串常量
    const STYLE_NORMAL = 0x00           // 整数常量
    const SIZES        = [8, 12, 16, 24] // 数组常量
}
```

**结论**：enum 和 struct 关联常量是互补关系。enum 适合整数型分组常量，struct 关联常量适合多类型配置常量。

---

## 七、与泛型的交互

```leno
struct Box[T] {
    const MAX_SIZE = 100    // 所有 Box[int]、Box[string] 共享同一个值
    T value
}
```

**分析**：关联常量存储在 `ObjStructDef`（类型定义）上，而非 `ObjStruct`（实例）上。泛型特化创建的是不同实例，但它们共享同一个 `ObjStructDef`。因此关联常量天然不参与泛型特化，这是合理的——常量值与类型参数无关。

**难度**：无额外改动。现有泛型机制不需要修改。

---

## 八、全链路改动文件清单

| # | 文件 | 层 | 改动概要 | 工作量 |
|---|------|---|---------|-------|
| 1 | `src/include/leno_ast.h` | AST | `struct_def` 加 `const_names`/`const_values`/`const_count` | 3 行 |
| 2 | `src/ast.c` | AST | `ast_free` 的 `AST_STRUCT_DEF` 加 const 释放 | 8 行 |
| 3 | `src/parser/parser_func.c` | Parser | `parse_struct_stmt` 加 `TOK_CONST` 分支 | ~30 行 |
| 4 | `src/include/leno_value.h` | 运行时 | `ObjStructDef` 加 `const_names`/`const_values`/`const_count` | 3 行 |
| 5 | `src/object/object_struct.c` | 运行时 | `struct_def_new` 初始化 const 字段 | 3 行 |
| 6 | `src/semantic/visitinc/visit_type_def.inc` | 语义 | 遍历 const 值表达式，存入 `ObjStructDef` | ~15 行 |
| 7 | `src/semantic/visitinc/visit_module.inc` | 语义 | `AST_MODULE_ACCESS` 加 struct 关联常量解析分支 | ~20 行 |
| 8 | `src/codegen/codegen_stmt.c` | Codegen | `OP_STRUCT_DEF` 编码 const 信息 | ~15 行 |
| 9 | `src/vm/vminc/op_struct.inc` | VM | `OP_STRUCT_DEF` 读取 const 信息 | ~15 行 |
| 10 | `src/gc.c` | GC | `OBJ_STRUCT_DEF` 大小计算加 const | ~5 行 |
| 11 | `src/serialize/serialize.c` | 序列化 | const 数组序列化/反序列化 | ~15 行 |
| 12 | `src/debug.c` | 调试 | `OP_STRUCT_DEF` 反汇编加 const | ~5 行 |
| 13 | `src/optimize/optimize.c` | 优化 | const 值表达式常量折叠 | ~3 行 |
| 14 | `src/semantic/semantic.c` | 语义 | 早期注册 `ObjStructDef` 时处理 const | ~5 行 |
| 15 | `leno_lsp/comp_symbols.c` | LSP | 补全列表加关联常量 | ~10 行 |
| 16 | `src/include/module_symbol_table.h` | 符号表 | `ModuleStructSymbol` 加 const | 3 行 |
| 17 | `src/module_symbol_table/inc/scan/scan_struct.inc` | 符号表 | struct 体内扫描 const | ~20 行 |
| 18 | `src/module_symbol_table/inc/sym_table_add.inc` | 符号表 | `module_symbol_table_add_struct` 处理 const | ~10 行 |
| 19 | `src/module_symbol_table/inc/sym_table_cache.inc` | 符号表 | const 序列化缓存 | ~10 行 |

**总计**：约 200 行新代码，涉及 19 个文件。绝大多数是机械式扩展（照搬现有 fields/methods 的模式）。

---

## 九、最终结论与建议

### 9.1 是否值得做？

**值得做，但优先级中等。**

**理由**：

1. **痛点真实存在**：LenoSDL3 和 LenoWin32 中有大量散落在模块顶层的关联常量，命名空间扁平、LSP 补全断裂。

2. **实现量可控**：约 200 行代码，其中 80% 是机械式扩展。核心逻辑（parser 加分支 + 语义分析加路由）不超过 50 行。

3. **与现有架构契合度高**：enum 的 `TypeName.MEMBER` 编译期常量解析路径已经验证了这条路线可行，struct 关联常量只是复用同一模式。

4. **不引入运行时复杂度**：关联常量在编译期求值，不占实例内存，不影响 GC，不影响 NaN-boxing。

5. **非阻塞性**：当前模块级 const + 命名前缀约定"勉强够用"，不做不会阻碍开发。但 LSP 补全优化时会显著放大价值。

### 9.2 实现顺序建议

1. **Parser + AST**（先让语法能解析，不影响其他功能）
2. **语义分析**（求值并存入 `ObjStructDef`，此时 `StructName.CONST` 的访问就能工作）
3. **Codegen + VM**（编码到字节码，支持跨模块）
4. **LSP 补全**（让 `StructName.` 补全列表出现关联常量）
5. **模块符号表**（支持跨模块的关联常量类型传播）
6. **序列化 + GC**（机械式扩展）

### 9.3 不建议做的

1. **实例 const 字段**：需要在 `OP_SET_FIELD` 加运行时不可变性检查，影响 NaN-boxing 内存布局和 GC，复杂度远超收益。
2. **enum 支持字符串值**：虽然有价值，但会改变 enum 的 `int64_t` 存储模型，影响面比 struct 关联常量大。如果需要字符串常量，struct 关联常量已经覆盖了这个场景。
3. **关联常量参与泛型特化**：常量值与类型参数无关，不需要支持 `Box[int].MAX_SIZE` 与 `Box[string].MAX_SIZE` 不同。

### 9.4 语法设计建议

```leno
struct Widget {
    // 关联常量——支持两种语法形式，与全局 const 完全一致
    const MAX_CHILDREN = 256               // 形式1：类型推断（const NAME = value）
    const int LIMIT = 100                  // 形式2：显式类型（const TYPE NAME = value）
    const float DEFAULT_WIDTH = 100.0      // 显式类型
    const string NAME = "widget"           // 显式类型
    const COLORS = [0xFF000000, 0xFFFFFFFF] // 类型推断

    // 字段
    int x
    int y

    // 方法
    func calc(): int { return x + y }
}

// 访问
int limit = Widget.MAX_CHILDREN
float w = Widget.DEFAULT_WIDTH
```

**设计要点**：
- **语法与全局 const 完全一致**，支持两种形式：类型推断（`const X = value`）和显式类型（`const int X = value`）。这是通过复用 `parse_var_decl_internal` 中的预读逻辑实现的：消费 `const` 后，预读标识符后面的 token，如果是 `=` 或 `,` 则走类型推断，否则走 `parse_type()` 解析显式类型
- 关联常量声明顺序自由（可以在字段前/后/中间）
- 通过 `StructName.CONST_NAME` 访问，不需要实例
- 关联常量可以被 `export struct` 导出
- 跨模块访问走 `ModuleName.StructName.CONST_NAME`（复用 `AST_MODULE_ACCESS` → struct → const 的两级解析）

### 9.4.1 语法一致性说明

全局 const 和 struct 关联常量的语法对照：

| 语法形式 | 全局级 | struct 关联常量 | 示例 |
|---------|-------|----------------|------|
| 类型推断 | `const X = 42` | `const X = 42` | `const MAX = 256` |
| 显式类型 | `const int X = 42` | `const int X = 42` | `const int LIMIT = 100` |
| 浮点显式 | `const float Y = 3.14` | `const float Y = 3.14` | `const float PI = 3.14` |
| 字符串显式 | `const string S = "abc"` | `const string S = "abc"` | `const string NAME = "widget"` |

**实现方式**：struct 内 const 分支直接复用全局 const 的解析逻辑——即 `parse_var_decl_internal` 中行 648-662 的预读判定。无需额外设计，保证两种上下文行为完全一致。

> **注**：虽然 Leno 当前实际代码库中（LenoSDL3/LenoWin32）所有 const 均使用类型推断形式，但 parser 已完整支持显式类型形式，struct 关联常量应保持一致。
