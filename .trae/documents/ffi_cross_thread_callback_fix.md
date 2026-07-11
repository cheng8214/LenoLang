# FFI 跨线程回调修复方案

## 摘要

修复 SDL3 文件对话框在 Windows 上回调于工作线程触发导致 Leno VM 崩溃的问题。采用方案 A（FFI 回调主线程编组 marshalling）：在 `callback_dispatch` 中检测跨线程调用，将参数打包到线程安全队列，阻塞工作线程等待；主线程通过 `ffi.pump_callbacks()` 取出队列项并在主线程 VM 上执行回调，完成后唤醒工作线程。

## 当前状态分析

### 问题根因
1. SDL3 在 Windows 上通过 `SDL_CreateThread` 在独立工作线程中弹出对话框并触发回调
2. Leno VM 的 `current_exec_vm` 是 `THREAD_LOCAL`，工作线程中为 NULL
3. `callback_dispatch` 中 `if (!vm_ptr) vm_ptr = &vm;` 回退到全局 VM，但该 VM 正被主线程使用 → 并发读写同一 VM 栈/寄存器 → 崩溃
4. 工作线程的 `gc`、`string_table` 均为未初始化的空状态

### 关键文件
- `src/module/ffi/ffi.c` L1992-2131 — `callback_dispatch` 核心函数
- `src/module/ffi/ffi.c` L2133-2348 — `create_callback_trampoline` JIT 生成
- `src/module/ffi/ffi.c` L2361-2418 — `ffi_callback_create_with_sig` 回调注册
- `src/module/ffi/ffi.c` L2430-2558 — `ffi_init_module` 模块方法注册
- `src/vm/vm.c` L39-40 — `VM vm = {0}; int vm_initialized = 0;`
- `src/vm/vm.c` L183 — `THREAD_LOCAL VM* current_exec_vm = NULL;`
- `src/include/platform_thread.h` — 平台线程抽象层（已有 mutex/cond 实现）
- `src/platform/platform_thread.c` — 平台线程抽象层实现（完整）
- `examples/clib/test_sdl_dialog.leno` — 测试脚本（注释需更正）

### 已有基础设施
- `PlatformMutex` / `PlatformCondVar` / `platform_thread_self()` 已实现，可直接使用
- `CallbackRegState` 结构体已包含所有寄存器参数（int_args[6] + float_args[8]）
- `FFICallbackEntry` 全局数组 `g_callback_registry[MAX_FFI_CALLBACKS]`

## 修改方案

### 1. 新增跨线程回调队列数据结构（`ffi.c`）

在 `CallbackRegState` 定义之后添加：

```c
/* ===== 跨线程回调编组（marshalling） ===== */
#define MAX_PENDING_CALLBACKS 64

typedef struct {
    int cb_id;                    /* 回调 ID */
    CallbackRegState regs;        /* 寄存器参数快照 */
    FFIValue result;              /* 主线程执行后的返回值 */
    double float_result;          /* 浮点返回值 */
    int completed;                /* 主线程是否已执行完 */
    PlatformThreadID caller_tid;  /* 调用线程 ID（用于判断是否跨线程） */
} PendingCallback;

static PendingCallback g_pending_queue[MAX_PENDING_CALLBACKS];
static int g_pending_count = 0;
static int g_pending_head = 0;   /* 环形队列头 */
static int g_pending_tail = 0;   /* 环形队列尾 */

static PlatformMutex g_pending_mutex;      /* 队列互斥锁 */
static PlatformCondVar g_pending_cond;     /* 队列有新项条件变量 */
static PlatformCondVar g_complete_cond;    /* 单项完成条件变量 */
static int g_callback_marshal_initialized = 0;

/* 记录主线程 ID（首次 callback_dispatch 时捕获） */
static PlatformThreadID g_main_thread_id = {0};
static int g_main_thread_id_set = 0;
```

### 2. 初始化/销毁跨线程编组设施

在 `ffi_init_module` 开头添加初始化，在合适位置添加销毁：

```c
static void ffi_callback_marshal_init(void) {
    if (g_callback_marshal_initialized) return;
    platform_mutex_init(&g_pending_mutex);
    platform_cond_init(&g_pending_cond);
    platform_cond_init(&g_complete_cond);
    g_pending_count = 0;
    g_pending_head = 0;
    g_pending_tail = 0;
    g_callback_marshal_initialized = 1;
}
```

在 `ffi_init_module()` 函数开头调用 `ffi_callback_marshal_init()`。

### 3. 修改 `callback_dispatch` — 检测跨线程并编组

核心修改逻辑：

