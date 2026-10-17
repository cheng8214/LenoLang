# LenoQR v0.1.0

Leno 的二维码模块。目标形态与 LenoCrypto 一致：**编码端纯 Leno、解码端 FFI**。

> **当前进度**
> - ✅ **识别（decode）**：quirc 后端，`gray` / `ARGB8888` / `RGB24` 三种像素入口
> - ⏳ **生成（encode）**：纯 Leno 实现（位流 + GF(256)/Reed-Solomon + 掩码评分），下一阶段落地
>   —— 届时 `QR.leno` 增加 `encode()` / `matrix()` / `toPNG()`，现有识别 API 不变

## 用法

```leno
import "QR" as qr

// ① 灰度缓冲（每像素 1 字节，0=黑 255=白）
var r = qr.decodeGray(grayPtr, w, h, stride)

// ② ARGB8888（内存序 B,G,R,A —— SDL 的 PIXELFORMAT_ARGB8888 / pngFileToPixels / 截屏都是它）
var r = qr.decodeARGB(pxPtr, w, h, pitch)

// ③ RGB24（内存序 B,G,R）
var r = qr.decodeRGB24(pxPtr, w, h, pitch)

if r.found { print(r.text) }
```

三者都返回同一种 Dict：

| 键 | 类型 | 含义 |
|---|---|---|
| `found` | bool | 是否识别到二维码 |
| `text` | string | 载荷（UTF-8/ASCII 字节原样转成 Leno string） |
| `len` | int | 载荷字节数 |
| `err` | string | 空串 = 正常；非空 = 真错误（库没编 / ABI 不符 / 参数非法） |

**`found=false` 且 `err=""` 表示"图里没有二维码"** —— 扫一张截图没扫到码是日常，不是异常，
所以这里不抛异常（与 LenoSDL3 的 capture 系列返回 Dict 是同一口径）。

其它诊断/元信息：

| 函数 | 说明 |
|---|---|
| `qr.version()` | quirc 自身版本串（当前 `1.0`） |
| `qr.abi()` | 原生库 ABI 版本（当前 `1`，用于和 `.leno` 侧握手） |
| `qr.maxPayload()` | 单枚码的最大载荷字节数（quirc 给的是 8896） |
| `qr.libName()` / `qr.libPath()` / `qr.available()` | 加载诊断 |

示例：`examples/decode_png.leno`（用 LenoSDL3 解 PNG → 交给 LenoQR 识别）；
屏幕框选识别可直接把 `SDL3.captureScreen()` / `Win32.captureScreen()` 返回的
`{w,h,pitch,pixels}` 喂给 `decodeARGB`。

## 原生库怎么来的

```
src/
  quirc.h quirc_internal.h quirc.c decode.c identify.c version_db.c   ← 上游原样（ISC）
  leno_qr.c      ← 我们唯一的 C 入口（包装层）
  build_dll.bat  ← Windows：产出 lib\quirc.dll
  build_so.sh    ← Linux / macOS：产出 lib\quirc.so
```

**为什么要包装层**（而不是让 Leno 直接调 `quirc_*`）：

1. `quirc_code` / `quirc_data` 各带 ~9KB 内嵌数组，交给 Leno 用 cstruct 严格对齐 =
   把 quirc 的 ABI 抄进 `.leno`，换版本就静默错位；
2. quirc 只吃**紧凑灰度缓冲**（w×h，无 stride），而调用方给的是带 pitch 的 ARGB8888
   —— 逐行拷贝与灰度转换（BT.601 luma）都放在 C 里做一次。

对外只有 5 个导出：`leno_qr_abi` / `leno_qr_version` / `leno_qr_max_payload` /
`leno_qr_scan_gray` / `leno_qr_scan_argb`（返回 `>=0` 字节数、`0` 没找到、`-2` 参数、`-3` 内存）。

**ABI 握手**：`.leno` 侧启动时比对 `leno_qr_abi()` 与 `qr_native.leno` 里的 `ABI` 常量。
dlopen 成功 ≠ 签名一致（旧 DLL 配新 `.leno` 在 x64 上不会崩，只会读出垃圾）⇒ 不一致直接报
"请重跑 build 脚本"。

**库的查找**：只把裸文件名交给 `ffi.load`，由 FFI 层按
`模块目录/bin → 模块目录 → 已加载模块目录 → 单文件打包资源目录 → exe 目录 → PATH`
去找。`quirc.dll` 与 `qr_native.leno` **同在 `lib/`** ⇒ 走"模块目录"这条即命中。

