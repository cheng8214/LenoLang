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
| ⑤ **风控 9074「参与用户太多」⇒ 自动换设备号重试**（失败重试的「接口层」）| ✅ 完成（`trae_core.leno` ③b；离线端到端验证 `tools/test_9074_retry.js` ✓）|
| ⑥ 托盘图标 + 定时自动签到 + 失败重试的「时间层」（5/15/30/60/120 分钟）| ⏳ 待做 |
| ⑦ 历史记录 `history.json`（日历按账号独立）| ✅ 完成（`trae_history.leno`；键 = `brand+username+日期` ⇒ **天然按账号独立** ✓）|
| ⑧ **设备号按账号独立**（`devices.json`）⇒ 同机多账号才能**各自**签到 | ✅ 完成（`trae_devices.leno`；对标参考件「每账号独立设备号 + 可手动更换」✓）|

### CLI 用法（位置子命令：解释器会先吃掉自己的 `--xxx` 旗标 ⇒ 用位置词）

```
build\leno.exe trae_sign.leno              # 签到（默认；今天已签到会提示无需重复）
build\leno.exe trae_sign.leno status       # 只查状态
build\leno.exe trae_sign.leno list         # 列出各品牌登录态账号（不打印 token）
build\leno.exe trae_sign.leno diag         # 打印账号/token 长度/Aha 设备号/本应用设备号（交叉验证用，不打印 token）
build\leno.exe trae_sign.leno json         # 结果输出一行 JSON（便于脚本调用）
build\leno.exe trae_sign.leno app "Trae CN"
set TRAE_CHECKIN_DEBUG=1                   # 打印 HTTP 码与原始响应（诊断 1001/9074/9095 等）
set TRAE_DEVICE_ID=1111111111111111        # 调试/测试：强制指定设备号（可**确定性**触发 9074 ✓）
set TRAE_API_BASE=http://127.0.0.1:8080    # 调试/测试：把接口指到本地 stub（见 tools/test_9074_retry.js ✓）
```

> 设备号：⑧ 每个账号一个**独立**号（首次使用时分配并落盘到 `%APPDATA%\TraeSignLeno\devices.json` ✓）。
> 想给某个账号换号 ⇒ GUI 的「更换设备号」，或直接删掉 `devices.json` 里它那一行 ✓。

### ④ GUI 用法

```
build\leno.exe trae_gui.leno               # 打开窗口（手动签到 / 看签到日历）
```

无头自检（本机没有显示器/在 CI 里也能跑）：

```
set SDL_VIDEODRIVER=dummy
set TRAE_GUI_AUTO=40                       # 跑满 40 帧自动退出
set TRAE_GUI_NO_NET=1                      # 启动不查接口（离线看界面/日历）
set TRAE_GUI_ACCOUNT=2                     # 启动就选中第 2 个账号（设备号按账号独立 ⇒ 无头也能验证 ✓）
```

界面构成：顶部**状态徽标**（`● 今日已签到` / `○ 今日未签到` / `× 失败`，颜色随之变）＋
**账号下拉**（多品牌登录态；右侧**显示该账号当前的设备号** —— `（本机）` = 登录态里那个机器级
Aha 号、`（独立）` = 本工具给它的新号，切号即跟着换 ✓）＋ **今日积分** ＋
`立即签到` / `刷新状态` / `回到今天` / `更换设备号` 四个按钮 ＋
**自绘签到日历**（🟩 已签到 / 🟨 有记录但未成功 / 蓝框 = 今天；点格子看当天记录 ✓）。

两条实现约定：
- **启动背填**：接口说"今天已签到"而本地 `history.json` 没有 ⇒ 补记一条（`code=backfill`），
  否则"程序没开着的那几天"日历会漏显示 ✓；
- **签到失败也记**（`ok=0` + code）⇒ 日历画成暖色，⑥ 的失败重试因此**有据可查** ✓。

### ⑧ 设备号按账号独立（`devices.json`）—— "同机两个账号只有一个能签到"的根因修复

**现象**：本机两个账号里当天只有一个能签，第二个点"立即签到"拿到 `9095`（本地 `history.json`
`2026-09-23` 有实测：`cheng8214` `code=0` 成功、另一个 `0|||9095`）。

**根因**：签到接口的风控/额度按 **`x-device-id`** 判 —— ① 同机同一天只允许**一个**账号签到
（第二个 ⇒ `9095`）；② 多账号**共用**一个设备号还容易被判 `9074`。而登录态 `storage.json` 里的
`dcId` 是**机器级**的 ⇒ 两个品牌读出来是**同一个号**（本机 `2029015639819002`）⇒ 服务端认为
"同一台设备两个账号" ✗。

