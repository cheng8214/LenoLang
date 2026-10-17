# LenoQR v0.3.0

Leno 的二维码模块。形态与 LenoCrypto 一致：**编码端纯 Leno、解码端 FFI**。

> **当前进度**
> - ✅ **生成（encode）**：**纯 Leno** —— 版本 1–40 自动选择、L/M/Q/H、数字/字母数字/字节(UTF-8)
>   三种模式、8 个掩码 + 4 条罚分、format/version 的 BCH、GF(256)/Reed-Solomon（0x11D）
> - ✅ **识别（decode）**：quirc 后端（ISC），`gray` / `ARGB8888` / `RGB24` 三种像素入口
> - 两端**不互相依赖**：编码不需要原生库；解码不依赖编码器

## 生成（encode，纯 Leno）

```leno
import "QR" as qr

var r = qr.encode("https://gitee.com/cheng8214/LenoLang", "M")   // 纠错档 L/M/Q/H，默认 M
if not r.ok { print(r.err); return }

print("v" + _str(r.version) + "/" + r.eccName + " mask=" + _str(r.mask))
var rows = r["rows"] as Array[string]      // size 行，每行 '0'/'1'（'1' = 黑）
```

返回 Dict：

| 键 | 类型 | 含义 |
|---|---|---|
| `ok` / `err` | bool / string | 失败原因（如"内容超出该版本容量：101 字节，v1/H 只能装 7 字节"） |
| `version` / `ecc` / `eccName` / `mask` / `size` | int / string | 实际选中的规格（版本、纠错档、掩码、边长模块数） |
| `mode` | int | 0=数字 / 1=字母数字 / 2=字节(UTF-8) |
| `rows` | Array[string] | 矩阵（'0'/'1' 文本，便于直接打印/对拍） |
| `cw` | Array[int] | 交织后的完整码字流（数据 + 纠错）—— 仅对拍/诊断用 |

配套：

```leno
// 矩阵 → 像素（scale 像素/模块，quiet 模块静区；静区不足 4 格很多识别器会扫不到）
string gray = qr.toGray(r, 4, 4)     // 1 字节/像素，0=黑 255=白（可直接喂 decodeGray）
string argb = qr.toARGB(r, 4, 4)     // ARGB8888（B,G,R,A）

// 指定版本 / 查容量
var r2 = qr.encodeVersion(text, "H", 10)   // 不做自动选择；装不下就 ok=false
qr.capacityBytes(1, 0)                      // v1/L 字节模式 = 17
qr.dataCodewords(1, 0)                      // v1/L 数据码字 = 19
qr.gfMul(a, b)                              // GF(256)/0x11D 乘法（自测用）
```

**ECI 26（UTF-8 显式声明）**：少数老扫码器会按 Latin-1 解释未声明编码的字节，
显式声明能避免乱码（代价 +12 bit 容量）：

```leno
var r = qr.encodeEci("你好，Leno", "M")     // r.eci == 1
```

**Kanji 模式**（`Shift-JIS` 字节 → 每字 13 bit；**本库不做转码**）：

```leno
import strings
// "こんにちは" 的 SJIS 字节
string sjis = strings.from_bytes([130, 177, 130, 241, 130, 201, 130, 191, 130, 205])
var r = qr.encodeKanjiSJIS(sjis, "M")       // r.mode == 3
```
不做自动转码的理由：Leno 没有 iconv，跨平台转码要么加原生依赖、要么维护大表；
调用方通常本来就在处理 SJIS 数据（从文件/网络拿到的就是字节）。

存成 PNG 的完整例子：`examples/gen_png.leno`（编码纯 Leno，只有"写 PNG"借 LenoSDL3 的 `IMG_SavePNG`）。
不想依赖 SDL 的场合，直接拿 `rows` 自己画即可。

## 识别（decode，quirc）

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

**多码**（一张图里多枚码：海报拼图、成对贴纸、界面截图）—— 检测**只做一次**，
按 index 逐个解：

