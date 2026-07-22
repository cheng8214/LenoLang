# LSP 符号缓存复用 + 补齐标准 LSP 功能

## Context（为什么做这件事）

当前 LSP 已实现 completion / hover / definition / diagnostic 四个核心功能且相当完善，但存在两个问题：

1. **重复扫描，性能差**：补全和 hover 里 `add_module_symbol_completions`（lsp_complete.c:846）和 `get_module_symbol_hover`（lsp_hover.c:1302）每次都 `read_module_file` + `compiler_analyze_with_filename` 重新分析模块。SDL3 有 29 个子模块，每次按键补全都可能触发多次完整词法+语法+语义分析。而编译器已有现成的 `ModuleSymbolTable`（module_symbol_table.h，结构含 funcs/structs/enums/faces/vars/aliases/clibs），其 `module_symbol_table_scan` 自带 `.lenosymc` 磁盘缓存——但 LSP 补全/hover 没用它，只有跳转（lsp_definition.c:278）用了。

2. **标准 LSP 功能缺失**：documentSymbol、signatureHelp、documentHighlight、references、rename 都没实现，而 SDL3 这种多参数 GUI 框架对参数提示尤其刚需。

**目标**：复用编译器符号（不重新扫描）+ 补齐 5 个标准 LSP 功能。

## 架构决策：混合缓存方案

两套数据结构各取所长：
- **模块导出符号** → 走 `ModuleSymbolTable`（`module_symbol_table_scan` 自带 `.lenosymc` 磁盘缓存，秒级命中）。不含局部变量，但补全/hover 里查模块符号时不需要局部变量。
- **当前文件局部变量** → 走 `CompilerContext`（Scope 树，含参数/局部 var）。一次 LSP 请求内只分析一次，按文档内容 hash 失效。

## 实现分阶段（增量，每阶段可独立验证）

### 阶段 1：符号缓存基础设施

**新建 `leno_lsp/lsp_symbol_cache.h` + `lsp_symbol_cache.c`**

进程内两层缓存：

```c
// 模块符号表缓存（path → ModuleSymbolTable*，按 mtime 失效）
ModuleSymbolTable* lsp_cache_get_module_symtable(const char* module_path, const char* current_file);
//   内部：先查进程内 map；未命中则 module_symbol_table_create + module_symbol_table_scan
//   （scan 自身走 .lenosymc 磁盘缓存）；存入进程内 map 返回。不 destroy，常驻到 mtime 变化。

// 当前文件分析缓存（path+content_hash → CompilerContext*，文档变更时失效）
CompilerContext* lsp_cache_analyze_current(const char* content, const char* file_path);
//   内部：按 content hash 查缓存；未命中则 compiler_analyze_with_filename；命中直接返回。
//   LSP 请求结束由 lsp_cache_release_current 释放（或文档 didChange 时清空）。
void lsp_cache_invalidate_path(const char* path);   // didChange/didClose 时调用
```

进程内 map 用简单的数组+线性查找即可（模块数有限，<128）。

### 阶段 2：改造补全/hover 复用缓存

**`lsp_complete.c`**：
- `add_module_symbol_completions`（:846）：从 `read_module_file + compiler_analyze` 改为 `lsp_cache_get_module_symtable`，遍历 `table->funcs/structs/enums/faces/vars/aliases/clibs` 填补全项（替换原来遍历 `ctx.root_scope->syms` 的逻辑）。
- `lsp_get_completions`（:1776）入口：调一次 `lsp_cache_analyze_current` 拿 `ctx*`，传给 `add_symbols_from_compiler` / `get_variable_type` / `add_symbol_table_struct_completions` / `is_type_annotation_context` 分支，消除一次请求内多次 `compiler_analyze_with_filename`。
- struct 字段补全：当前文件用 `ctx*` 的 `scope_resolve_tree_bfs`；跨模块用 `lsp_cache_get_module_symtable` 的 `module_symbol_table_find_struct` 拿 `ModuleStructSymbol.fields`。

