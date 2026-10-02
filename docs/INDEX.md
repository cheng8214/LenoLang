# docs/ 文档索引

> **本文件是 `docs/` 的唯一索引**：分类、一句话主题、状态与过时标注。
> 两份 `README`（`README.md` / `README_EN.md`）只保留几条最常用链接，**找文档一律从这里进**。
>
> - **最后核对：2026-10-02**（本轮：JIT 系 5 篇归档到 `docs/archive/`、5 篇杂名改名、
>   17 篇加"历史 / 局部过时"头、新增本索引；明细见 `待办与路线图.md` 第六节）。
> - 状态图例：**✅ 现行**（可照着写代码）· **🟡 现行但含过时段落**（文件头部已加"历史 / 局部过时"块，
>   引用前先看那一段）· **📜 历史留档**（功能已不存在或已完结，只作考古）· **🧭 台账**（待办与路线图）。
> - **维护约定**：新增 / 改名 / 状态变化（实现完成、功能移除）**都要回来改这一行**；
>   文档里**不要写死断言总数** —— 一律以 `.\build\leno.exe assert\run_tests.leno build\leno.exe assert` 实跑为准
>   （历史上同一事实被写成 245/256/263/298/305/352/366/380/410/412 十个版本）。

---

## 一、指南（面向 `leno` 使用者）

| 文档 | 一句话 | 状态 |
|---|---|---|
| [Leno入门教程.md](Leno入门教程.md) | 语言全貌教程：语法 + 类型系统 + 模块，最全的一份 | ✅ |
| [FAQ.md](FAQ.md) | 常见问题（`.leno` 直跑 vs `.lenb`、缓存、退出码…） | ✅ |
| [Leno_规范草稿.md](Leno_规范草稿.md) | 语言规范草案（设计口径，不是教程） | ✅（草稿） |
| [import使用指南.md](import使用指南.md) | `import` / 模块加载与缓存 | ✅ |
| [包管理与安装使用指南.md](包管理与安装使用指南.md) | `leno --init/install/publish` 与 `leno.toml` | ✅ |
| [单文件打包使用指南.md](单文件打包使用指南.md) | `-p --onefile` 打包与资源内嵌 | ✅ |
| [async_await入门指南.md](async_await入门指南.md) | 协程 / `async` / `await` | ✅ |
| [threads使用指南.md](threads使用指南.md) | 线程 / `Channel`（与 `module_threads_api.md` 有重叠） | ✅ |
| [并发选择指引.md](并发选择指引.md) | async / threads / Channel 怎么选 | ✅ |
| [类型收窄与绑定语法改进.md](类型收窄与绑定语法改进.md) | `is` 收窄与 `=>` 绑定的语义与用法 | ✅ |
| [FFI使用指南.md](FFI使用指南.md) | FFI / clib 调用全流程 | 🟡 版本历史里有一条 JIT 记录已过时 |
| [加密算法示例指南.md](加密算法示例指南.md) | `examples/` 里纯 Leno 密码学算法导览 | ✅ |
| [Leno跨模块clib开发经验.md](Leno跨模块clib开发经验.md) | 跨模块 clib 的踩坑与写法 | ✅ |

## 二、模块 API 参考（18 篇）

| 文档 | 内容 |
|---|---|
| [module_io.md](module_io.md) | 文件读写与标准流 |
| [module_files.md](module_files.md) | files 模块（注意"字节保真 / 不翻译行尾"口径） |
| [module_dirs.md](module_dirs.md) | 目录操作 |
| [module_strings.md](module_strings.md) | 字符串方法 |
| [module_arrays.md](module_arrays.md) | 数组（`arr[:]` 双侧省略**不支持**） |
| [module_dicts.md](module_dicts.md) | 字典 |
| [module_types.md](module_types.md) | 类型判定与转换 |
| [module_maths.md](module_maths.md) | 数学 |
| [module_times.md](module_times.md) | 时间 |
| [module_rands.md](module_rands.md) | 随机数 |
| [module_regexs.md](module_regexs.md) | 正则 |
| [module_jsons.md](module_jsons.md) | JSON |
| [module_sockets.md](module_sockets.md) | 网络 |
| [module_sys.md](module_sys.md) | 系统（环境变量 / 进程 / 线程） |
| [module_asyncs.md](module_asyncs.md) | 异步协程 |
| [module_ffi_api.md](module_ffi_api.md) | FFI（命名带 `_api`，历史遗留） |
| [module_cstructs.md](module_cstructs.md) | cstruct 内存布局 |
| [module_threads_api.md](module_threads_api.md) | 线程 / Channel（命名带 `_api`，历史遗留） |

## 三、GUI / LenoSDL3

