# leno_module —— 模块区与模块级测试

## 两层断言，别混

| 层 | 位置 | 跑什么 | 入口 |
|---|---|---|---|
| **语言级** | `assert/` | 编译器 / VM 语义（460 条） | `build\leno.exe assert\run_tests.leno build\leno.exe assert` |
| **模块级** | `build\leno_module/<Mod>/test/test_*.leno` | 各模块自己的断言（CSV / 压缩 / 归档 / xlsx / HTTP / 日志 / UUID / CLI / glob / FS / 统计 / QR …） | `build\leno.exe build\leno_module\run_all_tests.leno` |

模块断言**必须放在模块自己的 `test/` 下**，不进 `assert/`：

- `assert/` 是语言级断言，要求零原生依赖、能在任意平台跑；
- 模块用例可能依赖 `quirc.dll` / `SDL3.dll` / 本地端口 / 临时目录，混进语言套件会让“语言是否健康”这个信号失真。

## 总入口

```bat
:: 全部模块（缺原生库的自动 SKIP，不算失败）
build\leno.exe build\leno_module\run_all_tests.leno

:: 只跑指定模块
build\leno.exe build\leno_module\run_all_tests.leno --only LenoCSV,LenoZip

:: 只列用例，不执行
build\leno.exe build\leno_module\run_all_tests.leno --list
```

输出示例：

```
[PASS] LenoCSV/test/test_csv.leno (23 ms)
[SKIP] LenoQR/test/test_decode_gray.leno（缺原生库 lib/quirc.dll）
Results: 12 passed, 0 failed, 1 skipped (total 13)
```

- 单个用例 **120s 超时**（防止网络/死循环把总入口挂住）
- 失败会打印该用例输出的最后 8 行
- SKIP 的判据：`leno.toml` 里**当前平台**声明的 `.dll` / `.so` / `.dylib` 文件不存在
  （所以 Windows 上不会因为“没写 linux = …”而误判）

## 模块一览（20 个）

> ⚠ **包名 ≠ import 名**：`import "X"` 里的 X 是 `lib/` 下的**入口文件名**（不含 `.leno`），
> 不是 `leno.toml` 里的 `[package] name`。照包名写会直接报「找不到模块」。
> 最容易错的是这几个：`LenoFS` → `"FS"`、`LenoSqlite` → `"sqlite3"`、
> `LenoHack` → `"GameHelper"`、`LenoMusic` → `"Music"`。

| 包名 | **import 名** | 一句话 | 有 `test/` |
|---|---|---|---|
| LenoCLI | `"CLI"` | 命令行解析 | ✓ |
| LenoCompress | `"Compress"` | DEFLATE/gzip/zlib 压缩与解压 + CRC32/Adler-32 | ✓ |
| LenoCrypto | `"Crypto"` | 加密原语 | ✓ |
| LenoCSV | `"CSV"` | CSV 读写（RFC 4180 容错） | ✓ |
| LenoFS | `"FS"` | rmtree / copytree / copy | ✓ |
| LenoGlob | `"Glob"` | 通配符匹配与查找 | ✓ |
| LenoHack | `"GameHelper"` | 进程操作：内存读写、模拟输入、进程附加、内存扫描 | — |
| LenoHTTP | `"HTTP"` | HTTP/1.1 服务端子集（含 SSE、按路由 maxBody） | ✓ |
| LenoLog | `"Log"` | 日志（等级 / 多目的地 / raw） | ✓ |
| LenoMusic | `"Music"` | 音频（miniaudio 流式播放、解码、频谱） | — |
| LenoQR | `"QR"` | 二维码生成（纯 Leno）+ 识别（quirc） | ✓ |
| LenoSDL3 | `"SDL3"` | 窗口、渲染器、事件、控件（58 个 lib 文件） | — |
| LenoSqlite | `"sqlite3"` | SQLite3 绑定 | — |
| LenoStats | `"Stats"` | 统计 + 金额整数分 | ✓ |
| LenoSys | `"Sys"` | 跨平台系统：进程 / 磁盘 / 回收站 / 开机项 / 已装软件 | ✓ |
| LenoUUID | `"UUID"` | UUID v4 / v7 / 短 ID | ✓ |
| LenoWeb | `"Web"` | HTTP 客户端 + HTML 解析 + 爬虫框架 | — |
| LenoWin32 | `"Win32"` | 桌面截屏、全局热键、剪贴板图片 | ✓ |
| LenoXLSX | `"XLSX"` | Excel 读写（zip + XML） | ✓ |
| LenoZip | `"Zip"` | ZIP 归档（stored + deflate） | ✓ |

> **`LenoWeb` 的现状（值得注意）**：`Web.leno` 是门面，但 Aria/LenoMusic/LenoTrae/LenoWorkBuddy
> 四个项目 **0 次** `import "Web"`、**37 次** `import "web_net"`（直接吃内部文件）。
> 门面本身没错，问题是没有"该用门面"的共识。要用 HTTP 客户端请优先 `import "Web"`。

> 各模块 README 里的“单跑命令”仍然有效（`build\leno.exe build\leno_module\<Mod>\test\<file>.leno`），
> 适合开发时只跑一个；提交前请跑总入口。

## 什么时候才需要跑 `assert/`

| 改动范围 | 要跑的 |
|---|---|
| 只改 `build/leno_module/**`（纯 `.leno` 模块） | **只跑模块总入口** `run_all_tests.leno` |
| 动 `src/`（编译器 / VM / 内置模块的 C 实现，如 `sockets.c`） | `build.bat`（涉及入口行为还要 `build_vm.bat`）**+ assert 全量 460 条** |
| 动 `assert/` 本身（新增语言级用例） | assert 全量 |

理由：`assert/` 守的是“语言是否健康”（编译器/VM 语义），纯 Leno 模块改了不该让这个信号变红；
反过来，动了 `src/` 就必须跑 —— 例如 2026-10-17 修 `sockets.send` 的截断问题时，
因为改了 C 实现，所以重建 + assert 全量是必须的。