```leno
var all = qr.decodeAllARGB(px, w, h, pitch)
// {found, count, texts: Array[string], metas: Array[int], err}
//   count = 检测到的枚数（含解不开的）
//   texts = 成功解开的载荷
//   metas = 每枚 4 个整数 [version, ecc_level, mask, data_type]
//           ecc_level 是 quirc 口径：M=0 L=1 H=2 Q=3；data_type：1=数字 2=字母 4=字节 8=Kanji
print(_str(all.count) + " 枚")
for (all.texts as Array[string]) to t { print("  - " + t) }
```

单码接口（`decodeGray/ARGB/RGB24`）在多码图上只返回**第一枚**能解开的 ——
不确定图里有几枚时用 `decodeAll*`。

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

⚠ **POSIX 那一行还没加**：本机（Windows）产不出 `.so`，也没有 bash / WSL / Docker
可以验证脚本 —— 所以 `build_so.sh` **未经运行验证**（脚本本身用 `cc` 而非写死 gcc，
macOS 上 `cc` 就是 clang）。在 Linux/macOS 上跑一次并跑通五个测试后，把
`linux = "lib/quirc.so"` / `mac = "lib/quirc.so"` 补进上面的表即可
（声明了但文件不存在 ⇒ 打包会直接报错，所以现在不写）。

## 测试

三个用例，各自守一段（都在仓库根目录跑）：

```bat
:: ① 解码：已知矩阵 → 灰度/ARGB → 解出原文（不依赖 LenoSDL3）
build\leno.exe build\leno_module\LenoQR\test\test_decode_gray.leno

:: ② 编码 ↔ 解码回环（GF 向量 / 容量表 / 版本边界 / 模式选择 / 8 条回环 / 超容量报错）
build\leno.exe build\leno_module\LenoQR\test\test_encode_decode.leno

:: ③ 真实图片：生成 → 写 PNG → 读回 → 解码（依赖 LenoSDL3 的图片编解码）
build\leno.exe build\leno_module\LenoQR\test\test_roundtrip_png.leno

:: ④ 多码：两枚码拼一张图，一次检测全部解出（count / texts / metas）
build\leno.exe build\leno_module\LenoQR\test\test_decode_multi.leno

:: ⑤ ECI(UTF-8) + Kanji(Shift-JIS) 回环 + data_type 校验 + 非法输入
build\leno.exe build\leno_module\LenoQR\test\test_eci_kanji.leno
```

**矩阵级对拍（开发期工具，随模块入库但不参与运行时）**：
`test/dev_diff_vs_python.py` 用 python `qrcode` 8.2 生成同一版本/纠错/掩码的矩阵，与
Leno 侧（`test/dev_emit_matrix.leno` 的输出）**逐格 diff**，同时用 qrcode 的罚分独立算
8 个掩码，验证"我们选的掩码 = 最优掩码"。需要 `pip install qrcode`：

```bat
python build\leno_module\LenoQR\test\dev_diff_vs_python.py
```

实测 **11 个用例全一致（矩阵 + 掩码最优性）**：

| 用例 | 版本 | 结果 |
|---|---|---|
| `LENOQR TEST 001` / M、`1234567890` / L、`HELLO WORLD 123` / Q | v1 | 矩阵逐格一致 + 掩码最优 |
| 36B URL / M、中文 `你好，Leno 二维码` / H | v3 | 同上 |
| 55B 长句 / Q、100B / L | v5 | 同上 |
| 300B / M | v10 | 同上（16 位字符计数） |
| 1200B / L | v25 | 同上 |
| 2900B 字母数字 / L、5000 位数字 / L | v33 / v34 | 同上（14/13 位计数 + 多组交织 + version info） |

## 实测（Windows x64 · gcc 15.2.0）

- 编码耗时（纯 Leno，含 8 掩码评分）—— 优化前后：

