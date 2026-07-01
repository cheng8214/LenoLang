# Feature: `&` 取地址运算符（cstruct 字段取地址）

## Context

当前 FFI 调用需要向 C 函数传递输出参数指针时，必须先 `ffi.malloc()` 分配临时内存、传指针、调用后 `ffi.read_int()` 读回、最后 `ffi.free()`。例如 `SDL_GetRenderDrawColor(handle, &r, &g, &b, &a)` 需要分配 4 个临时指针。

添加 `&c.r` 取地址语法后，可直接传 cstruct 字段地址给 C 函数，C 函数直接写入字段内存，零拷贝：

```leno
// 之前：4 个 malloc + 4 个 read_int + 4 个 free
var cr = ffi.malloc(4); var cg = ffi.malloc(4)
_lib.SDL_GetRenderDrawColor(handle, cr, cg, cb, ca)
c.r = ffi.read_int(cr, 0) & 0xFF; ...
ffi.free(cr); ffi.free(cg); ...

// 之后：直接传字段地址
var c = core.SDL_Color.malloc()
_lib.SDL_GetRenderDrawColor(handle, &c.r, &c.g, &c.b, &c.a)
```

**限制**：`&` 取地址仅支持 **cstruct 字段**（C 布局，有稳定内存地址）。普通 struct 字段是 GC 管理的 NaN-tagged Value，无法取 C 兼容地址，语义分析阶段报错。

## 实现步骤

### 1. AST — 添加 `AST_ADDRESS_OF` 节点

**文件**: `d:\CLeno\LenoC\src\include\leno_ast.h`

- 在 `AstKind` 枚举中 `AST_FIELD_ACCESS` 后添加 `AST_ADDRESS_OF`
- 在 `Ast.u` 联合体中添加：`struct { Ast* operand; } address_of;`

### 2. AST 释放 — 处理 `AST_ADDRESS_OF`

**文件**: `d:\CLeno\LenoC\src\ast.c`

- 在 `ast_free` 的 switch 中添加 `case AST_ADDRESS_OF`：释放 `ast->u.address_of.operand`

### 3. 解析器 — `&` 前缀运算符

**文件**: `d:\CLeno\LenoC\src\parser\parser_expr.c`

- 添加 `parse_addr_of()` 函数：消耗 `&`，解析操作数（`PREC_UNARY`），创建 `AST_ADDRESS_OF` 节点
- 修改 Pratt 规则表：`[TOK_BITAND] = {parse_addr_of, parse_binary, PREC_BITAND}`（原来 prefix 是 NULL）
- **无需修改词法分析器** — `&` 已经是 `TOK_BITAND`，前缀/中缀由 Pratt parser 自动区分

### 4. 语义分析 — 访问 `AST_ADDRESS_OF`

**文件**: `d:\CLeno\LenoC\src\semantic\visitinc\visit_field_access.inc`（或新建 `visit_addr_of.inc`）

- 在 visit dispatch 中添加 `case AST_ADDRESS_OF`
- 访问操作数（`visit(s, ast->u.address_of.operand)`）
- 验证操作数是 `AST_FIELD_ACCESS`
- 验证操作数对象类型是 `TYPE_CSTRUCT`（普通 struct 报语义错误："& 取地址仅支持 cstruct 字段"）
- 确保 `field_index` 已解析

### 5. 类型推断 — `AST_ADDRESS_OF` 返回 `TYPE_PTR`

**文件**: `d:\CLeno\LenoC\src\semantic\semantic_type.c`

- 在 `infer_expr_type` 的 switch 中添加 `case AST_ADDRESS_OF`
- 返回 `type_new(TYPE_PTR)`，`element_type` 设为字段的类型（如果已知）

### 6. 字节码 — 添加 `OP_GET_FIELD_ADDR` 操作码

**文件**: `d:\CLeno\LenoC\src\include\leno_vm.h`

- 在 `OP_GET_FIELD` / `OP_SET_FIELD` 附近添加 `OP_GET_FIELD_ADDR`

### 7. 代码生成 — `AST_ADDRESS_OF` 生成 `OP_GET_FIELD_ADDR`

**文件**: `d:\CLeno\LenoC\src\codegen\codegen_expr.c`

- 在 `gen_expr` 的 switch 中添加 `case AST_ADDRESS_OF`
- 操作数必须是 `AST_FIELD_ACCESS`
- 生成对象表达式：`gen_expr(gen, operand->u.field_access.obj)`
- 发射 `OP_GET_FIELD_ADDR` + 1 字节 `field_index`

### 8. VM — 实现 `OP_GET_FIELD_ADDR` 操作码

**文件**: `d:\CLeno\LenoC\src\vm\vminc\op_struct.inc`

- 实现 `OPCODE(OP_GET_FIELD_ADDR)`：
  - 读取 `field_idx` 字节
  - 弹出栈顶对象，验证是 `OBJ_CSTRUCT`
  - 计算 `addr = obj->data + def->fields[field_idx].offset`
  - 创建 `ObjFFIPointer`：`ptr = addr, size = field->size, owned = 0, freed = 0, element_type = field->type`
  - 压入栈

**文件**: `d:\CLeno\LenoC\src\vm\vminc\vm_run.inc`

- 在 dispatch 表中添加 `[OP_GET_FIELD_ADDR] = &&LABEL(OP_GET_FIELD_ADDR),`

### 9. 优化器 — 处理 `AST_ADDRESS_OF`

**文件**: `d:\CLeno\LenoC\src\optimize\optimize.c`

- 如果优化器遍历 AST，需添加 `case AST_ADDRESS_OF` 递归访问 `operand`

### 10. 更新 SDL3 渲染器 — 简化 FFI 调用

**文件**: `d:\CLeno\LenoC\leno_module\LenoSDL3\lib\sdl_renderer.leno`

- 简化 `getColor()` 等方法，用 `&c.r` 替代 `ffi.malloc/read_int/free` 模式

## 关键设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| 词法分析 | 无修改 | `&` 已是 `TOK_BITAND`，Pratt parser 区分前缀/中缀 |
| AST 节点 | 新增 `AST_ADDRESS_OF` | 比 `AST_UNARY{op=TOK_BITAND}` 语义更清晰 |
| 适用范围 | 仅 cstruct 字段 | 普通 struct 字段是 NaN-tagged Value，无 C 稳定地址 |
| 操作码 | 新增 `OP_GET_FIELD_ADDR` | 语义与 `OP_GET_FIELD` 不同（返回指针 vs 返回值） |
| 链式访问 `&c.nested.field` | 自然支持 | 内层 `OP_GET_FIELD` 产生 cstruct view，外层 `OP_GET_FIELD_ADDR` 取其字段地址 |

## 验证

1. 编译：`build.bat`
2. 基础测试：
   ```leno
   cstruct Color { u8 r; u8 g; u8 b; u8 a }
   var c = Color.malloc()
   var pr = &c.r
   print(ffi.is_ptr(pr))   // true
   ffi.write_int(pr, 0, 255)
   print(c.r)              // 255
   ffi.free(c.to_ptr())
   ```
3. FFI 集成测试：简化 `sdl_renderer.leno` 的 `getColor()` 并运行烟花示例
4. 全部测试套件：`build\leno.exe assert\run_tests.leno build\leno.exe assert`
