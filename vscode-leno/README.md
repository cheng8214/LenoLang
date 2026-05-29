# Leno 语言 VS Code 扩展

为 Leno 编程语言提供完整的语言支持。

## 功能特性

- **语法高亮**：完整的 `.leno` 文件语法高亮
- **代码补全**：智能代码补全，支持关键字和类型提示
- **错误诊断**：使用 Leno 编译器进行实时错误检测
- **悬停信息**：显示类型信息和文档
- **跳转到定义**：导航到符号定义
- **文档格式化**：自动代码格式化

## 系统要求

- VS Code 1.74.0 或更高版本
- Leno 语言服务器（包含在扩展中）

## 安装方法

### 方法一：从 VSIX 文件安装

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

## 使用示例

```leno
// Leno 语言的 Hello World
func greet() {
    print("Hello, World!")
}

greet()
```

## 配置选项

可以通过 VS Code 设置进行配置：

- `leno.languageServer.path`：Leno 语言服务器可执行文件的路径
- `leno.trace.server`：LSP 通信跟踪级别（off/messages/verbose）

### 配置 LSP 服务器路径

如果 LSP 服务器未自动找到，请手动配置路径：

1. 打开设置（Ctrl+,）
2. 搜索 "leno languageServer"
3. 在 `leno.languageServer.path` 中输入 LSP 服务器路径，例如：
   - `D:\CLeno\LenoC\leno_lsp\build\leno_lsp.exe`
4. 重新加载窗口（Ctrl+Shift+P -> "开发人员: 重新加载窗口"）

### 关联 .leno 文件

如果 .leno 文件没有自动识别为 Leno 语言：

1. 按 Ctrl+Shift+P
2. 输入 "Leno: 关联文件"
3. 选择该命令，.leno 文件将被自动关联到 Leno 语言

## 开发构建

从源码构建扩展：

```bash
cd vscode-leno
npm install
npm run compile
```

调试扩展：

1. 在 VS Code 中打开项目
2. 按 F5 启动扩展开发主机
3. 打开 `.leno` 文件进行测试

## 测试功能

1. **语法高亮**：打开 `.leno` 文件，查看关键字、字符串、注释等是否正确高亮
2. **代码补全**：输入 `va` 按 Ctrl+Space，应该看到 `var` 等建议
3. **错误诊断**：输入错误的代码，保存后应该看到红色波浪线
4. **悬停提示**：悬停在函数上查看文档信息
5. **跳转到定义**：右键点击函数调用，选择"跳转到定义"

## 许可证

MIT
