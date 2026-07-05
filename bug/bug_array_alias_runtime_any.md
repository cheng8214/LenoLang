# Bug: `Array[alias_func]` 直接调用 `arr[i](args)` 解析错误

## 环境
- 发现: 2026-07-05
- 复现: `examples/测试/test_arr_call.leno`
- 状态: ✅ 已修复

## 现象

```leno
alias Handler = func(int):int
Array[Handler] _handlers = []
_handlers.add(func(int x):int { return x * 2 })

// ❌ 报错："只能调用函数（对象类型不正确）"
_handlers[i](5)

// ✅ 正常（workaround）
var cb = _handlers[i]
cb(5)
```

## 根本原因

**解析器 bug**：`parse_index()` 在 [parser_expr.c:805](file:///d:/CLeno/LenoC/src/parser/parser_expr.c#L805) 把 `_handlers[i](args)` 错误地解析为**泛型函数调用** `_handlers<i>(args)`，而不是正确的 **数组索引 + 调用** `(_handlers[i])(args)`。

原因链：
1. `is_generic_type_start_token()` 接受 `TOK_IDENT`
2. 变量名 `i` 是标识符 → 触发泛型尝试解析路径
3. 泛型路径成功消费了 `[i]` 作为类型参数，`(args)` 作为调用括号
4. 生成的字节码：`push arg + push arr + OP_PUSH_TYPE_ARGS + OP_CALL`（缺少 OP_GET_LOCAL + OP_INDEX）
5. VM 执行时栈上 callee 位置错误，拿到的是字符串/数字而非闭包

## 修复

**文件**: [parser_expr.c:807-812](file:///d:/CLeno/LenoC/src/parser/parser_expr.c#L807)

在 `parse_index()` 中添加小写字母检查：当 `[` 后面的标识符以小写字母开头时，跳过泛型解析路径，按普通数组索引处理。

```c
// 小写开头的标识符不可能是泛型类型参数，跳过泛型解析路径
// 这样 arr[i](args) 会被正确解析为数组索引+调用，而非泛型调用
int is_lowercase_ident = (p->lex.current.type == TOK_IDENT &&
                          p->lex.current.text && p->lex.current.len > 0 &&
                          p->lex.current.text[0] >= 'a' && p->lex.current.text[0] <= 'z');
if (is_generic_call_candidate && is_generic_type_start_token(p->lex.current.type) && !is_lowercase_ident) {
```

## 同时修复的问题

1. **debug.c 操作码名称表不同步**: 添加缺失的 `OP_GET_FIELD_ADDR` 条目
2. **op_call.inc debug fprintf 移除**: 清理临时调试代码

## 测试结果

- 150 个测试全部通过 ✅
- 基本场景 `arr[0](5)` ✅
- 循环中 `arr[i](i+10)` ✅
- sdl_window.leno 模式 `_runEvts[i](ev)`, `_runRends[i](ren)` ✅
- 多元素数组 `arr[0](x)`, `arr[1](y)` ✅
- 泛型调用 `Ok[int](42)`, `makeBox[T](v)` ✅ 不受影响

## 已知限制

当使用**大写开头变量名**作为索引时（如 `_handlers[Idx](5)`），仍会被解析为泛型调用。
Workaround：先提取到变量 `var cb = _handlers[Idx]; cb(5)`。
