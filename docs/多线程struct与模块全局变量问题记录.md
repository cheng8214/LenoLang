# 多线程 struct 定义与模块全局变量问题记录

> 记录时间：2026-08-16
> 关联：LenoWeb 模块多线程化调查、Leno 运行时线程模型
> 状态：**部分修复**（struct 定义跨线程已修；模块级全局变量串台待重构）

---

## 一、背景

对 LenoWeb 模块进行 struct 化重构后，写了一个多线程并发调用 `htmls.parse()` 的测试
（`leno_module/LenoWeb/examples/test_thread_parse.leno`），发现两个问题：

1. **struct 类型在子线程不可见**：`new HtmlNode()` 报 `未定义的结构体 'HtmlNode'`
2. **模块级全局变量多线程串台**：多个线程并发 `parse()` 时解析结果互相污染（标题串台、节点丢失）

---

## 二、Leno 线程模型（关键前提）

`src/object/object_thread.c` 的 `thread_entry_point` 揭示了线程模型：

- **每个子线程有独立 VM**（`child_vm`）：独立栈、GC、字符串表
- **主脚本全局变量会深拷贝到子线程**（`child_vm.globals` 深拷贝，基本类型直接复制值）
- **cstruct 定义表是全局共享**（不带 `THREAD_LOCAL`，只读类型元数据跨线程共享）
- 子线程入口还做了：`native_init_module("threads")`、`native_init_module("regexs")`、`threads_init_instance_methods()`

---

## 三、问题一：struct 定义跨线程不可见（✅ 已修复）

### 根因

- 普通 `struct`（非 cstruct）的定义表 `struct_def_table` 在
  `src/object/object_struct.c` 中声明为 `static THREAD_LOCAL`（线程局部变量）。
- 子线程创建时，自己的 `struct_def_table` 是**空表**。
- 子线程执行 `new HtmlNode()` 时，`struct_def_find("HtmlNode")` 在空表中查不到
  → 报 `未定义的结构体 'HtmlNode'`。
- 这与 cstruct 形成对比：cstruct 定义表**不带** `THREAD_LOCAL`（全局共享），
  所以 cstruct 在所有线程可见；普通 struct 反而被标成线程局部，子线程不可见。
  **这是设计上的不一致，本质是 bug。**

### 修复内容（3 个文件，+60 行）

**`src/object/object_struct.c` 新增 3 个跨线程接口：**

```c
// 返回当前线程结构体定义表数量（主线程抓取快照）
int struct_def_get_count(void);

// 返回当前线程结构体定义表中第 i 个定义
ObjStructDef* struct_def_get(int i);

// 将主线程定义的结构体导入当前（子）线程的定义表。
// struct 定义是只读类型元数据，与 cstruct 定义一样可跨线程共享。
void struct_def_import_from_thread(ObjStructDef** defs, int count);
```

**`src/include/leno_value.h`**：声明上述 3 个函数。

**`src/object/object_thread.c`：**
- `ThreadStartArgs` 结构体新增字段：
  ```c
  ObjStructDef** struct_defs;   // 主线程结构体定义快照（供子线程注册）
  int struct_def_count;
  ```
- `thread_new_with_args()`（主线程上下文）：调用 `struct_def_get_count()` / `struct_def_get()`
  抓取主线程 struct 定义快照，存入 args。
- `thread_entry_point()`（子线程上下文）：
  - 在 `threads_init_instance_methods()` 之后调用
    `struct_def_import_from_thread(saved_struct_defs, saved_struct_def_count)`
    把主线程定义注册进子线程。
  - 闭包失败、协程失败、正常退出三条路径都 `free(saved_struct_defs)` 释放快照内存。

### 验证

- 最小多线程 struct 测试（`examples/threads/test_thread_struct.leno`）：
  两个子线程各自 `new Point()`，正常返回结果，无崩溃、无卡死。
- 结论：修复后普通 struct 在子线程可用，与 cstruct 行为一致。

---

## 四、问题二：模块级全局变量多线程串台（⚠️ 未修复，待重构）

### 根因

`web_html.leno` 用**模块级可变全局变量**做解析状态：

```leno
var _g_pos = 0       // 解析光标（模块级全局）
var _g_html = ""     // 当前 HTML 文本（模块级全局）
```

`_parseText` / `_parseAttrs` / `_parseElement` / `parse` 共 **63+ 处**引用这两个变量。

- 模块级全局变量存在 **`ObjModule.globals`**（`src/include/leno_value.h` 中
  `ObjModule` 结构体的字段），**不在** `child_vm.globals`。
- `object_thread.c` 的深拷贝只复制 `child_vm.globals`（主脚本全局变量），
  **不复制模块对象的 globals**。
- `.leno` 模块由 `loaded_modules` **全局缓存**管理，主线程和子线程访问的是
  **同一个共享 `ObjModule`** → `_g_pos` / `_g_html` 跨线程共享。
- 多个线程并发 `parse()` 时读写同一对变量 → 竞态：光标互相移动、文本互相覆盖。

