# Leno 编译器 Bug 报告：行内注释中的花括号导致跨模块 struct 方法丢失

## Bug ID
`BUG-INLINE-COMMENT-BRACE-001`

## 严重级别
**P0 — 阻断功能**

## 发现时间
2026-08-31

## 概述

当 `export struct` 的字段定义行内包含带花括号 `{}` 的注释时，编译器在跨模块导入该 struct 时无法正确导出其方法表，导致所有 struct 方法在导入方模块中均不可见。

## 根因分析

### 真正根因

Bug 位于 `src/module_symbol_table/inc/scan/scan_struct.inc` 文件中，模块符号表扫描器的 struct 字段默认值跳过逻辑。

当扫描器解析 `export struct` 体的内容时，对于每个字段，有两段"跳过默认值"的逻辑：

1. **第一段（do-while 循环内，第 694-720 行）**：处理逗号分隔的多字段声明，正确跳过 `= {}` 中的花括号。

2. **第二段（do-while 循环后，第 724-754 行）**：重复的默认值跳过逻辑。这段代码的 while 循环（第 726 行）：
   ```c
   while (*after_struct && *after_struct != '\n' && *after_struct != ';' 
          && *after_struct != '=' && *after_struct != '}') after_struct++;
   ```
   在第一段已经消费了 `= {}` 之后，指针停在行内注释 `// 模块 -> {字段 -> 偏移}` 的位置。该 while 循环**不识别 `//` 注释**，将注释内容中的 `}` 误判为 struct 体的结束符，导致 `after_struct` 指针提前停在注释中间。

后续的解析将注释中的 `}` 当作 struct 体的 `}`，导致剩余的所有方法定义被跳过，struct 方法表为空。

### 影响范围
- `scan_struct.inc` — struct 字段默认值跳过逻辑（两段）
- `scan_struct.inc` — struct 方法体跳过逻辑
- `scan_struct.inc` — struct 方法参数列表解析循环
- `scan_cstruct.inc` — cstruct 字段默认值跳过逻辑
- `scan_cstruct.inc` — cstruct 方法体跳过逻辑
- `scan_cstruct.inc` — cstruct 方法参数列表跳过循环
- `scan_face.inc` — face "跳过其他内容"逻辑
- `scan_face.inc` — face 方法参数列表跳过循环
- `scan_pass1.inc` — cfunc 参数列表解析循环（多处）
- `scan_pass1.inc` — clib 函数参数列表解析循环
- `scan_enum.inc` — enum 体循环

### 注意

主解析器（lexer）的 `skip_whitespace` 函数（`src/lexer.c` 第 223-256 行）已经完整处理了 `//` 和 `/* */` 注释，因此主解析器层面不存在此问题。本修复仅针对模块符号表扫描器（`scan_*.inc`），它手工扫描源码字符串而非使用 lexer 的 token 流。

## 最小复现

### 模块 A (lib/bug_repro.leno)
```leno
export struct MyStruct {
    string field1 = ""
    Dict data = {}        // 注释有 {花括号} 导致 bug
    string field2 = ""

    func getValue(): string {
        return field1
    }
}
```

### 模块 B (caller.leno)
```leno
import "lib/bug_repro.leno" as mod
use mod.MyStruct

main() {
    MyStruct obj = new MyStruct()
    string v = obj.getValue()   // 错误: struct MyStruct 没有方法 'getValue'
}
```

## 修复方案

### 修复内容

在以下文件的所有花括号/方括号深度跳过循环、参数列表解析循环中，加入 `//` 和 `/* */` 注释跳过逻辑：

1. **`scan_struct.inc`**：
   - struct 体循环开头注释检测
   - 第一段 do-while 循环内的 `{}` 跳过、`[]` 跳过、简单值跳过
   - 第二段的 `{}` 跳过、`[]` 跳过、简单值跳过
   - 方法体 `{}` 跳过
   - 方法参数列表解析循环（跳空白后、默认值跳过中）

2. **`scan_cstruct.inc`**：
   - cstruct 体循环开头注释检测
   - 字段默认值 `{}` 跳过、`[]` 跳过、简单值跳过
   - 方法体 `{}` 跳过
   - 方法参数列表跳过循环

3. **`scan_face.inc`**：
   - face 体循环开头注释检测
   - "跳过其他内容"的 while 循环
   - 方法参数列表跳过循环

4. **`scan_pass1.inc`**：
   - cfunc 参数列表解析循环（5 处，包括循环开头、跳空白后、参数名跳过前）
   - clib 函数参数列表解析循环

5. **`scan_enum.inc`**：
   - enum 体循环开头注释检测

### 修复模式

在每个花括号深度计数循环的开头加入注释检测：
```c
// 修复前
while (*p && depth > 0) {
    if (*p == '{') depth++;
    else if (*p == '}') depth--;
    p++;
}

// 修复后
while (*p && depth > 0) {
    // 跳过单行注释 //
    if (*p == '/' && *(p+1) == '/') {
        while (*p && *p != '\n') p++;
        continue;
    }
    // 跳过多行注释 /* ... */
    if (*p == '/' && *(p+1) == '*') {
        p += 2;
        while (*p && !(*p == '*' && *(p+1) == '/')) p++;
        if (*p) p += 2;
        continue;
    }
    if (*p == '{') depth++;
    else if (*p == '}') depth--;
    p++;
}
```

## 验证

修复后，以下编译全部通过：
```bash
build\leno.exe -c leno_module\LenoHack\lib\offsets_config.leno
build\leno.exe -c leno_module\LenoHack\lib\GameHelper.leno
build\leno.exe -c leno_module\LenoHack\lib\hack_log.leno
build\leno.exe -c leno_module\LenoHack\lib\w32_memory.leno
build\leno.exe -c leno_module\LenoHack\lib\w32_input.leno
```

内置测试全部通过（256 项，零回归）：
```
build\leno.exe assert\run_tests.leno build\leno.exe assert
Results: 256 passed, 0 failed (total 256)
```

带花括号注释的 struct 现在可以正确跨模块导出方法。
