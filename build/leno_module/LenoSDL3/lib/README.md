# LenoSDL3 / lib 目录说明

| 文件 | 作用 |
|---|---|
| `SDL3.leno` | 模块入口（re-export 全部子模块） |
| `sdl_core.leno` | clib 声明、常量、`lib()` + **路径助手** `libDir()` / `libPath()` / `libBare()` |
| `sdl_*.leno` | 窗口 / 渲染 / 字体 / 图片 / 控件等 |
| `win/` `linux/` | SDL3 及其扩展库 |

## 库按平台分目录（2026-11-19 起）

```
win/SDL3.dll / SDL3_image.dll / SDL3_ttf.dll
win/libwebp-7.dll / libwebpdemux-2.dll / libwebpmux-3.dll     （可选，默认不打包）
linux/SDL3.so / SDL3_image.so / SDL3_ttf.so
linux/libwebp.so.7 / libwebpdemux.so.2 / libwebpmux.so.3      （可选，默认不打包）
```

原来是平铺在 `lib/` 下。改成子目录是为了和 `LenoSqlite` / `LenoQR` / `LenoWeb` / `LenoMusic`
统一，也为将来可能带的 Android 产物（SDL3 的 Android 产物要走官方 Gradle/CMake 工程，
不是"丢个 .so 就行"）留位置。

### 路径算法只有一份

模块里有 4 个加载点：`sdl_core.lib()`、`sdl_font.ttfLib()`、`sdl_image.imgLib()`、
`sdl_filedialog._ensureLib()`。它们**都调用 `sdl_core` 导出的助手**，不要各写一份：

```leno
// 平台子目录优先，兜底裸名（打包后库被复制到 exe 同目录）
try { g_lib = ffi.load(libPath("SDL3_ttf")) }
catch e { g_lib = ffi.load(libBare("SDL3_ttf")) }
```

`libwebp` 系列命名不规则（`libwebp-7.dll` vs `libwebp.so.7`），用 `libDir()` 自己拼前缀。

## 注意

- **`mac` 键已移除**。原先写的是 `mac = "lib/SDL3.so"`，但那个 `.so` 是 Linux ELF ——
  `mac` 键只检查"文件存在"，于是变成**假通过**：打包能过、运行时 dlopen 必失败。
  要支持 macOS：取官方 dmg 里的 dylib 放进 `lib/mac/`，补 `mac = "lib/mac/SDL3.dylib"`，
  并在 `libPath()` 里加 mac 分支。
- 换库后跑 `build\leno.exe build\leno_module\run_all_tests.leno --only LenoSDL3`。
  `test_renderer_texbatch` 会真的建窗口并渲染 ⇒ 能证明库确实加载成功，不是只拼对了路径。