```c
static FFIValue callback_dispatch(int cb_id, CallbackRegState* regs) {
    FFIValue result = {0};
    if (cb_id < 0 || cb_id >= MAX_FFI_CALLBACKS || !g_callback_registry[cb_id].active) {
        return result;
    }

    /* ===== 跨线程检测 ===== */
    /* 首次调用时记录主线程 ID */
    if (!g_main_thread_id_set) {
        g_main_thread_id = platform_thread_self();
        g_main_thread_id_set = 1;
    }

    /* 如果当前线程不是主线程，走编组路径 */
    if (g_main_thread_id_set && !platform_thread_equal(platform_thread_self(), g_main_thread_id)) {
        return callback_dispatch_marshal(cb_id, regs);
    }

    /* ===== 同线程：原有逻辑不变 ===== */
    // ... 原有参数提取 + VM 调用代码 ...
}
```

### 4. 新增 `callback_dispatch_marshal` — 跨线程编组

```c
static FFIValue callback_dispatch_marshal(int cb_id, CallbackRegState* regs) {
    FFIValue result = {0};

    platform_mutex_lock(&g_pending_mutex);

    /* 等待队列有空位 */
    while (g_pending_count >= MAX_PENDING_CALLBACKS) {
        platform_cond_wait(&g_pending_cond, &g_pending_mutex);
    }

    /* 将回调参数入队 */
    PendingCallback* pending = &g_pending_queue[g_pending_tail];
    pending->cb_id = cb_id;
    pending->regs = *regs;           /* 拷贝寄存器快照 */
    pending->result = result;        /* 初始化为 0 */
    pending->float_result = 0.0;
    pending->completed = 0;
    pending->caller_tid = platform_thread_self();

    g_pending_tail = (g_pending_tail + 1) % MAX_PENDING_CALLBACKS;
    g_pending_count++;

    /* 通知主线程有新的待处理回调 */
    platform_cond_signal(&g_pending_cond);

    /* 阻塞等待主线程执行完成 */
    while (!pending->completed) {
        platform_cond_wait(&g_complete_cond, &g_pending_mutex);
    }

    /* 取回执行结果 */
    result = pending->result;
    g_callback_float_result = pending->float_result;

    /* 释放队列项（head 不动，因为可能有多项按序完成） */
    g_pending_count--;
    g_pending_head = (g_pending_head + 1) % MAX_PENDING_CALLBACKS;

    /* 通知其他等待空位的线程 */
    platform_cond_signal(&g_pending_cond);

    platform_mutex_unlock(&g_pending_mutex);

    return result;
}
```

**注意**：上述简单的 FIFO 队列 + head/tail 移动有一个问题 —— 如果多个回调同时在队列中等待，head 的移动顺序必须和入队顺序一致。由于 SDL3 对话框一次只有一个回调，这不会是问题。但为了健壮性，改用"完成标记"方式：不移动 head/tail，仅通过 `completed` 标记让调用者取回自己的结果。只有当 head 项完成后才推进 head。

改进方案：使用固定槽位而非严格 FIFO head/tail：

```c
static FFIValue callback_dispatch_marshal(int cb_id, CallbackRegState* regs) {
    FFIValue result = {0};

    platform_mutex_lock(&g_pending_mutex);

    /* 查找空闲槽位 */
    int slot = -1;
    for (int i = 0; i < MAX_PENDING_CALLBACKS; i++) {
        if (g_pending_queue[i].completed && g_pending_count_per_slot[i] == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        /* 队列满，直接返回（极端情况） */
        platform_mutex_unlock(&g_pending_mutex);
        return result;
    }

    /* 填入参数 */
    g_pending_queue[slot].cb_id = cb_id;
    g_pending_queue[slot].regs = *regs;
    g_pending_queue[slot].result = result;
    g_pending_queue[slot].float_result = 0.0;
    g_pending_queue[slot].completed = 0;
    g_pending_queue[slot].in_use = 1;
    g_pending_count++;

    /* 通知主线程 */
    platform_cond_signal(&g_pending_cond);

    /* 阻塞等待自己的槽位完成 */
    while (!g_pending_queue[slot].completed) {
        platform_cond_wait(&g_complete_cond, &g_pending_mutex);
    }

    /* 取回结果 */
    result = g_pending_queue[slot].result;
    g_callback_float_result = g_pending_queue[slot].float_result;

    /* 释放槽位 */
    g_pending_queue[slot].in_use = 0;
    g_pending_count--;

    platform_cond_signal(&g_pending_cond);
    platform_mutex_unlock(&g_pending_mutex);

    return result;
}
```

### 5. 新增 `ffi_pump_callbacks_func` — 主线程泵送

