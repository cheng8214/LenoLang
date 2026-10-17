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

## 模块一览（有 `test/` 的）

| 模块 | 一句话 | 用例 |
|---|---|---|
| LenoCSV | CSV 读写（RFC 4180 容错） | `test/test_csv.leno` |
| LenoCompress | DEFLATE/gzip/zlib 压缩与解压 + CRC32/Adler-32 | `test/test_inflate_crc.leno`、`test/test_deflate.leno` |
| LenoZip | ZIP 归档（stored + deflate） | `test/test_zip.leno` |
| LenoXLSX | Excel 读写（zip + XML） | `test/test_xlsx.leno` |
| LenoHTTP | HTTP/1.1 服务端子集 | `test/test_http.leno` |
| LenoLog | 日志（等级/多目的地/raw） | `test/test_log.leno` |
| LenoUUID | UUID v4 / v7 / 短 ID | `test/test_uuid.leno` |
| LenoCLI | 命令行解析 | `test/test_cli.leno` |
| LenoGlob | 通配符匹配与查找 | `test/test_glob.leno` |
| LenoFS | rmtree / copytree / copy | `test/test_fs.leno` |
| LenoStats | 统计 + 金额整数分 | `test/test_stats.leno` |
| LenoQR | 二维码生成（纯 Leno）+ 识别（quirc） | `test/test_*.leno`（5 个） |
| LenoCrypto | 加密原语 | `test/test_crypto_vectors.leno` |

> 各模块 README 里的“单跑命令”仍然有效（`build\leno.exe build\leno_module\<Mod>\test\<file>.leno`），
> 适合开发时只跑一个；提交前请跑总入口。
