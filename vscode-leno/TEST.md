# VS Code 扩展测试指南

## 安装方法

### 方法一：从 .vsix 文件安装

1. 打开 VS Code
2. 进入扩展视图（Ctrl+Shift+X）
3. 点击 "..." 菜单（更多操作）
4. 选择 "从 VSIX 安装..."
5. 选择 `vscode-leno-1.0.0.vsix` 文件

### 方法二：开发模式

1. 打开 VS Code
2. 文件 -> 打开文件夹 -> 选择 `vscode-leno` 文件夹
3. 按 F5 启动扩展开发主机
4. 新的 VS Code 窗口将加载扩展

## 功能测试

### 1. 语法高亮

打开 `test/test.leno` 文件，验证：
- 关键字（`func`、`var`、`if`、`for` 等）被高亮显示
- 字符串使用不同颜色
- 注释显示为灰色
- 数字被高亮显示
- 类型（`int`、`string`、`bool` 等）被着色

```leno
// Leno 语言的 Hello World
func greet() {
    print("Hello, World!")
}

greet()
```

### 2. 代码补全

1. 输入 `va` 并按 Ctrl+Space
2. 应该看到 `var` 等建议
3. 输入 `func` 查看函数相关补全

### 3. 错误诊断

1. 打开 `test/test.leno`
2. 取消注释第 62 行：`// var x = `
3. 保存文件（Ctrl+S）
4. 应该在错误处看到红色波浪线
5. 悬停在错误上查看错误信息

### 4. 悬停信息

1. 悬停在 `print` 函数上
2. 应该看到文档信息
3. 悬停在 `Point` 结构体上
4. 应该看到类型信息

### 5. 跳转到定义

1. 右键点击 `greet` 函数调用（第 37 行）
2. 选择 "跳转到定义" 或按 F12
3. 光标应该跳转到函数定义（第 11 行）

### 6. 文档符号

1. 按 Ctrl+Shift+O
2. 应该看到文件中所有符号列表：
   - `message`（变量）
   - `count`（变量）
   - `greet`（函数）
   - `Point`（结构体）
   - `main`（函数）

## 故障排除

### LSP 服务器未启动

1. 检查输出面板（Ctrl+Shift+U）
2. 从下拉菜单选择 "Leno Language Server"
3. 查看错误信息

### 没有语法高亮

1. 检查文件扩展名是否为 `.leno`
2. 检查语言模式是否设置为 "Leno"（VS Code 右下角）

### 扩展未加载

1. 打开开发者工具（帮助 -> 切换开发者工具）
2. 检查控制台中的错误
3. 重新加载窗口（Ctrl+Shift+P -> "开发人员: 重新加载窗口"）

## 调试模式

启用 LSP 通信跟踪：

1. 打开设置（Ctrl+,）
2. 搜索 "leno trace"
3. 将 "Leno: Trace Server" 设置为 "messages" 或 "verbose"
4. 在输出面板中查看 LSP 消息