| 文档 | 一句话 | 状态 |
|---|---|---|
| [LenoSDL3像素直写渲染优化.md](LenoSDL3像素直写渲染优化.md) | 像素直写渲染的优化记录 | ✅ |
| [LenoSDL3_裁剪过多导致闪烁排查记录.md](LenoSDL3_裁剪过多导致闪烁排查记录.md) | clip / flush 闪烁排查（**仍有残留闪烁**） | 🟡 |
| [LenoSDL3_Table方法表断裂编译器bug排查记录.md](LenoSDL3_Table方法表断裂编译器bug排查记录.md) | 符号表截断 bug 的根因与修复 | 📜 已修（含旧断言数） |
| SDL3 常用 API / 控件优化清单 | 在模块目录下：`build/leno_module/LenoSDL3/docs/` 的两篇 | ✅ |

## 四、设计 · 台账（仍在推进）

| 文档 | 一句话 | 状态 |
|---|---|---|
| [待办与路线图.md](待办与路线图.md) | **待办总表**（T1…T28 + ⑤-* 批次记录 + 数值结论），一件事一条带判据 | 🧭 |
| [待办_单一事实来源与重复实现收敛.md](待办_单一事实来源与重复实现收敛.md) | 重复实现收敛（多条目对象是已移除的 JIT） | 🟡 |
| [待办_GC与分配优化.md](待办_GC与分配优化.md) | GC / 分配优化交接（多处依据 JIT 实测） | 🟡 |
| [待办_易用性痛点（TraeSign 移植实录）.md](<待办_易用性痛点（TraeSign 移植实录）.md>) | 易用性痛点清单（部分判据是 JIT 口径） | 🟡 |
| [错误堆栈追踪改进方案.md](错误堆栈追踪改进方案.md) | 异常对象与报错文案（现行契约在"引擎错误自带位置"） | 🟡 内联器那段已过时 |
| [类型收窄与绑定语法改进.md](类型收窄与绑定语法改进.md) | `is` 收窄 / `=>` 绑定的设计 | ✅ |
| [Leno报错提示改进计划.md](Leno报错提示改进计划.md) | 报错提示改进清单 | 🟡 落点文件已删 |
| [Leno语言改进建议.md](Leno语言改进建议.md) | 语言特性去留评估（结论：不再追加特性） | ✅ |
| [cstruct_packed_align_设计文档.md](cstruct_packed_align_设计文档.md) | cstruct `packed` / `align(N)`（已实现） | ✅ |
| [struct 里const可行性.md](<struct 里const可行性.md>) | struct 关联常量可行性分析 | 📜 分析稿 |
| [VM字节码校验与崩溃防护设计方案.md](VM字节码校验与崩溃防护设计方案.md) | 字节码校验分层防护（复盘的内联器 bug 已随文件删除） | 🟡 教训仍有效 |
| [解构语法实现文档.md](解构语法实现文档.md) | 解构声明语法与语义（Phase 1/2/3） | 🟡 引用表含已删文件 |
| [错误列号准确性_工作进度.md](错误列号准确性_工作进度.md) | 编译器错误列号准确性进度 | ✅（进度文档） |
| [字节码优化分析.md](字节码优化分析.md) | 融合指令建议清单（栈式时代，未回填状态） | 🟡 |
| [字节码融合指令优化_光线追踪对象版.md](字节码融合指令优化_光线追踪对象版.md) | 融合指令落地记录（模型描述是栈式） | 🟡 |
| [优化方案_内存态序列化运行.md](优化方案_内存态序列化运行.md) | 内存态序列化 / `.lenb` 运行方案 | ✅ |
| [FFI优化方案.md](FFI优化方案.md) | FFI 模块优化清单（两条依赖 JIT） | 🟡 |

## 五、性能记录

| 文档 | 一句话 | 状态 |
|---|---|---|
| [性能优化记录.md](性能优化记录.md) | fib(30) 与操作栈优化的取舍记录（**整篇是栈式 VM**） | 🟡 |
| [函数调用性能优化_调用帧与多返回值解构.md](函数调用性能优化_调用帧与多返回值解构.md) | 调用帧 / 多返回值优化（含"否决寄存器机"的旧结论） | 🟡 |
| [代码生成性能排查记录.md](代码生成性能排查记录.md) | codegen 首次编译 4.3s 的瓶颈排查 | ✅ |
| [leno对比.md](leno对比.md) | Leno vs Python 早期实测对比 | 📜 已被归档的性能总结取代 |

## 六、排查记录（已完结，留档）

| 文档 | 一句话 |
|---|---|
| [枚举求值与模块符号表问题.md](枚举求值与模块符号表问题.md) | 枚举三路径求值不一致 + 符号表扫描（全部已修；含已删文件的行号） |
| [多线程struct与模块全局变量问题记录.md](多线程struct与模块全局变量问题记录.md) | 线程 struct 定义已修，模块级全局变量串台待重构 |
| [LenoNet_崩溃问题排查记录.md](LenoNet_崩溃问题排查记录.md) | HttpClient 堆损坏崩溃（已修；根因文件已删） |
| [LenoNet开发问题记录.md](LenoNet开发问题记录.md) | libcurl 绑定踩坑 |
| [LenoHtml_开发问题记录.md](LenoHtml_开发问题记录.md) | HTML 解析器开发中暴露的编译器问题（术语是栈式） |
| [内联器崩溃排查记录.md](内联器崩溃排查记录.md) | 内联器 `patch_ast_indices` 越界导致的间歇 segfault（**方法论与教训仍有效**） |
| [窥孔优化复盘.md](窥孔优化复盘.md) | 窥孔优化 bug 复盘（旧缓存名 `.lenocache`） |

