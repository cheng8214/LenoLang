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

### `remove(path)`

删除文件。

**参数**:
- `path` (string): 文件路径

**返回**: `bool` - 是否删除成功

```leno
dirs.remove("old_file.txt")     // true
dirs.remove("not_exist.txt")    // false
```

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

---

### `walk(path)`

遍历目录树，返回目录结构信息。

**参数**:
- `path` (string): 目录路径

**返回**: `array` - 包含 [[root, dirs, files], ...] 的数组

```leno
var entries = dirs.walk("src/module")
// entries = [["src/module", ["io", "times", ...], ["dirs"]], ...]

for entries to entry {
    var root = entry[0]     // 当前目录路径
    var dirs = entry[1]     // 子目录数组
    var files = entry[2]    // 文件数组
    
    io.print("目录: " + root)
    io.print("  子目录: " + dirs)
    io.print("  文件: " + files)
}
```

---

## 文件信息

### `stat(path)`

获取文件的详细信息。

**参数**:
- `path` (string): 文件或目录路径

**返回**: `dict` - 包含以下字段的字典：
- `size` (int): 文件大小（字节）
- `is_file` (bool): 是否是文件
- `is_dir` (bool): 是否是目录
- `mtime` (int): 最后修改时间（Unix 时间戳）

```leno
var info = dirs.stat("build.bat")
// info = {
//     size: 2917,
//     is_file: true,
//     is_dir: false,
//     mtime: 1777650164
// }

io.print("大小: " + info.size + " 字节")
io.print("是文件: " + info.is_file)
io.print("是目录: " + info.is_dir)
```

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
        var root = entry[0]
        var files = entry[2]
        
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
        var root = entry[0]
        var files = entry[2]
        
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
                if dirs.remove(f) {
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

*文档版本: 1.0*  
*最后更新: 2026-05-07*