```c
/* ffi.pump_callbacks() - 主线程泵送待处理的跨线程回调
 * 在事件循环中调用，处理所有排队的跨线程回调。
 * 无返回值。
 */
static Value ffi_pump_callbacks_func(int argc, Value* args) {
    (void)argc;
    (void)args;

    if (!g_callback_marshal_initialized) return val_null();

    platform_mutex_lock(&g_pending_mutex);

    /* 处理所有待处理的回调 */
    while (true) {
        /* 查找一个 in_use 且未 completed 的槽位 */
        int slot = -1;
        for (int i = 0; i < MAX_PENDING_CALLBACKS; i++) {
            if (g_pending_queue[i].in_use && !g_pending_queue[i].completed) {
                slot = i;
                break;
            }
        }
        if (slot < 0) break;  /* 没有待处理的 */

        PendingCallback* pending = &g_pending_queue[slot];

        /* 在主线程上执行回调（复用原有逻辑） */
        FFIValue cb_result = callback_dispatch_direct(pending->cb_id, &pending->regs);

        pending->result = cb_result;
        pending->float_result = g_callback_float_result;
        pending->completed = 1;

        /* 唤醒等待的工作线程 */
        platform_cond_broadcast(&g_complete_cond);
    }

    platform_mutex_unlock(&g_pending_mutex);

    return val_null();
}
```

### 6. 重构原有回调执行逻辑为 `callback_dispatch_direct`

将 `callback_dispatch` 中「参数提取 + VM 调用」部分抽取为 `callback_dispatch_direct`：

```c
/* 同线程直接执行回调（原有逻辑） */
static FFIValue callback_dispatch_direct(int cb_id, CallbackRegState* regs) {
    FFIValue result = {0};
    if (cb_id < 0 || cb_id >= MAX_FFI_CALLBACKS || !g_callback_registry[cb_id].active) {
        return result;
    }

    FFICallbackEntry* entry = &g_callback_registry[cb_id];
    FFISignature* sig = entry->sig;
    int total = sig->nargs > FFI_MAX_ARGS ? FFI_MAX_ARGS : sig->nargs;

    FFIArg ffi_args[FFI_MAX_ARGS];
    memset(ffi_args, 0, sizeof(ffi_args));

    /* ... 参数提取逻辑（原有 L2008-2032 不变） ... */

    VM* vm_ptr = current_exec_vm;
    if (!vm_ptr) vm_ptr = &vm;
    if (!vm_initialized) return result;

    /* ... VM 调用逻辑（原有 L2039-2130 不变） ... */

    return result;
}
```

### 7. 修改 `callback_dispatch` 为入口调度函数

```c
static FFIValue callback_dispatch(int cb_id, CallbackRegState* regs) {
    /* 首次调用时记录主线程 ID */
    if (!g_main_thread_id_set) {
        g_main_thread_id = platform_thread_self();
        g_main_thread_id_set = 1;
    }

    /* 跨线程：走编组路径 */
    if (!platform_thread_equal(platform_thread_self(), g_main_thread_id)) {
        return callback_dispatch_marshal(cb_id, regs);
    }

    /* 同线程：直接执行 */
    return callback_dispatch_direct(cb_id, regs);
}
```

### 8. 注册 `ffi.pump_callbacks` 到模块

在 `ffi_init_module()` 末尾、`/* ===== 回调函数 ===== */` 部分添加：

```c
/* ===== 跨线程回调泵送 ===== */
native_register_module_method("ffi", "pump_callbacks", ffi_pump_callbacks_func, 0, -1, -1, TYPE_NULL, TYPE_UNKNOWN, NULL);
```

### 9. 更新测试脚本 `test_sdl_dialog.leno`

主要修改：
1. 更正第 11-15 行注释（Windows 上是跨线程，不是同线程）
2. 在 `while not g_done` 循环中添加 `ffi.pump_callbacks()` 调用

```leno
// 修改 while 循环：
while not g_done {
    ffi.pump_callbacks()     // 泵送跨线程回调（SDL3 对话框在 Windows 上于工作线程触发回调）
    lib.SDL_PumpEvents()
    lib.SDL_Delay(10)
    wait = wait + 1
    if wait > 3000 { print("（等待超时，对话框可能未返回）"); break }
}
```

### 10. 更新 bug 报告文档状态

将 `docs/bug_sdl3_dialog_callback_cross_thread.md` 的状态从"待修复"改为"已修复"。

## 数据结构细节

### `PendingCallback` 结构体

```c
typedef struct {
    int cb_id;                    /* 回调 ID */
    CallbackRegState regs;        /* 寄存器参数快照（从工作线程拷贝） */
    FFIValue result;              /* 主线程执行后的返回值 */
    double float_result;          /* 浮点返回值 */
    int completed;                /* 主线程是否已执行完（0=等待，1=完成） */
    int in_use;                   /* 槽位是否被占用（0=空闲，1=占用） */
} PendingCallback;
```

### 并发流程

