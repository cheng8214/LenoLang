# LenoFS v0.1.0

Leno 的文件系统扩展（**纯 Leno**）：补 `dirs` 缺的三件（shutil 的核心）。

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

## 验证

```bat
build\leno.exe build\leno_module\LenoFS\test\test_fs.leno
```

6 组：count（3 文件/3 目录）/ copytree（逐文件校验，含 6 字节二进制与中文）/
copy 单文件与源不存在 / rmtree 计数 / 删除不存在路径 / 清理源树。
