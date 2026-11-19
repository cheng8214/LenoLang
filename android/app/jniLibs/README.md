# jniLibs —— 随 APK 打进 `lib/<abi>/` 的第三方 native 库

## 为什么需要这个目录

Android **只允许 dlopen `public.libraries.txt` 里的系统库**（API 30 上是 28 个）。
系统自带的 `libsqlite.so` 不在白名单，实测报错：

```
dlopen failed: library "/system/lib64/libsqlite.so" ... is not accessible
for the namespace "classloader-namespace"
```

所以想让 Leno 用 SQLite，只能**自带一份**。放进 `lib/<abi>/` 后
`ffi.load("libsqlite3.so")` 就能从 app 自己的库目录命中（已验证：3.46.1，建库/查询/重开都正常）。

## 重新生成（换 sqlite 版本时）

```bash
# 1. 取官方 amalgamation（自带 sqlite3.c，无需 autoconf）
curl -LO https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
unzip sqlite-amalgamation-3460100.zip

# 2. 用 NDK 给每个 ABI 编一份（-Os 控体积；省略 load_extension 缩攻击面）
NDKBIN=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/windows-x86_64/bin
for pair in "aarch64-linux-android21-clang arm64-v8a" "x86_64-linux-android21-clang x86_64"; do
    set -- $pair
    $NDKBIN/$1 -shared -fPIC -Os -DSQLITE_OMIT_LOAD_EXTENSION \
        -o app/jniLibs/$2/libsqlite3.so sqlite-amalgamation-3460100/sqlite3.c -lm
done
```

构建脚本会自动做 `llvm-strip` 并以 **STORED（不压缩）** 方式写进 APK ——
`.so` 若被压缩，`extractNativeLibs=true` 时安装器不会把它解成可加载的库。

## 体积代价

每个 ABI 约 860KB，双 ABI 合计 +1.7MB（APK 2.1MB → 3.8MB）。
只发 arm64 可以砍掉一半。
