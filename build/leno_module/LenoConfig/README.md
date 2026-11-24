# LenoConfig

把"读配置"收成一次：**JSON 文件 + 环境变量覆盖 + 默认值 + 类型化读取**。纯 Leno，无原生依赖。

## 用法

```leno
import "Config" as config
use config.Config

var r = config.load("app.json", "APP")     // 第二个参数 = ENV 前缀
if r.isErr() {
    print("配置有问题：" + r.msg)
    _exit(1)
}
var c = r.get()

var port  = c.getInt("server.port", 8080)   // 缺 ⇒ 8080
var host  = c.getStr("server.host", "0.0.0.0")
var debug = c.getBool("debug", false)
var token = c.requireStr("auth.token")      // 缺 ⇒ 抛错
```

`app.json`：

```json
{
  "server": { "port": 8080, "host": "127.0.0.1" },
  "debug": true
}
```

环境变量覆盖：`APP_SERVER_PORT=9090` 会盖掉文件里的 `server.port`。

## API

| 函数 | 说明 |
|---|---|
| `load(path, prefix = "")` | → `Result[Config]`。**文件不存在 ⇒ ok（空配置）**；读不了 / 解析失败 ⇒ err |
| `loadJson(text, prefix = "")` | 同上，从文本 |
| `fromDict(d, prefix = "")` | 从已有 `Dict[string, any]` |

| 方法 | 说明 |
|---|---|
| `getInt` / `getStr` / `getFloat` / `getBool(key, default)` | 缺 ⇒ 默认值 |
| `getAny(key, default)` | 原样返回 |
| `requireStr` / `requireInt(key)` | 缺（含显式 `null`）⇒ 抛错，错误里带可用的顶层键 |
| `has(key)` | 是否存在（ENV 或文件） |
| `source(key)` | 值来自 `"env"` / `"file"` / `"missing"` |
| `keys()` | 顶层键（调试用） |

## 取值顺序

```
默认值（调用点给的）  <  JSON 文件  <  环境变量
```

ENV 名约定：`<PREFIX>_<KEY 全大写、点号转下划线>`，如 prefix=`APP`、键 `server.port` ⇒ `APP_SERVER_PORT`。

**`prefix` 为空 ⇒ 不读 ENV**（否则会去捡一堆同名系统变量，那种"配置自己变了"最难查）。

## 类型规则（不猜）

- JSON 里类型写错（`"port": "8080"`）⇒ **报错**，不自动转
- ENV 是字符串，按目标类型**严格**解析（`res.parseInt` / `parseFloat`），`abc` ⇒ 报错
- 放行的隐式转换只有无损那些：`int → float`、`int/float/bool → str`
- `1.5` 用 `getInt` 读 ⇒ 报错（不静默截断成 1）

## 为什么只做 JSON（不做 TOML/YAML）

TOML/YAML 的规范长尾（多行串、数组表、锚点、缩进敏感）写不全，**写不全的部分会变成新的静默错误源** ——
这与 Leno 一贯的"写错就要报出来"相反。JSON 已能表达全部配置结构，且 `jsons` 的严格解析已经修过一轮。

真需要 TOML：用 `leno.toml` 那套工具链先转成 JSON，再交给本模块。

## 一个容易踩的点

`Result[X]` 里的 `X` **必须在当前文件 `use` 导入**：

```leno
import "Config" as config
use config.Config          // ← 少了这行，`Result[Config]` 的实参就解析不出来
import "Result" as res
use res.Result
```

少了它，`r.get()` 会退化成未约束的 `T`，后续 `c.getInt(...)` 报「类型 'T' 没有方法 getInt」。
这是"类型名要导入"的通用规则（编译器会在报错里提示补哪一句 `use`），不是本模块特有的限制。

## 跑测试

```
build\leno.exe build\leno_module\LenoConfig\test\test_config.leno
```
