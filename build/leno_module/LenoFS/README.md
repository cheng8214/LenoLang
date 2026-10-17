# LenoFS v0.1.0

Leno 的文件系统扩展（**纯 Leno**）：补 `dirs` 缺的三件（shutil 的核心）。

```leno
import "FS" as fs

fs.rmtree("build/tmp")                 // 递归删除（文件或目录）
fs.copytree("src", "backup/src")       // 整树复制（自动建父目录，目标已存在则合并）
fs.copy("a.zip", "b.zip")              // 单文件复制（二进制安全）
fs.count("src")                        // {files: 12, dirs: 3}
```

## API

| 函数 | 返回 | 说明 |
|---|---|---|
| `rmtree(path)` | `{ok, files, dirs, err}` | 递归删除；路径不存在也算成功（计数 0） |
| `copytree(src, dst)` | `{ok, files, dirs, err}` | 整树复制；`dst` 已存在则**合并覆盖** |
| `copy(src, dst)` | `{ok, files, dirs, err}` | 单文件复制（源是目录会报错，请用 copytree） |
| `count(path)` | `{files, dirs}` | 目录项数量（删前确认 / 复制后核对） |

## 口径

- **不跟随符号链接**：链接本身被删除/复制内容，但不递归进链接目录（避免 junction 指回祖先时无限展开）
- **不保留 mtime / 权限 / 属性**：跨平台一致的成本太高；要保留请调系统工具
- 二进制安全（`files.read/write` 按字节，含 NUL 与 0xFF）
- 失败返回 `ok=false` + 可读原因（权限、被占用、源不存在）

## 验证

```bat
build\leno.exe build\leno_module\LenoFS\test\test_fs.leno
```

6 组：count（3 文件/3 目录）/ copytree（逐文件校验，含 6 字节二进制与中文）/
copy 单文件与源不存在 / rmtree 计数 / 删除不存在路径 / 清理源树。