## 七、已归档（`docs/archive/`）

| 文档 | 一句话 | 为什么归档 |
|---|---|---|
| [JIT实现与调试记录.md](archive/JIT实现与调试记录.md) | JIT 架构 / 寄存器约定 / bailout / 踩坑（7104 行） | JIT 已不在代码中 |
| [JIT模块调用优化与bailout排查记录.md](archive/JIT模块调用优化与bailout排查记录.md) | JIT callout 三项优化与 bailout 排查 | 同上 |
| [JIT闭包与upvalue设计_R5.md](archive/JIT闭包与upvalue设计_R5.md) | JIT 闭包 / upvalue 设计稿 | 同上（当时即未动手） |
| [JIT安全点与去优化_参考实现调研.md](archive/JIT安全点与去优化_参考实现调研.md) | JIT 安全点 / GC 的行业调研 | 同上 |
| [性能测试总结_Leno_vs_Python.md](archive/性能测试总结_Leno_vs_Python.md) | JIT vs VM vs Python 的基准与结论 | JIT 列全部失效 |

> ⚠ **JIT 不在本仓库的历史里**：`git ls-files "*jit*"` 与
> `git log --all --diff-filter=AD -- "*jit*"` 均为空（最早提交 `e8ccc46 2026-09-20 Initial commit`）
> ⇒ 归档文档里的 `src/jit/*`、`LENO_NO_JIT=1`、`JIT_HOT_THRESHOLD` 等**别去当前源码里找**，
> 其中的 `file:line` 一律不可用。

**另外两篇"事实上已归档但没搬家"**（被多处按名引用，路径保持原样）：

| 文档 | 状态 | 不搬的理由 |
|---|---|---|
| [寄存式与栈式的差异清单.md](寄存式与栈式的差异清单.md) | 📜 正文首部已自标归档（2026-09-23），46 处讲"与栈式对照" | 被 11 处按名引用，且是 `待办与路线图.md` 的历史锚点 |
| [窥孔优化复盘.md](窥孔优化复盘.md) | 📜 讲的是已重写的栈式 codegen | 方法论仍被引用 |

## 八、图片与其他

| 资源 | 用途 |
|---|---|
| `docs/images/dashboard-main.png` / `-usage.png` / `-checkin.png` / `-settings.png` | 两份 `README` 里的数据看板真实截图（`leno_gui/应用/数据看板/`） |

## 九、本轮（2026-10-02）变更摘要

1. **新增本索引**；两份 README 的文档段落改为"以本索引为准 + 保留常用链接"。
2. **JIT 系 5 篇 `git mv` → `docs/archive/`**（共 8218 行 / 767KB ≈ docs 字节数的 40%），
   每篇头部加统一归档块，全仓引用（含 `src/*.c` 注释、`assert/*.leno`、两份 README）同步改路径。
3. **5 篇杂名英文改名**：`error_column_progress` → `错误列号准确性_工作进度`、
   `codegen-perf-investigation` → `代码生成性能排查记录`、`peephole-optimization-postmortem` → `窥孔优化复盘`、
   `debug-inline-segfault` → `内联器崩溃排查记录`、`destructuring_design` → `解构语法实现文档`。
4. **给 17 篇加"历史 / 局部过时"头**（正文一字不动）：凡"把栈式 / JIT 当现状""引用已删文件
   （`codegen_inline.c`、`op_*.inc`、`src/jit/*`）""写着旧版本号 / 旧断言总数"的，一眼能看到。
5. `Leno入门教程.md` 另修一处**事实错误**：原文"多返回值函数不会被内联" ⇒ 现状是
   **所有脚本函数调用都是真调用**（寄存器式 codegen 缺函数内联器，见 `待办与路线图.md` 第六节）。

## 十、为什么保持扁平（没建子目录）

`docs/*.md` 的路径被 **67 个文件**按名引用（含 `src/*.c` 注释、`assert/*.leno`、`leno_gui`、
`build/leno_module`、两份 README）。热点：`待办_单一事实来源` 28 处、`JIT实现与调试记录` 32 处、
`待办与路线图` 21 处。换成子目录/前缀要同步上百处引用，收益不抵风险 ⇒
**布局保持扁平、分类交给本索引**；`docs/archive/` 只放"整篇都是历史"的文档。
`module_*.md` 的命名约定（18 篇）也被两份 README 当规格引用，一并保持不动。
