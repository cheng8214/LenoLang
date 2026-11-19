# Android「正经 App」路线：复用清单 + 缺口清单

> 写于 2026-11-19。前提是 17 个探针在模拟器上 **157/157 全绿**，
> 且一个 WebView 形态的小应用（`android/app/assets/app_web.leno`）已经端到端跑通。
> 本文只回答两件事：**手上现成的东西哪些能直接用**、**要补哪些才算正经 app**。

---

## 0. 一句话结论

构建链、宿主骨架、22 个 Leno 模块、5 个原生库的双 ABI 产物、17 个探针，
这些**都不用重写**；真正缺的只有两块 —— **界面层** 和 **系统能力桥**（通知/权限/分享/生命周期）。
界面层现在有两条已验证的路（文本、WebView），第三条（原生控件）的基础（`dlsym` 自身符号）也已实测可用。

---

## 1. 零改动直接复用

| 组件 | 位置 | 规模 | 作用 |
|---|---|---|---|
| 打包脚本 | `android/build_android.sh` | 219 行 | aapt2 link → javac → d8 → apkadd → zipalign → apksigner，全程无 Gradle |
| APK 注入器 | `android/tools/apkadd.leno` | 215 行 | 往 base.apk 里补 dex / `.so`（已有条目按原始字节搬运，不重压） |
| JNI 胶水 | `android/app/jni/leno_jni.c` | 286 行 | `JNI_OnLoad` 存 JavaVM、stdout→管道→Java、`setDataDir`、`buildInfo` |
| 宿主 Activity | `android/app/java/.../MainActivity.java` | 212 行 | asset 解包、后台线程跑 VM、**双形态 UI（log / web）** |
| 探针套件 | `android/app/assets/c*.leno` + `run_probes.sh` | 17 个 / 157 断言 | 每次改 `.so` 或模块后一键回归 |
| 能力文档 | `docs/Android能力实测_2026-11-18.md` | — | 能力分档 + 11 条踩坑记录 |

### 1.1 Leno 侧模块（都能直接 import）

| 模块 | 行数 | 正经 app 里的位置 |
|---|---|---|
| LenoHTTP | 2346 | 本地 HTTP 服务（WebView 方案的骨架）：路由/静态文件/JSON/表单/keep-alive |
| LenoSqlite | 882 | 结构化数据持久化（`libsqlite3.so` 随包） |
| LenoWeb | 9168 | HTTPS 客户端（curl + mbedTLS，CA 走系统证书目录动态打包） |
| LenoCrypto | 1644 | sha256/md5/hmac/AES-CBC（登录口令、接口签名） |
| LenoZip | 356 | zip 读写（导出数据、装插件包） |
| LenoCompress | 960 | crc32 / deflateRaw（APK 注入器就是用它） |
| LenoCSV / LenoXLSX | 314 / — | 导入导出 |
| LenoLog | 96 | 分级日志 + 文件落盘 |
| LenoResult | 263 | `Result[T]` / `Option[T]`，替代到处判 null |
| LenoUUID / LenoCLI / LenoStats / LenoGlob / LenoFS | 97/283/213/171/147 | 主键、参数解析、统计、批处理、目录操作 |
| LenoSDL3（控件层） | **32920** | 60 个控件文件：button/table/treeview/tab/menu/toast/chart/theme/layout… 见第 4 节 |

原生库（`lib/android/<abi>/`，arm64-v8a + x86_64 双份）：
`libsqlite3.so`、`libquirc.so`、`libminiaudio.so`、`libSDL3.so`+`_image`+`_ttf`、`libcurl.so`(含 mbedTLS)。

---

## 2. 改一点就能变成 app 基础设施

| 现状 | 要加什么 | 代价 |
|---|---|---|
| `MainActivity` 只有两种 UI 形态 | 拆成 `LenoHost`（VM 运行/资产/输出/事件队列）+ 具体界面 | 半天 |
| `build_android.sh` 固定 `versionCode=1`、debug 签名 | 版本号注入、release keystore、图标/启动图（aapt2 `-R`）、按 ABI 拆包 | 半天 |
| 探针靠 `run_probes.sh` 手跑 | 接进 CI：装包 → 跑探针 → 断言 `NG=0` | 2 小时 |
| 只有 `INTERNET` 权限 | 权限清单 + 运行时申请桥（见 3.5） | 1 天 |

---

## 3. 缺口清单（按"上来就会踩"排序）

| # | 缺口 | 现状证据 | 影响 |
|---|---|---|---|
| 1 | **界面层** | 原来只有 `TextView` 刷 stdout；本次新增 `ui=web` 的 WebView 形态 | 已完成两条路，第三条见第 4 节 |
| 2 | **assets 只能按名拷一个** | `MainActivity.copyAsset(name, dst)` 一次一个文件；Leno 侧无 `AAssetManager` | 多页面/图片/字体资源没发用 |
| 3 | **生命周期不通知 VM** | `onPause/onStop/onDestroy` 都没有；VM 线程阻塞在脚本里 | 切后台不停算、来电不暂停、退出不落盘 |
| 4 | **没有 Java→Leno 的函数调用** | `leno_vm_runtime.h` 只导出 `run_lenb` / `run_chunk_only` | 事件只能靠"脚本轮询"，不能"回调进函数" |
| 5 | **权限只有 INTERNET** | `AndroidManifest.xml` 仅一条 `<uses-permission>` | 相机/相册/通知/定位全不可用 |
| 6 | **存储只有 `filesDir()`** | `setDataDir(getFilesDir())` 之后 `_tmpdir()` 指向应用私有目录 | 导出文件到"下载"、读相册、共享给别的 app 都做不到 |
| 7 | **无后台 / 通知 / 定时** | 无 `Service` / `NotificationChannel` / `AlarmManager` | 只能是前台 app |
| 8 | **debug 签名 + 无图标** | `debug.keystore` 自动生成；manifest 无 `android:icon` | 不能上架，装到别人机器上没图标 |
| 9 | **崩溃无兜底** | Java 侧 `catch (Throwable)` 只兜 Java 异常；native SIGSEGV 直接挂进程 | 线上排障只能看 tombstone |
| 10 | **单 APK 双 ABI 17MB** | `ABIS="arm64-v8a x86_64"` 一起打 | 体积翻倍，需 ABI split / AAB |
| 11 | **无 i18n 资源** | 无 `res/values-*`，文案写死在脚本 | 可以在 Leno 里用 Dict 按 locale 取，代价低 |

