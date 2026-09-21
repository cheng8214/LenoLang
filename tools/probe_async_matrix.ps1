# ============================================================================
# probe_async_matrix.ps1 -- async 能力面审计（不是差分对照，是"对不对"审计）
# ----------------------------------------------------------------------------
# 为什么需要它：tools\diff_vs_stack.ps1 只能发现"两侧不一样"，
#   发现不了 **两侧一致地错**（docs/寄存式与栈式的差异清单.md 的 C 类）。
#   这个脚本按 async 的能力面 / 官方文档承诺逐条跑最小用例，把**两侧的真实输出**
#   并排打出来，由人判断"对不对"。
#
# 用法：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\probe_async_matrix.ps1 [超时秒]
#
# 覆盖的case（每条都是最小复现，注释里写了"期望"）：
#   t01 Future 作形参 + await（期望：能推出结果类型）
#   t02 不 await 的协程仍执行（期望：副作用生效）—— 正常
#   t03 单个未捕获异常被 run() 收集（期望：抛该异常）—— 正常
#   t04 多个协程失败（期望：汇总成 "N 个协程失败: ..."）
#   t05 协程内嵌套 run()（文档：不支持，但应可预期）
#   t06/t07 asyncs.current()/is_done()/get_result()（文档里有这三个 API）
#   t08 async 函数作 func 形参后被 await
#   t09 struct 的 async 方法 + self 调另一个 async 方法 —— 正常
#   t11 sleep(0)/sleep(负数)
#   t12 asyncs.all([]) / all([未完成的 future])
#   t13 all([失败的 future])
#   t14 局部 async 函数定义在 if 分支里 —— 正常
#   t15 深递归 async（fib 12）—— 正常
#   t16 局部 async 函数定义在 for 循环内（值捕获）—— 正常
#   t17 async 函数经变量间接调用（var f = work; await f(3)）
#   t18 模块导出 struct 的 async 方法（跨模块）
#   t19 模块 export async func（跨模块）
# ============================================================================
param([int]$Timeout = 15)
$ErrorActionPreference = 'Continue'

$workspace = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$reg = Join-Path $workspace 'build\lenoreg.exe'
$stk = 'D:\CLeno\Leno\build\leno.exe'
$dir = Join-Path $workspace 'build\_probe_async'

if (-not (Test-Path $reg)) { Write-Output "[ERR] not found: $reg"; exit 2 }
if (-not (Test-Path $stk)) { Write-Output "[ERR] not found: $stk"; exit 2 }
New-Item -ItemType Directory -Force -Path $dir | Out-Null

# ⚠ 用双引号显式带换行：单引号 here-string 与后面拼接时行尾换行会被吃掉，
#   于是 `import io` / `import asyncs` 会粘成 `asyncsasync` 这种模块名。
$H = "import io`nimport asyncs`n"

$cases = @{}
$cases['t01_future_param'] = $H + @'
async func task(): int {
    await asyncs.sleep(1)
    return 42
}
async func wait_it(Future f): int {
    return await f
}
async func driver() {
    var f = task()
    io.print("type=", type(f))
    io.print("awaited=", await wait_it(f))
}
main() {
    driver()
    asyncs.run()
}
'@
$cases['t02_no_await_runs'] = $H + @'
var g = 0
async func task() {
    g = 7
    io.print("task ran")
}
main() {
    task()
    asyncs.run()
    io.print("g=", g)
}
'@
$cases['t03_run_collect_one'] = $H + @'
async func risky(): int {
    await asyncs.sleep(1)
    throw "boom"
}
main() {
    risky()
    try {
        asyncs.run()
        io.print("run ok")
    } catch e {
        io.print("run caught: ", e.msg)
    }
}
'@
$cases['t04_run_collect_many'] = $H + @'
async func riska(): int {
    await asyncs.sleep(1)
    throw "A"
}
async func riskb(): int {
    await asyncs.sleep(1)
    throw "B"
}
main() {
    riska()
    riskb()
    try {
        asyncs.run()
        io.print("run ok")
    } catch e {
        io.print("run caught: ", e.msg)
    }
}
'@
$cases['t05_nested_run'] = $H + @'
async func inner() {
    await asyncs.sleep(1)
    io.print("inner before nested run")
    asyncs.run()
    io.print("inner after nested run")
}
main() {
    inner()
    asyncs.run()
    io.print("main done")
}
'@
$cases['t06_api_current'] = $H + @'
main() {
    io.print("current=", asyncs.current())
}
'@
$cases['t07_api_is_done'] = $H + @'
async func task(): int {
    await asyncs.sleep(30)
    return 1
}
main() {
    var f = task()
    io.print("done0=", asyncs.is_done(f))
    asyncs.run()
    io.print("done1=", asyncs.is_done(f), " result=", asyncs.get_result(f))
}
'@
$cases['t08_async_as_func_param'] = $H + @'
async func work(int n): int {
    await asyncs.sleep(1)
    return n * 2
}
async func call_it(func cb): int {
    return await cb(7)
}
async func driver() {
    io.print("passed cb=", await call_it(work))
}
main() {
    driver()
    asyncs.run()
}
'@
$cases['t09_self_async_method'] = $H + @'
struct Machine {
    int n = 0
    async func step(): int {
        await asyncs.sleep(1)
        self.n += 1
        return self.n
    }
    async func twice(): int {
        await self.step()
        return await self.step()
    }
}
async func driver() {
    var m = new Machine()
    io.print("twice=", await m.twice(), " n=", m.n)
}
main() {
    driver()
    asyncs.run()
}
'@
$cases['t11_sleep_zero_negative'] = $H + @'
async func a() {
    await asyncs.sleep(0)
    io.print("zero ok")
}
async func b() {
    await asyncs.sleep(-5)
    io.print("negative ok")
}
main() {
    a()
    b()
    asyncs.run()
    io.print("done")
}
'@
$cases['t12_all_empty_and_pending'] = $H + @'
async func ok(): int {
    await asyncs.sleep(1)
    return 5
}
async func driver() {
    var r = asyncs.all([])
    io.print("all([]).len=", r.len())
    var f1 = ok()
    var r2 = asyncs.all([f1])
    io.print("all([pending]).len=", r2.len(), " v0=", r2[0])
}
main() {
    driver()
    asyncs.run()
}
'@
$cases['t13_all_with_failure'] = $H + @'
async func bad(): int {
    await asyncs.sleep(1)
    throw "fail"
}
async func driver() {
    var f = bad()
    try {
        var r = asyncs.all([f])
        io.print("all returned len=", r.len())
    } catch e {
        io.print("all threw: ", e.msg)
    }
}
main() {
    driver()
    asyncs.run()
}
'@
$cases['t14_local_async_in_if'] = $H + @'
async func driver(bool flag) {
    if flag {
        async func local_work(): int {
            await asyncs.sleep(1)
            return 11
        }
        io.print("in-if local=", await local_work())
    } else {
        io.print("else branch")
    }
}
main() {
    driver(true)
    asyncs.run()
}
'@
$cases['t15_deep_recursion'] = $H + @'
async func fib(int n): int {
    if n < 2 {
        return n
    }
    var a = await fib(n - 1)
    var b = await fib(n - 2)
    return a + b
}
async func driver() {
    io.print("fib(12)=", await fib(12))
}
main() {
    driver()
    asyncs.run()
}
'@
$cases['t16_local_async_in_loop'] = $H + @'
async func driver() {
    for 2 to i {
        async func w(): int {
            await asyncs.sleep(1)
            return i * 10
        }
        io.print("loop ", i, " -> ", await w())
    }
}
main() {
    driver()
    asyncs.run()
    io.print("done")
}
'@
$cases['t17_async_via_variable'] = $H + @'
async func work(int n): int {
    await asyncs.sleep(1)
    return n * 2
}
async func driver() {
    var f = work
    io.print("value call=", await f(3))
}
main() {
    driver()
    asyncs.run()
}
'@
$cases['t18_module_struct_async_method'] = $H + @'
import "t18_mod.leno" as m
use m.Worker
async func driver() {
    var w = new Worker()
    w.n = 41
    io.print("cross-module async method=", await w.run())
}
main() {
    driver()
    asyncs.run()
}
'@
$cases['t19_export_async_func'] = $H + @'
import "t19_mod.leno" as m
async func driver() {
    io.print("export async=", await m.work())
}
main() {
    driver()
    asyncs.run()
}
'@

