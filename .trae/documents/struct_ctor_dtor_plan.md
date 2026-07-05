# Struct 构造函数/析构函数 实现计划

## Context

Dialog 等资源管理 struct 需要在创建时自动初始化窗口句柄，在离开作用域时自动销毁。当前 struct 只有字段赋值，没有构造/析构生命周期管理。

设计文档：`d:\CLeno\LenoC\bug\design_struct_ctor_dtor.md`

核心规则：
- `func StructName()` — 构造函数，字段赋值后自动调用，无参
- `func ~StructName()` — 析构函数，变量离开作用域时自动调用，无参无返回
- **引用语义**：`d2 = d` 是引用，只有 `var d = new Struct()` 的变量负责析构
- 析构触发：作用域结束、return 语句

## 实现步骤

### 步骤 1：数据结构扩展

**1.1 `src/include/leno_ast.h`** — AST 增加标志

在 `u.func` 联合体（第182-208行）中，`is_async` 之后新增：
```c
int is_ctor;    // 是否是构造函数
int is_dtor;    // 是否是析构函数
```

在 `u.struct_init` 联合体中新增：
```c
int has_dtor;   // 该 struct 是否有析构函数（语义分析阶段设置）
```

**1.2 `src/include/leno_value.h`** — ObjStructDef 增加字段

在 ObjStructDef（第556-569行）末尾新增：
```c
int has_ctor;        // 是否有构造函数
int ctor_index;      // 构造函数在 methods[] 中的索引（-1 表示无）
int has_dtor;        // 是否有析构函数
int dtor_index;      // 析构函数在 methods[] 中的索引（-1 表示无）
```

在 ObjFunction（第337-352行）末尾新增：
```c
int is_ctor;   // 是否是构造函数（用于 OP_RETURN 返回 self）
```

**1.3 `src/include/leno_codegen.h`** — CodeGen 增加析构追踪

```c
typedef struct {
    int local_slot;    // 局部变量槽位索引
} DtorEntry;

DtorEntry* dtor_entries;   // 需要析构的局部变量数组
int dtor_count;            // 当前条目数
int dtor_capacity;         // 数组容量
int dtor_temp_slot;        // return 时保存返回值的临时槽位（-1=未分配）
```

**1.4 `src/include/leno_vm.h`** — 新增操作码

在 `OP_PUSH_TYPE_ARGS` 之前新增：
```c
OP_DTOR_LOCAL,     // 对局部变量调用析构函数（操作数：2字节 local_slot）
```

> **说明**：不需要 OP_CTOR_CALL。构造函数直接在 OP_STRUCT_INIT 的 VM 处理中调用，无需新操作码。

**1.5 `src/vm/debug.c`** — 反汇编表

在 `opCodeNames[]` 中添加 `"OP_DTOR_LOCAL"`。

**1.6 `src/object/object_struct.c`** — struct_def_new 初始化

在 `struct_def_new()` 中初始化新字段为 0/-1。

---

### 步骤 2：解析器支持

**`src/parser/parser_func.c`** — parse_struct_stmt 修改（第1384-1431行区域）

当消费完 `func` 关键字后：
1. 检查 `p->lex.current.type == TOK_BITNOT`（`~` 已作为 `TOK_BITNOT` 存在）
2. 如果是 `~`：消费它，标记 `is_dtor = 1`，期望方法名与 struct 名相同
3. 如果方法名与 struct 名相同且无 `~`：标记 `is_ctor = 1`
4. 设置 `func_ast->u.func.is_ctor` 和 `func_ast->u.func.is_dtor`

同时更新预读逻辑：`func (` → 函数类型字段，`func ~` → 析构函数，`func IDENT` → 方法/构造函数

错误提示：
- `~` 后名称与 struct 名不匹配 → "析构函数名必须为 ~StructName"
- 构造/析构函数带参数 → "构造函数/析构函数不能有参数"

---

### 步骤 3：语义分析验证

**`src/semantic/visitinc/visit_type_def.inc`**

3.1 在方法处理阶段（第131-189行），对构造/析构函数验证：
- 构造函数不能有显式参数（pcnt==0，隐式 self 由后续添加）
- 析构函数不能有显式参数和返回类型
- 不允许多个构造函数或多个析构函数

3.2 在 ObjStructDef 创建阶段（第334-357行），设置 ctor/dtor 标志：
```c
for (int i = 0; i < method_count; i++) {
    if (methods[i]->u.func.is_ctor) {
        early_def->has_ctor = 1;
        early_def->ctor_index = i;
    }
    if (methods[i]->u.func.is_dtor) {
        early_def->has_dtor = 1;
        early_def->dtor_index = i;
    }
}
```