| 内容 | 优化前 | 优化后 |
|---|---|---|
| 15B（v1/M） | 1.6 ms | **0.67 ms** |
| 300B（v10/M） | 16.2 ms | **5.4 ms** |
| 1200B（v25/L） | 69 ms | **24 ms** |

  优化点：① 罚分**单遍扫描**（原 6 遍）；② 掩码**就地切/撤**（原每掩码复制整张矩阵）；
  ③ 掩码条件表按 size 缓存（批量生成同版本时零重算）；④ RS 生成多项式按 n 缓存。
- 识别耗时：一张 328×328 PNG（v4/Q）从读盘到出文本 **≈ 5 ms**；多码检测与单码同量级
  （检测只做一次，逐枚解码另算）
- 生成的 PNG 头/尺寸经独立解析核对（328×328 / 360×360，5–6 KB）

## 边界与已知限制

- **不做**：Structured Append、Micro QR、ECI 分配号 ≥128（只支持单字节分配号，
  足够覆盖 UTF-8/ISO-8859/SJIS 等常见值）、Kanji 的自动转码（要调用方给 SJIS 字节）。
- 编码只吃 `string`（UTF-8 字节）：二进制载荷请先自己转成字符串载体。
- 单码接口在多码图上只返回**第一枚**；需要全部用 `decodeAll*`。
- 二维码四周必须留 **≥4 模块静区**，否则定位图案认不出来（这是 QR 规范，不是库的毛病）。
- `text` 是 quirc 给出的**原始载荷字节**：UTF-8 载荷（绝大多数生成器）直接用；
  Kanji/Shift-JIS 或 ECI 声明了别的编码时，需要调用方自行转码（quirc 里 `eci` / `data_type`
  暂时没往上暴露 —— 需要时在 `leno_qr.c` 里加一个 meta 出口）。
- 低质量**拍照**的鲁棒性弱于 zxing-cpp（quirc 的定位/纠错更简单）；截图与清晰照片够用。
  真要压拍照场景，再考虑加 zxing-cpp 兜底（C++ 构建链代价另算）。
- POSIX 分支**未在本机验证**（本机无 WSL/Docker）。脚本与 Windows 同构，需在 Linux/macOS
  上跑一遍测试才算收口。

## 规格表从哪来

`lib/qr_tables.leno` **不要手改**：它由 `src/gen_tables.py` 从 python qrcode 的
`RS_BLOCK_TABLE` / `PATTERN_POSITION_TABLE` 导出（`python src/gen_tables.py` 重新生成）。
手抄 PDF 的 160 组容量表+分块结构，错一位的症状是"某些版本扫不出来"，且很难肉眼发现。

## 上游与许可

- quirc（QR-code recognition library, © 2010-2012 Daniel Beer），**ISC 许可**，
  全文见 `LICENSE-quirc.txt`；与本体 Apache-2.0 兼容。
- 来源：<https://github.com/dlbeer/quirc>（2026-10-17 取 master tarball，
  SHA256 `FBEFCF8E04BE88F1FA43C68A39C9C6C42E5AB6FE91453B614AC58250821436AA`）；
  上游文件未做任何修改（`quirc_version()` 报 `1.0`）。

## 后续可选（都不阻塞现在）

- **性能**：v10 已从 16ms 降到 5.4ms；若还要更快，下一步是把矩阵按"每行一个 int 位图"
  表示（罚分与掩码都用整型位运算），预计能再快 3–5 倍，但要重写放置与罚分。
- **识别增强**：多码的**坐标**（quirc 的 corners 已有，只是没往上暴露）、ECI 编号
  透传（`quirc_data.eci`）。
- **编码增强**：ECI 分配号 ≥128（GB18030 等）、SVG 输出（不需要 SDL）。
- **POSIX**：在 Linux/macOS 上跑 `build_so.sh` + 五个测试，再往 `leno.toml` 补 `linux`/`mac`。