**参考件怎么解的**（`D:\软件\TraeTools-main`，全部有据）：

| 点 | 证据 |
| --- | --- |
| 设备号挂在**账号**上 | `Services/Checkin/TraeAccount.cs`：「x-device-id（16 位数字，风控关键），**每账号独立**」|
| 发号保证互不重复 | `ViewModels/AccountHelpers.cs` `EnsureDeviceId()`：「缺号则生成**不与其它账号重复**的 16 位数字设备号（**风控要求，多账号共用会触发 9074**）」＋ `do { … } while (used.Contains(id))` |
| 首个账号优先复用真实 Aha 号 | `Services/Checkin/AppConfig.cs` `TryResolveAhaDeviceId()`（读 `iCubeAuthInfo://icube-dc:`）|
| 被 9074 标记可手动换号 | `ViewModels/SettingsViewModel.cs` `RegenerateDeviceId()`（注释：「设备号被 Trae 风控标记时更换即可解除」）|

（它因此**从不处理 `9095`**：全仓搜 `9095` = 0 命中 ✓）

**本项目的做法**（`trae_devices.leno`）—— 落盘 `%APPDATA%\TraeSignLeno\devices.json`（扁平键 `brand|username`）：

```json
{
  "TRAE SOLO CN|cheng8214": "2029015639819002",
  "Trae CN|用户6834498633": "9816087524605965"
}
```

- `ensure(brand, user, aha_id)`：已有记录 ⇒ 直接用（幂等 ✓）；没有 ⇒ Aha 号**合法且未被别的账号占用**
  就沿用它（对齐参考件的"优先复用真实 Aha 号" ✓），否则发一个与**全表都不重复**的新号 ⇒ **落盘** ✓；
- `regenerate()`：手动换号 —— GUI 第 4 个按钮「更换设备号」✓（对齐参考件）；被 9074 打满时点它 ✓；
- **9074 换出来的号立刻落盘** ⇒ 下次运行沿用（之前是"只在单次运行内换"，等于白换 ✗）；
- `diag` 同时打印 **Aha 号**与**本应用给该账号的号**（只读、不分配 ✓）；
- GUI **账号下拉右侧**显示该账号当前的设备号（`（本机）`/`（独立）`，未分配时写"未分配"，切号即变 ✓）；
- `TRAE_DEVICE_ID` 覆盖模式是测试口子 ⇒ **不写** devices.json（免得把测试号写进真实配置 ✗）。

实测（2026-09-25）：

```
GUI 启动查询   → 状态已刷新（沿用登录态里的 Aha 设备号 2029015639819002 ✓）
CLI 第二个账号 → 设备号：本账号独立新号 9816087524605965（已记入 devices.json ✓）
devices.json  → 两条记录，两个账号的号**不同** ✓（同机各自签到的前提 ✓）
GUI 账号行右侧 → 账号1「设备号 2029015639819002（本机）」，账号2「设备号 9816087524605965（独立）」
                （无头验证：TRAE_GUI_ACCOUNT=2 切到第二个账号，标签随切随变 ✓；
                  文字宽 200px ≤ 可用 214px ✓ 不溢出）
```

⚠ 效果边界：换号只改变**服务端怎么看这台设备**，不改变"某账号今天签没签"（签到是**按账号**算的）
⇒ 当天已经签过的账号不会因此多拿一份；今天两个账号服务端都已是"已签到"，所以"两个都签成功"
要**明天**才看得到 ✓。

已知限制（⑥ 一起处理）：网络调用是**同步阻塞**的（点签到/刷新时窗口僵约 1 秒 ✗；若撞上 9074
换号重试，最坏再叠 4×0.8~1.5s ⇒ 更明显 ✗）；托盘图标、定时自动签到、**时间层**失败重试
（5/15/30/60/120 分钟）尚未接 ✓ —— 注意 ⑤ 的「接口层」失败重试（9074 换设备号）**已完成** ✓。

### ⑤ 风控 9074：换设备号重试

`9074` =「参与用户太多」的**服务端风控**。被拒的是**设备号**、不是 token ⇒ 换一个全新设备号就能
继续，**不用**重新登录 ✓（对标参考件 `TraeTools-main` 的 `checkin.py` L145-154：随机设备号 +
最多 5 次 + 间隔 0.8~1.5s ✓）。

