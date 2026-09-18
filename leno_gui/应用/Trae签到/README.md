# Trae 签到（Leno 移植）

对标参考件：`D:\Leno工程\TraeSign-main` —— C# 主程序 `TraeCheckinApp.cs` + 自绘日历 `CalendarControl.cs`
+ 早期 Node 原型 `trae-checkin.js`（**算法以此为准**，exe 只是把 JS 逻辑内置了）。

目标：用 Leno + `LenoSDL3`（GUI）+ `LenoWeb`（HTTP）复刻这个"每日自动签到托盘助手"。

## 进度

| 层 | 状态 |
| --- | --- |
| ① **派生解密**（读 Trae 登录态用的 AES-128-CBC）| ✅ 完成，金标自测通过（`trae_crypto.leno`）|
| ② 读登录态（`storage.json` → `enc` / `dcId` / 品牌/账号）| ✅ 完成（`trae_sign.leno` 的 `pick_auth/load_account/scan_accounts`）|
| ③ 签到接口（`checkin_credits/status` / `claim` + 二次确认 + 错误码 9095/9074…）| ✅ 完成（`LenoWeb`；与 JS 参考件流程一致）|
| ④ GUI（标题状态徽标 / 账号下拉 / 今日积分 / 手动签到按钮 / **签到日历**）| ✅ 完成（`trae_gui.leno`；日历用 `Canvas` **自绘**，对标参考件的 `CalendarControl.cs` ✓）|
| ⑤ 托盘图标 + 每日 00:05 自动签到 + 失败重试（5/15/30/60/120 分钟）| ⏳ 待做 |
| ⑥ 历史记录 `history.json`（日历按账号独立）| ✅ 完成（`trae_history.leno`；键 = `brand+username+日期` ⇒ **天然按账号独立** ✓）|

### CLI 用法（位置子命令：解释器会先吃掉自己的 `--xxx` 旗标 ⇒ 用位置词）

```
build\leno.exe trae_sign.leno              # 签到（默认；今天已签到会提示无需重复）
build\leno.exe trae_sign.leno status       # 只查状态
build\leno.exe trae_sign.leno list         # 列出各品牌登录态账号（不打印 token）
build\leno.exe trae_sign.leno diag         # 打印账号/token 长度/设备ID（交叉验证用，不打印 token）
build\leno.exe trae_sign.leno json         # 结果输出一行 JSON（便于脚本调用）
build\leno.exe trae_sign.leno app "Trae CN"
set TRAE_CHECKIN_DEBUG=1                   # 打印 HTTP 码与原始响应（诊断 1001/9074/9095 等）
```

### ④ GUI 用法

```
build\leno.exe trae_gui.leno               # 打开窗口（手动签到 / 看签到日历）
```

无头自检（本机没有显示器/在 CI 里也能跑）：

```
set SDL_VIDEODRIVER=dummy
set TRAE_GUI_AUTO=40                       # 跑满 40 帧自动退出
set TRAE_GUI_NO_NET=1                      # 启动不查接口（离线看界面/日历）
```

界面构成：顶部**状态徽标**（`● 今日已签到` / `○ 今日未签到` / `× 失败`，颜色随之变）＋
**账号下拉**（多品牌登录态）＋ **今日积分** ＋ `立即签到` / `刷新状态` / `回到今天` 三个按钮 ＋
**自绘签到日历**（🟩 已签到 / 🟨 有记录但未成功 / 蓝框 = 今天；点格子看当天记录 ✓）。

两条实现约定：
- **启动背填**：接口说"今天已签到"而本地 `history.json` 没有 ⇒ 补记一条（`code=backfill`），
  否则"程序没开着的那几天"日历会漏显示 ✓；
- **签到失败也记**（`ok=0` + code）⇒ 日历画成暖色，⑤ 的失败重试因此**有据可查** ✓。

已知限制（⑤ 一起处理）：网络调用是**同步阻塞**的（点签到/刷新时窗口僵约 1 秒 ✗）；
托盘图标、每日 00:05 自动签到、失败重试（5/15/30/60/120 分钟）尚未接 ✓。

