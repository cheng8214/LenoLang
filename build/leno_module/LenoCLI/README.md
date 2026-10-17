# LenoCLI v0.1.0

Leno 的命令行参数解析（argparse 的常用子集，**纯 Leno**）。此前每个 CLI 工具都在
手写同一段 `while + if` 解析 `sys._args()`。

```leno
import "CLI" as cli

var p = cli.spec("mytool", "做某事的工具", "mytool [选项] <input>")
cli.flag(p, "-v", "--verbose", "详细输出")
cli.option(p, "-o", "--output", "输出文件", "out.txt")
cli.optionInt(p, "-n", "--count", "次数", 3)
cli.positional(p, "input", "输入文件")

var r = cli.parse(p, _args())
if not r.ok  { print(r.err); print(cli.helpText(p)); return }
if r.help    { print(cli.helpText(p)); return }

var verbose = (r.values as Dict).get("verbose", false) as bool
var output  = (r.values as Dict).get("output", "") as string
var input   = (r.positionals as Dict).get("input", "") as string
```

## API

| 函数 | 说明 |
|---|---|
| `spec(name, desc="", usage="")` | 新建规格 |
| `flag(p, short, long, desc="")` | 布尔旗标 |
| `option(p, short, long, desc, def)` | 带值选项（string，有默认值） |
| `optionInt(p, short, long, desc, def)` | 带值选项（int；解析失败报错） |
| `positional(p, name, desc="", required=true)` | 位置参数（按注册顺序取） |
| `parse(p, args)` | → `{ok, err, help, values: Dict, positionals: Dict, rest: Array[string]}` |
| `helpText(p)` | 自动生成帮助（用法/选项/参数，含默认值） |

## 支持的写法

```
--output a.txt      --output=a.txt      -o a.txt      -oa.txt
-v  /  -ab（组合短旗标）      -- 之后的都进 rest
-h / --help（自动，ok=true 且 help=true）
```

错误都会给出可读原因：未知选项（附可用列表）、选项缺值、缺少必填位置参数、
int 解析失败（“需要一个整数，收到「abc」”）。

## 实现要点（踩过的坑）

- `values` 必须是 `Dict[string, any]`：裸 `Dict` 的元素类型会被**第一次写入**定死
  （先塞 bool 默认值，后面就塞不进 string/int）
- 默认参数要写字面量（同 LenoLog 的坑）：`cli.option(..., def)` 里的 `def` 由调用方给，
  模块内部不依赖常量默认值

## 验证

```bat
build\leno.exe build\leno_module\LenoCLI\test\test_cli.leno
```

7 组断言：默认值 / 长选项（空格与 `=`）/ 短选项与 `-n7` 连写 / 位置参数与 `--` /
四类错误 / help 文本 / 组合短旗标 `-ab`。