| 要点 | 做法 | 为什么不能想当然 ✗ |
| --- | --- | --- |
| 新设备号 | `rands.ints(10^15, 10^16-1) as string` ⇒ **16 位纯数字、首位非 0** | GUID/UUID 形式会被**直接**判风控（参考件注释：「使用 GUID/UUID 会触发 9074」）✗ |
| 重试单位 | **整跑一遍**"查状态 → claim → 二次确认"（`do_claim_retry` ✓） | 只重发 claim，会在"不知道服务端是否已签"的情况下盲提交 ⇒ 结果无法解释 ✗ |
| 次数 / 间隔 | 最多 5 次、每次 0.8~1.5s 随机（全局 `sleep(ms)` ✓，**不是** `times.sleep` ✗） | 打满就**如实报失败**（不死循环 ✓）；与参考件逐字对齐 ✓ |
| 只对 9074 重试 | `need_retry_9074`：9095 / 1001 / 0 都**不**重试 ✓ | 换设备号解决不了"本机已有账号签到"或"token 失效" ✗ |
| 状态那步也要换 | `fetch_status_retry`（同款包装 ✓） | 若状态先被 9074 挡掉，CLI 会在**进入 claim 之前**就退出 ✗ |
| 换号后要带过去 | 结果的**追加 slot** 带回"实际用的设备号"（`st_device`/`cl_device` ✓） | 起始号用回被风控的旧号 ⇒ 又白撞一次 9074 ✗ |

实现落在 `trae_core.leno` 的 **③b**；CLI（`trae_sign.leno`）与 GUI（`trae_gui.leno`）都走这两个
wrapper ✓。真的换过号时会**多打一行**「风控 9074：设备号已换新 …」✓（便于确认换号真的发生）。

**离线端到端验证**（不碰真接口 ✓；只读本机登录态拿一个 token 用 ✓）：

```bash
node tools/test_9074_retry.js             # 三种场景都跑
node tools/test_9074_retry.js status      # 只跑 fetch_status_retry 的换号
node tools/test_9074_retry.js claim       # 只跑 do_claim_retry 的换号
node tools/test_9074_retry.js always      # 只跑"重试打满 5 次"的收口
```

做法：node 起一个**本地 stub** 冒充 `api.trae.cn`（用 `TRAE_API_BASE` 把 CLI 指过去 ✓），按脚本
先回 9074、再回正常结果 ⇒ 确定性地验证"第一次 9074 → 换设备号 → 第二次成功"✓。
⚠ stub 与子进程必须**同进程异步**跑：`spawnSync` 会阻塞事件循环 ⇒ stub 收不到请求、子进程必然
超时 ✗（踩过："Timeout was reached"）。

实测（2026-09-25）：

```
=== 场景 status ===  1111111111111111 → 9801201360937274   status 4 次；claim 1 次   全部 ok
=== 场景 claim  ===  1111111111111111 → 6407119865645948   status 4 次；claim 2 次   全部 ok
=== 场景 always ===  5 次全被拒（用了 5 个不同设备号）⇒ 失败退出 +「重试 5 次仍被拒」  全部 ok
```

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

### ④⑦ 这轮新踩到的（都改成"用之前先看一眼实现"了）

- `jsons.write_file(path, v)` 会把 `v` **再 JSON 编码**一次 ⇒ 想写"空对象"**不能**传字符串 `"{}"`
  （落盘成带引号的 `"{}"`，读回来是 `string` 而不是对象 ✗）；而 `{}` 字面量的类型是 `any` ✗ 又不能
  直接赋给 `Dict` ⇒ 正解：`var cur = jsons.decode("{}")` ＋ `if old is Dict { cur = old }` ＋ 收窄 ✓；
- `var X: T = v` 这种**类型后置**写法不支持 ✗ ⇒ 写 `T X = v`（空数组更要这样标注，否则元素类型是 `any` ✓）；
- `Font.measureString(s)` 是**多返回值** ⇒ 必须 `var[float, float](w, h) = f.measureString(s)` ✓
  （写成 `var m = ...` 只会拿到第一个 float，之后 `m[0]` 报"索引操作需要对象类型" ✗）；
- `win.run` 是**事件驱动重绘**且窗口库**没有程序化关闭 API** ⇒ 无头（`SDL_VIDEODRIVER=dummy`）下
  "在 `onEvent` 里数帧然后退出"会**永久挂起** ✗（dummy 无事件 ⇒ onEvent 不被调用、也不重绘）；
  自动退出要：定时器保证 idle 也重绘 ＋ 渲染回调里数帧 ＋ `_exit()` ✓（或外部超时杀进程 ✓）。

### ⑤/③b 这轮新踩到的

