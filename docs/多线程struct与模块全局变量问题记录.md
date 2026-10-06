# 多线程 struct 定义与模块全局变量问题记录

> 记录时间：2026-08-16
> 关联：LenoWeb 模块多线程化调查、Leno 运行时线程模型
> 状态：**部分修复**（struct 定义跨线程已修；模块级全局变量串台待重构）

---

## 一、背景

对 LenoWeb 模块进行 struct 化重构后，写了一个多线程并发调用 `htmls.parse()` 的测试
（`leno_module/LenoWeb/examples/03_HTML解析/test_thread_parse.leno`，2026-10-06 归类前为 `examples/test_thread_parse.leno`），发现两个问题：

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

- 最小多线程 struct 测试（**已收编**进 `assert/test_threads.leno`；原 `examples/threads/test_thread_struct.leno`
  于 2026-09-27 随目录删除 —— 收编时补上了断言 `r1==3 / r2==30`，原先只 print）：
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

- `assert/test_threads.leno`：最小多线程 struct 测试（验证问题一已修；2026-09-27 从
  `examples/threads/test_thread_struct.leno` 收编并补断言，原示例随目录删除）
- `leno_module/LenoWeb/examples/03_HTML解析/test_thread_parse.leno`：LenoWeb 多线程 parse 测试
  （验证问题二已修：8 线程并发全部 PASS）

### 后续可选优化

- [ ] 可考虑把"多线程 struct"能力写入 `threads使用指南.md`
- [ ] 若未来需要"每个线程独立模块状态"（类似 Python `threading.local`），
      需在语言运行时层实现模块 globals 的线程隔离（方案 C 的延伸，成本高）

---

## 八、后续：`web_net.leno` 的同类全局状态已移除（✅ 2026-10-06）

方案 A 只改了 `web_html.leno`（解析状态），**HTTP 层还留着同样形状的全局**：

```leno
Response? _currentResp = null      // libcurl 写回调靠它找到"当前响应"
```

已按同一思路重构掉：每个 `HttpClient` 自带一个累加器实例 `_RespAcc`，
由 `ffi.callback` 的**闭包捕获**交给回调（不依赖任何模块级变量）。
实测确认 `ffi.callback` 支持闭包捕获并回写 struct 实例（用 `qsort` 比较函数验证过）。

顺带修掉的两个问题：

| 问题 | 原先 | 现在 |
| --- | --- | --- |
| 回调泄漏 | `close()` 只清 curl handle，不 `ffi.free` 回调；回调上限 **256 个**（实测第 257 个报"回调数量已达上限"）⇒ 模块级 `get()` 到第 **128 次**必挂 | `close()` 里 `ffi.free(writeCb/headerCb)`；`test_http_repeat.leno` 连跑 300 次验证 |
| 大响应体 O(n²) | `body = body + chunk`，每块都整段重拷 | 分块收集 `Array[string]`，结束时 `strings.join` 一次 |

覆盖用例：`examples/tests/test_http_repeat.leno`（离线，走本机服务端）。

### ✅ "HTTP 多线程"已打通——根因在运行时的 FFI 回调（2026-10-06 修）

去掉模块全局**还不够**：实测子线程里 `web.get()` 曾表现为「`statusCode=200` 但 `body=""`」，
两个并发线程直接 `0xC0000005`。用与 LenoWeb 无关的最小复现（`qsort` + `ffi.callback`）测得：

| 场景 | 回调被调用次数 |
| --- | --- |
| 主线程 | ✅ 10 次（= qsort 对 5 元素的实际比较次数） |
| 子线程 | ❌ **1 次**（第一次成功、闭包回写可见；之后"成功但结果丢失"） |
| 子线程 + 并发 | ❌ 直接崩 |

#### 根因（一处：写错了对象）

`src/vm/vm.c` 的 `vm_call_value` 末尾有唯一一行**用宏 `vm`** 的赋值：

```c
    vm.stop_frame_cnt = saved_frame_cnt;      // ✗ 宏 `vm` 在此处已不在作用域
```

该文件里 `#define vm (*current_exec_vm)` 被多处 `#undef vm` 中途摘掉过，到这一行时
`vm` 解析成**全局主 VM** —— 于是 stop 条件被写到主 VM 上，而嵌套解释器循环
（`REG_FINISH_RETURN` / `OP_RETURN`）读的是**执行 VM**（子线程 = 自己的 VM）：

