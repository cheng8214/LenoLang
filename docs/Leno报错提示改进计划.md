# Leno 报错提示改进计划

> 背景：在集成 Blend2D + LenoSDL3 的开发过程中，反复遇到了若干"编译器无提示、运行时才崩溃"或"提示过于模糊"的问题。本文档系统性地梳理这些问题，按优先级给出改进方案并落地实施。

---

## 一、问题清单与归类

### A 类：我写错了（无需改编译器，记录即可）
| # | 问题 | 说明 | 状态 |
|---|------|------|------|
| A1 | Blend2D 函数名记错 `bl_context_set_stroke_width_d` | 该函数没有 `_d` 后缀 | 已修复 |
| A2 | clib 签名漏参数 | `fill_path_d(self, origin, path)` 实际 3 参数 | 已修复 |
| A3 | cstruct 值类型与堆指针错配 | 用值类型字段存 `malloc()` 的堆指针 | 已修复 |

### B 类：编译器/框架行为导致（值得改进报错）
| # | 问题 | 当前表现 | 危害 | 优先级 |
|---|------|----------|------|--------|
| B1 | **clib 调用时接收方为 null 不报错** | 直接 0xC0000005 访问违规崩溃 | 极难定位 | 高 |
| B2 | 运行时"索引操作需要对象类型，但实际类型为 'null'" | 不指出是哪个变量、哪个方法 | 排查困难 | 高 |
| B3 | `.lenb` 编译后 `dirs.script_dir()` 语义改变 | DLL 路径解析失败，`ffi.load` 返回 null 无提示 | 隐藏路径问题 | 中 |
| B4 | clib 实例不能作为 struct 字段 / 函数返回 | 引用丢失后报"库对象已释放" | 语义不清 | 中 |
| B5 | 跨模块导入的 clib 跳过参数/类型校验 | 第 418 行"允许任意调用" | 错误延迟到运行时 | 中 |

---

## 二、改进方案（按优先级）

### 改进 1（高）：clib 调用接收方为空时给出明确报错
**现状**：`_b2d.bl_xxx(...)` 里 `_b2d` 若为 `null`，语义检查不拦（TYPE_CLIB 类型），运行时直接解引用崩溃。

**目标**：在 codegen 或运行时把"clib 接收方为 null"转为可读错误。

**实施位置**：clib 调用 codegen（`src/codegen`）。

### 改进 2（高）：运行时 null 索引报错补充上下文
**现状**：`op_utils.inc` 只输出 `value_type_desc(obj_val)`。

**目标**：报错中附加源码行号（已有）+ 尽量提供操作上下文。

**实施位置**：`src/vm/vminc/op_utils.inc`（已有行号），增强文案描述。

### 改进 3（中）：`ffi.load` 全失败时给出汇总提示
**现状**：`lib()` 里 7 个候选路径都失败时 `g_b2d` 保持 null，无提示。

**目标**：当 `ffi.load` 从未被成功调用（或全部候选失败）时，提示"所有候选路径均未找到库"。

**实施位置**：`sdl_blend2d.leno` 的 `lib()`（脚本层包装）+ `ffi.c` 加载失败文案已有，可补充候选路径汇总。

### 改进 4（中）：跨模块 clib 增加"空接收方"提示
**现状**：第 417-420 行"允许任意调用"导致 `null.bl_xxx` 无编译期提示。

**目标**：当接收方类型为 clib 但值为 null 时，在语义阶段给出警告或运行时明确报错。

**实施位置**：`src/semantic/visitinc/visit_expr.inc` + codegen。

---

## 三、已确认的既有良好机制（勿破坏）
- `ffi_load_func` 加载失败已 `native_throw_error`（`ffi.c:419`），含错误码/搜索路径。
- clib 本文件定义时已有参数数量/类型校验（`visit_expr.inc:360-378`）。
- `format_type_error` 的 `%s1~%s4` 位置占位符是**有意设计**，非 bug。

---

## 四、实施记录
（实施过程中逐项勾选）
- [x] 改进 1：clib 空接收方报错 —— 已在 `ffi.c` 的 `ffi_call_impl()` 开头增加 `val_is_null/val_is_obj` 检查，报错含函数名、可能原因、排查建议。验证：`test_null_clib.leno` 由崩溃变为可读运行时错误。
- [x] 改进 2：运行时 null 索引报错文案增强 —— 已在 `op_utils.inc` 的 `OP_INDEX` 增加排查提示。验证：`test_null_index.leno` 报错含提示。
- [x] 改进 3：ffi.load 全失败提示 —— 已在 `sdl_blend2d.leno` 的 `lib()` 增加"所有候选路径均未找到"的汇总打印。
- [x] 改进 4：跨模块 clib 空接收方提示 —— 与改进 1 共用运行时检查（`ffi_call_impl` 对 null 库对象的检查对本地/导入 clib 均生效）。

### 改动文件清单
| 文件 | 改动 |
|------|------|
| `src/module/ffi/ffi.c` | `ffi_call_impl` 开头增加库对象 null 检查，报错含函数名/原因/建议 |
| `src/vm/vminc/op_utils.inc` | `OP_INDEX` null 对象报错增加排查提示 |
| `leno_module/LenoSDL3/lib/sdl_blend2d.leno` | `lib()` 候选路径全失败时打印汇总 |
| `docs/Leno报错提示改进计划.md` | 本文档 |

### 验证结果
- 编译器重新构建成功（gcc 无警告）
- `test_null_clib.leno`：null clib 调用 → 从 0xC0000005 崩溃 → 变为可读运行时错误
- `test_null_index.leno`：null 索引 → 报错含排查提示

### 测试脚本
- `examples/改进测试/test_null_clib.leno`
- `examples/改进测试/test_null_index.leno`
