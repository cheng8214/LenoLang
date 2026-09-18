# 待办：Leno 易用性痛点清单（来自 TraeSign 移植实录）

> **来源**：2026-09-18 用 Leno 从零移植 TraeSign（读 Trae **加密**登录态 → 派生解密拿 token → 真 HTTPS
> 签到）的全过程逐步踩坑记录。素材现场：
> `leno_module/LenoSDL3/examples/应用示例/Trae签到/`（`trae_crypto.leno` / `trae_sign.leno` / `README.md`）。
>
> **与其他文档的关系（做了去重，避免重复维护）**：
> - 类型收窄体系本身见 `类型收窄与绑定语法改进.md` ⇒ 本文 P1 **不是机制缺失**，而是"报错/文档引导不到位"✗；
> - 报错文案的总体改进见 `Leno报错提示改进计划.md`；语言级建议总表见 `Leno语言改进建议.md`；
> - 本文只记**这次实测中新发现、或上面文档没覆盖**的项 ✓。

## 清单（按"消掉卡顿"的性价比排序）

| # | 痛点 | 层面 | 优先级 | 本次实际成本 |
| --- | --- | --- | --- | --- |
| P1 | `any` 收窄报错**没说必须收窄到带泛型参数的容器** | 报错文案 | 高 | 5~6 轮编译失败 ✗ |
| P2 | 解释器把脚本的 `--xxx` 当**自己的**旗标 ⇒ 打印帮助、脚本不跑 | 工具链 | 高 | 逼改全部子命令 ✗ |
| P3 | `LenoWeb.setHeaders` **替换**语义，静默覆盖（且示例在教错误用法）| 模块 API | 高 | 误判成 token 过期 ✗✗ |
| P4 | 空数组 `var a = []` 元素类型是 `any`，报错点离现场很远 | 类型系统 | 中 | 2 轮 ✗ |
| P5 | 只有 `_env`（读），**没有 `_setenv`** | 标准库 | 中 | 写门禁时绕 shell ✗ |
| P6 | `_exec` 与 `assert/run_tests.leno` **都没有超时** | 工具链 | 中 | 只能外部兜 ✗ |
| P7 | 跨模块相对路径冗长 ＋ **示例腐化**（硬编码别机器路径）| 工程 | 中 | 若干 ✗ |
| P8 | `files.read` 失败语义未写明（`""` 还是 `null`）| 文档 | 低 | 1 轮 ✗ |
| P9 | `build_vm.bat` 与 `build.bat` 是**两套源码清单**，且它是 GBK 文件 | 构建 | 中 | 一次全断 ✗✗ |
| J1 | 真实热点 `sha512_bytes` 被 `OP_CAST_INT` bail 拉黑 | JIT 性能 | 中 | 签到工具每次都要算 SHA-512 |

---

## P1 `any` 收窄的报错提示缺了最关键的一句（机制已有，只是没引导到位）

**实测三条真实报错**（`trae_sign.leno` 编译期）：

1. `不能在 any 类型 'd' 上直接访问字段 'token'`
   ＋ 提示 `使用 if d is Type { d.token } 进行类型收窄`；
2. 照提示加窄后仍然失败：
   `类型错误：变量 'ad' 声明类型与初始化值类型不匹配 / 期望类型: Dict[unknown, unknown]，实际类型: any`
   ＋ 提示 `any 类型需要显式转换后才能赋值给具体类型变量`；
3. 想找显式转换：`types` 模块只注册了
   `_bool/_byte/_float/_int/_int32/_int64/_ptr/_str/_uint32/_uint64/_uint8` ⇒ **没有 `_dict()`/`_array()`** ✗。

**真相**（`类型收窄与绑定语法改进.md` 第 10~17 行）：收窄成**裸 `Dict`**，取出的元素仍是 `any` ✗；
**必须收窄/声明成带泛型参数的**（`Dict[string, any]` / `Array[int]` …）取出来才带类型 ✓。

**建议（都很小、收益很大）**：
- 报错 1 的提示补一句：`收窄到裸 Dict 取元素仍是 any；请用 Dict[string, any]（见 docs/类型收窄与绑定语法改进.md）`；
- 报错 2 把"需要显式转换"改成"**转换成带泛型参数的容器类型**（如 `Dict[string, any]`）"；
- （可选）提供 `_dict(x)` / `_array(x)` 强转；或让 `jsons.decode` 返回 `Dict[string, any]`；
- `module_jsons.md` 的 `decode` 小节加一句"返回 `any`；取值需 `Dict[string, any]` 收窄"。

