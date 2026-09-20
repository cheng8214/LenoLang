# cstruct packed / align(N) 功能设计文档

> **状态：已实现 ✅** — 全部功能已编码、编译通过、245/245 测试通过
>
> 为 Leno cstruct 引入 `packed` 和 `align(N)` 属性，使用户能够精确控制内存布局，
> 对齐 C 编译器的 `#pragma pack` / `__attribute__((packed))` / `alignas` 行为。

---

## 1. 动机

### 当前状态

Leno cstruct 当前使用标准 C 对齐规则（`visit_ffi.inc:33-77`）：

1. 每个字段按其类型的自然对齐要求向上对齐 `current_offset`
2. 结构体总大小按最大字段对齐要求向上对齐

这意味着：

```leno
cstruct Demo {
    u8  a     // offset 0, size 1
    // 3 字节 padding
    i32 b     // offset 4, size 4
    u8  c     // offset 8, size 1
    // 3 字节 padding
}            // total_size = 12, alignment = 4
```

### 问题

许多 C 库（特别是网络协议、文件格式、Windows API 结构体）使用 `#pragma pack(1)` 或 `__attribute__((packed))`：
- 网络协议头：字段紧挨，无 padding
- Windows API 中某些结构体使用 `#pragma pack(push, 1)`
- 跨平台二进制格式需要精确控制布局

**当前 Leno 无法表达这些布局**，用户只能手动计算偏移并逐字节写入，极易出错。

---

## 2. 语法设计

### 2.1 `packed` — 取消所有 padding

```leno
packed cstruct Demo {
    u8  a     // offset 0
    i32 b     // offset 1  (无 padding!)
    u8  c     // offset 5
}            // total_size = 6, alignment = 1
```

`packed` 关键字放在 `cstruct` 之前，表示：
- 所有字段按 1 字节对齐（即不对齐）
- 结构体总对齐为 1
- 等价于 C 的 `#pragma pack(1)` 或 `__attribute__((packed))`

### 2.2 `align(N)` — 指定结构体对齐

```leno
align(16) cstruct CacheLine {
    i64 data[2]   // offset 0, size 16
}                // total_size = 16, alignment = 16
```

`align(N)` 放在 `cstruct` 之前，N 为正整数（1, 2, 4, 8, 16, 32...），表示：
- 字段仍按自然对齐排列
- 结构体总对齐要求 = max(自然对齐, N)
- 总大小按 max(自然对齐, N) 向上对齐
- 等价于 C 的 `alignas(N)` / `__declspec(align(N))`

### 2.3 组合使用

```leno
packed align(16) cstruct AlignedPacket {
    u8  magic   // offset 0
    u16 length  // offset 1
    u8  data[]  // offset 3
}               // total_size = 3, alignment = 16
                // (packed 使字段紧挨，align(16) 使结构体对齐 16 字节边界)
```

`packed align(16)` 语法：
- `packed` 先生效：所有字段按 1 字节对齐（无 padding）
- `align(16)` 后生效：结构体总对齐 = max(1, 16) = 16，总大小按 16 向上对齐

### 2.4 语法规则

```
cstruct_decl := [packed] [align(NUM)] 'cstruct' IDENT '{' fields '}'
              | [align(NUM)] [packed] 'cstruct' IDENT '{' fields '}'
```

- `packed` 和 `align(N)` 都是可选的
- 两者顺序可互换
- `align(N)` 的 N 必须是编译期正整数，且为 2 的幂（1, 2, 4, 8, 16, 32...）

---

## 3. 语义分析变更

### 3.1 AST 扩展

在 `cstruct_def` 结构体中新增两个字段（`leno_ast.h`）：

```c
struct {
    // ... 现有字段 ...
    bool is_packed;       // 是否 packed
    int explicit_align;   // 显式对齐要求（0 表示未指定）
} cstruct_def;
```

### 3.2 布局计算逻辑变更

当前逻辑（`visit_ffi.inc:38-72`）：

```
for each field:
    field_align = c_layout_type_align(field_type)
    max_alignment = max(max_alignment, field_align)
    current_offset = align_up(current_offset, field_align)
    field_offset[i] = current_offset
    current_offset += field_total_size
total_size = align_up(current_offset, max_alignment)
```

