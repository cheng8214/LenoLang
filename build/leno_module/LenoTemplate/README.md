# LenoTemplate

Mustache 风格的文本模板引擎（**纯 Leno**）：变量 / 循环 / 条件，给代码生成、邮件、报告、HTML 拼装用。

```leno
import "Template" as tpl

Dict[string, any] d = {}
d["name"] = "A&B"
d["items"] = ...
d["items"] = []      // 数组
Dict[string, any] i1 = {}; i1["t"] = "甲"
d["items"] = [i1]    // 示意，实际用 add

string s = tpl.render("<b>{{name}}</b>{{#items}} {{t}}{{/items}}", d)
```

## 语法（Mustache 子集）

| 写法 | 含义 |
|---|---|
| `{{ name }}` | 变量替换（**HTML 转义** `& < > " ' ` =`） |
| `{{{ name }}}` / `{{& name }}` | 不转义 |
| `{{# list }}…{{/ list }}` | 区块：数组→遍历、Dict→进作用域、标量 truthy→渲染一次 |
| `{{^ list }}…{{/ list }}` | 反向区块：falsy 才渲染 |
| `{{> name }}` | 片段：从 `renderWithPartials` 的 partials 表取（缺失 = 空串，递归深度上限 64） |
| `{{! 注释 }}` | 注释 |
| `{{ . }}` | 当前值（数组遍历里） |
| `{{ user.name }}` | 点路径 |

falsy = `null` / `false` / 空串 / 空数组 / 空 Dict；数字 `0` 是 truthy。

## API

| 函数 | 说明 |
|---|---|
| `render(tpl, data)` | 渲染（`data: Dict[string, any]`；结构错时**宽松**处理，见下） |
| `renderWithPartials(tpl, data, partials)` | 带片段渲染（`partials: Dict[string, string]`） |
| `renderStrict(tpl, data)` | 结构有错（未闭合 / 名字不匹配 / 多余闭合）**直接抛错** |
| `check(tpl)` | 只做结构检查 → `Array[string]`（空 = 可渲染），不需要数据 |
| `tokenize(tpl)` | 词法分析 → `Array[Token]`（调试/自写渲染用） |
| `escape(s)` | HTML 转义（六个字符：`& < > " ' \` =`） |

两条契约（2026-11-25 补）：

- **区块必须按名字配对**：`{{#a}}…{{/b}}` 不再"强行配对"（旧实现只数深度 ⇒ 写错也静默渲染出错位文本）。
  `render` 保持宽松（没配到就渲染到末尾），要用得放心就先 `check` 或直接 `renderStrict`。
- **片段缺失是空串**，不报错、不中断（partials 是运行期给的，模板里写死名字也可能暂时没给）。

## 变更记录

- **0.2.0**（2026-11-25）：新增 `{{> partial}}` + `renderWithPartials`、`check`、`renderStrict`；
  区块配对改为**按名字**（修掉"写错也静默渲染"）；转义补 `` ` `` 和 `=`。
- **0.1.0**：变量 / 区块 / 反向区块 / 注释 / 点路径 / 转义。

## 跑测试

```bat
build\leno.exe build\leno_module\LenoTemplate\test\test_template.leno
```