- **主线程**：全局 vm 恰好就是执行 VM ⇒ 一直看不出来 ✓（所以这个 bug 藏了很久）
- **子线程**：子 VM 的 `stop_frame_cnt` 恒为 0 ⇒ 嵌套循环的停止条件失效
  ⇒ 它**把调用方的帧也一路跑完**（`frame_cnt` 1 → 0），回调第二次起"成功但结果丢失"，
  随后 qsort/libcurl 继续在已弹出的帧上跑 ⇒ 内存损坏、崩溃

定位证据（临时探针打印结构体地址，同一个结构体、同一个字段偏移）：

```
vm_call_value:  vm_ptr=0xDCFCBB3630  &vm.stop_frame_cnt=0x7FF6983E66D8   ← 主 VM 的字段
01_prologue:    &vm    =0xDCFCBB3630  &vm.stop_frame_cnt=0xDCFCBFF788   ← 子 VM 的字段
```

#### 修法

`src/vm/vm.c`：改用 `vm_ptr->stop_frame_cnt`（与同函数其余赋值 `last_return_*` /
`host_floor` / stop 的恢复**本来就是** `vm_ptr->`，此行使之一致）。

#### 顺带修掉的第二个并发 bug：回调槽位竞态

`g_callback_registry` 是**进程级共享**的普通全局数组，而 `ffi.callback` 里
"找空槽 → 写槽位"不是原子操作 ⇒ 多线程并发建回调时两个线程会挑中**同一个 cb_id**，
后写者覆盖先写者（先者的 trampoline 分发到别人的闭包）。实测：8 线程 × 6 轮并发抓取时
约 1/3 的响应正文为空或错位（11/48、18/48 项失败）。
修法：`src/module/ffi/ffi.c` 新增 `g_callback_mutex` + `callback_registry_reserve/release`，
把槽位分配与释放串行化（占位后 trampoline 才交给 C 库，故不存在"分发到半初始化槽位"的窗口）。

#### 第三个：`curl_global_init` 被每个客户端调用

`curl_global_init` 是 libcurl 文档明确标注的**非线程安全**函数，原实现放在
`HttpClient.init()` 里 ⇒ 多线程并发建客户端时并发进入全局初始化。
修法：`web_net.leno` 提到**模块加载时**做一次（`int _g_curlGlobalReady = core.globalInit(...)`）。

#### 验证

| 验证项 | 结果 |
| --- | --- |
| 最小复现（`qsort` 驱动的回调） | 主线程 10 / 子线程 10，且排序结果正确 |
| `examples/tests/test_thread_http.leno` | 8 线程 × 6 轮 × 3 种模式（独立 client / 每轮新建 / 并发抓 GBK）全绿，连跑 3 次稳定 |
| LenoWeb 套件（`examples/tests/run_tests.leno`） | 11 个用例 / 233 项断言全绿 |
| **仓库自带断言套件** | **417 passed / 0 failed** —— 证明运行时改动无回归 |

回归用例：`examples/tests/test_ffi_callback_thread.leno`（运行时层）、
`examples/tests/test_thread_http.leno`（LenoWeb 层）。

## 九、接着查到的两个语言缺陷（✅ 2026-10-06 修）

都是在做 LenoWeb「爬虫优先队列」时被踩出来的——表面症状都在业务代码里，
根因都在语言实现。记录在这里，因为**症状与根因距离很远**，下次别再从头查一遍。

### 9.1 `("abc" + 1).len()` 恒为 0（`string_add` 漏设 `char_len`）

- **症状**：LenoWeb 的 `normalizeUrl("http://t/p1")` 返回 `http://t/`（路径被吃掉）。
- **缩小过程**：`pu(1)` 返回的串内容是对的（`byte_len=11`），但 `pu(1).len()` 是 **0**；
  再缩到 `("abc" + 1).len() == 0`，而 `("abc" + "de").len() == 5` 正常。
- **根因**：`ObjString.char_len` 是**字符数缓存**，`str_alloc` 只把它留成 0
  （注释写着"调用者需在填充内容后设置"），而 `len()` / `slice` / `to_lower` 都直接读它。
  `src/vm/vm.c` 的 `string_add` 在"两侧不都是字符串"的分支（`value_to_string` 再拼）
  建串后**漏了这一行**；两侧都是字符串时走 `str_concat` → `str_new`（内部已算）⇒ 那条路一直正常。
- **为什么难发现**：内容、`byte_len()`、打印都正确，只有 `len()` 系坏掉，而且会**传染**
  （`.to_lower()` 原样复制 0、`.slice(0, 2)` 返回空串）。纯 ASCII 且不用 `len()` 的代码完全无感。