## 本机真实登录态实测（2026-09-18）

`list`：`Trae CN  账号=用户5104013636  token=true` ✓（只读、未打印 token ✓）

**交叉验证**（同一份真实 `storage.json`，参考实现 vs Leno）：

```
[node/参考实现] 品牌=Trae CN 账号=用户5104013636 token长度=1004 设备ID=4320417253462172
[leno]          品牌=Trae CN 账号=用户5104013636 token长度=1004 设备ID=4320417253462172
```

⇒ 两者解出的 token **完全一致** ✓✓（`tools/check_real.js` 是 node 侧脚本，只打印可公开的元信息）。

**`status` / 签到结果**（修掉下面的坑之后）：

```
status → 今日未签到。积分 base=150 extra=50
签到   → 签到成功！积分 base=150 extra=50        ← 真跑完了 claim + 二次确认 ✓
```

> ⚠ **踩坑（已修，值得所有 LenoWeb 使用者注意）**：`client.setHeaders([...])` 是**替换**语义（不是追加）⇒
> 我一开始先设 `Content-Type/Authorization/x-device-id`、再用第二次调用补 `X-User-Region`，
> 结果前三个头**被覆盖掉** ⇒ 服务端看到未鉴权请求，返回
> `{"code":1001,"message":"...not able to authenticate you..."}` ✗。
> 现象很像「token 过期」，其实不是（token 与参考实现逐字相同 ✓）。**⇒ 所有头必须一次设完** ✓。
> `TRAE_CHECKIN_DEBUG=1` 会打印 HTTP 码与原始响应，用来分辨这类问题 ✓。

## 语言侧注意点（踩过的）

- `jsons.decode/read_file` 返回 `any`：**不能直接当 Dict 用**（也不能赋给 `Dict` 变量），
  必须 `if x is Dict { ... }` 收窄 ⇒ 本工具把收窄集中在一层（`json_get/json_obj/json_keys`）✓；
- 空数组字面量 `var a = []` 的元素类型是 `any` ⇒ 需要 `Array[string] a = []` 这类**显式标注** ✓；
- 解释器会先解析自己的旗标 ⇒ 脚本参数别用 `--xxx`（会被当成它的选项并打印帮助 ✗）✓。

### ④⑥ 这轮新踩到的（都改成"用之前先看一眼实现"了）

- `jsons.write_file(path, v)` 会把 `v` **再 JSON 编码**一次 ⇒ 想写"空对象"**不能**传字符串 `"{}"`
  （落盘成带引号的 `"{}"`，读回来是 `string` 而不是对象 ✗）；而 `{}` 字面量的类型是 `any` ✗ 又不能
  直接赋给 `Dict` ⇒ 正解：`var cur = jsons.decode("{}")` ＋ `if old is Dict { cur = old }` ＋ 收窄 ✓；
- `var X: T = v` 这种**类型后置**写法不支持 ✗ ⇒ 写 `T X = v`（空数组更要这样标注，否则元素类型是 `any` ✓）；
- `Font.measureString(s)` 是**多返回值** ⇒ 必须 `var[float, float](w, h) = f.measureString(s)` ✓
  （写成 `var m = ...` 只会拿到第一个 float，之后 `m[0]` 报"索引操作需要对象类型" ✗）；
- `win.run` 是**事件驱动重绘**且窗口库**没有程序化关闭 API** ⇒ 无头（`SDL_VIDEODRIVER=dummy`）下
  "在 `onEvent` 里数帧然后退出"会**永久挂起** ✗（dummy 无事件 ⇒ onEvent 不被调用、也不重绘）；
  自动退出要：定时器保证 idle 也重绘 ＋ 渲染回调里数帧 ＋ `_exit()` ✓（或外部超时杀进程 ✓）。

> 已知的 `Leno` 侧注意点：`jsons.decode(...)` 返回 `any`，**嵌套字段不能直接点访问**
> （编译器要求 `if x is T { ... }` 类型收窄）⇒ 用到的地方要么收窄、要么改用字符串断言 ✓。

