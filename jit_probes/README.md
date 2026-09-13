# jit_probes —— JIT 性能探针与差分工具

用途：给 JIT 改动做**同机同轮 A/B 对照**，回答两个问题 ——「改动真的生效了吗」、
「快了多少」。这里的文件都是探针，**不参与编译器构建**；`.leno` 直接用
`build/lenojit.exe` 跑即可。

## 文件清单

| 文件 | 测什么 |
| --- | --- |
| `probe_method.leno` | 方法调用成本。同一套算术分别写成「循环内手工展开」/「最简方法」/「中等方法体」/「完整 hitSingle 形态」四段，用来定位调用机制（callout 装箱 + 函数级 JIT 序言）的固定开销。1000 万次。 |
| `probe_alloc.leno` | 分配成本随**累计分配量**的阶梯（微秒计时，1M/10M 混合），用来判断单次成本是否随堆增长劣化、拐点在哪。 |
| `probe_alloc2.leno` | 分配路径隔离。四个循环体算术量级一致，唯一变量是「分配什么」：`new 2 字段` / `new 6 字段` / 数组字面量 `[1.0,2.0]` / 不分配。全部直接写在循环体里，排除方法调用干扰。 |
| `diff_examples.bat` | 基线二进制 vs 新版二进制，对 `examples/{struct,func,module_export_struct,cstruct}` 的示例做 stdout + 退出码差分（约 59 个文件）。 |

## 怎么跑

```bat
build\lenojit.exe jit_probes\probe_method.leno
build\lenojit.exe jit_probes\probe_alloc.leno
build\lenojit.exe jit_probes\probe_alloc2.leno
jit_probes\diff_examples.bat          REM 在仓库根目录执行
```

## A/B 对照的标准流程

1. **造基线二进制**（改动前的版本，供 A/B 用）：

   ```bat
   git stash push -m probe-baseline -- <你改动的文件>
   build.bat
   copy /y build\lenojit.exe build\lenojit_base.exe
   git stash pop
   build.bat
   ```

2. **交替**跑基线与新版（不要先跑完 3 轮 A 再跑 3 轮 B），各 3 轮取**最小值**。
   本机（i5-3450）负载波动可达 10~20%，取 min 比取平均更能反映真实成本。

3. **分离单项改动**：用环境开关只关掉其中一项（例：`LENO_JIT_NOINLINE=1` 只关闭
   内联，其它改动保留），于是「基线 / 只 A / A+B」三方对照能把每项贡献拆干净。

4. **差分验证语义**：跑 `diff_examples.bat`，并在 `.leno` 基准里比对**输出值本身**
   （如 `sum=` / `hits=`）。输出值比耗时更可信 —— 措辞、顺序、数值都能暴露静默算错。

## 已知坑（都是踩过的）

- **`times.ms()` 只有毫秒粒度**：小规模（≤2 万次）测出来直接是 `0ms`，要用 `times.us()`。
- **假差异**：输出里含 `<ptr 0x...>`（ASLR 地址）或计时数字的行必然不同。判定回归前，
  先用**同一个二进制跑两遍**自查该文件本身是否确定。
- **「改了但没效果」先怀疑快路径没进去**：用 `LENO_JIT_DEBUG=1` 看
  `Compiled / Executed / Bailouts` 计数与 `inline(...)` 日志；只看耗时容易把
  「快路径是死代码」误判成「这类操作没有优化空间」。
- **绝对值不要跨机比较**：本机为 **Intel i5-3450**（Ivy Bridge 4C/4T）+ Windows，
  见 `docs/JIT实现与调试记录.md` §9 测试环境；换机器后需要重建基线。
- **别把被测程序直接接进管道**再取退出码或比对输出：`prog 2>&1 | findstr ... > f`
  会让 `ERRORLEVEL` 变成 `findstr` 的退出码，长行还可能被 `findstr` 截断/吞掉，
  表现为**随机假差异**（`diff_examples.bat` 踩过：先落盘存原始输出、再过滤，
  退出码也在加过滤之前取）。

## 相关记录

- **§8.30**：struct 字段读 / 方法调用内联（提交 `c4b6bbd1`）——`probe_method.leno` 的实测表。
- **§8.31**：分配成本归因（**代码未改动**，待办按风险分三级）——`probe_alloc*.leno` 的实测表。
- **§12 未解决问题 第 10 条**：对象分配 ~200ns/次 + JIT 循环内无 GC 安全点。