- **修法**：`string_add` 补 `result->char_len = utf8_char_len(result->chars, total_len);`
  ＋ `src/string_table.c` 的 `intern_string`（当前无调用方）同类补漏。
- **回归**：`assert/test_str_concat_char_len.leno`。

### 9.2 `use mod.(StructType)` 后，跨模块方法默认参数丢失

- **症状**：`Crawler.enqueue(url, depth)` 的 `priority` 变成 `null` ⇒ 优先队列的
  同级 FIFO 比较被破坏（`null != null` 走成了"不相等"，于是堆不再交换）。
- **缩小过程**：同模块下 struct 方法默认参数正常；跨模块**模块级函数**也正常
  （`assert/xmod_defaults.leno` 已有覆盖）；只有**跨模块 struct 方法**不行。
  最后定位到触发条件是 **`use mod.(StructType)`**：把类型 use 进来才坏，
  用限定名（`new mod.StructType()`）就正常；把 `assert/xmod_defaults.leno`
  原样拷到别的目录也一样坏 ⇒ 与文件内容/位置/缓存都无关。
- **根因**：`use` 导入类型会在本编译单元留下一个**没有默认值表达式**的方法桩。
  而 `src/codegen/codegen_expr.c` 的 `gen_method_call` 里，两条补齐路径原先**互斥**：
  ① 本地有真 AST ⇒ 用 AST 求值默认值（桩不是真 AST ⇒ 发 `LOADNIL`）；
  ② 本地完全没有 AST ⇒ 查导入模块符号表（桩存在 ⇒ 这条被跳过）。
  ⇒ 两条都落空，省略的实参就是 null。
  字节码实证：`OP_LOADNIL A=7` 紧挨 `OP_INVOKE_METHOD_TYPED … 实参个数=3`。
- **修法**：符号表查找不再要求"本地无 AST"；AST 缺默认值表达式时**逐个参数**回退到
  符号表的 `param_default_texts`。
- **回归**：`assert/test_use_struct_default_param.leno`（`assert/xmod_defaults.leno` 补了个 `mkCalc()` 工厂）。

### 9.3 ✅ 链式接收者 + 省略默认参数（2026-10-06 修）

**症状**：`m.mkCalc().plus()` ⇒「调用方法 'plus' 时参数不足: 至少需要 1, 实际 0」
（`func plus(int v = 10)` 的最小实参应为 **0**）。与 `use` 无关 —— 换成变量接收者
`var c = m.mkCalc(); c.plus()` 就能过，**差别只在接收者的写法**。

**根因（两处，必须一起修）**：语义分析只对"接收者是变量/字段链"的形态把 receiver 插进
`args[0]`（`a.f(x)` ⇒ args=[a, x]），**链式不插** ⇒ 实参与形参的槽位坐标差一格：

1. **语义侧元数检查**：`required_arity = expected_arity - fallback_default_count`，
   而 `use` 留下的桩 `default_count == 0` ⇒ 把带默认值的形参当必填
   （修：桩缺默认计数时回退导入模块符号表，并按 `param_default_texts` 兜底计数）。
2. **codegen 补默认值**一律按"含 self"的坐标写槽 ⇒ 链式形态整体错一格：真默认值落到
   **没人读的下一格**、形参拿到 nil ✗（修：先判定 args 是否含隐式 self，把布局归一化成
   `R[base]=receiver、R[base+1]=self 副本、R[base+2..]=实参` —— 正是 VM 侧
   `OP_INVOKE_METHOD_TYPED` 的口径，它自己也用 `R(a+1)==receiver` 判 self 在不在实参里）。

两条实现上的坑（都留了注释）：

- `self 副本` 必须是**寄存器 MOV**（接收者求值之后），不能把接收者表达式再求值一遍 ——
  否则 `mkCalcCounted()` 这类带副作用的接收者会被调用两次。
- 归一化**只对 struct 方法**生效：dict / native / 一等函数字段的 `args[0]` 从来不是
  receiver，一起挪会把它们的实参整体错位（第一版就是这么写错的，靠 419 项套件当场发现）。

**回归**：`assert/test_use_struct_default_param.leno`（链式 + 副作用计数 + 各模块形态对照）。

### 验证（§9 三个缺陷修完后的最终状态）

| 验证项 | 结果 |
| --- | --- |
| 仓库自带断言套件 | **419 passed / 0 failed**（含 9.1 / 9.2 / 9.3 的回归用例） |
| LenoWeb 套件 | **14 个用例 / 333 项断言全绿** |
| LenoWeb 离线示例 | 43 个 / 0 失败 |
