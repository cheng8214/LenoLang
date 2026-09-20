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