### 实测现象（`test_thread_parse.leno`，8 线程）

- 部分线程解析正确（`title='Thread-N'`，p 数量正确）
- 部分线程失败：`h1 为 null!`（`_g_html` 被别的线程覆盖）
- 部分线程内容串台：`p[2] 不含 item-: 'ite4/>m4ad7h1p>tm70t--tm>-`（多个线程 HTML 混合）

### 与 Python 的对照

- Python `HTMLParser` / `html5lib` 也**不支持并发 parse**，原因相同：解析状态在
  解析器实例内部而非模块级。官方做法是"每个解析器一个实例"。
- Python `threading.local()` 提供线程局部变量，但 Leno 尚无此机制。

---

## 五、后续优化方向（方案评估）

### 方案 A：重构 web_html.leno，移除模块级全局解析状态（✅ 推荐，治本）

- 把 `_g_pos` / `_g_html` 收进一个**解析上下文对象**（例如 `_ParseCtx` struct：
  `string html` + `int pos`）。
- `parse()` 每次创建独立上下文，内部解析函数通过上下文读写。
- 收益：
  - 天然线程安全，支持"多线程并发解析不同 HTML"
  - 与 Python html5lib 的"解析器实例化"设计一致
  - 只改 `web_html.leno` 一个文件（63 处引用替换），不碰语言运行时，风险可控
- 成本：60+ 处 `_g_pos`/`_g_html` 引用要改为上下文成员访问。

### 方案 C：改语言运行时，让模块级变量按线程隔离（❌ 不建议）

- 让 `.leno` 模块的 globals 也按线程深拷贝隔离。
- **硬性障碍**：
  1. `ObjModule` 由 `loaded_modules` 全局缓存共享，子线程 import 同一模块返回
     同一个共享对象，globals 仍是共享的。
  2. 强行隔离会破坏模块缓存一致性、模块全局语义（模块级状态本该跨调用共享）、
     GC 的模块对象 mark 逻辑。
  3. 这是动语言核心根基，风险不可控，可能引发新的内存/并发 bug。
- **唯一值得做的场景**：未来 Leno 若需要类似 Python `threading.local()` 的能力，
  那是独立需求，不是修此 bug 的合理手段。

### 结论

**只实行 A，不实行 C。** C 的技术障碍（模块共享缓存）决定了它不适合作为修复手段。

---

## 六、方案 A 实施记录（✅ 已完成，2026-08-16）

### 重构内容（`leno_module/LenoWeb/lib/web_html.leno`）

把解析器的模块级全局状态改为**解析上下文对象**：

```leno
// 解析上下文：每次 parse 调用创建独立实例，保证多线程并发解析安全
struct _ParseCtx {
    string html   // 当前解析的 HTML 文本
    int pos       // 当前解析光标
    func byte(int off): int { ... }   // 越界安全的字节读取
}
```

- 删除模块级全局 `var _g_pos = 0` / `var _g_html = ""`
- 新增 `_ParseCtx` struct
- 解析函数全部改为接收 `_ParseCtx` 参数，全局访问改为 `ctx.html`/`ctx.pos`：
  - `_parseTagName(_ParseCtx ctx)`
  - `_skipWs(_ParseCtx ctx)`
  - `_parseAttrs(_ParseCtx ctx)`
  - `_parseText(_ParseCtx ctx)`
  - `_parseElement(_ParseCtx ctx)`
- `parse(html)` 每次创建独立 `_ParseCtx` 实例（`new _ParseCtx()`），天然线程安全

### 验证结果

| 测试 | 修复前 | 修复后 |
|------|--------|--------|
| 多线程并发 parse（8 线程） | 大量 FAIL：标题串台、`h1` 为 null、p 内容污染 | ✅ **8 个线程全部 OK** |
| html_test（18 项选择器） | — | ✅ |
| test_pseudo（伪类） | — | ✅ |
| test_css_attr（属性选择器） | — | ✅ |
| test_table（表格提取） | — | ✅ |
| crawl_quotes（真实爬虫） | — | ✅ 10 条名言全部正确 |

多线程串台问题**彻底解决**，单线程功能**零回归**。

---

## 七、补充

### 已保留的改动

- struct 定义跨线程导入（方案：快照传给子线程注册）是**正确且必要**的，
  否则连"单个子线程使用 struct"都不行，建议保留。

### 相关测试文件

- `examples/threads/test_thread_struct.leno`：最小多线程 struct 测试（验证问题一已修）
- `leno_module/LenoWeb/examples/test_thread_parse.leno`：LenoWeb 多线程 parse 测试
  （验证问题二已修：8 线程并发全部 PASS）

### 后续可选优化

- [ ] 可考虑把"多线程 struct"能力写入 `threads使用指南.md`
- [ ] 若未来需要"每个线程独立模块状态"（类似 Python `threading.local`），
      需在语言运行时层实现模块 globals 的线程隔离（方案 C 的延伸，成本高）