## ① 派生解密：算法（与 `trae-checkin.js` 的 `decrypt()` 逐字对齐）

```
enc(base64) 布局 = [6B 前缀][32B key][AES-128-CBC 密文]
派生: sha512(key) ─┐
      ure ^ dre  ─┴→ 拼成 128B → sha512 → 前 16B = aesKey、次 16B = iv
解密: AES-128-CBC 解出 → 去 PKCS7 填充 → 丢掉前 64B → 剩下的就是 auth JSON
     （JSON 里含 token / account.username / expiredAt / userRegion.region）
```

`ure` / `dre` 两张 64 字节表逐字取自参考件（源自官方客户端派生逻辑）✓。
AES 核与 SHA-512 是**机械拼接**自仓库现成的纯 Leno 实现（避免手抄 400 行出错）：
`examples/crypto/aes128.leno`（AES 块核）+ `examples/crypto/sha512.leno`（含 `sha512_bytes(Array[int])` ✓），
再去掉两份各自的 `main()`、删掉重复的 `byte_to_hex`（两份语义等价）、补上 CBC 链接与派生 ✓。

## 金标 fixture 与验证方式

真实登录态是**机密**（含 token）⇒ 不进仓库 ✗；派生算法又没有公开测试向量 ✗。
⇒ 用"**同算法正向加密**一段合成 payload + **参考实现反向自校验**"造 fixture：

```bash
node tools/gen_fixture.js            # 产出 fixture_enc.txt / fixture_expect_json.txt / fixture_login_state.json
build\leno.exe trae_crypto.leno      # Leno 侧解密必须与 fixture_expect_json.txt 逐字一致
```

- fixture 里的 token 是假串（`FAKE-TOKEN-...`）、含中文与 UTF-8 往返用例 ✓，**非机密、可进仓库** ✓；
- 生成器内部会先用自己的 `refDecrypt()`（`trae-checkin.js` 的移植）解一遍，自校验不通过就不产出 fixture ✓；
- ⚠ 纯文本 fixture 必须**无 BOM**（PowerShell 的 `-Encoding UTF8` 会写 BOM，会让逐字比对失败 ✗）
  ⇒ 生成器用 node 写 ✓（`gen_fixture.js` 同时输出 `.txt` 与 `.json`）。

实测（2026-09-18）：

```
node tools/gen_fixture.js   → OK: fixture 已生成（自校验通过 ✓）  enc 长度 = 392
build\leno.exe trae_crypto.leno → trae_crypto fixture test passed   （exit=0 ✓）
```

## 目录

| 路径 | 用途 |
| --- | --- |
| `trae_crypto.leno` | ① 派生解密（**40 行**；通用加密已抽到标准库 `leno_module/LenoCrypto` ✓，之前这里是 829 行机械拼接 ✗）|
| `trae_core.leno` | ②③ **共享核心**（登录态读取 / 签到接口 / JSON 收窄 / 日期助手）—— CLI 与 GUI **共用同一份** ✓ |
| `trae_history.leno` | ⑥ `history.json` 读写（扁平键 = `brand+username+日期`，值 = `ok+base+extra+code` ⇒ 按账号独立 ✓）|
| `trae_gui.leno` | ④ GUI（`LenoSDL3`：窗口 / 徽标 / 账号下拉 / 积分 / 按钮 ＋ `Canvas` **自绘签到日历**）✓ |
| `test/test_core_and_history.leno` | 数据层自测（日历算法用**已知日期**锚定 ✓ ＋ 历史 round-trip ✓，无 SDL、无网络 ⇒ 快）|
| `test/test_trae_decrypt_fixture.leno` | 金标 fixture 回归（绝对期望值 ✓）|
| `tools/gen_fixture.js` | 金标 fixture 生成器（node，无依赖）✓ |
| `tools/fixture_enc.txt` / `tools/fixture_expect_json.txt` | 合成 fixture（纯文本，无 BOM）✓ |
| `tools/fixture_login_state.json` | 同上（JSON 版，便于人看）✓ |