**`src/semantic/visitinc/visit_struct_init.inc`**

在处理 AST_STRUCT_INIT 时，查找 struct 的 has_dtor 并设置到 AST 节点：
```c
ast->u.struct_init.has_dtor = sdef->has_dtor;
```

**防止用户显式调用构造/析构函数**

在 OP_GET_METHOD / OP_GET_PROPERTY 的方法查找中，跳过构造/析构函数（通过检查方法名是否等于 struct 名或以 `~` 开头）。具体修改在 `src/vm/vminc/op_struct.inc` 的 OP_GET_METHOD 处理中和 `src/vm/vminc/op_property.inc` 的 OP_GET_PROPERTY 处理中。

---

### 步骤 4：构造函数 VM 实现

**4.1 `src/codegen/codegen_func.c`** — gen_func_proto 设置 is_ctor

在创建 ObjFunction 后（第52-67行区域），设置：
```c
func->is_ctor = ast->u.func.is_ctor;
```

**4.2 `src/codegen/codegen_stmt.c`** — OP_STRUCT_DEF 字节码编码

在 AST_STRUCT_DEF 代码生成中（第1450-1566行），在方法信息编码之后新增：
```c
// 构造/析构函数标志和索引
emit_byte(gen, (def->has_ctor ? 1 : 0) | (def->has_dtor ? 2 : 0), ast->line);
if (has_ctor) emit_byte(gen, ctor_index, ast->line);
if (has_dtor) emit_byte(gen, dtor_index, ast->line);
```

同样修改 `gen_struct_module()`（第1771-1870行）。

**4.3 `src/vm/vminc/op_struct.inc`** — OP_STRUCT_DEF 字节码解码

在方法信息读取之后，读取 ctor/dtor 标志和索引，设置到 ObjStructDef。

**4.4 `src/vm/vminc/op_struct.inc`** — OP_STRUCT_INIT 构造函数调用

在第426行（push instance）之后、DISPATCH 之前，增加构造函数调用：

```c
if (def->has_ctor && def->ctor_index >= 0) {
    StructMethodInfo* mi = &def->methods[def->ctor_index];
    ObjClosure* ctor_closure = mi->closure;
    if (!ctor_closure && mi->func) {
        ctor_closure = closure_new(mi->func);  // 创建闭包
        mi->closure = ctor_closure;            // 缓存
    }
    if (ctor_closure) {
        // 栈布局变为 [instance(self), closure(callee)]
        // call() 读取: arg[0]=instance, callee=closure
        vm_stack_push(&vm, val_obj((Object*)ctor_closure));
        call(ctor_closure, 1, GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;  // 进入构造函数执行
    }
}
```

**4.5 `src/vm/vminc/op_call.inc`** — OP_RETURN 的 is_ctor 处理

在第687行（`Value result = vm_stack_pop(&vm)`）之后、finally 检查之前：

```c
// 构造函数：返回 self 而不是返回值表达式
if (frame->closure && frame->closure->function &&
    frame->closure->function->is_ctor) {
    result = frame->locals[0];  // self
}
```

**4.6 `src/object/object_struct.c`** — GC 标记

在 gc_mark_object 中对 OBJ_STRUCT_DEF 增加对 ctor_closure 的标记（如果有）。

---

### 步骤 5：析构函数 CodeGen + VM 实现

**5.1 `src/codegen/codegen.h`** — 辅助函数声明

```c
void codegen_add_dtor_entry(CodeGen* gen, int local_slot);
```

**5.2 `src/codegen/codegen_stmt.c`** — gen_var_decl 修改（第630-695行）

在 `OP_SET_LOCAL_POP` 之后，检查是否需要追踪析构：

```c
// 仅追踪 var x = new StructWithDtor(...) 的变量
if (ref->kind == SYM_LOCAL &&
    ast->u.var_decl.init &&
    ast->u.var_decl.init->kind == AST_STRUCT_INIT &&
    ast->u.var_decl.init->u.struct_init.has_dtor) {
    codegen_add_dtor_entry(gen, ref->index);
}
```

**5.3 `src/codegen/codegen_stmt.c`** — gen_block 修改（第1166-1234行）

在第三遍语句生成之后，添加块级析构调用：

