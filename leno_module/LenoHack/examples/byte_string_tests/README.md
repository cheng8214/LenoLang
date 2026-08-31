# byte_string_tests - 二进制字符串操作测试

本目录包含两个测试脚本，验证 Leno 语言在二进制数据处理方面的能力。

## 文件列表

| 文件 | 说明 |
|------|------|
| `test_byte_find.leno` | 测试 `strings.byte_find()` 二进制安全搜索功能 |
| `patch_files.leno` | 使用 `files` 模块静态修改 PE exe 文件中的字符串 |

## 运行方式

```bash
# 从项目根目录运行
cd D:/CLeno/LenoC
build/leno.exe leno_module/LenoHack/examples/byte_string_tests/test_byte_find.leno
build/leno.exe leno_module/LenoHack/examples/byte_string_tests/patch_files.leno
```

---

## test_byte_find.leno

### 用途

验证 `strings.byte_find()` 方法的正确性，特别是其在含 `\x00` 字节的二进制数据中的搜索能力。

### 测试内容

1. **基本文本搜索** — ASCII 字符串中的子串查找
2. **start 参数** — 从指定字节偏移开始搜索，实现多次搜索
3. **二进制安全搜索** — 在含 `\x00` 的数据中搜索，对比 `find()` 的截断行为
4. **边界情况** — 空模式、start 超出范围、负数 start、pattern 长于字符串
5. **模块函数调用** — `strings.byte_find(s, pat)` 和 `strings.byte_find(s, pat, start)`
6. **Unicode 文本** — `byte_find` 返回字节偏移 vs `find` 返回字符索引

### 关键验证点

`byte_find` vs `find` 的核心区别在二进制场景：

```
// 数据: \x00 \x01 \x00 \x02 \x00 \x03
// 搜索: \x00 \x02

bin.byte_find(0x00 0x02) = 2    // 正确，memcmp 逐字节比较
bin.find(0x00 0x02) = 0         // 错误，strstr 遇 \x00 截断 pattern 为空串
```

Unicode 文本中两种返回值的区别：

```
'你好World'.byte_find('World') = 6   // 6 = 2中文x3字节的字节偏移
'你好World'.find('World') = 2        // 2 = 字符索引
```

### 预期输出

```
=== byte_find test ===

[1] 基本文本搜索
  s1 = Hello, World
  s1.byte_find('World') = 7
  s1.byte_find('o') = 4
  s1.byte_find('xyz') = -1
  s1.byte_find('Hello') = 0

[2] start 参数 - 多次搜索
  s2 = ababab
    found at: 0
    found at: 2
    found at: 4
  total found: 3

[3] 二进制安全搜索 (含 null 字节)
  bin.byte_find(0x00 0x02) = 2
  bin.find(0x00 0x02) = 0
  bin.byte_find(0x00 0x03) = 4

[4] 边界情况
  'hello'.byte_find('') = 0
  'hello'.byte_find('h', 100) = -1
  'hello'.byte_find('o', -1) = 4
  'hi'.byte_find('hello') = -1

[5] 模块函数调用
  strings.byte_find('Hello, World', 'World') = 7
  strings.byte_find('Hello, World', 'o', 5) = 8

[6] Unicode 文本
  '你好World'.byte_find('World') = 6
  '你好World'.find('World') = 2

=== all tests passed ===
```

---

## patch_files.leno

### 用途

使用 Leno 内置 `files` 模块（不依赖任何 FFI/clib）静态修改 `cache_cleaner.exe` 中的窗口标题字符串，验证 `files` 模块的二进制文件 I/O 能力。

### 涉及的 API

| API | 说明 |
|-----|------|
| `files.open(path, "rb")` | 二进制只读模式打开文件 |
| `files.open(path, "r+b")` | 二进制读写模式打开文件 |
| `f.len()` | 获取文件大小（字节） |
| `f.read(n)` | 读取 n 字节到字符串（二进制模式保留原始字节） |
| `f.write(string)` | 写入字符串（用 `str->len`，null 字节安全） |
| `f.seek(off, "set")` | 定位到指定字节偏移 |
| `f.close()` | 关闭文件 |
| `str.byte(i)` | 获取第 i 个字节的值 (0-255) |
| `str.byte_slice(s, e)` | 取字节区间 [s, e) 的子串 |
| `strings.char(b1, b2, ...)` | 从字节值构造字符串 |

### 工作流程

```
1. files.open(exe, "rb") 读取整个 3.4MB 文件到内存
2. 用自定义 findPattern() 搜索 XOR 编码的旧标题 pattern
   - pattern.byte(4) 做快速过滤（避开 0x00 首字节）
   - byte_slice + == 做完整比较
3. files.open(exe, "r+b") 以读写模式打开
4. 对每个匹配偏移: seek + write 覆写新标题
5. 重新读取验证: 旧 pattern 0 处，新 pattern 2 处
```

### 为什么不用 strings.find()

`strings.find()` 内部使用 C 的 `strstr`，遇到 `\x00` 会截断搜索内容。PE 文件中的 XOR 编码字符串经常包含 `\x00` 字节，`find()` 无法正确搜索。

脚本使用 `str.byte(i)` 逐字节过滤 + `byte_slice` + `==` 做完整比较，绕过了这个限制。

> 注：`byte_find()` 实现后，可以直接用 `content.byte_find(pattern, start)` 替代自定义的 `findPattern` 函数。

### 预期输出

```
========================================
  Leno 静态 Patch (files 模块版)
  目标: cache_cleaner.exe
========================================

  旧标题: Leno 缓存清理工具
  新标题: DuMate 破解版-Crack!
  Pattern 长度: 23 bytes
  过滤字节: pattern.byte(4) = 0x6C

[1] 检查文件: D:/cs/cache_cleaner.exe
  v 文件存在

[2] 读取文件内容...
  文件大小: 3429646 bytes
  读取 3429646 bytes
  ...

[3] 搜索 XOR 编码的旧标题...
  找到 2 处匹配
    偏移: 1020563
    偏移: 1031069

[4] 写入新标题到文件...
  v 偏移 1020563 写入成功 (23 bytes)
  v 偏移 1031069 写入成功 (23 bytes)
  共修补 2 处

[5] 验证 patch 结果...
  旧标题 pattern 残留: 0 处
  新标题 pattern 出现: 2 处
  v 验证通过!

========================================
  静态 Patch 完成! (纯 files 模块)
  ...
```

---

## 相关文档

- [strings 模块文档](../../../../docs/module_strings.md) — `byte_find()`、`byte()`、`byte_slice()`、`byte_len()` 等 API 说明
- [files 模块文档](../../../../docs/module_files.md) — `files.open`、`f.read`、`f.write`、`f.seek` 等 API 说明

## 技术背景

Leno 打包的 exe 将字节码明文追加在 PE 尾部（魔数 `BNEL`），字符串仅用 4 字节 XOR 加密（密钥为 `LENO`）。这使得静态修改 exe 中的字符串成为可能——搜索 XOR 编码后的字节序列，覆写为新标题的 XOR 编码即可。
