# LenoMusic / lib 目录说明

| 文件 | 作用 |
|---|---|
| `Music.leno` | 模块入口（对外 API） |
| `music_core.leno` | clib 声明、常量、`find_dll()` 选库逻辑 |
| `win/` `linux/` | 各平台的 miniaudio 动态库 |

## 库按平台分目录（2026-11-19 起）

```
win/miniaudio.dll        Windows 64 位（src/build_dll.bat 产出）
linux/miniaudio.so       Linux 桌面（src/build_so.sh 产出）
```

原来是平铺在 `lib/` 下。改成子目录是为了和 `LenoSqlite` / `LenoQR` / `LenoWeb` / `LenoSDL3`
统一，也为将来可能带的 Android 产物（miniaudio 在 Android 上走 AAudio/OpenSL，
且要按 ABI 各一份）留位置。

`music_core.leno` 的 `find_dll()` 逐级降级：

- 每个候选根目录下先试 **平台子目录**（`win/miniaudio.dll`），再试 **裸文件名**（`miniaudio.dll`）
- 候选根目录：`~/.leno/pkgs/LenoMusic/lib` → CWD → `lib/` → `../../lib/` → 入口脚本目录及其 `../../lib/`

## 怎么重编

```bat
cd src
build_dll.bat            :: Windows → ..\lib\win\miniaudio.dll
```

```bash
cd src
./build_so.sh            # Linux → ../lib/linux/miniaudio.so
```

`src/` 里**只放源码**（`miniaudio.h` / `miniaudio_dll.c` / 两个构建脚本）。
构建产物不再同时往 `src/` 和 `lib/` 各放一份 —— 那 1.6MB 的重复副本已于 2026-11-19 移出版本库。

## 注意

- 换库后跑一次 `build\leno.exe build\leno_module\run_all_tests.leno --only LenoMusic`
  （要用**编译器**跑，不是 VM）。注意现有用例只覆盖 `music_tag` 的纯字节读写，
  **不加载 miniaudio**；要验库是否真能加载，跑一句 `Music.getVersion()` 看返回值是否 > 0。
- `leno.toml` 的 `[native-libs.miniaudio]` 只声明 `win` / `linux`。macOS 不列键：
  `lib/linux` 里那个 `.so` 是 Linux ELF，macOS dlopen 必失败，写上去只会让
  "文件不存在就报错"的检查变成假通过。