**本次绕法（30 行，供参考）**：把收窄集中到一层 —— `json_get / json_obj / json_keys`
（业务代码只跟 `string` / `Array[string]` 打交道 ✓）。**工作量大头全在这里**，去掉它这次能省一半时间 ✓。

## P2 解释器吃掉脚本的 `--xxx` 旗标

**实测**：`build\leno.exe trae_sign.leno --list` ⇒ **打印解释器自己的帮助、脚本根本没跑** ✗
（参考件 `trae-checkin.js` 的 CLI 全是 `--status/--json/--list` ⇒ 照抄必踩 ✗）。

**建议**：遇到**第一个非旗标参数（脚本路径）后停止解析自己的旗标**；或支持 `--` 终止符；或整体透传 ✓。

**本次绕法**：改成位置子命令 `list/status/diag/json/app` ✓（已写进工具 README ✓）。

## P3 `LenoWeb.setHeaders` 是替换语义 —— 静默覆盖 ＋ 服务端误导性错误

**实测**：分两次调用 `setHeaders`（第二次只补 `X-User-Region`）⇒ 前三个头（`Content-Type` /
`Authorization: Cloud-IDE-JWT …` / `x-device-id`）**被覆盖**⇒ 服务端收到未鉴权请求，回
`HTTP 200 {"code":1001,"message":"...not able to authenticate you..."}` —— **看起来完全像"token 过期"** ✗✗
（差点误判；实测 token 与参考实现逐字相同 ✓）。

**加重问题**：`leno_module/LenoWeb/examples/custom_headers.leno` **自己就分了两次** `setHeaders`
（先设 5 个头，再单独补 `Referer`）⇒ **示例在教一个会丢头的用法** ✗。

**建议**：
- 改追加语义；或保留替换但改名 `replaceHeaders`，另提供 `addHeader`；
- 修 `custom_headers.leno`；`docs/` 里补一句语义说明；
- 服务端 401/业务 1001 这类"鉴权失败"，模块层若能给出"是否真的发出了 Authorization 头"的诊断会更好 ✓。

**本次绕法**：按有无 region 走**两条完整头列表**、只调一次 `setHeaders` ✓（并在工具里写了注释警告 ✓）。

## P4 空数组 `var a = []` ⇒ 元素类型 `any`，报错点离现场很远

**实测**：`Array[string] acc = []` 才对；写 `var acc = []` 之后 `emit_json(acc[0], …)` 报
`期望 string，实际 any` ✗ —— 报错在**使用处**，而修法在**声明处** ✗。

**建议**：空数组字面量的元素类型**从首次 `.add(v)` 推断**；或在声明处给"元素类型未定，后续只能当 any 用"的提示 ✓。

## P5 只有 `_env`（读），没有 `_setenv`（写）

**实测**：给子进程设环境变量只能借 shell —— Windows `set X=v&& …`（`&&` 前不能留空格，否则值带尾空格 ✗）、
POSIX `X=v cmd` ⇒ 平台分支 ＋ 引号坑 ✓（写 `assert/test_plane_war_headless.leno` 时踩过 ✓）。

**建议**：加 `_setenv(name, value)`；或 `_exec(cmd, env_dict)` ✓。

## P6 `_exec` 与测试运行器都没有超时

**实测**：子进程挂住 ⇒ 父进程／整套断言一起挂 ✗（本次只能用 `Start-Process + WaitForExit` 从**外部**兜 ✓）。

**建议**：`_exec(cmd, timeout_ms)`；`assert/run_tests.leno` 给每个测试加超时，超时计 FAIL 并 kill ✓
（现在一个挂死的 GUI 测试会拖死整个套件 ✗）。

## P7 跨模块相对路径冗长 ＋ 示例腐化

**实测**：从 `应用示例/Trae签到/` 引 LenoWeb 要写 `../../../../LenoWeb/lib/web_net.leno` ✗；
`examples/jsons/复杂json解析.leno` 硬编码了**另一台机器**的路径 `D:\CLeno\LenoC\test\jsons\cs.json` ✗
⇒ 示例已经在腐化，且它是 `jsons` 的"示范用例"之一 ⇒ 会误导（我就是先被它误导过一次 ✓）。

