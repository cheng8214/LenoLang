# Leno 跨模块 clib 开发经验

> 记录于 2026-07-27，基于 LenoWin32 模块开发过程中的踩坑。

---

## 1. 跨模块调用 clib 方法的正确姿势

**必须**：`use` 导入 clib 类型 + 模块级变量引用。

**错误写法**（函数内链式调用，编译器无法解析类型）：
```leno
import "sdl_core.leno" as core

export func myFunc(): Ptr[u8] {
    return core.lib().SDL_CreateSurfaceFrom(...)  // ❌ 报"clib 函数未找到"
}
```

**正确写法**（参照 `sdl_renderer.leno`）：
```leno
import "sdl_core.leno" as core
use core.sdl3                // ← 关键：导入 clib 类型

sdl3 _sdlLib = core.lib()   // ← 模块级变量，编译器可追踪类型

export func myFunc(): Ptr[u8] {
    return _sdlLib.SDL_CreateSurfaceFrom(...)  // ✅
}
```

**编译器报错**（✅ 已改进）：当未 `use` 导入 clib 类型时，编译器会提示：
```
clib 'sdl3' 函数 'SDL_Init' 未找到，请添加 use 语句导入 clib 类型（如 use module.sdl3）
```

---

## 2. 返回类型标注限制

~~`Dict` / `Dict[...]` 不能作为函数返回类型标注~~（✅ 已修复，`Dict` 和 `Dict[K, V]` 均可作为返回类型）。

```leno
// ✅ 直接使用 Dict 作为返回类型
export func captureScreen(): Dict { ... }

// ✅ 使用泛型 Dict
export func getSettings(): Dict[string, int] { ... }
```

---

## 3. ffi 内存写入方法

实际可用的方法（非 `write_int32` 等）：

| 方法 | 字节数 | 用途 |
|------|--------|------|
| `ffi.write_int(p, offset, val)` | 4 | 写入 32 位有符号整数 |
| `ffi.write_int8(p, offset, val)` | 1 | 写入 8 位整数 |
| `ffi.write_int64(p, offset, val)` | 8 | 写入 64 位整数 |
| `ffi.write_float(p, offset, val)` | 4 | 写入 float |
| `ffi.write_byte(p, offset, val)` | 1 | 写入字节 |
| `ffi.write_string(p, offset, val)` | — | 写入字符串 |
| `ffi.write_ptr(p, offset, val)` | 8 | 写入指针 |

写入 16 位值需拆为两个 `write_int8`：
```leno
// 写入 u16 = 32
ffi.write_int8(buf, off, 32); ffi.write_int8(buf, off+1, 0)
```

**编译器报错**（✅ 已改进）：当调用不存在的 ffi 方法时，编译器会列出可用方法：
```
未找到模块方法: ffi.write_int32（可用: utf8_to_utf16, write_bool, call_void, alloc, call, ...）
```

---

## 4. Windows DLL 函数归属

开发 Windows API 调用时常见混淆：

| DLL | 包含函数 |
|-----|---------|
| `user32.dll` | `GetDC`, `ReleaseDC`, `GetDesktopWindow`, `GetSystemMetrics`, `RegisterHotKey`, `UnregisterHotKey`, `OpenClipboard` 等 |
| `gdi32.dll` | `CreateCompatibleDC`, `CreateCompatibleBitmap`, `SelectObject`, `BitBlt`, `DeleteDC`, `DeleteObject`, `GetDIBits`, `GetDeviceCaps` 等 |
| `shell32.dll` | 文件操作相关 |
| `kernel32.dll` | `GlobalAlloc`, `GlobalFree`, `GlobalLock`, `GlobalUnlock` 等 |

**运行时错误**（✅ 已改进）：`clib.call()` 找不到函数时，错误信息会附带 DLL 文件名：
```
在库 'SDL3.dll' 中找不到函数 'SDL_FakeFunction'，错误码: 127
```

---

## 5. 模块结构约定

独立模块文件夹结构（参照 `LenoSDL3/`）：

```
LenoWin32/
├── leno.toml              # 包元数据
├── lib/
│   ├── Win32.leno         # 统一入口（re-export + OS 守卫）
│   ├── w32_capture.leno   # 桌面截屏
│   ├── w32_hotkey.leno    # 全局热键
│   └── w32_clipboard.leno # 剪贴板图片
└── examples/
    └── test_capture.leno  # 测试用例
```

- 入口文件对流出的每个函数做 `_is_supported()` 检查
- 子模块不依赖其他 Leno 库，仅用 `ffi` + 系统 DLL
- SDL 桥接留给调用方（如 `SDL3.leno` 的 `createSurfaceFromRaw`）