新逻辑：

```
for each field:
    if is_packed:
        field_align = 1
    else:
        field_align = c_layout_type_align(field_type)
    
    // 嵌套 cstruct 的对齐也受 packed 影响
    if field_type == TYPE_CSTRUCT:
        nested_align = nested_cstruct.cstruct_alignment
        if is_packed:
            field_align = 1
        else:
            field_align = nested_align
    
    max_alignment = max(max_alignment, field_align)
    current_offset = align_up(current_offset, field_align)
    field_offset[i] = current_offset
    current_offset += field_total_size

// 应用显式对齐
int final_align = max(max_alignment, explicit_align)
total_size = align_up(current_offset, final_align)
```

**关键点**：
- `packed` 使每个字段的对齐变为 1，相当于字段紧挨
- `align(N)` 只影响结构体的最终对齐和总大小，不影响字段间 padding
- 嵌套 cstruct 在 packed 模式下也按 1 对齐

### 3.3 验证规则

- `align(N)` 的 N 必须是 2 的幂（N & (N-1) == 0）
- `align(N)` 的 N 范围：1 ≤ N ≤ 64（覆盖所有主流平台）
- `align(0)` 或负值报错
- 如果不指定 `packed` 也不指定 `align(N)`，行为与当前完全一致（向后兼容）

---

## 4. 全链路改动清单

### 4.1 词法层（Lexer）

**文件**：`src/lexer.c`

新增两个关键字到关键字表：

```c
{"packed", TOK_PACKED},
{"align",  TOK_ALIGN},
```

**文件**：`src/include/leno_types.h`

新增两个 token 类型：

```c
TOK_PACKED,     // packed 关键字
TOK_ALIGN,      // align 关键字
```

### 4.2 语法层（Parser）

**文件**：`src/parser/parser_func.c` — `parse_cstruct_stmt()`

在消费 `cstruct` 关键字之前，先检查可选的 `packed` 和 `align(N)` 前缀：

```
parse_cstruct_stmt():
    is_packed = false
    explicit_align = 0
    
    if current == TOK_PACKED:
        is_packed = true
        consume(TOK_PACKED)
    
    if current == TOK_ALIGN:
        consume(TOK_ALIGN)
        consume(TOK_LPAREN)
        expect(TOK_NUM) → read N
        consume(TOK_RPAREN)
        explicit_align = N
        // 验证 N 是 2 的幂
    
    if current == TOK_PACKED:  // 处理互换顺序
        is_packed = true
        consume(TOK_PACKED)
    
    // 原有逻辑：consume(TOK_CSTRUCT) ...
    ast->u.cstruct_def.is_packed = is_packed
    ast->u.cstruct_def.explicit_align = explicit_align
```

### 4.3 AST 层

**文件**：`src/include/leno_ast.h`

在 `cstruct_def` 结构体中新增：

```c
bool is_packed;         // 是否 packed（取消所有 padding）
int  explicit_align;    // 显式对齐要求（0 表示未指定，否则必须是 2 的幂）
```

### 4.4 语义分析

**文件**：`src/semantic/visitinc/visit_ffi.inc`

修改 `AST_CSTRUCT_DEF` case 中的布局计算逻辑：
- 读取 `is_packed` 和 `explicit_align`
- packed 模式下 field_align = 1
- 最终对齐 = max(max_alignment, explicit_align)

### 4.5 符号表

**文件**：`src/include/leno_vm.h`

在 `Symbol` 中新增（紧挨现有 cstruct 字段）：

```c
bool cstruct_is_packed;       // cstruct 是否 packed
int  cstruct_explicit_align;  // cstruct 显式对齐（0 = 未指定）
```

语义分析阶段将 AST 中的值复制到 Symbol。

### 4.6 代码生成

**文件**：`src/codegen/codegen_stmt.c` — `AST_CSTRUCT_DEF` case

在 `OP_CSTRUCT_DEF` 指令中新增两个字节：
- 1 字节：`is_packed`（0 或 1）
- 1 字节：`explicit_align`