**建议**：模块路径支持包名解析（`import "LenoWeb"`，与 `--install` 体系打通）；示例一律用**仓库相对路径**；
加一个"示例可运行"冒烟门禁（跑一遍 `examples/**` 里的小脚本，失败即报）✓。

## P8 `files.read` 失败语义没写清

**实测**：写 fixture 冒烟测试时得靠 `enc.len() < 32` 兜底 ✗；`module_files.md` 没写失败返回值 ✓。

**建议**：文档写明；或提供 `files.read_or(path, default)` / 失败返回 `null` 且类型上可见 ✓。

## P9 构建：`build_vm.bat` 与 `build.bat` 是**两套源码清单**；且 `build_vm.bat` 是 GBK 文件

**现象**（2026-09-18，本机）：

```
module_symbol_table.c:(.text+...): undefined reference to `parser_eval_const_expr_text'
jit_scan.c:(.text+...):           undefined reference to `opcode_name'   ×6（jit.c 还有）
collect2.exe: error: ld returned 1 exit status
VM build failed
```

**真因**（两条都是**清单漏项**，不是代码错 ✗）：

1. `src\debug.c` 被 `build.bat` 收录、被 `build_vm.bat` **漏掉** —— 而 `debug.c` 不只有反汇编函数，
   它还是 `opcode_name()` 的**唯一**定义点，JIT 拒收直方图直接用（`jit_scan.c` ×5、`jit.c` ×3）✓。
   `build_vm.bat` 里原本写着"debug.c 仅包含反汇编函数 ⇒ 通过 LENO_VM_ONLY 条件编译排除" ✗，
   **但 `debug.c` 从来没有这个守卫**（`LENO_VM_ONLY` 只出现在 `type.c` / `leno_vm.h`）⇒ 注释与事实不符。
