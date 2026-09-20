# Bug 报告：模块函数默认参数在跨模块调用时不生效

## 复现版本
LenoC 主线 main 分支

## 复现步骤
1. 创建模块文件 `mod_default.leno`，定义带默认参数的导出函数：
   ```leno
   export func test_int(int a, int b = 10): int { return a + b }
   export func test_any(any a, any b = 10) { return a + b }
   ```
2. 创建主文件 `main_default.leno`，导入模块并省略有默认值的参数：
   ```leno
   import "mod_default.leno" as mod
   main() {
       print(mod.test_int(5))  // 期望 15
       print(mod.test_any(5))  // 期望 15
   }
   ```

## 预期行为
- `mod.test_int(5)` 应返回 15（5 + 10，10 为默认值）
- `mod.test_any(5)` 应返回 15

## 实际行为
- `mod.test_int(5)` 返回 5（默认值未被填充，b 收到 0/null）
- `mod.test_any(5)` 运行时崩溃："null 不能参与算术运算"

## 根因分析
模块函数调用（`AST_MODULE_CALL`）在代码生成阶段，没有像普通函数调用那样查找函数定义并填充缺失的默认参数值。

具体问题：
1. **ModuleFuncSymbol 缺少默认参数信息**：`ModuleFuncSymbol` 结构体没有存储 `param_count`、`default_count` 和默认值文本。符号表扫描器（`scan_func.inc`）在遇到默认值时直接跳过。
2. **语义分析缺少参数数量检查**：`visit_module.inc` 中处理用户定义模块函数调用时，没有检查参数数量是否在 [required, expected] 范围内。
3. **代码生成不填充默认值**：`codegen_expr.c` 的 `AST_MODULE_CALL` 分支中，直接使用 `ast->u.module_call.args.count` 作为调用参数数量，没有查找函数定义来填充缺失的默认参数值。

## 修复方案
1. 在 `ModuleFuncSymbol` 中添加 `param_count`、`default_count` 和 `param_default_texts` 字段
2. 更新 `scan_func.inc` 扫描器，解析参数并捕获默认值文本
3. 更新 `module_symbol_table_add_func` 函数签名和实现
4. 更新符号表缓存序列化/反序列化（版本号递增）
5. 更新 `visit_module.inc` 语义分析，正确检查参数数量
6. 更新 `codegen_expr.c` 代码生成，为缺失参数填充默认值
