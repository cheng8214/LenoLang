# LenoFS v0.2.0

Leno 的文件系统扩展（**纯 Leno**）：补 `dirs` 缺的三件（shutil 的核心）+ 目录变化监控。

```leno
import "FS" as fs
import "Result" as res
use res.Result
use fs.FsCount

Result[FsCount] r = fs.rmtree("build/tmp")       // 递归删除（文件或目录；不存在也算成功）
if r.isErr() { print(r.errText()) }
print(_str(r.val.fileCount) + " 个文件")

fs.copytree("src", "backup/src")                 // 整树复制（自动建父目录，目标已存在则合并）
fs.copy("a.zip", "b.zip")                        // 单文件复制（二进制安全）
fs.count("src")                                  // Result[FsCount]{fileCount, dirCount}
```

## API

★ 2026-11-09（P2）：返回值从 `Dict{ok,files,dirs,err}` 改成 **`Result[FsCount]`** ——
失败带 `ErrCode`（`NOT_FOUND` / `INVALID_ARG` / `CONFLICT` / `IO`）+ 可读 `msg`，
成功带强类型计数（`val.fileCount` / `val.dirCount`），不用再 `_str(d["err"])` / `as int`。

| 函数 | 返回 | 说明 |
|---|---|---|
| `rmtree(path)` | `Result[FsCount]` | 递归删除；路径不存在也算成功（计数 0，幂等） |
| `copytree(src, dst)` | `Result[FsCount]` | 整树复制；`dst` 已存在则**合并覆盖**；源非目录 ⇒ `NOT_FOUND`，目标是文件 ⇒ `CONFLICT` |
| `copy(src, dst)` | `Result[FsCount]` | 单文件复制；源是目录 ⇒ `INVALID_ARG`（请用 copytree） |
| `count(path)` | `Result[FsCount]` | 目录项数量（删前确认 / 复制后核对）；只是统计，从不失败 |

## 口径

- **不跟随符号链接**：链接本身被删除/复制内容，但不递归进链接目录（避免 junction 指回祖先时无限展开）
- **不保留 mtime / 权限 / 属性**：跨平台一致的成本太高；要保留请调系统工具
- 二进制安全（`files.read/write` 按字节，含 NUL 与 0xFF）
- 失败一律 `Result.isErr()` + `code` 分清"源不存在 / 类型用错 / 目标冲突 / 删不掉"

## 目录变化监控（v0.2 新增）

```leno
import "FS" as fs

var w = fs.watch("d:\\proj", recursive = true)   // engine 默认 "auto"
if not fs.isWatching(w) { print(w.err) }         // 创建失败原因在这里

// 主循环 / 每帧
Array[Dict] evs = w.poll()                       // **非阻塞**：没有变化返回 []
for evs to e {
    // e["action"]: "add" | "del" | "mod" | "rename" | "overflow"
    // e["path"]  : 相对 watch 根的路径（统一用 '/' 分隔）
    print((e["action"] as string) + "  " + (e["path"] as string))
}
w.stop()
```

**为什么是 `poll()` 而不是回调**：Leno 的闭包**不能跨线程调用**（`threads` 只传值），
回调式 API 就得把事件搬回主线程再调，反而更绕。poll 模型下不需要后台线程——
Windows 的 `ReadDirectoryChangesW` 本来就是"先发请求、再查事件"。

| 引擎 | 平台 | 机制 | 延迟 / 开销 |
|---|---|---|---|
| `native` | Windows | `ReadDirectoryChangesW`（overlapped + 事件，非阻塞） | 内核级，几乎即时 |
| `poll` | 全平台 | `dirs.walk` + `stat` 比对 size/mtime | 每 `intervalMs`（默认 300）一轮，一轮扫全树 |

`engine` 参数：`"auto"`（默认，Windows 用 native、其它用 poll）/ `"native"` / `"poll"`。

**口径与边界**（都是刻意的）：

- **一次 `poll` 会排空所有已完成的事件**，调用方不用每帧调多次；同一次 poll 内同路径合并
  （一次「保存」Windows 会连报 修改/修改，这里只留一条；`add` 不会被紧跟的 `mod` 覆盖）
- `poll` 引擎的 `mod` 靠 **size + mtime** 判断 ⇒ mtime 精度是**秒**，同一秒内改两次且大小不变
  可能漏报（要更严就自己 hash）
- **缓冲区溢出**：Windows 侧变化太密集时会回一个 `{action: "overflow", path: ""}` 事件，
  调用方应据此**全量重扫**（这是 Windows 官方口径，不是错误）
- `stop()` 之后 `poll()` 返回空数组，重复 `stop()` 安全
- ⚠ **Linux 的 inotify 引擎未实现**（本机没有 Linux 环境可实测，不写未验证代码）——
  Linux/macOS 上请用默认的轮询引擎；`engine="native"` 在非 Windows 上返回 `err` 而不是
  静默退化，避免调用方误以为拿到内核事件
- `rename` 只给动作名，不给"旧名"（Windows 会连发两条 rename，`path` 分别是新旧名）；
  轮询引擎识别不出改名，会表现为 `del` + `add`

## 验证

```bat
build\leno.exe build\leno_module\LenoFS\test\test_fs.leno
build\leno.exe build\leno_module\LenoFS\test\test_watch.leno     :: 双引擎各跑 add/mod/递归/rename/del/stop
```

6 组：count（3 文件/3 目录）/ copytree（逐文件校验，含 6 字节二进制与中文）/
copy 单文件与源不存在 / rmtree 计数 / 删除不存在路径 / 清理源树。
