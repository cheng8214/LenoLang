# LenoWeb / src —— 随模块入库的上游源码

这里是 curl 与 mbedTLS 的官方源码（裁掉测试/文档），目的是让 Android 侧的
`libcurl.so` **可复现**：拿到模块目录就能重编，不用猜当初用的什么版本、什么 TLS 后端。

## 版本清单

| 目录 | 上游 | 版本 | 说明 |
|---|---|---|---|
| `curl/` | curl/curl | **8.23.0** | 裁剪：删 `tests/ projects/ packages/ docs/examples/ .github/`；**保留 `docs/` 与 `scripts/`**（CMakeLists 无条件 `add_subdirectory`，删了 configure 直接失败） |
| `mbedtls/` | Mbed-TLS/mbedtls | **3.6 分支**（实际 3.6.8） | 裁剪：删 `tests/ programs/ docs/` |
| `mbedtls/framework/` | Mbed-TLS/mbedtls-framework | main | mbedTLS 3.6 的**必需子模块**：`library/Makefile` 与 `library/CMakeLists.txt` 都直接引用 `../framework/exported.make`、`framework/scripts/*.py` |
| `mbedtls/scripts/` | 同 mbedTLS | 3.6 | 生成 `error.c` / `version_features.c` / PSA 包装层要用 |

## 一键重编 Android 产物

```bash
cd build/leno_module/LenoWeb/src
./build_android.sh              # arm64-v8a + x86_64
./build_android.sh x86_64
```

产物：`../lib/android/<abi>/libcurl.so`（约 1.7MB，strip 后）。
中间树在 `src/build/<abi>/`（已 gitignore）。

前置依赖（脚本会检查并给出提示）：

- **Perl** —— mbedTLS 的 `generate_errors.pl`。Git for Windows 自带（`/c/Program Files/Git/usr/bin/perl.exe`），
  但**不在 PATH 里**，脚本会自动找出来临时加进 PATH。
- **Python + `jsonschema` + `jinja2`** —— mbedTLS 生成 PSA 驱动包装层。
  `python -m pip install jsonschema jinja2`。
  ⚠ 脚本会把 CMake 的 `Python3_EXECUTABLE` **钉死**在检查过的那个解释器上 ——
  不钉的话 CMake 会自己挑一个（实测挑到了另一个没装 jsonschema 的 Python，
  结果要到 build 阶段才报 `No module named 'jsonschema'`）。

## 四个关键决定

### 1. TLS 必须自己编，不能用系统的

Android 系统里 `libssl.so` 是 BoringSSL，**不在** `public.libraries.txt` 白名单
⇒ `dlopen` 会被 namespace 拒（和 SQLite 那边 `libsqlite.so` 踩的是同一个坑）。
所以 curl 的 TLS 后端 mbedTLS **静态链进** `libcurl.so`，`NEEDED` 里只剩 `libc/libm/libdl`。

### 2. 用 mbedTLS 而不是 OpenSSL

体积小、CMake 交叉编干净，且与随包的 Windows `libcurl-x64.dll` 是同一个后端，
两边行为一致（同一个 `CURLOPT_*` 支持面）。

### 3. 证书**不随包**，运行时从系统证书目录拼

`libcurl.so` 是裸编的，没有编译期默认 CA 路径；Android 的系统 CA 又放在 Java KeyStore 里，
native 拿不到 ⇒ 不设 `CURLOPT_CAINFO` 时任何 https 都会报
`CURLE_PEER_FAILED_VERIFICATION(60)`。

`lib/web_curl_core.leno` 的 `caBundle()` 会：

1. 列 `/system/etc/security/cacerts/`（每个文件**就是一张 PEM**，0644 可读）
2. 把它们拼成一个 bundle 写到 `_tmpdir()/leno_ca_bundle.pem`（实测 741KB）
3. 缓存路径，`HttpClient.init()` 自动设 `CURLOPT_CAINFO`

好处是证书跟着系统更新走，不用随包塞一份会过期的 `cacert.pem`。

### 4. 关掉用不到的特性

`CURL_USE_LIBPSL=OFF`、`USE_NGHTTP2=OFF`、`CURL_ZLIB=OFF`、`BROTLI/ZSTD=OFF`、
`LDAP/LDAPS/SMB/TELNET/GOPHER` 全关 —— 每个都省一个依赖。
`ENABLE_THREADED_RESOLVER=ON` 保留（多线程抓取时 DNS 不阻塞）。

## 实测（API30 x86_64 模拟器，c16 探针）

```
OK   curl.load     | libcurl.so 已加载
OK   curl.global   | curl_global_init = 0
OK   curl.version  | libcurl/8.23.0-DEV mbedTLS/3.6.8
OK   curl.cabundle | bundle=/data/user/0/com.leno.app/files/leno_ca_bundle.pem（741446 字节）
OK   curl.http     | http://example.com/  → 200，577 字节
OK   curl.https    | https://example.com/ → 200，577 字节，curlCode=0
OK   curl.body     | 正文含「Example Domain」= true
```
