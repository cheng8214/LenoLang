调查完成，以下是 LenoC 语言当前功能全景和差距分析：

## LenoC 已有功能（相当丰富）

| 类别 | 已实现功能 |
|------|-----------|
| **基本类型** | int/float/string/bool/null/bigint/array/dict/any/function |
| **C布局类型** | i8~u64/f32/f64/cstruct/Ptr，完整 FFI 互操作 |
| **运算符** | 算术/位运算/比较/逻辑/自增自减/复合赋值/is/as/in/三元/范围 |
| **控制流** | if/eif/else/while/for/switch/break/continue/return |
| **函数** | 闭包/匿名函数/默认参数/尾调用优化/前向引用/一等公民 |
| **面向对象** | struct(字段+方法+impl)/face(接口)/enum(枚举)/cstruct(C布局) |
| **错误处理** | try/catch/finally/throw |
| **并发** | threads+Channel / async+await+EventLoop |
| **模块** | import/export/use/别名/.leno模块编译 |
| **字符串** | 插值$/原始@/UTF-8/中文标识符/丰富方法 |
| **VM优化** | NaN-tagging/类型特化指令/内联缓存/常量折叠/死代码消除/分代GC |
| **工具链** | 编译器/VM/REPL/LSP/--compile/--pack/--debug |
| **内置模块** | io/maths/strings/arrays/dicts/files/times/rands/jsons/sockets/dirs/ffi/threads/asyncs/types/assert/sys |

### 类型系统深度（超出预期）

| 特性 | 说明 |
|------|------|
| **泛型容器** | `Array[int]`, `Dict[string, int]`, `Array[Array[int]]` 完整支持 |
| **类型推断** | `var` 自动推断，数组/字典元素类型精确推导 |
| **类型守卫** | `if x is Array[Point]` 支持具体泛型类型守卫 |
| **函数类型** | `func(int):int` 可作为参数、返回值、闭包 |
| **安全转型** | `as` 运算符，匹配返回原值，不匹配返回 null |

### Face 接口系统（非常完善）

| 特性 | 说明 |
|------|------|
| **隐式实现** | 鸭子类型，方法签名匹配自动满足 |
| **显式声明** | `struct Cat impl Speaker` 编译期检查 |
| **多接口实现** | `struct FileLogger impl Writer, Speaker` |
| **多态参数** | `func printArea(Shape s)` 接受任何实现者 |
| **类型守卫** | `if obj is Speaker` 运行时检查 |
| **安全向下转型** | `var c = s as Circle` face/struct 安全转型 |

---

## 缺失或可改进的功能

### 🔴 高优先级（常用语言必备）

| 缺失功能 | 说明 |
|---------|------|
| **正则表达式** | strings.find() 仅支持简单模式，缺少 regex 模块 |
| **自定义异常类型** | 目前只能 throw 字符串，无法定义异常类和分类 catch |
| **HTTP 客户端/服务器** | 有 socket 但无 http 模块，网络应用必备 |
| **函数泛型参数** | 容器泛型已完整，但无法写 `func foo<T>(x: T)` |

### 🟡 中优先级（提升语言表达力）

| 缺失功能 | 说明 |
|---------|------|
| **Set 集合类型** | 去重、集合运算，常用数据结构 |
| **Tuple 元组类型** | 多返回值、解构赋值 |
| **迭代器协议** | Generator/yield，统一可迭代对象遍历 |
| **包管理器** | 依赖管理、版本控制 |
| **命令行参数解析** | 有 `_args()` 但无 argparse 模块 |
| **加密/哈希** | md5/sha256/aes 等，安全场景必备 |
| **日志模块** | log 级别控制、格式化输出 |

### 🟢 低优先级（锦上添花）

| 缺失功能 | 说明 |
|---------|------|
| **struct 继承** | 组合优于继承，当前 impl face 已够用 |
| **模式匹配/解构** | 语法糖，可用 if+is 替代 |
| **宏系统/编译时反射** | 元编程，高级特性 |
| **源码级调试器** | 目前只有 --debug 字节码输出 |
| **增量式 GC** | 当前分代 GC 已可用，进一步减少暂停 |
| **标准文档生成工具** | 从注释生成文档 |

---

## 总结

**LenoC 核心语言特性已经相当完整，VM 优化达到专业水准：**

1. **类型系统**：容器泛型、类型守卫、函数类型、安全转型都已实现，超出预期
2. **Face 接口系统**：隐式/显式实现、多接口、多态、安全转型，非常完善
3. **VM 性能**：NaN-tagging、类型特化指令、内联缓存、分代GC，专业级优化

**真正的短板：**

1. **标准库覆盖面** —— 正则、HTTP、加密是实际项目的高频需求
2. **函数泛型参数** —— `func foo<T>(x: T)` 让泛型表达力更完整
3. **自定义异常类型** —— 从字符串异常升级到结构化异常处理

**类型系统深度已很高**，只需要补充函数泛型就能达到生产级语言的类型表达能力。