```c
// 块结束时，逆序生成新增 dtor 条目的析构调用
for (int i = gen->dtor_count - 1; i >= dtor_count_at_entry; i--) {
    emit_byte(gen, OP_DTOR_LOCAL, ast->line);
    emit_byte(gen, (gen->dtor_entries[i].local_slot >> 8) & 0xff, ast->line);
    emit_byte(gen, gen->dtor_entries[i].local_slot & 0xff, ast->line);
    emit_byte(gen, OP_POP, ast->line);  // 弹出析构函数返回值(null)
}
gen->dtor_count = dtor_count_at_entry;  // 移除当前块的 dtor 条目
```

其中 `dtor_count_at_entry` 在 gen_block 入口处记录。

**5.4 `src/codegen/codegen_stmt.c`** — gen_return 修改（第1126-1142行）

```c
static void gen_return(CodeGen* gen, Ast* ast) {
    if (gen->dtor_count > 0) {
        // 有需要析构的变量
        if (ast->u.ret) {
            gen_expr(gen, ast->u.ret);
            // 保存返回值到临时槽位
            if (gen->dtor_temp_slot < 0) {
                gen->dtor_temp_slot = gen->current_func->local_count;
                gen->current_func->local_count++;
            }
            emit_bytes_2(gen, OP_SET_LOCAL, gen->dtor_temp_slot, ast->line);
        }
        // 逆序调用所有析构函数
        for (int i = gen->dtor_count - 1; i >= 0; i--) {
            emit_byte(gen, OP_DTOR_LOCAL, ast->line);
            emit_byte(gen, (gen->dtor_entries[i].local_slot >> 8) & 0xff, ast->line);
            emit_byte(gen, gen->dtor_entries[i].local_slot & 0xff, ast->line);
            emit_byte(gen, OP_POP, ast->line);
        }
        // 恢复返回值
        if (ast->u.ret) {
            emit_bytes_2(gen, OP_GET_LOCAL, gen->dtor_temp_slot, ast->line);
        } else {
            emit_byte(gen, OP_NULL, ast->line);
        }
        emit_byte(gen, OP_RETURN, ast->line);
    } else {
        // 原有逻辑不变
        ...
    }
}
```

**有 dtor 时不使用尾调用优化**（保持正确析构顺序）。

**5.5 `src/vm/vminc/op_struct.inc`** — OP_DTOR_LOCAL 实现

```c
OPCODE(OP_DTOR_LOCAL) {
    frame = &vm.frames[vm.frame_cnt - 1];
    uint16_t slot = READ_SHORT();

    if (slot >= frame->local_count) DISPATCH;

    Value val = frame->locals[slot];

    // 检查是否是 struct 实例且有析构函数
    if (val_is_obj(val) && val_as_obj(val)->type == OBJ_STRUCT) {
        ObjStruct* obj = (ObjStruct*)val_as_obj(val);
        ObjStructDef* def = obj->def;

        if (def->has_dtor && def->dtor_index >= 0) {
            StructMethodInfo* mi = &def->methods[def->dtor_index];
            ObjClosure* dtor_closure = mi->closure;
            if (!dtor_closure && mi->func) {
                dtor_closure = closure_new(mi->func);
                mi->closure = dtor_closure;
            }
            if (dtor_closure) {
                // 栈布局: [instance(self), closure(callee)]
                vm_stack_push(&vm, val);                          // self
                vm_stack_push(&vm, val_obj((Object*)dtor_closure)); // callee
                call(dtor_closure, 1, GET_CURRENT_LINE());
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH;  // 进入析构函数执行
            }
        }
    }

    // 不是 struct 或没有析构函数 → 压入 null 作为"返回值"
    vm_stack_push(&vm, val_null());
    DISPATCH;
}
```

析构函数返回后，null 被压入栈（OP_RETURN 正常流程），由紧跟的 OP_POP 清理。

**5.6 `src/codegen/codegen_func.c`** — gen_func_proto 初始化

在 gen_func_proto 中初始化 dtor 相关字段：
```c
gen->dtor_count = 0;  // 每个函数开始时重置
gen->dtor_temp_slot = -1;
```

---

### 步骤 6：防止显式调用构造/析构

**`src/vm/vminc/op_struct.inc`** — OP_GET_METHOD

在方法查找循环中（第669-703行），跳过构造/析构方法：
```c
// 跳过构造/析构函数（不允许显式调用）
if (i == def->ctor_index || i == def->dtor_index) continue;
```

**`src/vm/vminc/op_property.inc`** — OP_GET_PROPERTY

在 struct 方法查找中同样跳过 ctor/dtor。