> 3.4 的临时解法已在用：`ffi.pump_callbacks()` 可以在脚本里主动"收事件"，
> 配合 `ffi.callback` 把 C 侧回调排进队列（本次探针已验证该机制在 Android 上可用）。

---

## 4. 界面层三条路线

| 路线 | 复用 | 需新写 | 手感 | 风险 | 适合 |
|---|---|---|---|---|---|
| **A 文本** | 现有宿主 0 改动 | 0 | 无 | 无 | 命令行工具、探针、内部工具 |
| **B WebView + 本地 HTTP** | LenoHTTP(2346) + sockets + 现有宿主（已加 `ui=web`） | HTML/CSS/JS（纯前端） | 接近混合应用 | 首帧偶发绘制残留（模拟器软渲染，见 5.3） | 表单/列表/看板类业务 app |
| **C JNI 原生控件桥** | 现有 `leno_jni.c` + `ffi.load("libleno.so").dlsym(...)`（已验证） | C 胶水 ~200 行 + 每个控件两侧各写一次 | 原生 | 控件多则胶水膨胀 | 少量系统级控件（Toast/通知/权限弹窗） |
| **D SDL3 控件层** | **32920 行控件现成**（跨桌面/安卓同一套 UI） | 宿主改成继承 `org.libsdl.app.SDLActivity`，VM 跑在 SDL 的 native 线程 | 自绘，跨平台一致 | 当前 `ffi.load("libSDL3.so")` 拿不到 JavaVM（SDL 只在自身 `JNI_OnLoad` 缓存），必须走官方 android-project 形态；SDL 官方 Java 类**已在** `LenoSDL3/src/sdl3/android-project/` | 游戏化/自绘/要桌面安卓同一套界面 |

**推荐组合**：B 起步（业务界面），C 补 3~5 个系统能力（Toast、通知、权限、分享），
D 只在"确实需要自绘"的产品上再上。

---

## 5. 本次实测证据

### 5.1 新探针 `c17_realapp`（设备 13/13）

三块地基一次测完：

```
OK   自加载 libleno.so              → 能 dlopen 自己
OK   dlsym lenolang_run_lenb        → VM 入口可见
OK   dlsym Java_..._buildInfo       → 宿主胶水可见 ⇒ 可扩展自写胶水（路线 C 的前提）
OK   加载 liblog.so / dlsym         → 系统库与系统函数同样可达
OK   ffi.callback 建槽 / callbackCount / pump_callbacks
OK   listen 127.0.0.1:18099         → 应用内可绑本地端口
OK   connect / accept / 请求行 / 响应 → 环回自连自发成立（路线 B 的前提）
```

全部 17 个探针：**157 OK / 0 NG**（`bash android/run_probes.sh`）。

### 5.2 WebView 形态端到端

- 页面由 `app_web.leno` 生成（170 行）：SQLite 表 + 三个路由（`/`、`/api/list`、`/api/add`）
- 界面：卡片式看板，点「保存」→ `fetch POST` → SQLite 落盘 → `location.reload()`
- 设备侧另用 `adb forward tcp:18081 tcp:18080` + `curl` 直接验证接口：
  `POST /api/add` 后 `GET /api/list` 返回 `count:2`，重启 app 后数据仍在（真落盘）
- **同一份脚本在桌面直接可跑**：`build\leno.exe android\app\assets\app_web.leno`，浏览器开 `127.0.0.1:18080`

### 5.3 一个已知小问题

模拟器（swiftshader 软渲染）首次进入 WebView 时，第一帧偶发"内容错位"——
卡片被画进相邻卡片的位置，等二次重绘（重进或数据刷新）就正常。
真机 GPU 渲染未复现；若在意，可在 `onPageFinished` 里下发一次 `invalidate()`。

---

## 6. 跑法

```bash
# 桌面：同一份脚本，浏览器当界面
build\leno.exe android\app\assets\app_web.leno

# 安卓：文本形态
adb shell am start -n com.leno.app/.MainActivity --es asset app_demo.lenb

# 安卓：WebView 形态
adb shell am start -n com.leno.app/.MainActivity --es asset app_web.lenb --es ui web

# 全量探针
ADB=/c/android-toolchain/sdk/platform-tools/adb.exe bash android/run_probes.sh
```

---

## 7. 下一步的最小闭环（若要正式起一个 app 项目）

1. `MainActivity` 拆 `LenoHost`：VM 运行、资产解包、输出、事件队列分离
2. asset 全量解包（首次启动拷整棵 `assets/` 到 `filesDir`）——解决缺口 2
3. 加 `onPause/onResume/onDestroy` → 写一个 `state` 文件，脚本轮询——解决缺口 3
4. 清单加 `INTERNET + POST_NOTIFICATIONS`，配一个 `leno_ui_toast()` 胶水——解决缺口 5、6（部分）
5. release keystore + `versionName/versionCode` + 图标——解决缺口 8
