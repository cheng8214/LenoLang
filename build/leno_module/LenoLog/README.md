# LenoLog v0.1.0

Leno 的日志模块（**纯 Leno**，零原生依赖）：等级过滤 + 时间戳 + 名字前缀 + 控制台/文件双通道。

```leno
import "Log" as log

var lg = log.create("app")              // 默认 INFO
log.addFile(lg, "D:/logs/app.log")      // 追加写（父目录自动创建）
log.addConsole(lg)                      // 同时打到 stdout

log.debug(lg, "默认不输出")
log.info(lg, "启动完成")
log.warn(lg, "磁盘快满了")
log.error(lg, "任务失败：ID=" + _str(42))
```

输出：

```
2026-10-17 22:17:39 [INFO ] app: 启动完成
2026-10-17 22:17:39 [WARN ] app: 磁盘快满了
2026-10-17 22:17:39 [ERROR] app: 任务失败：ID=42
```

## API

| 函数 | 说明 |
|---|---|
| `create(name, level=INFO, console=false)` | 新建 logger；`name` 出现在每行 `name: msg` |
| `addFile(lg, path)` | 追加目的地（父目录自动 `mkdir_p`） |
| `addConsole(lg)` | 同时输出到控制台 |
| `setLevel(lg, lv)` / `getLevel(lg)` | 运行时改等级 |
| `debug/info/warn/error(lg, msg)` | 写日志（低于等级直接丢弃） |
| `logAt(lg, lv, msg)` | 指定等级的底层入口 |
| `child(lg, sub)` | 子 logger：继承等级与目的地，名字变成 `app.db` 这种 |
| 常量 | `DEBUG=10` `INFO=20` `WARN=30` `ERROR=40` `OFF=100` |

## 设计取舍

- **不做模块级全局 logger**：模块级可变状态在多线程下要额外加锁；这里显式传 `lg`
  （要"全局"就自己 `var LOG = log.create("app")`）。
- **每次写文件都 `open("a")/writeln/close`**：慢一点，但不会因为忘记 `close` 丢最后几行
  —— 日志场景可靠性优先。要高频写请自己攒批。
- **不做自动轮转**（按大小/按天）：需要时用 `files`/`dirs` 自己切。
- 两个实现坑（都已写进代码注释）：
  1. 函数不能叫 `new`（保留字，`log.new(...)` 直接语法错误）⇒ 叫 `create`；
  2. **默认参数必须是字面量**：`int level = INFO` 会让默认值变成 0（什么都输出），
     必须写 `20`。

## 验证

```bat
build\leno.exe build\leno_module\LenoLog\test\test_log.leno
```

8 组断言：INFO 级过滤掉 debug / 行格式（时间戳+等级补位+名字）/ 改等级后 debug 可见 /
同一文件追加不覆盖 / 中文 / 子 logger 名字 / `levelName` / `OFF` 全静默。
