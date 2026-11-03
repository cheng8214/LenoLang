# LENB 字节码格式逆向备忘（aria.exe）

> 来源：对 `D:\ceshi\aria.exe`（Leno + LenoSDL3 编译的 GUI 程序）的纯黑盒逆向结论。
> 方法约束：全程未查看 Leno 编译器源码，仅通过 PE 结构分析、LENB 字节码数据分析和 `.text` 反汇编（capstone）交叉验证。
> 日期：2026-11-03

## 1. 总体结构

- 目标 `aria.exe`：PE32+ x86-64 GUI，12,547,180 字节（0xBF746C），imagebase `0x140000000`，10 个 section。
- **LENB 字节码是 PE overlay**：位于文件偏移 `[0xAB8A7B, 0xBF7464)`，不属任何 section，大小 1,305,065（0x13E9E9）。
- **整块逐字节反转存储**：磁盘上 blob 头部 = 逻辑尾部；运行时内存中的 LENB 副本与磁盘 blob **逐字节完全一致**（编码发生在编译期，运行时原样加载，解码表另建）。
- 版本字段：文件偏移 `0xAB8A82`，反转存储 `00 03 02 1c` = 版本 `0x0003021C`。
- 魔数：`0x424E454C`（小端 "LENB"）。

## 2. 字符串编码算法（核心结论）

**逐字节 XOR 循环 4 字节密钥 `"LENO"`**（`[0x4c, 0x45, 0x4e, 0x4f]`）：

```
enc[i] = plain[i] ^ "LENO"[i % 4]      // i 从 0 开始
```

- 密钥表位于 `.rdata` VA `0x1400D483B`，内容：`4c 45 4e 4f 00 42 4e 45 4c 00 03 02 1c ...` = `"LENO"` + `"BNEL"` + 版本常量。
- 解码原语 VA `0x1400B3A40`：读 4B 大端长度 → malloc(len+1) → memcpy → 首字节 xor 0x4c → i>=1 循环 xor key[i & 3]。
  - 其调用 `0x1400B3A10` 是 4 字节大端读原语（bswap）。
- 验证样本（blob 中）：`3c 37 27 21 38` → `print`；`2d 36 3d 2a 3e 31` → `assert`。

## 3. 标题记录定位与替换示例

- 标题记录：`blob + 0x131081`：`[4B BE len=0x00000011]` + `[17B data @ blob+0x131085]`。
- 原编码（`Aria — LenoSDL3`，含 U+2014 em dash）：
  `0d 37 27 2e 6c a7 ce db 6c 09 2b 21 23 16 0a 03 7f`
- 替换为 `Hack — LenoSDL3`：
  `04 24 2d 24 6c a7 ce db 6c 09 2b 21 23 16 0a 03 7f`
- 替换为 `破解版LenoSDL3`（17 字节，len 不变）：
  `ab e5 fa a7 eb e6 a9 c6 c4 09 2b 21 23 16 0a 03 7f`
- 标题后一字节 `0x05`（`blob+0x131096`）不可占用。

## 4. 修改约束（重要）

- **长度字段参与 LENB 结构流顺序解析，禁止改动**：改 len 会导致后续字节错位、触发校验失败（exit=1 拒载）。
- **payload 数据字节不参与内容校验，可自由等长改写**：只改 payload 不报错、运行时正常解码。
- 槽位是硬约束：`「破解版 LenoSDL3」`（带空格）UTF-8 18 字节超槽位（17B），必须等长；`「破解版LenoSDL3」` 17 字节可行。
- 改动后窗口标题确实变化且进程正常驻留（exit=259），说明标题来自 LENB 数据、无独立内容校验。

## 5. 运行时内存布局（单次实例，地址随 ASLR 变化）

- 内存 LENB 副本 = 磁盘 blob（逐字节一致）。
- 内存解码表记录格式：`[4B hash][00][2B id LE][1B type][UTF-8 payload]`，32 字节对齐。
  - `de 83 a7 49 00 67 03 97` → "Spectrum"+"乐"
  - `d8 83 a1 49 00 68 03 94` → "ProgressBar"+"失败："
  - `da 83 a3 49 00 69 03 8e` → "Aria — LenoSDL3"（标题 id=0x0369, type=0x8e）
  - `d4 83 ad 49 00 6a 03 96` → "ARIA_FONT"
- id 连续 `0x0367–0x036c`；4B hash **不是** FNV-1a（第 2 字节固定 0x83、第 4 字节固定 0x49），算法未确定。
- 其它 type（0x97/0x94/0x96/0x90）语义不明。

## 6. LENB 加载器 / 解析函数（VA，供后续分析）

- 加载器：`0x1400B91D0`（版本校验 @`0x1400B935E`）、`0x1400B97B0`（版本校验 @`0x1400B984E`）
- 内容解析：`0x1400B4720`、`0x1400B89F0`、`0x1400BCC10`、`0x1400045C0`（分配对象）、`0x140005070`（按字节查表）
- `.text` 原生 x86-64 指令 182,440 条，100% 可反汇编。
- 单字节破坏实验结论：改尾魔数 → exit=0 静默退出；改头魔数/版本/中段 → exit=1 报错。运行时对 LENB 结构强校验。

## 7. 反调试环境注意

- aria.exe / SDL3 有反调试，Debug API 附加即静默退出。
- 可行方案：`CreateProcessW` + `OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ)` + `ReadProcessMemory`（分块读取）。
- 窗口标题验证：`EnumWindows` + `GetWindowTextW`（非调试 API，可行）。
- `VirtualQueryEx` 遍历内存须用 `c_size_t` 定义 `BaseAddress`/`AllocationBase`，显式处理 `RegionSize==0`，否则 `NoneType` 比较报错。

## 8. 踩坑记录（不要重走）

- **字符串不是明文、不是单 key XOR/ADD/SUB、不是 UTF-16、不是位置相关 key**：Aria/LenoSDL3/SDL3/Spectrum 等短词在 blob/rev 中各种变换均 0 命中。`"Aria"^0x61` 曾命中 rev 两处但整体解码为乱码——**巧合，勿当真**。
- **必须从反汇编定位解码原语**（`0x1400B3A40`）才能破译编码；纯数据侧暴力扫描方向不可行。
- 磁盘 blob 与内存副本逐字节一致 ⇒ 不用在内存里找"运行时生成的字符串"，改磁盘即生效。

## 9. 相关产物

- 原文件：`D:\ceshi\aria.exe`（保留），备份 `aria_orig_backup.exe`
- 已交付副本：`aria_patch_tailmagic.exe` / `version` / `midbyte` / `headmagic` / `bypassver` / `skipver` / `badver_skip` / `blobdata` / `blobdata_only1` / `bloblen` / **`aria_patch_title.exe`**（最新，标题=`破解版LenoSDL3`）
- 内存 dump：`D:/ceshi/_mem_lenb_full_0.bin`（1,306,624 字节）、`D:/ceshi/_mem_table1.bin`（104,576 字节，解码表）