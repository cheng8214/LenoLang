# Bug 报告：SDL3 原生文件对话框回调在 Windows 上跨线程调用导致 Leno VM 崩溃

| 字段 | 内容 |
| --- | --- |
| 编号 | BUG-FFI-001 |
| 模块 | `module/ffi`（跨线程 C 回调）、`clib` 调用、`SDL3.dll` 文件对话框 |
| 严重级别 | 高（功能完全不可用 + 进程崩溃/死锁） |
| 状态 | 已修复（通过 FFI 跨线程回调编组机制） |
| 平台 | Windows（Windows 线程模型相关）；Linux 下 SDL 行为不同，见影响章节 |
| 报告时间 | 2026-07-11 |

---

## 1. 现象

测试脚本 `examples/clib/test_sdl_dialog.leno` 用于验证「SDL3 原生文件对话框 + Leno `ffi` 回调」链路。

运行 `leno examples/clib/test_sdl_dialog.leno` 后：

1. 对话框能正常弹出，用户也可正常选择文件并点「确定」。
2. **回调 `on_dialog` 内的任何代码都没有执行**——控制台没有 `=== 对话框回调触发 ===` 这一行（见 `test_sdl_dialog.leno:45`），也没有任何后续 `print` 输出。
3. 主流程 `while not g_done`（`test_sdl_dialog.leno:91`）一直空转，30 秒超时后才打印 `(等待超时，对话框可能未返回)`。
4. 进程表现为「卡住」或直接崩溃/无响应，没有任何异常栈、没有任何 Leno 侧日志。

> 关键判断：由于连回调第一行的 `print` 都没有输出，问题**不是** `while` 循环逻辑（文件遍历 `test_sdl_dialog.leno:54-62` 是无辜的），而是**回调本身压根没有被正确调用进 Leno VM**。

---

## 2. 复现步骤

1. 确保 `SDL3.dll` 在 `PATH` 或工作目录（`clib` 通过 `ffi.load("SDL3.dll")` 加载，见 `test_sdl_dialog.leno:68`）。
2. 执行 `leno examples/clib/test_sdl_dialog.leno`。
3. 在弹出的「打开文件」对话框中选择若干文件，点击确定。
4. 观察控制台：无 `=== 对话框回调触发 ===` 输出，等待约 30 秒后超时打印。

---

## 3. 期望行为

用户点击确定后，回调 `on_dialog`（`test_sdl_dialog.leno:44`）应在**主线程**执行，依次打印 `=== 对话框回调触发 ===`、各文件路径，并把结果写入 `g_files`，最终由主流程（`test_sdl_dialog.leno:98-111`）输出所选文件列表。

---

## 4. 根因分析

根因是 **SDL3 文件对话框回调在 Windows 上于独立工作线程触发，而 Leno VM 是单线程、不可重入的线程局部运行时**，两者撞车。

### 4.1 SDL3 侧：Windows 上回调在独立工作线程触发

SDL3 官方签名与我们的声明一致：

```c
// SDL_ShowOpenFileDialog
void SDL_ShowOpenFileDialog(
    SDL_DialogFileCallback callback,
    void *userdata,
    SDL_Window *window,
    const SDL_DialogFileFilter *filters,
    int nfilters,
    const char *default_location,
    bool allow_many);
```

官方文档明确：

> This function should be called only from the **main thread**. The callback **may be invoked from the same thread or from a different thread**.

查看 SDL3 官方源码 `src/dialog/windows/SDL_windowsdialog.c`，Windows 实现内部起了一个工作线程跑 COM 原生对话框（`IFileDialog`）：

```c
thread = SDL_CreateThread(windows_file_dialog_thread,
                          "SDL_Windows_ShowFileDialog", args);
SDL_DetachThread(thread);
```

**回调 `callback(userdata, files, filter)` 正是在这个 `windows_file_dialog_thread` 工作线程里被调用的**，而非调用 `SDL_ShowOpenFileDialog` 的主线程。

### 4.2 Leno 侧：VM 运行时是线程局部，且只在主线程初始化

Leno 的整个运行时状态都是 `THREAD_LOCAL`（线程局部），并且只在主线程初始化过一次：

- `current_exec_vm` —— `src/vm/vm.c:183`
  ```c
  THREAD_LOCAL VM* current_exec_vm = NULL;
  ```
- `gc` —— `src/gc.c:53`
- `string_table` —— `src/string_table.c:10`
- `tombstone`（io）等同样为线程局部

工作线程有**自己独立的** `current_exec_vm == NULL`、`gc` 为空、`string_table` 为空。

### 4.3 撞车点：`callback_dispatch` 的回退逻辑

回调 trampoline 进入 `callback_dispatch`（`src/module/ffi/ffi.c:2035` 附近）：

```c
VM* vm_ptr = current_exec_vm;
if (!vm_ptr) vm_ptr = &vm;      // 工作线程里 current_exec_vm==NULL → 回退到全局 &vm
if (!vm_initialized) return result;
```

