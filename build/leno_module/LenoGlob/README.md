# LenoGlob v0.1.0

Leno 的通配符匹配与文件查找（**纯 Leno**）。`files` 模块没有 glob，此前只能
`dirs.walk` 后自己写匹配。

```leno
import "Glob" as glob

glob.match("**/*.txt", "a/b/c.txt")   // true
glob.glob("src/**/*.leno")            // 相对路径数组（已排序）
glob.globIn("D:/www", "**/*.css")     // 指定根目录
glob.filter("*.md", list)             // 过滤已有列表，不访问磁盘
```

## 语义

| 模式 | 含义 |
|---|---|
| `*` | 一个路径段内任意字符（**不跨** `/`） |
| `?` | 一个非 `/` 字符 |
| `[abc]` / `[a-z]` / `[!0-9]` | 字符类（`!` 或 `^` 取反） |
| `**` | 跨目录任意层级；`**/` 还能匹配**零层**（`**/*.txt` 命中根目录 `a.txt`） |

- 路径分隔符统一按 `/`（Windows 的 `\` 会先归一化）
- **区分大小写**（与 shell/Linux 一致；要忽略大小写请两侧 `to_lower`）
- 结果按字典序排序，跨平台稳定

## API

| 函数 | 说明 |
|---|---|
| `match(pattern, path)` | 纯匹配，不访问磁盘 |
| `glob(pattern)` | 相对当前目录查找 |
| `globIn(base, pattern)` | 指定根目录查找（结果含 base 前缀） |
| `filter(pattern, paths)` | 过滤已有路径列表 |

## 边界

- 不跟随符号链接 / junction（沿用 `dirs.walk` 的口径）
- 无花括号展开（`{a,b}`）、无转义字符
- 只匹配**文件**（目录本身不返回）

## 验证

```bat
build\leno.exe build\leno_module\LenoGlob\test\test_glob.leno
```

3 组：12 条匹配规则（含 `**` 零层、`?` 不跨目录、反字符类、范围类、反斜杠归一化）+
真实目录查找（`**/*.txt` 3 个 / `*.txt` 1 个 / `a/*.txt` / `a/**/z.md`）+ filter。