**`lsp_hover.c`**：
- `get_module_symbol_hover`（:1302）：同样改用 `lsp_cache_get_module_symtable`，从 `ModuleStructSymbol/ModuleEnumSymbol/ModuleFuncSymbol/ModuleVarSymbol` 取信息。
- `get_symbol_hover_from_compiler`（:545）：入口用 `lsp_cache_analyze_current`。

### 阶段 3：documentSymbol（文档大纲，最简单，验证缓存）

**新建 `lsp_document_symbol.c`**：
- `lsp_handle_document_symbol(server, id, params)` → 遍历 `lsp_cache_analyze_current` 的 `root_scope->syms`，按 `sym->kind` / `sym->type->kind` 映射到 `SymbolKind`（Function=12 / Struct=23 / Enum=10 / EnumMember=22 / Interface=11 / Variable=13 / Constant=14）。
- 取每个符号的定义位置：用 lsp_definition.c 里 `find_definition_in_content` 的模式匹配（复用），或更好——在 Symbol 上找 ast 节点行号（若 scope 已记录）。
- 返回 `DocumentSymbol[]`（含 name / kind / range / selectionRange / detail）。

### 阶段 4：documentHighlight（同名高亮）

**新建 `lsp_document_highlight.c`**：
- `lsp_handle_document_highlight`：取光标下单词（复用 lsp_hover.c 的 `get_word_at_position`），在当前文档内扫描所有相同标识符的出现位置，跳过字符串字面量和注释（复用 lsp_hover.c 的 `is_inside_string_literal` / `is_inside_comment`）。
- 返回 `DocumentHighlight[]`（kind 统一用 Text，或对定义处用 Write）。

### 阶段 5：signatureHelp（参数提示，实用度最高）

**新建 `lsp_signature.c`**：
- `lsp_handle_signature_help(server, id, params)`：
  1. 从光标向前找最近未闭合的 `(`，提取 `(` 前的函数名（支持 `module.func` / `func` / `Type.method`）。
  2. 数逗号确定当前参数索引（跳过嵌套 `()`、`[]`、`{}`、字符串、注释）。
  3. 查签名来源优先级：
     - 内置函数：`find_builtin_function(name)`（leno_builtins.h）→ 参数表
     - 模块方法：`native_get_module_method_metas(module, ...)`（native.h）→ ModuleMethodMeta.param_types
     - 模块导出函数：`lsp_cache_get_module_symtable` → `module_symbol_table_find_func` → ModuleFuncSymbol（注意：当前 ModuleFuncSymbol 只存 return_type，**参数类型未存**，需扩展或在 scan 时补录 param 信息；若工作量过大，先只对内置函数和模块方法做 signatureHelp，模块 .leno 函数降级为只显示参数个数）
     - 当前文件函数：从 `lsp_cache_analyze_current` 的 root_scope 找函数 Symbol（若有参数信息）
  4. 返回 `SignatureInformation[]`（label=完整签名，parameters[]，activeParameter=当前索引）。
- 触发字符：`(` 和 `,`。

### 阶段 6：references（跨文件查找引用）

**新建 `lsp_references.c`**：
- `lsp_handle_references`：取光标下单词。
  1. 当前文件：同 documentHighlight 扫描。
  2. 跨文件：遍历 import 链，对每个导入的模块用 `lsp_cache_get_module_symtable` 确认该符号存在（`module_symbol_table_find_func/struct/enum/var`），存在则 `read_module_file` 扫描源码中的引用位置（位置定位复用字符串扫描）。
  3. `includeDeclaration` 参数控制是否包含定义处。
- 返回 `Location[]`（uri + range）。

### 阶段 7：rename（基于 references）

**新建 `lsp_rename.c`**：
- `lsp_handle_rename`：取光标下单词，先做 references 查找所有出现位置（复用阶段 6 逻辑），构建 `WorkspaceEdit`（`changes: { uri: [TextEdit...] }`，每个 TextEdit 的 range 用引用位置，newText 用新名字）。
- 不做实际文件写入（LSP 协议由客户端应用 edits）。

### 阶段 8：注册到 LSP server

**`lsp_server.c`** `lsp_server_create`：capabilities 增加：
```c
server->document_symbol_provider = true;
server->signature_help_provider = true;   // triggerChars: ["(", ","]
server->document_highlight_provider = true;
server->references_provider = true;
server->rename_provider = true;
```

