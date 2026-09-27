# LenoC 目录操作模块 (dirs)

本文档详细说明 `dirs` 模块提供的所有目录和文件操作方法。

## 目录

- [使用方式](#使用方式)
- [路径操作](#路径操作)
- [路径检查](#路径检查)
- [目录操作](#目录操作)
- [文件操作](#文件操作)
- [目录遍历](#目录遍历)
- [文件信息](#文件信息)
- [示例代码](#示例代码)
- [注意事项](#注意事项)

---

## 使用方式

```leno
import dirs
import io

main() {
    // 获取当前目录
    var cwd = dirs.cwd()
    io.print(cwd)
    
    // 创建目录
    dirs.mkdir("new_folder")
}
```

---

## 路径操作

### `cwd()`

获取当前工作目录。

**参数**: 无  
**返回**: `string` - 当前工作目录的绝对路径

```leno
dirs.cwd()  // "D:\\CLeno\\LenoC" (Windows)
            // "/home/user/LenoC" (Linux/macOS)
```

---

### `abspath(path)`

将相对路径转换为绝对路径。

**参数**:
- `path` (string): 相对路径或绝对路径

**返回**: `string` - 绝对路径

```leno
dirs.abspath("src/module/io")     // "D:\\CLeno\\LenoC\\src\\module\\io"
dirs.abspath("./test.txt")        // "D:\\CLeno\\LenoC\\test.txt"
```

---

### `basename(path)`

获取路径中的文件名部分。

**参数**:
- `path` (string): 文件路径

**返回**: `string` - 文件名

```leno
dirs.basename("/home/user/file.txt")    // "file.txt"
dirs.basename("src/module/io/io.c")     // "io.c"
dirs.basename("file.txt")               // "file.txt"
```

---

### `dirname(path)`

获取路径中的目录部分。

**参数**:
- `path` (string): 文件路径

**返回**: `string` - 目录路径

```leno
dirs.dirname("/home/user/file.txt")     // "/home/user"
dirs.dirname("src/module/io/io.c")      // "src/module/io"
dirs.dirname("file.txt")                // "."
```

---

### `extname(path)`

获取文件的扩展名。

**参数**:
- `path` (string): 文件路径

**返回**: `string` - 扩展名（包含点），如果没有扩展名返回空字符串

```leno
dirs.extname("file.txt")        // ".txt"
dirs.extname("io.c")            // ".c"
dirs.extname("Makefile")        // "" (无扩展名)
dirs.extname(".bashrc")         // "" (隐藏文件)
```

---

### `join(part1, part2, ...)`

将多个路径部分拼接成一个完整路径。

**参数**:
- `part1, part2, ...` (string): 路径部分

**返回**: `string` - 拼接后的路径

```leno
dirs.join("a", "b", "c")              // "a\\b\\c" (Windows)
                                       // "a/b/c" (Linux/macOS)
dirs.join("/home", "user", "file")    // "/home\\user\\file"
dirs.join("src", "module", "io.c")    // "src\\module\\io.c"
```

---

### `sep()`

获取当前平台的路径分隔符。

**参数**: 无  
**返回**: `string` - 路径分隔符

```leno
dirs.sep()  // "\\" (Windows)
            // "/" (Linux/macOS)
```

---

## 路径检查

### `exists(path)`

检查路径是否存在。

**参数**:
- `path` (string): 路径

**返回**: `bool` - 是否存在

```leno
dirs.exists("src")              // true
dirs.exists("build.bat")        // true
dirs.exists("not_exist")        // false
```

---

### `is_file(path)`

检查路径是否是文件。

**参数**:
- `path` (string): 路径

**返回**: `bool` - 是否是文件

```leno
dirs.is_file("build.bat")       // true
dirs.is_file("src")             // false
dirs.is_file("not_exist")       // false
```

---

### `is_dir(path)`

检查路径是否是目录。

**参数**:
- `path` (string): 路径

**返回**: `bool` - 是否是目录

```leno
dirs.is_dir("src")              // true
dirs.is_dir("build.bat")        // false
dirs.is_dir("not_exist")        // false
```

---

## 目录操作

### `mkdir(path)`

创建单个目录。

**参数**:
- `path` (string): 目录路径

**返回**: `bool` - 是否创建成功

```leno
dirs.mkdir("new_folder")        // true
dirs.mkdir("a/b/c")             // false (父目录不存在)
```

---

### `mkdir_p(path)`

递归创建目录（包括所有父目录）。

**参数**:
- `path` (string): 目录路径

**返回**: `bool` - 是否创建成功

```leno
dirs.mkdir_p("a/b/c")           // true (创建 a, a/b, a/b/c)
dirs.mkdir_p("deep/nested/dir") // true
```

---

### `rmdir(path)`

删除空目录。

**参数**:
- `path` (string): 目录路径

**返回**: `bool` - 是否删除成功

```leno
dirs.rmdir("empty_folder")      // true
dirs.rmdir("non_empty")         // false (目录非空)
```

---

## 文件操作

### `delete(path)`

删除文件或目录。

- **文件**：直接删除。
- **目录**：递归删除整个目录树（先删除其所有子文件和子目录，再删除自身），因此**非空目录也能删除**。
- 全程使用 UTF-16 处理路径，支持中文等非 ASCII 路径。
- 路径不存在时返回 `false`。

**参数**:
- `path` (string): 文件或目录路径

**返回**: `bool` - 是否删除成功

```leno
dirs.delete("old_file.txt")            // true（删文件）
dirs.delete("empty_folder")            // true（删空目录）
dirs.delete("project")                 // true（递归删非空目录及其全部内容）
dirs.delete("not_exist.txt")           // false（路径不存在）
```

> 注意：删除非空目录会**永久移除其下所有内容**，请谨慎使用。若只想删除空目录，可用 `dirs.rmdir()`。

---

### `rename(old_path, new_path)`

重命名文件或目录。

**参数**:
- `old_path` (string): 原路径
- `new_path` (string): 新路径

**返回**: `bool` - 是否重命名成功

```leno
dirs.rename("old.txt", "new.txt")           // true
dirs.rename("folder1", "folder2")           // true
dirs.rename("not_exist.txt", "new.txt")     // false
```

---

## 目录遍历

### `listdir(path)`

列出目录中的所有文件和子目录。

**参数**:
- `path` (string): 目录路径

**返回**: `array` - 文件和目录名称数组（不包含 `.` 和 `..`）

```leno
var files = dirs.listdir("src")
// files = ["error.c", "gc.c", "lexer.c", "main.c", ...]

for files to f {
    io.print(f)
}
```

### `list_drives()`

返回系统盘符列表（用于文件管理器等需要全盘导航的场景）。

- **Windows**: 返回 `["C:\\", "D:\\", ...]`（按位枚举 `GetLogicalDrives`，每条带末尾分隔符）。
- **Unix / 其他**: 返回 `["/"]`。

**返回**: `array` - 盘符路径数组

```leno
var drives = dirs.list_drives()   // Windows: ["C:\\", "D:\\"]
```

---

### `walk(path)`

递归遍历目录树，返回 `Array[DirEntry]` —— 每项是结构体 `DirEntry{ string root, Array[string] dirs, Array[string] files }`。

**参数**:
- `path` (string): 目录路径

**返回**: `Array[DirEntry]`（类型串按既有的 `type_to_string` 风格渲染为 `Array[struct DirEntry]`）

```leno
Array[DirEntry] es = dirs.walk("src/module")   // 元素类型可以直接标出来（v3.2.5 起）
for es to e {
    string root = e.root            // 编译期就知道是 string（无需 as / 收窄）
    Array[string] ds = e.dirs       // 子目录名
    Array[string] fs = e.files      // 文件名；元素类型是 string ⇒ 能直接进 Array[string] 的方法
    io.print(root + " → " + _str(fs.len()) + " 个文件")
    for fs to f { io.print("  " + f) }
}
```

**字段含义**（`dirs` / `files` 都只给**名字**、不含路径 ⇒ 完整路径用 `dirs.join(e.root, name)`）:

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `root` | `string` | 本次扫到的目录路径 |
| `dirs` | `Array[string]` | 它的直接子目录名 |
| `files` | `Array[string]` | 它的直接文件名 |

递归深度 = 目录树深度，每层一条（先父后子）。

**为什么返回结构体、而不是 `[root, dirs, files]` 三元组**：三元组的静态类型是 `Array[Array]` ⇒ 元素是 `any`，只能靠 `e[0] / e[1] / e[2]` 位置索引取值，既不可读、顺序一改还会静默错位。现在用 native 的**类型规格**（`NativeTypeSpec`，见 `docs/待办_单一事实来源与重复实现收敛.md`）声明返回 `Array[DirEntry]`：编译期字段表与运行期 `ObjStructDef` 是**同一份声明** ⇒ 字段名 / 类型 / 顺序同源，编译期即知字段类型 ⇒ 调用点零收窄 ✓

> ⚠ **v3.2.3 起返回形态变了**：`walk` 由 `Array[Array]` 三元组改为 `Array[DirEntry]`（同一版本里过渡性的 `walk_entries` 已删除、能力并入 `walk`）⇒ 旧代码的 `entry[0]/entry[1]/entry[2]` 要改成 `entry.root/entry.dirs/entry.files`；返回类型变了 ⇒ 旧 `.lenb` 需重编译。
> ⚠ 别把局部变量命名成 `files`：那是 native 模块名，会被优先当模块解析（用 `fs` 之类）。
> ⚠ `DirEntry` 与脚本自定义的 struct **共享同一个全局名字空间**；若你自己也定义了同名 struct 且形状不同，运行期会**报错**（而不是静默按错序号读字段）。

---

## 文件信息

### `stat(path)`

获取文件信息，返回结构体 `DirInfo` —— **字段类型编译期已知**（v3.2.4 起）。

**参数**:
- `path` (string): 文件或目录路径

**返回**: `DirInfo{ bool exists, int size, bool is_file, bool is_dir, int mtime }`

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `exists` | `bool` | 是否存在 |
| `size` | `int` | 文件大小（字节）—— **该条目自身**的大小，不是递归总大小 |
| `is_file` | `bool` | 是否是文件 |
| `is_dir` | `bool` | 是否是目录 |
| `mtime` | `int` | 最后修改时间（Unix 时间戳） |

> ⚠ `mtime` 在 Windows 上恒为 `0`（已知限制，见 `src/module/dirs/dirs.c` 的"简化版"注释）。
> ⚠ `size` 在 2026-09-26 前是 `(int)` 强转 ⇒ **≥2GB 的文件会读出负数**（实测 2GB+1KB ⇒ `-2147482624`）；
> 现已改为 48 位 `int`，大文件读数正确。

```leno
DirInfo info = dirs.stat("build.bat")     // 类型可以直接标出来（v3.2.5 起；此前只能 var）
// info = DirInfo{ exists: true, size: 2917, is_file: true, is_dir: false, mtime: 0 }

io.print("大小: " + info.size + " 字节")     // info.size 直接是 int ⇒ 能直接参与运算
io.print("是文件: " + info.is_file)          // 直接是 bool ⇒ 能直接进 if
io.print("是目录: " + info.is_dir)
```

> ⚠ **v3.2.4 起返回结构体、不再是 `dict`**：旧形态的键名与现在的字段名**逐字相同** ⇒ `info.size` /
> `info.is_file` 这类调用点**不用改**；要改的是 `info["size"]` 下标式与 `if info is Dict { ... }` 收窄
> —— 编译期就会挡住。
> 为什么不用 `Dict[string, ...]`：这五个键**类型不齐**（`exists` / `is_file` / `is_dir` 是 `bool`，
> `size` / `mtime` 是 `int`），同质的 `Dict` 表达不了"这个键是 bool、那个键是 int"。
> ✅ native struct 名（`DirInfo` / `DirEntry`）**可以当类型标注**（v3.2.5 起）：`DirInfo d = dirs.stat(p)`、
> `Array[DirEntry] w = dirs.walk(p)`、`func f(DirInfo d)`、脚本 struct 的字段类型都能写；`x is DirInfo`
> 也照旧可用。之前只能 `var d = ...` 让编译器推断 —— 因为它俩**不在符号表里**（native 模块没有
> `sym_table`），类型名校验会判"未定义的类型"。现在校验处补了 native 兜底判据
> （来源就是 `native_find_struct_spec`，与字段解析、运行期 `ObjStructDef` 同源）。
> 📌 **取单个字段**就写 `dirs.stat(p).size` —— 并行的 **`dirs.size()` 已于 2026-09-27 删除**
> （它当初只是"`stat` 返回无类型 `dict` 时补的类型化入口"；`stat` 定型后它就是第二个入口）。
> 口径不变：不存在的路径 ⇒ `0`；目录给的是**目录条目自身**的大小（Windows 报 `0`，POSIX 报 `st_size`），
> **不是**递归总大小 ⇒ 要递归总大小自己用 `dirs.walk()` 累加。

---

## 示例代码

### 1. 批量重命名文件

```leno
import dirs
import io

main() {
    var files = dirs.listdir(".")
    var count = 0
    
    for files to f {
        // 将所有 .txt 文件重命名为 .bak
        if dirs.extname(f) == ".txt" {
            var new_name = dirs.basename(f) + ".bak"
            if dirs.rename(f, new_name) {
                count = count + 1
                io.print("重命名: " + f + " -> " + new_name)
            }
        }
    }
    
    io.print("共重命名 " + count + " 个文件")
}
```

---

### 2. 递归查找文件

```leno
import dirs
import io

// 递归查找所有 .leno 文件
func find_leno_files(var path) -> array {
    var result = []
    var entries = dirs.walk(path)
    
    for entries to entry {
        var root = entry.root
        var files = entry.files
        
        for files to f {
            if dirs.extname(f) == ".leno" {
                result.add(dirs.join(root, f))
            }
        }
    }
    
    return result
}

main() {
    var leno_files = find_leno_files("src")
    io.print("找到 " + leno_files.count() + " 个 .leno 文件:")
    
    for leno_files to f {
        io.print("  " + f)
    }
}
```

---

### 3. 文件备份工具

```leno
import dirs
import io
import times

main() {
    var source_dir = "src"
    var backup_dir = "backup_" + times.now()
    
    // 创建备份目录
    if !dirs.mkdir_p(backup_dir) {
        io.print("创建备份目录失败")
        return
    }
    
    // 复制所有 .c 文件
    var files = dirs.listdir(source_dir)
    var count = 0
    
    for files to f {
        if dirs.extname(f) == ".c" {
            var src = dirs.join(source_dir, f)
            var dst = dirs.join(backup_dir, f)
            
            // 读取并写入（简化版复制）
            // 实际应用中应该使用 files 模块
            count = count + 1
        }
    }
    
    io.print("备份完成: " + count + " 个文件")
}
```

---

### 4. 目录大小统计

```leno
import dirs
import io

// 计算目录总大小（简化版，只统计当前层）
func calc_dir_size(var path) -> int {
    var total = 0
    var entries = dirs.walk(path)
    
    for entries to entry {
        var root = entry.root
        var files = entry.files
        
        for files to f {
            var full_path = dirs.join(root, f)
            var info = dirs.stat(full_path)
            total = total + info.size
        }
    }
    
    return total
}

main() {
    var size = calc_dir_size("src")
    io.print("src 目录总大小: " + size + " 字节")
    io.print("约 " + (size / 1024) + " KB")
}
```

---

### 5. 清理临时文件

```leno
import dirs
import io

main() {
    var temp_patterns = [".tmp", ".log", ".cache"]
    var removed = 0
    
    var files = dirs.listdir(".")
    
    for files to f {
        var ext = dirs.extname(f)
        
        for temp_patterns to pattern {
            if ext == pattern {
                if dirs.delete(f) {
                    removed = removed + 1
                    io.print("删除: " + f)
                }
            }
        }
    }
    
    io.print("共清理 " + removed + " 个临时文件")
}
```

---

## 注意事项

1. **路径分隔符**
   - Windows 使用 `\`，Linux/macOS 使用 `/`
   - 使用 `dirs.sep()` 获取当前平台的分隔符
   - `dirs.join()` 会自动处理分隔符

2. **权限问题**
   - 某些操作（如创建目录、删除文件）可能需要相应权限
   - 操作失败时返回 `false`，不会抛出异常

3. **符号链接**
   - 当前版本对符号链接的处理取决于操作系统
   - `is_dir()` 和 `is_file()` 会跟随符号链接

4. **路径长度限制**
   - Windows 传统路径限制为 260 字符
   - 使用绝对路径时注意长度限制

5. **线程安全**
   - 目录遍历操作不是原子性的
   - 遍历过程中目录内容变化可能导致不一致

---

## 跨平台注意事项

| 特性 | Windows | Linux/macOS |
|------|---------|-------------|
| 路径分隔符 | `\` | `/` |
| 根目录 | `C:\`, `D:\` 等 | `/` |
| 大小写敏感 | 不敏感 | 敏感 |
| 隐藏文件 | 属性标记 | 以 `.` 开头 |

---

*文档版本: 1.2*  
*最后更新: 2026-07-22*
