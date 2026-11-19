# LenoSDL3 / src —— 随模块入库的上游源码

这里的源码**不是** Leno 的代码，是 SDL 官方发行版的原样拷贝（裁掉了测试/示例/IDE 工程），
目的是让这个模块**自包含**：拿到 `LenoSDL3` 目录就能自己重编 Android 产物，
不需要联网、不需要 git submodule、不需要向仓库另要一份二进制。

## 版本清单

| 目录 | 上游 | 版本 | 说明 |
|---|---|---|---|
| `sdl3/` | libsdl-org/SDL | **3.4.20** | 裁剪：删 `test/ examples/ Xcode/ VisualC*/ docs/ .github/` |
| `sdl3_image/` | libsdl-org/SDL_image | **3.4.8** | 裁剪：删 `test/ examples/ docs/ Xcode/ VisualC/`，`external/` 只留 png/jpeg/webp/zlib |
| `sdl3_image/external/libpng` | libsdl-org/libpng | v1.6.59-SDL | 保留 `scripts/`（CMake 必需） |
| `sdl3_image/external/jpeg` | libsdl-org/jpeg | v9f-SDL | |
| `sdl3_image/external/libwebp` | libsdl-org/libwebp | 1.3.2-SDL | 保留 `cmake/`（CMake 必需） |
| `sdl3_image/external/zlib` | libsdl-org/zlib | v1.3.1-SDL | |
| `sdl3_ttf/` | libsdl-org/SDL_ttf | **3.2.2** | 裁剪：删 `.github/`，`external/` 只留 freetype/harfbuzz |
| `sdl3_ttf/external/freetype` | libsdl-org/freetype | VER-2-13-2-SDL | **必须保留 `builds/`**（CMake 从 `builds/unix/ftconfig.h.in` 生成配置头） |
| `sdl3_ttf/external/harfbuzz` | libsdl-org/harfbuzz | 8.5.0-SDL | 删 `test/`（90MB 的字形测试数据）、`perf/ docs/` |

## 一键重编 Android 产物

```bash
cd build/leno_module/LenoSDL3/src
./build_android.sh              # arm64-v8a + x86_64
./build_android.sh x86_64       # 只编一个 ABI
```

产物落在 `../lib/android/<abi>/`：`libSDL3.so`、`libSDL3_image.so`、`libSDL3_ttf.so`。
中间构建树在 `src/build/<abi>/`（**已 gitignore**，可以随时删）。

需要：NDK（默认 `/c/android-toolchain/android-ndk-r27c`，可用 `ANDROID_NDK_HOME` 覆盖）、
CMake ≥ 3.16、ninja 或 GNU make。

## 三个关键决定（改之前先读）

### 1. 依赖全部 `vendored`，不找系统库

`SDLIMAGE_VENDORED=ON` / `SDLTTF_VENDORED=ON` ⇒ libpng/jpeg/webp/zlib/freetype/harfbuzz
全部**静态链进** `libSDL3_image.so` / `libSDL3_ttf.so`。用 `llvm-readelf -d` 看，它们的
`NEEDED` 里只有 `libSDL3.so` + `libc/libm/libdl`，**没有** `libwebp.so.7` 之类。

这一点和 Windows 侧不同：Windows 的 `SDL3_image.dll` 旁边还摆着三个 `libwebp-*.dll`。
Android 侧不这么做是因为省事且更稳 —— 少三个必须按 ABI 分发的文件，就少三处会走岔的地方。

### 2. 关掉 AVIF / JXL / TIFF / SVG / plutosvg

它们的依赖（aom、dav1d、libjxl、libtiff、plutosvg）会让产物翻几倍，而 Leno 这边用不到。
`build_android.sh` 里显式 `-DSDLIMAGE_AVIF=OFF -DSDLIMAGE_JXL=OFF -DSDLIMAGE_TIF=OFF
-DSDLIMAGE_SVG=OFF -DSDLTTF_PLUTOSVG=OFF`。

要开的话必须先把对应的 `external/` 目录补回来（`external/download.sh` 里有上游地址）。
注意 `SDLIMAGE_AVIF=ON` 还要求 `libavif + aom + dav1d` 三件套齐全。

### 3. 开窗渲染需要 Java 侧的 `SDLActivity`，纯 ffi 做不到

SDL3 的 android 视频驱动不是纯 native 的：它要 Java 侧 `org.libsdl.app.SDLActivity`
提供 Surface、把触摸/生命周期转给 native，而且 SDL 的 JavaVM 是在**它自己的
`JNI_OnLoad`** 里缓存的 —— 那个钩子只有 `System.loadLibrary` 才会触发，
`ffi.load`（dlopen）不会。

实测结论（c14 探针在 API30 x86_64 上）：

```
INFO  SDL_Init(0) = false
INFO  SDL_GetError = Application didn't initialize properly,
      did you include SDL_main.h in the file containing your main() function?
```

试过在 `leno_jni.c` 里 dlopen 后手动调 `libSDL3.so` 的 `JNI_OnLoad(vm)` —— **会 abort**：
SDL 的 JNI_OnLoad 里 `FindClass("org/libsdl/app/SDLActivity")` 找不到就抛 pending
exception，ART 于是 `AssertNoPendingException` 直接杀进程。

所以当前能用的是**不碰窗口**的那条链（c14 已全部实测通过）：

- `SDL_CreateSurface` / `SDL_FillSurfaceRect`（CPU 侧 surface 全部可用）
- SDL3_image：PNG/JPEG/WebP 编解码
- SDL3_ttf：freetype + harfbuzz 栅格化，中文/emoji 正常（用 `/system/fonts/NotoSansCJK-Regular.ttc`）

要做真正的窗口/GPU 渲染，正路是把宿主改成 SDL 官方的 `android-project` 形态
（`sdl3/android-project/` 里有现成的 Java 与 Gradle 骨架），而不是继续在
`ffi.load` 这条线上补。详见 `../../../../docs/Android能力实测_2026-11-18.md`。