---

## 6. 原生库按平台分目录（2026-11-19 起全模块统一）

### 布局

```
LenoXXX/
├── leno.toml
├── lib/
│   ├── win/<名>.dll                    ← win 键
│   ├── linux/<名>.so                   ← linux 键
│   ├── android/<abi>/lib<名>.so        ← 不写进 toml（见下）
│   ├── mac/<名>.dylib                  ← 有 Mach-O 产物时才建这个目录
│   └── *.leno                          ← 源码
└── src/                                ← 只放源码（构建产物不许落这里）
```

现状：`LenoSqlite`（win/linux/android 三平台齐全）、`LenoMusic`、`LenoQR`、`LenoWeb`、
`LenoSDL3`（都是 win/linux）。

### 为什么必须分目录

Android 的 `.so` 是**按 ABI 各一份**的不同 ELF（`arm64-v8a` / `x86_64`）。
平铺目录只有一个文件名 ⇒ 两份互相覆盖，或把桌面的那份挤掉。
混合平台同理：`.dll` 与 `.so` 放同一层，看一眼分不清谁是谁的产物。

### 两条配套约束（漏了就跑不起来）

1. **`ffi.load` 要能吃子目录相对路径。**
   它原来的自动搜索会把传入路径**剥成纯文件名**（`filename = last_sep + 1`）再拼模块目录
   ⇒ `ffi.load("win/quirc.dll")` 永远命中不了子目录。
   已在 `src/module/ffi/ffi.c` 补了 2a-2「模块目录 + 完整相对路径」，**并且 2c
   （遍历所有已加载模块目录那一轮）也补了** —— 编译期内联的模块 `source_path` 可能为空，
   `mod_dir` 取不到时 2a/2a-2/2b 全跳过，真正救场的是 2c。

2. **loader 要「子目录 → 裸名」两路。**
   打包时 `collector_add` 只取**文件名**复制到 exe 同目录（子目录前缀被剥掉）
   ⇒ 打包产物里没有 `win/` 这层，只能按纯名字找到。

```leno
// 平台子目录优先，兜底裸文件名（打包产物 / 旧布局）
try { g_lib = ffi.load(libPath("SDL3")) }
catch e { g_lib = ffi.load(libBare("SDL3")) }
```

`libPath()` / `libBare()` 这类算路径的小函数**放在模块的核心文件里导出，不要四处重写**：
`LenoSDL3` 有 4 个加载点（core / font / image / filedialog），各写一份迟早走岔。

### 三个会吃时间坑

| 坑 | 现象 |
|---|---|
| 用 `dirs.script_dir()` 当模块目录 | 它给的是**入口脚本**目录。从 `test/` 或 `examples/<示例>/` 跑时与模块目录不同 ⇒ 拼出来的路径必然不存在。`LenoQR` / `LenoSDL3` 都踩过 |
| `mac = "lib/xxx.so"` 指向 Linux ELF | `mac` 键只检查"文件存在"，于是变成**假通过**：打包跑完、运行时 dlopen 必失败。没有 Mach-O 就别写这个键 |
| `leno.toml` 路径没跟着改 | `run_all_tests.leno` 的 `missing_lib()` 判定缺库后**静默 SKIP** 整个模块 ⇒ 汇总只多一个 SKIP，极易当成跑过了 |

### 新增一个模块到新平台时的清单

1. 产出物放进 `lib/<平台>/`（Android 再深一层 `lib/android/<abi>/`）；
2. loader 加该平台分支（`_os()` 现在能区分 `android`，见下）；
3. 桌面的 `leno.toml` 加对应键；**Android 不加**（一个键只能指一个路径，装不下两个 ABI，
   由 `android/build_android.sh` 从 `lib/android/<abi>/` 取）；
4. 跑 `build\leno.exe build\leno_module\run_all_tests.leno --only 你的模块`，
   确认输出里**没有 `[SKIP]`**。

### 相关：`_os()` 能分辨 Android 了

`native_os()`（`src/module/sys/sys.c`）新增 `#ifdef __ANDROID__ → "android"`，
**必须排在 `__linux__` 之前**（bionic 同时定义 `__linux__`）。
这是**行为变更**：原来 `_os() == "linux"` 在 Android 上成立，现在不成立
（`LenoSys.isLinux()` 因此返回 false，语义上更正确）。
要表达"POSIX 就行"必须写 `_os() == "linux" or _os() == "android"`。
