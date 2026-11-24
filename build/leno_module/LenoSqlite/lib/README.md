# LenoSqlite / lib 目录说明

这个目录里的东西**分两种**：

| 文件 | 作用 |
|---|---|
| `sqlite3.leno` | 模块入口（对外 API：`open` / `queryAll` / `Database` / `Statement`） |
| `sqlite3_core.leno` | clib 声明、常量、`lib()` 选库逻辑 |
| `win/` `linux/` `android/` | 各平台的 SQLite 动态库 |

## 库按平台分目录（2026-11-18 起）

```
win/sqlite3.dll                  Windows 64 位
linux/sqlite3.so                 Linux 桌面（x86_64）
android/arm64-v8a/libsqlite3.so  Android arm64
android/x86_64/libsqlite3.so     Android x86_64（模拟器）
```

原来是平铺在 `lib/` 下，只有两个文件。改成子目录的原因很实际：
**Android 的库必须按 ABI 各带一份**，而平铺目录只能占一个文件名 —— 两份会互相覆盖。
Windows 的 dll 与 Linux 的 so 也本就不同格式，混放只会互相踩。

`sqlite3_core.leno` 的 `lib()` 按 `_os()` 选目录、按 `_arch()` 选 ABI，逐级降级：

- Windows：`win/sqlite3.dll` → `sqlite3.dll`（旧布局）
- Android：`android/<abi>/libsqlite3.so` → `libsqlite3.so`（**APK 里真正生效的那条**）
- 其他 POSIX：`linux/sqlite3.so` → `sqlite3.so` → `libsqlite3.so.0` → `libsqlite3.so`

## 怎么换 SQLite 版本

三个平台的库要一起换，否则会出现"桌面 3.53、设备 3.46"这种版本不一致。

**桌面（Windows / Linux）** —— 下载官方 amalgamation（`sqlite3.c` + `sqlite3.h`）：

```bash
# Linux
gcc -shared -fPIC -O2 -o linux/sqlite3.so sqlite3.c -lpthread -ldl -lm

# Windows（MSYS2 / MinGW-w64）
gcc -shared -O2 -o win/sqlite3.dll sqlite3.c -Wl,--out-implib,libsqlite3.a
```

**Android** —— 用 NDK 交叉编译（`-O2` 之外只关掉用不上的扩展加载，省体积）：

```bash
NDK=/path/to/android-ndk
for ABI in arm64-v8a x86_64; do
  case $ABI in
    arm64-v8a) TRI=aarch64-linux-android ;;
    x86_64)    TRI=x86_64-linux-android  ;;
  esac
  $NDK/toolchains/llvm/prebuilt/*/bin/${TRI}21-clang \
      -shared -fPIC -Os -std=c99 \
      -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=1 \
      -o android/$ABI/libsqlite3.so sqlite3.c -lm
  $NDK/toolchains/llvm/prebuilt/*/bin/llvm-strip --strip-unneeded android/$ABI/libsqlite3.so
done
```

## Android 打包怎么用

`android/build_android.sh` 会优先读 `lib/android/<abi>/` 打进 APK 的 `lib/<abi>/`，
不需要手工往 `app/jniLibs/` 拷贝。裸文件名 `libsqlite3.so` 是刻意的 —— Android linker
按裸名在自己 lib 目录里找，拼绝对路径反而会被 namespace 拒绝。

系统自带的 `/system/lib64/libsqlite.so` **不能用**：那是另一个库名，而且不在
`public.libraries.txt` 白名单里，`dlopen` 必被拒。

## 注意

- `leno.toml` 的 `[native-libs.sqlite3]` 只声明 `win` / `linux` 两个键。
  Android **不写进去**：一个平台键只能指一个路径，装不下两个 ABI，写了反而会让
  `leno pack` 把某一 ABI 的库复制到 exe 目录。Android 侧一律走 `build_android.sh`。
- 换完库记得跑一次 `leno.exe build/leno_module/run_all_tests.leno`（要用**编译器**跑，
  不是 VM），52 个用例里包含 SQLite 的 NULL 语义、事务回滚、坏 SQL 报错等契约。

## 记录写入：`insert` / `insertMany`（2026-11-24）

`Database` 现在能直接吃**有字段名的记录**（struct）：列名由 `reflect.fields(rec)` 生成，
不用再在调用点手抄列名 / `?` 个数 / bind 序号三处 —— 漏改其中一个就是"值塞进错的列"，
**不报错**，只是数据悄悄错位。

```leno
struct Song {
    int id = 0
    string name = ""
    float dur = 0.0
    bool liked = false
}

var d = sqlite3.open("music.db")
// 列 = 字段名（声明顺序）；id 是自增主键 ⇒ skip 掉
var r = d.insert("song", new Song(name = "晴天", dur = 269.0), ["id"])
if r.isOk() {
    print("rowid=" + _str(r.get()))
} else {
    print("失败：" + r.msg)
}

// 批量：同一个事务，要么全进、要么全不进
Array[any] many = [new Song(name = "七里香"), new Song(name = "稻香")]
var rm = d.insertMany("song", many, ["id"])
```

口径：

- 列名 = 字段名，按**声明顺序**；表名由调用方给（不做转义 —— 别把用户输入拼进来）
- 值走 `bind()` 自动分派 `int` / `float` / `string` / `null`；**`bool` 存 0/1**
- 失败一律 `Result.err` 并带 SQLite 原话（表不存在、列数不匹配可以分开看）
- 返回值：`insert` 给 `lastInsertRowid()`，`insertMany` 给成功条数

> 只适合"手里已经有 struct 记录"的场景（模型直接落库）。如果数据本来就是
> HTTP 请求里的 `Dict`，`db.run(sql, [params])` 那条路更直接 —— 不要为了用它多绕一层。