2. 整个 `src\parser\` 不在 VM 清单里（这是"no compiler"的**本意** ✓），但 `module_symbol_table.c` 的
   **扫描链**（`inc/sym_table_scan.inc → inc/scan/*.inc → scan_enum.inc`）在 Phase 1 收敛后
   开始调用真 parser 的 `parser_eval_const_expr_text` ✗ ⇒ VM-only 链接缺符号。

**修法**（已做）：

- `build_vm.bat` 补 `src\debug.c` ✓（反汇编在 VM 里没人调，链进去无害，`-s` 会剥符号 ✓）；
- `parser_eval_const_expr_text`：VM 运行时**不做源码扫描**（`module_symbol_table_scan` 的唯一外部
  调用方是语义分析 `src/semantic/visitinc/visit_module.inc`；VM 侧 `module_loader.c` / `vm.c` /
  `serialize.c` 都不碰 `module_symbol_table_*`，它靠**反序列化**符号表 + 吃 `.lenb` ✓）⇒ VM-only 下
  给占位实现，**够不到就硬失败**（`abort()` + `[fatal]`：宁可炸，也不要静默给错枚举值 ✓）；
- **验证手段**（可照抄）：链接后拿**只存在于 `debug.c` 的枚举名**当探针，在 exe 字节流里搜
  `OP_CMPJMP_LI_INT` ⇒ `True` 才说明表真链进去了 ✓（只看 `build successful` 会被下面的坑骗 ✗）；
  再加端到端 `leno.exe -c x.leno` → `leno_vm.exe x.lenb` 真跑一遍 ✓。

**附带踩到（比上面阴得多）**：`build_vm.bat` 原本是 **GBK** 文件，我用编辑器改一次 ⇒ **整个文件被重写成
UTF-8** ✗ ⇒ cmd 按 GBK 控制台解码 ⇒ 中文注释被切碎、碎片被当命令执行：

```
'ebug.c' 不是内部或外部命令
'defined' 不是内部或外部命令
'入此版本，启动时无黑窗口' 不是内部或外部命令
```

却因为**功能性的 `set` 行都是 ASCII** 而照样 `VM build successful` ✗✗ —— 最危险的"看起来成功了"。
⇒ 该文件现已**纯 ASCII** ✓（文件头写明原因 ✓）；**`build.bat` 里也有中文注释、同样是 GBK** ✗，
下次谁用 UTF-8 编辑器改它就会复现 ✓。

**建议（根治）**：① 两份清单**合一**（抽 `sources_core.txt` / `sources_compiler.txt`：`build.bat`
= 两者相加、`build_vm.bat` = 前者，或让 VM 清单由全量清单**减去**编译器清单生成 ✓）；
② 加**构建门禁**：改了公共文件就至少 `build_vm.bat` 必须能过（这次是"只跑了 `build.bat`" ⇒ VM-only
静默腐化 ✓）；③ `.bat` 一律 ASCII 注释（中文解释放文档/源码注释 ✓，或用 `chcp 65001` 且确保无 BOM ✓）。

## J1 JIT（性能向，非正确性）：真实热点 `sha512_bytes` 被 `OP_CAST_INT` bail 拉黑

> **状态：已记录 · 暂缓（2026-09-18）** —— 本机太慢（`_jit_bugs/bench_sha512.leno` 的 2000 次
> SHA-512 基准要跑 ~40 s），**暂不动 JIT 代码** ✓。量化数据与修法方向见文末**附 B**；
> 影响面已实测：对 TraeSign 签到本身只有 ~60 ms（≈无感），只有**批量哈希**负载才明显 ✓。

跑签到工具时的真实统计：

```
Bailouts: 6
Bailout: fn='sha512_bytes' loop_bc=650 x3 — 非溢出类 @bc_off=716 | 触发指令=OP_CAST_INT
Bailout: fn='sha512_bytes' loop_bc=821 x3 — 非溢出类 @bc_off=886 | 触发指令=OP_CAST_INT
```

⇒ 两个循环各 3 次后**被拉黑** ⇒ 纯 Leno 的 SHA-512 **从 JIT 掉回解释器** ✓（结果正确 ✓，纯性能）。
签到工具每次都要算 SHA-512（派生密钥要两次 SHA-512）⇒ 值得单独当一条 JIT 案例（补 `OP_CAST_INT` 覆盖）✓。

**顺带：本轮验证有效的"修法范式"（建议固化成 JIT 规则）**
1. **类型只能来自"产出者"**：产出指令白名单 / 序言按 slot 建的位图 / callout 的 Value→raw 转换点标志 ✓；
   **不要用值域猜** ✗ —— 任何 int48 raw 同时都是合法 double 位型（小=次正规、大=正规）⇒ 值域判据**必错一半**
   （`§8.103 / §8.105 / §8.126 / §8.127` 四处同族缺陷的总根因 ✓）；
2. **优先在"转换点"修、不要在"是否编译"上修** ✗✗ —— 后者命中即 bail ⇒ 解释器重做整轮、JIT 半轮副作用
   不回滚 ⇒ **副作用执行两遍的数据污染**（`§8.126` 实测 `G.len()=204` 应为 201 ✓）；
3. 写 JIT×VM 分叉探针时，**RHS 必须依赖循环变量** ✗ —— 否则 LICM 把循环不变量提升出循环，测试**假绿**
   （`§8.128` 的教训：5 个形态全空结论 ✓）。

## 别改坏的东西（这次实测里明确"好用"的部分）

1. **报错带行列 ＋ 原因 ＋ 修法提示** ✓（`提示: 使用 if x is Type { … }` 这类是真的省时间 ✓ —— 只需按 P1 补一句）；
2. **JIT 统计里的 `Bailout: fn=… 触发指令=OP_xxx`** ✓ —— 这次一眼定位 sha512 的拉黑点，硬资产 ✓；
3. **`LENO_*` 诊断开关**（`LENO_NO_JIT` / `LENO_JIT_CLOG` / `LENO_GC_FORCE_EVERY` / `LENO_NO_JIT_YIELD` …）✓；
4. **`assert` 套件自动发现 `test_*.leno`** ＋ `jit_probes` 门禁 ＋ 绝对期望值断言的纪律 ✓；
5. **`§编号` 式复盘文档**（`docs/JIT实现与调试记录.md`）✓ —— 这个项目最值钱的习惯，别丢 ✓。

## 落地顺序建议

1. **P1 / P2 / P3**：都是小改，直接消掉"卡住人"的体验（一次提交能全做完 ✓）；
2. **P5 / P6**：工具链必备（写测试、写工具都要用）✓；
3. **P4 / P7 / P8** ＋ **J1**（`OP_CAST_INT` 覆盖）✓；
4. 例行的**示例冒烟门禁**（防 P7 复发）✓。

---

## 附：两项"根治类"方案的性价比实测（2026-09-18）

### A. 全量运行期类型位图（覆盖所有 vstack 槽 + 所有局部量）—— **结论：不做**

**成本**（代码清点，不是估计）：

- 局部量写点：`EMIT_STORE_LOCAL` 宏**只有 8 个调用点**
  （`x86_64.c:1254`、`ops_loop.inc:59/77/102/287/348`、`ops_stack.inc:155/164`）⇒ 改 1 个宏即可覆盖 ✓；
- 但有 **6 处不走该宏**：`ops_local.inc` 的 `OP_INC/DEC_LOCAL`、`OP_INC/DEC_LOCAL_NOPUSH`、
  `OP_PRE_INC/PRE_DEC_LOCAL`（§8.44 寄存器折叠路径），再加 `OP_MOVE_LOCAL/_POP`、`OP_SET_LOCAL_CONST`、
  `OP_CLEAR_LOCAL_RANGE` ⇒ **共约 10~12 处**；
- 每处 +1 条指令（`BTS`/`BTR` 常量位号），但局部量写点在**热路径**上（每次循环迭代都可能命中）✗；
- "来源未知"的写点只能保守 `BTR`（清位）⇒ 多数写点白付 ✗。

**收益**（逐项核对）：

| 期望收益 | 实测结论 |
| --- | --- |
| 修掉"写回把裸 int48 当 float 存进 `frame->locals`"（静默错值）| **无已证错值** ✗ —— 静态类型局部量不可能变型（`float→int` 编译期报错、`int→float` 隐式转换后存的仍是 double），`any` 型局部量存的是**装箱 Value** ⇒ 写回两条分支都安全（§8.128 已记录）|
| 撤掉 §8.127 的"局部量在循环内不被写入"限制 | **唯一确定收益**，但只是**精度**：`±0.0 / 次正规`元素的类型标签（数值相同 ✓）|
| 覆盖 `arr[i]` / callout 返回值 | 已由 §8.128 的"callout 转换点标志"覆盖 ✓（1 条 `setne`/callout，比全量位图便宜两个数量级）|

⇒ **成本压在热路径、收益只是窄精度** ⇒ 不做 ✓。要"统一语义"就继续走
"**在转换点 / 产出点顺手记类型**"这条路（§8.127 静态通道 + RBX 位图、§8.128 callout 标志
都是这个模式，边际成本 0~1 条指令 ✓）。

### B. J1（`sha512_bytes` 的 `OP_CAST_INT` bailout）—— **已量化**

新增 `_jit_bugs/bench_sha512.leno`（2000 × SHA-512(64B)，自报耗时；两模式 checksum 必须一致）：

| 模式 | checksum | 耗时 |
| --- | --- | --- |
| JIT（默认）| 388000 ✓ | **39.88 s**（`sha512_bytes` 两个循环被 `触发指令=OP_CAST_INT` bail ×3 拉黑）|
| `LENO_NO_JIT=1` | 388000 ✓ | **38.38 s** |

⇒ **JIT 在这条真实热路径上是零收益（反而慢 4%）** ✗：热循环被拉黑后整段掉回解释器，
只剩下 JIT 的进出 / 写回开销 ✓。

**影响面（如实）**：TraeSign 每次签到只算 2~3 次 SHA-512（≈60 ms）⇒ 对**签到工具**几乎无感 ✗；
对**批量哈希**类负载（`examples/crypto/file_encrypt`、大文件校验）才明显 ✓。

**可行修法方向**（**不是**全量位图）：给 `OP_CAST_INT` 一条**整数来源通道** —— `& | ^ << >>` 这类
在"两个操作数都已是 int"时结果必为 int（JIT 若走了 int 路径，就说明两个操作数已经是 int ✓）
⇒ 按**逐 opcode 的来源规则**扩展 provenance 格，**不需要在每个写点付指令** ✓。
动手前要先确认这几条 opcode 在 JIT 里是否单路径（§8.116 的教训：只信单路径 ✓）。