- 16 位设备号这类大数，**别指望"转成数值再比范围"** ✗（两条路都踩过）：
  ① `_int(s)`：**已在解释器里修好**（`src/module/types/types.c`）—— 原写法是 `strtol` + `(int)`，
     而 Windows 的 `long` 只有 32 位 ⇒ `_int("9999999999999999")` 会**饱和**成 `2147483647`
     （在 `long` 是 64 位的平台上 `(int)` 又变成截断，跨平台两种错法 ✗）⇒ 现改为
     `strtoll` + `val_int_safe`（超出 int64 交 bigint ⇒ `_int("2147483648")` = 2147483648 ✓）；
  ② `_float(s)` 再比范围：**double 在 1e16 附近间距是 2** ⇒ `9999999999999999` 被舍入成 `1e16`，
     正好撞上上界 ⇒ 合法号被判非法（自测第 6 节真的踩到过：Aha 号被误判成非法 ⇒ 没沿用 ✗）；
  ⇒ 本应用的 `is_valid_device_id` 仍用**逐字符**校验（长度 + 首位非 0 + 每个字符 ∈ `0~9`）：精确、
  且不依赖数值表示（大数在 Leno 里可能是 bigint，别再引入"它是什么类型"的额外假设 ✓）；
- **`sleep` 是全局原生函数，不在 `times` 模块里** ⇒ `times.sleep(ms)` 报"未找到模块方法：
  times.sleep（可用: now, ns, us, format, ms, datetime）" ✗，直接写 `sleep(ms)` 就行 ✓
  （③b 的换号间隔用的就是它；探针实测 `sleep(300)` ≈ 305ms ✓。`docs/module_times.md` 里把它
  写成 `sleep(...)` 是对的，别自己加模块前缀 ✗）；
- `_env(name)` 返回 `any` ⇒ 赋值给 string 变量要 `_str(...)`；但**必须先判 `null`**：
  `_str(null)` 得到的是字符串 `"null"`（不是空串 ✗）⇒ 写成 `if x == null { ... }` 再用 `_str(x)` ✓；
  ⚠ **别想着把 `_env` 的注册类型改成 `TYPE_STRING`**（2026-09-25 查证过）：`native_env`
  （`src/module/sys/sys.c`）一次函数有**三种**返回 —— 参数不对 ⇒ `null`、**2 参形态是"设置"**⇒ 返回
  `bool`、取不到 ⇒ `null`、取到 ⇒ `string` ⇒ 注册成 `TYPE_ANY` 才是对的 ✓。硬改会让 `if x == null`
  这类判空在类型上失效、运行时却仍是 `null` ⇒ 后续 `null + "\\User\\..."` **静默**拼出坏路径 ✗
  （比现在"编译期逼你写 `_str()`"更糟）。要"总是 string"的入口只能**加法**：新增
  `_env_or(name[, default])` 之类的函数（本次决定先不做 ✓）；
- `rands.ints(min, max)` 收、发都是 int64 ✓（`10^15 ~ 10^16-1` 没问题 ✓），配合 `as string` 就是
  16 位数字设备号 ✓（不要用 `range`/`str` 手搓，`rands.str(len, "0123456789")` 会产生前导 0 ✗）。

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
| `trae_core.leno` | ②③ **共享核心**（登录态读取 / 签到接口 / JSON 收窄 / 日期助手）＋ **③b 9074 换设备号重试** —— CLI 与 GUI **共用同一份** ✓ |
| `trae_history.leno` | ⑦ `history.json` 读写（扁平键 = `brand+username+日期`，值 = `ok+base+extra+code` ⇒ 按账号独立 ✓）|
| `trae_devices.leno` | ⑧ `devices.json` 设备号存储（扁平键 = `brand+username`；`ensure/regenerate/remove` ⇒ **每账号独立** ✓）|
| `trae_gui.leno` | ④ GUI（`LenoSDL3`：窗口 / 徽标 / 账号下拉 / 积分 / 按钮 ＋ `Canvas` **自绘签到日历**）✓ |
| `test/test_core_and_history.leno` | 数据层自测（日历算法用**已知日期**锚定 ✓ ＋ 历史 round-trip ✓，无 SDL、无网络 ⇒ 快）|
| `test/test_trae_decrypt_fixture.leno` | 金标 fixture 回归（绝对期望值 ✓）|
| `tools/gen_fixture.js` | 金标 fixture 生成器（node，无依赖）✓ |
| `tools/test_9074_retry.js` | ⑤ **9074 换设备号重试**的离线端到端验证（node：本地 stub 冒充 `api.trae.cn` ⇒ 不碰真接口 ✓；三场景 status/claim/always ✓）|
| `tools/fixture_enc.txt` / `tools/fixture_expect_json.txt` | 合成 fixture（纯文本，无 BOM）✓ |
| `tools/fixture_login_state.json` | 同上（JSON 版，便于人看）✓ |