# 被上面两个 case 引用的模块文件
$mods = @{}
$mods['t18_mod.leno'] = @'
import asyncs
export struct Worker {
    int n = 0
    async func run(): int {
        await asyncs.sleep(1)
        return self.n + 1
    }
}
'@
$mods['t19_mod.leno'] = @'
import asyncs
export async func work(): int {
    await asyncs.sleep(1)
    return 7
}
'@

function Run-One([string]$exe, [string]$file, [int]$timeoutSec) {
    $tag = [System.IO.Path]::GetRandomFileName()
    $out = Join-Path $env:TEMP "$tag.out"
    $err = Join-Path $env:TEMP "$tag.err"
    $p = Start-Process -FilePath $exe -ArgumentList @('--no-cache', $file) -PassThru -NoNewWindow `
                       -RedirectStandardOutput $out -RedirectStandardError $err
    $done = $p.WaitForExit($timeoutSec * 1000)
    if (-not $done) { try { $p.Kill() } catch {}; Start-Sleep -Milliseconds 150 }
    $t = ''
    if (Test-Path $out) { $t += (Get-Content -Encoding UTF8 $out -Raw) }
    if (Test-Path $err) { $t += (Get-Content -Encoding UTF8 $err -Raw) }
    Remove-Item $out, $err -Force -ErrorAction SilentlyContinue
    $code = if ($done) { $p.ExitCode } else { 'TIMEOUT' }
    return @{ text = $t; code = $code }
}

Get-ChildItem $dir -File -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
foreach ($k in $mods.Keys) { Set-Content -Encoding UTF8 (Join-Path $dir $k) $mods[$k] }

Write-Output ("reg: {0}" -f $reg)
Write-Output ("stk: {0}" -f $stk)
Write-Output ("probes: {0}   timeout: {1}s" -f $cases.Count, $Timeout)
Write-Output '========================================================================'
foreach ($k in ($cases.Keys | Sort-Object)) {
    $f = Join-Path $dir ("$k.leno")
    Set-Content -Encoding UTF8 $f $cases[$k]
    $r = Run-One $reg $f $Timeout
    $s = Run-One $stk $f $Timeout
    $rt = ($r.text -replace '\s+', ' ').Trim()
    $st = ($s.text -replace '\s+', ' ').Trim()
    if ($rt.Length -gt 200) { $rt = $rt.Substring(0, 200) + ' ...' }
    if ($st.Length -gt 200) { $st = $st.Substring(0, 200) + ' ...' }
    $tag = if ($rt -eq $st) { 'SAME' } else { 'DIFF' }
    Write-Output ("===== {0}   {1}" -f $k, $tag)
    Write-Output ("  reg[{0}] {1}" -f $r.code, $rt)
    Write-Output ("  stk[{0}] {1}" -f $s.code, $st)
}
Write-Output '========================================================================'
Write-Output ("probe files: {0}" -f $dir)