```
工作线程 (SDL3 dialog thread)        主线程 (Leno VM event loop)
    │                                      │
    ├─ callback_dispatch(cb_id, regs)      │
    │  └─ 检测: 非主线程                    │
    │     └─ callback_dispatch_marshal()    │
    │        ├─ lock(mutex)                 │
    │        ├─ 填入 pending[slot]          │
    │        ├─ signal(cond)  ──────────>   │
    │        ├─ wait(complete_cond)  <────  ├─ ffi.pump_callbacks()
    │        │  (阻塞，等主线程执行)          │  ├─ lock(mutex)
    │        │                               │  ├─ 找到 pending[slot]
    │        │                               │  ├─ callback_dispatch_direct()
    │        │                               │  │  └─ VM 上执行 Leno 回调
    │        │                               │  ├─ 标记 completed=1
    │        │                               │  ├─ broadcast(complete_cond)
    │        │  <────────────────────────    │  └─ unlock(mutex)
    │        ├─ 取回 result                  │
    │        ├─ 释放 slot                    │
    │        └─ unlock(mutex)                │
    ├─ 返回 result 给 C 调用者               │
```

## 关键设计决策

1. **阻塞工作线程而非丢弃**：SDL3 在回调返回后会立即释放 `filelist`，如果丢弃回调，工作线程立即返回 SDL → `filelist` 被释放 → 主线程读不到数据。必须阻塞工作线程，等主线程读完再放行。

2. **槽位式而非 FIFO 式**：使用固定槽位数组而非 head/tail 环形队列，避免顺序依赖问题。每个工作线程占一个槽位，完成后由自己的线程检查 `completed` 标记取回结果。

3. **`ffi.pump_callbacks()` 显式调用**：需要用户在事件循环中调用。自动泵送（如在 `ffi_call_impl` 返回后自动泵送）虽方便但有两个问题：(a) 可能不在事件循环中调用 ffi.call，(b) 嵌套回调可能导致递归。

4. **平台抽象层**：使用已有的 `PlatformMutex`/`PlatformCondVar`，无需新依赖。

5. **主线程 ID 懒初始化**：首次 `callback_dispatch` 调用时记录主线程 ID，无需额外初始化步骤。

## 文件修改清单

| 文件 | 修改内容 |
| --- | --- |
| `src/module/ffi/ffi.c` | 1) 新增 `PendingCallback` 结构和全局队列 2) 新增 `callback_dispatch_marshal` 3) 将原有逻辑重构为 `callback_dispatch_direct` 4) `callback_dispatch` 改为线程检测+调度 5) 新增 `ffi_pump_callbacks_func` 6) `ffi_init_module` 注册 `pump_callbacks` 并初始化编组设施 |
| `examples/clib/test_sdl_dialog.leno` | 1) 更正注释 2) while 循环中添加 `ffi.pump_callbacks()` |
| `docs/bug_sdl3_dialog_callback_cross_thread.md` | 更新状态为已修复 |

## 验证步骤

1. 编译 Leno 编译器
2. 运行 `leno examples/clib/test_sdl_dialog.leno`
3. 弹出对话框后选择文件并确定
4. 预期：控制台打印 `=== 对话框回调触发 ===`，逐行打印文件路径，主流程输出所选文件列表，最后打印 `done`
5. 运行全部 152 个回归测试，确保无回归
6. 验证同线程回调（如 `qsort`）仍正常工作

## 验证结果（2026-07-11 实测通过）

环境：Windows，Leno VM + `SDL3.dll`，修复方案 A（跨线程回调编组 + `ffi.pump_callbacks()`）已落地。

`leno examples/clib/test_sdl_dialog.leno` 依次弹出三个对话框，全部正常返回，控制台输出符合预期：

- **【打开文件】多选**：成功回调，读取到 9 个文件路径（`D:\CLeno\LenoC\examples\crypto\*.leno`）。
- **【保存文件】**：成功回调，返回 `D:\1.txt`。
- **【选择文件夹】**：成功回调，返回 `D:\软件`——**中文路径也能正确读取**，说明 `ffi.read_string` 的 UTF-8 处理在跨线程编组后仍正常。

关键日志（节选）：

```
window 创建: true
callback 创建: true
>>> 弹出【打开文件】对话框（可多选）...
=== 对话框回调触发 ===
filter index = -1
  文件[0] = D:\CLeno\LenoC\examples\crypto\caesar.leno
  ...
  文件[8] = D:\CLeno\LenoC\examples\crypto\rc4.leno
>>> 弹出【保存文件】对话框...
=== 对话框回调触发 ===
  文件[0] = D:\1.txt
>>> 弹出【选择文件夹】对话框...
=== 对话框回调触发 ===
  文件[0] = D:\软件
done
```

结论：**BUG-FFI-001 已修复验证通过**。对应的 bug 报告（原 `docs/bug_sdl3_dialog_callback_cross_thread.md`）状态应更新为「已修复」；测试脚本 `examples/clib/test_sdl_dialog.leno` 已扩展覆盖打开/保存/文件夹三种对话框。