**`lsp_protocol.c`** `lsp_handle_message`：增加 case 分发到新 handler。
**`leno_lsp.h`**：声明新 handler + capabilities 字段 + 请求/响应构造辅助函数。
**`leno_lsp/leno_lsp.h`** 若有 capabilities 序列化（initialize 响应），补充对应字段。

## 关键复用点（不要重写）

| 已有资源 | 位置 | 用途 |
|---|---|---|
| `module_symbol_table_create/scan/find_*` | src/module_symbol_table/ | 模块符号（带 .lenosymc 缓存） |
| `compiler_analyze_with_filename` | leno_compiler_lib.c:39 | 当前文件分析（Scope 树） |
| `scope_resolve_tree_bfs` | src/include/leno_semantic.h | 当前文件符号查找 |
| `native_get_module_method_metas` | src/include/native.h | 内置模块方法签名 |
| `find_builtin_function` / `builtin_functions` | leno_builtins.h | 内置函数签名 |
| `get_word_at_position` | lsp_hover.c:97 | 取光标单词 |
| `is_inside_string_literal/comment` | lsp_hover.c:14/56 | 跳过字符串/注释 |
| `find_definition_in_content` | lsp_definition.c:312 | 定义位置定位 |
| `lsp_uri_to_path` / `lsp_path_to_uri` | leno_lsp.h | URI 转换 |

## 关键文件

**新建**（7 个）：
- `leno_lsp/lsp_symbol_cache.h` / `lsp_symbol_cache.c`
- `leno_lsp/lsp_document_symbol.c`
- `leno_lsp/lsp_document_highlight.c`
- `leno_lsp/lsp_signature.c`
- `leno_lsp/lsp_references.c`
- `leno_lsp/lsp_rename.c`

**修改**：
- `leno_lsp/lsp_complete.c`（改造复用缓存）
- `leno_lsp/lsp_hover.c`（改造复用缓存）
- `leno_lsp/lsp_server.c`（capabilities）
- `leno_lsp/lsp_protocol.c`（方法分发）
- `leno_lsp/leno_lsp.h`（声明）
- `leno_lsp/build_lsp.bat` / `build_lsp.sh`（加入新 .c 文件）
- `leno_lsp/leno_compiler_lib.h`（若需扩展 ModuleFuncSymbol 参数信息，改 module_symbol_table.h）

## 验证

1. **编译**：`cd leno_lsp && build_lsp.bat`（零警告零错误）
2. **打包 VS Code 扩展**：重新生成 `vscode/vscode-leno-1.1.0.vsix`（若 package.json 需声明新 capabilities）
3. **场景测试**（用 `leno_module/LenoSDL3/examples/UI组件/总览/showcase.leno`）：
   - 补全速度：SDL3. 后补全，首次后应秒级（缓存命中）
   - documentSymbol：左侧大纲显示 SDL3/Event/Renderer 等
   - signatureHelp：`SDL3.createWindow(` 显示参数提示，逗号切换高亮参数
   - documentHighlight：光标在变量上，文件内同名高亮
   - references：右键 SDL3.Event → 查找引用，列出所有使用处
   - rename：重命名变量，跨文件生效
4. **回归**：普通 .leno 文件（assert/ 下测试用例）的补全/hover/跳转不受影响

## 风险与降级

- **ModuleFuncSymbol 不存参数类型**：signatureHelp 对模块 .leno 导出函数可能拿不到参数类型。降级方案：先只对内置函数 + 内置模块方法（native_get_module_method_metas 已有完整参数类型）做 signatureHelp，模块 .leno 函数显示参数个数即可。后续若要完整参数，需扩展 module_symbol_table 的 scan_func 记录参数。
- **references 跨文件扫描成本**：SDL3 子模块多，全扫描可能慢。限制只扫描"导入了该符号所在模块"的文件，且用模块符号缓存先确认符号存在再扫源码。
- **rename 准确性**：纯字符串匹配会误改注释/字符串中的同名文本。用 `is_inside_string_literal/comment` 过滤，定义处和引用处都过滤。