---

### 步骤 7：测试用例

在 `d:\CLeno\LenoC\examples\struct\struct test\` 目录下创建测试文件：

**test_ctor.leno** — 构造函数基本测试
```leno
struct Counter {
    int count = 0
    func Counter() { count = 10 }
}
main() {
    var c = new Counter()
    print(c.count)  // 期望: 10
}
```

**test_dtor.leno** — 析构函数基本测试
```leno
int alive = 0

struct Resource {
    Ptr handle = null
    func Resource() { handle = ffi.malloc(1024); alive = alive + 1 }
    func ~Resource() { if handle != null { ffi.free(handle); handle = null }; alive = alive - 1 }
}
main() {
    print(alive)  // 0
    func test() {
        var r = new Resource()
        print(alive)  // 1
    }
    test()
    print(alive)  // 0
}
```

**test_ctor_dtor.leno** — 构造+析构完整测试
```leno
struct Dialog {
    Dict style = {}
    Ptr handle = null
    func Dialog() { handle = ffi.malloc(64) }
    func ~Dialog() { if handle != null { ffi.free(handle); handle = null } }
}
main() {
    func test() {
        var d = new Dialog(style={title: "Test"})
        print(d.handle != null)  // true
    }
    test()
    // d 离开作用域，析构自动调用
}
```

**test_dtor_return.leno** — return 时析构
```leno
int destroyed = 0
struct Resource {
    func Resource() {}
    func ~Resource() { destroyed = destroyed + 1 }
}
func test(): int {
    var r = new Resource()
    return 42
}
main() {
    var result = test()
    print(result)    // 42
    print(destroyed) // 1
}
```

**test_dtor_inner_scope.leno** — 内部块作用域析构
```leno
int order = ""
struct S {
    string name = ""
    func S() {}
    func ~S() { order = order + name }
}
main() {
    var a = new S(name="A")
    if true {
        var b = new S(name="B")
    }  // B 析构
    var c = new S(name="C")
    print(order)  // "B" (只有 B 离开了作用域)
}  // C 析构，A 析构 → order = "BCA"
```

**test_dtor_reference.leno** — 引用不触发析构
```leno
int destroyed = 0
struct S {
    func S() {}
    func ~S() { destroyed = destroyed + 1 }
}
main() {
    var d = new S()
    var d2 = d  // 引用，不追踪析构
    print(destroyed)  // 0
}  // 只有 d 触发析构
```

---

## 关键设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| 构造函数调用位置 | VM 内 OP_STRUCT_INIT | 最小侵入，不改变字节码布局 |
| 析构函数调用位置 | CodeGen 生成 OP_DTOR_LOCAL | 编译器确定何时调用，VM 执行 |
| 析构追踪条件 | 仅 `var x = new StructWithDtor()` | 实现引用语义，避免 double-free |
| 构造函数返回值 | is_ctor 标志，OP_RETURN 返回 self | 实例在栈上保留，对调用方透明 |
| 作用域追踪方式 | dtor_count_at_entry | 不依赖 scope_depth，更安全 |
| return 时临时保存 | dtor_temp_slot | 按需分配，零开销（无 dtor 时不分配） |
| 尾调用 + dtor | 禁用尾调用优化 | 保证析构顺序正确 |

## 潜在陷阱

1. **call() 栈布局**：`call(closure, arg_count)` 期望栈为 `[arg0, arg1, ..., callee]`。构造/析构调用时需按此顺序推入 self 和 closure。
2. **OP_DTOR_LOCAL 后必须 OP_POP**：析构函数返回 null，会留在栈上，必须紧跟 OP_POP。
3. **gen_block 三遍扫描**：析构调用必须在第三遍（语句生成）之后插入，不能在第一/二遍。
4. **gen_func_proto 的 dtor 重置**：每个函数开始时 dtor_count=0, dtor_temp_slot=-1。
5. **upvalue 捕获的 struct 变量**：当前方案不检查是否被 upvalue 捕获。OP_DTOR_LOCAL 会设置 `locals[slot] = null` 防止重复析构，但被 upvalue 引用的实例可能在析构后仍被访问。后续可增加检查。
6. **循环中的 struct**：for 循环体内声明的 struct，每次迭代结束时由 gen_block 的 dtor 逻辑处理。

## 验证方式

1. 编译器编译通过
2. 运行现有 152 个测试全部通过（无回归）
3. 新增测试用例全部通过
4. 使用 `--debug` 查看字节码确认 OP_DTOR_LOCAL 正确生成
