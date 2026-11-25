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
| `{{ name }}` | 变量替换（**HTML 转义** `& < > " '`） |
| `{{{ name }}}` / `{{& name }}` | 不转义 |
| `{{# list }}…{{/ list }}` | 区块：数组→遍历、Dict→进作用域、标量 truthy→渲染一次 |
| `{{^ list }}…{{/ list }}` | 反向区块：falsy 才渲染 |
| `{{! 注释 }}` | 注释 |
| `{{ . }}` | 当前值（数组遍历里） |
| `{{ user.name }}` | 点路径 |

falsy = `null` / `false` / 空串 / 空数组 / 空 Dict；数字 `0` 是 truthy。

## API

| 函数 | 说明 |
|---|---|
| `render(tpl, data)` | 渲染（`data: Dict[string, any]`） |
| `tokenize(tpl)` | 词法分析 → `Array[Token]`（调试/自写渲染用） |
| `escape(s)` | HTML 转义 |

## 跑测试

```bat
build\leno.exe build\leno_module\LenoTemplate\test\test_template.leno
```