- 工作线程中 `current_exec_vm == NULL` → 回退到全局 `&vm`，但**该全局 VM 正被主线程的 `while` 循环占用**（同一份栈/寄存器上下文被两个线程同时读写）。
- 工作线程的 `gc`、`string_table` 是未初始化的空状态。
- 回调第一步 `print("=== 对话框回调触发 ===")` / `ffi.read_ptr`（会触发 `gc_alloc` 分配字符串）就在一个「无 GC、无字符串表、还和主线程抢同一 VM 栈」的环境里执行 → **直接崩溃或死锁**，所以连一行 `print` 都出不来。

### 4.4 附加问题：测试脚本注释是错误假设

`test_sdl_dialog.leno:11-12` 的注释写的是：

```
//   - Windows 上这些函数内部是模态框，回调在“函数返回前”于调用线程同步触发，
//     因此与 qsort 回调同线程，不存在跨线程问题（Linux 上跨线程需谨慎）。
```

这与 SDL3 实际实现**相反**：Windows 上正是跨线程触发的。该注释属于误导，应在修复时一并更正。

---

## 5. 影响范围

- **Windows 平台**：所有通过 Leno `ffi` 注册、并期望在「非主线程」被触发的 C 库回调，都会撞到同一问题（不只是 SDL 对话框，任何内部起线程的 C 库均如此）。
- **Linux 平台**：SDL3 在 Linux 上实现不同（通常通过事件队列、在主线程泵送事件时触发回调），是否跨线程取决于具体 backend，需另行验证，但根因（Leno 不可跨线程重入）同样存在。
- 当前仅影响 `ffi` 回调的跨线程场景；同线程回调（如 `qsort` 比较函数）不受影响。

---

## 6. 修复方案

核心难点：回调**必须**在主线程、用主线程那份 VM/GC 来跑（因为它要写全局 `g_files`、要 `print`）；而 SDL 在回调返回后会**立即释放 `filelist`**，所以不能「存指针、事后读」。

### 方案 A：FFI 回调主线程编组（marshalling）—— 推荐，治本

改造 `src/module/ffi/ffi.c` 的 trampoline：

1. 检测到当前线程非主线程时，把原始寄存器参数（按回调签名类型）打包成一个结构，推入一个**线程安全队列**，并**阻塞当前工作线程**等待一个信号量。
2. 主线程在事件循环里调用新增的 `ffi.pump_callbacks()`（或自动在 `clib`/`ffi` 调用返回后泵送），取出队列项，在**主线程 VM** 上执行对应 Leno 回调。
3. 执行完毕后唤醒工作线程放行（此时 `filelist` 尚未被 SDL 释放，读取安全）。

- 优点：通用，所有跨线程 C 回调统一受益，一次性补齐 Leno 跨线程回调能力。
- 缺点：需改动运行时（加锁队列 + 主线程泵送点），工作量中等。

### 方案 B：纯 C 采集回调，绕过 Leno VM —— 改动小、先打通链路

提供一个内建的 C 回调采集器：只把 `filelist` 的每个字符串 `strdup` 拷贝进一个全局 C 缓冲区并记录数量，置标志位；**不碰 VM**。主线程在 `while` 循环里检测到标志后，用 `ffi.read_string` 逐个读出再 `free`。

- 优点：改动小、稳定，不涉及跨线程 VM 重入。
- 缺点：需为 `ffi` 增加「对话框专用采集器」，通用性差，仅解决本场景。

### 方案 C：换用同步 API —— 不可行

SDL3 的三个对话框（`SDL_ShowOpenFileDialog` / `SDL_ShowSaveFileDialog` / `SDL_ShowOpenFolderDialog`）**全部为异步 + 可能跨线程**，官方不提供同步版本，此路不通。

---

## 7. 临时规避

在方案 A/B 落地前，可用 **方案 B 思路**先打通对话框链路；或在文档中标注：当前 Leno `ffi` 回调**仅保证同线程安全**，跨线程回调（SDL3 文件对话框、部分异步 C 库）暂不可用。

---

## 8. 相关文件

| 文件 | 说明 |
| --- | --- |
| `examples/clib/test_sdl_dialog.leno` | 复现脚本；第 11-12 行注释为错误假设，需更正 |
| `src/module/ffi/ffi.c` | 回调 trampoline / `callback_dispatch`（`~2035` 行）；`ffi.callback` 注册 |
| `src/vm/vm.c:183` | `THREAD_LOCAL VM* current_exec_vm` |
| `src/gc.c:53` | 线程局部 `gc` |
| `src/string_table.c:10` | 线程局部 `string_table` |
| `docs/FFI使用指南.md` | FFI/回调使用文档，目前未覆盖跨线程约束，建议补充 |

---

## 9. 验证修复后的预期

修复后运行 `examples/clib/test_sdl_dialog.leno`：

1. 控制台打印 `=== 对话框回调触发 ===`。
2. 逐行打印所选文件路径。
3. 主流程正常输出「所选文件」列表，进程正常结束打印 `done`。