> ⚠ 不要自己用 `dirs.script_dir()` 拼路径：它给的是**入口脚本**所在目录（不是模块目录），
> 从 `test/` 跑时会拼出 `leno_module/lib/` 这种不存在的路径（第一版踩过）。

## 构建

```bat
cd build\leno_module\LenoQR\src
build_dll.bat          :: Windows → ..\lib\quirc.dll（-static-libgcc，自包含）
```

```bash
cd build/leno_module/LenoQR/src
./build_so.sh          # Linux / macOS → ../lib/quirc.so（macOS 用 .so，与 LenoSDL3 同口径）
```

- 上游 quirc 源**不加 `-Wall`**（它本身不是零告警，不为消告警去 fork）；
  我们自己的 `leno_qr.c` 用 `-Wall -Wextra`（当前**0 warning**）。
- 产物直接落 `lib/`，`src/` 只放源码。

## 打包（`-p`）

`leno.toml` 里声明：

```toml
[native-libs.quirc]
win   = "lib/quirc.dll"
```

`-p --onefile` 会把它内嵌进 exe，首次运行时解包到用户缓存目录（实测：897KB 单文件产物，
`[pack] 内嵌 1 个原生库`，运行后能正常调 `qr.abi()`）。

⚠ **POSIX 那一行还没加**：本机（Windows）产不出 `.so`。在 Linux/macOS 上跑一次
`build_so.sh` 后，把 `linux = "lib/quirc.so"` / `mac = "lib/quirc.so"` 补进上面的表即可
（声明了但文件不存在 ⇒ 打包会直接报错，所以现在不写）。

## 测试

```bat
build\leno.exe build\leno_module\LenoQR\test\test_decode_gray.leno
```

`test/test_decode_gray.leno` 把一个**已知矩阵**（版本 1 · ECC M · `LENOQR TEST 001`，
由 python `qrcode` 8.2 生成，权威期望值）放大成灰度图后识别，逐项断言：

1. 库能加载、ABI = 1、quirc 版本非空；
2. 灰度路径解出 `LENOQR TEST 001`；
3. ARGB8888 路径（内部灰度转换）解出同一内容；
4. **反向对照**：全白图 ⇒ `found=false` **且** `err=""`（"没码"与"出错"必须可区分）。

## 边界与已知限制

- 只返回**第一枚**能解开的码。一张图多枚码的枚举（坐标 + 逐个 payload）等有真实需求再加。
- 二维码四周必须留 **≥4 模块静区**，否则定位图案认不出来（这是 QR 规范，不是库的毛病）。
- `text` 是 quirc 给出的**原始载荷字节**：UTF-8 载荷（绝大多数生成器）直接用；
  Kanji/Shift-JIS 或 ECI 声明了别的编码时，需要调用方自行转码（quirc 里 `eci` / `data_type`
  暂时没往上暴露 —— 需要时在 `leno_qr.c` 里加一个 meta 出口）。
- 低质量**拍照**的鲁棒性弱于 zxing-cpp（quirc 的定位/纠错更简单）；截图与清晰照片够用。
  真要压拍照场景，再考虑加 zxing-cpp 兜底（C++ 构建链代价另算）。
- POSIX 分支**未在本机验证**（本机无 WSL/Docker）。脚本与 Windows 同构，需在 Linux/macOS
  上跑一遍测试才算收口。

## 上游与许可

- quirc（QR-code recognition library, © 2010-2012 Daniel Beer），**ISC 许可**，
  全文见 `LICENSE-quirc.txt`；与本体 Apache-2.0 兼容。
- 来源：<https://github.com/dlbeer/quirc>（2026-10-17 取 master tarball，
  SHA256 `FBEFCF8E04BE88F1FA43C68A39C9C6C42E5AB6FE91453B614AC58250821436AA`）；
  上游文件未做任何修改（`quirc_version()` 报 `1.0`）。

## 下一步（编码端，纯 Leno）

1. GF(256)（本原多项式 **0x11D** —— 注意与 LenoCrypto 的 AES 用 0x11B 不同）+ RS 纠错；
2. 版本/ECC 容量表、位流（模式指示 + 字符计数 + 终止符 + 0xEC/0x11 填充）；
3. 块交织 → 矩阵放置（定位/定时/对齐图案）→ 8 个掩码 + 4 条罚分选最优；
4. format/version 的 BCH；
5. **对拍**：同一文本的矩阵与 python `qrcode` / `qrencode` 逐格 diff，再做
   「自编码 → quirc 解码」回环（版本 × ECC × 模式全覆盖）。