指令格式变更：

```
OP_CSTRUCT_DEF:
  2 bytes: name constant index
  1 byte:  field_count
  2 bytes: total_size
  1 byte:  alignment
  1 byte:  is_packed        ← 新增
  1 byte:  explicit_align   ← 新增
  ... 字段信息不变 ...
```

### 4.7 虚拟机

**文件**：`src/vm/vminc/op_cstruct.inc` — `OP_CSTRUCT_DEF`

读取新增的两个字节，传递给 `cstruct_def_new`。

### 4.8 对象层

**文件**：`src/include/leno_value.h` — `ObjCStructDef`

新增两个字段：

```c
bool is_packed;         // 是否 packed
int  explicit_align;    // 显式对齐（0 = 未指定）
```

**文件**：`src/object/object_cstruct.c` — `cstruct_def_new()`

新增参数 `is_packed` 和 `explicit_align`，设置到 `ObjCStructDef` 中。

### 4.9 序列化

**文件**：`src/serialize/serialize.c`

- 序列化：在 `ObjCStructDef` 写入时追加 `is_packed` + `explicit_align`
- 反序列化：读取时追加解析这两个字段

### 4.10 运行时方法

**文件**：`src/module/cstructs/cstructs.c`

`to_str()` / `debug()` 方法输出中包含 `packed` / `align(N)` 标记，让用户能看到布局属性。

### 4.11 文档

- `docs/module_cstructs.md`：新增 packed/align 语法说明和示例
- `docs/FFI使用指南.md`：在 cstruct 对齐章节补充 packed/align 说明

---

## 5. 实现优先级

| 阶段 | 内容 | 预计改动量 |
|------|------|-----------|
| 1 | Lexer + Token：新增 `packed` / `align` 关键字 | 小 |
| 2 | Parser：解析 `packed` / `align(N)` 前缀 | 中 |
| 3 | AST + 语义分析：布局计算逻辑 | 中 |
| 4 | Codegen + VM：指令格式扩展 | 小 |
| 5 | 对象 + 序列化：字段持久化 | 小 |
| 6 | 运行时方法：debug 输出增强 | 小 |
| 7 | 测试：编写测试用例 + 编译验证 | 中 |
| 8 | 文档更新 | 小 |

---

## 6. 测试计划

### 6.1 基本功能测试

```leno
// 测试 packed 布局
packed cstruct PackedDemo {
    u8  a
    i32 b
    u8  c
}
// 预期: total_size=6, alignment=1
// a@0, b@1, c@5

// 测试 align(16)
align(16) cstruct AlignedDemo {
    i64 x
}
// 预期: total_size=16, alignment=16

// 测试组合
packed align(16) cstruct ComboDemo {
    u8  magic
    u16 length
}
// 预期: total_size=16 (packed 后 3 字节, align(16) 后 16), alignment=16
```

### 6.2 嵌套测试

```leno
packed cstruct Inner {
    u8  a
    i32 b
}

cstruct Outer {   // 非 packed
    Inner inner    // 嵌套 packed cstruct
    i64  data
}
```

### 6.3 FFI 互操作测试

验证 packed cstruct 与 C 库函数交互时的内存布局正确性。

### 6.4 回归测试

运行 `assert` 目录下全部 244 个测试，确保零回归。

---

## 7. 向后兼容性

- 不指定 `packed` 和 `align(N)` 时，布局计算逻辑与当前完全一致
- 现有所有 cstruct 代码无需修改
- 序列化格式向后兼容（新字段追加在末尾，旧格式读取时默认为 0/false）

---

## 8. 风险评估

| 风险 | 严重性 | 缓解措施 |
|------|--------|----------|
| 嵌套 cstruct 在 packed 模式下的对齐传播 | 中 | 明确规则：packed 只影响当前结构体，嵌套 cstruct 的对齐由其自身属性决定 |
| 非对齐内存访问在某些平台上可能触发硬件异常 | 低 | x86/x64 支持非对齐访问；ARM 默认不支持但 OS 可配置。文档中标注注意事项 |
| 序列化格式变更 | 低 | 新字段追加在末尾，旧文件读取时默认 0/false |
