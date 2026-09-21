# ============================================================================
# diff_vs_stack.ps1 -- 寄存器式(LenoReg) vs 栈式(Leno) 差分对照
# ----------------------------------------------------------------------------
# 同一份 .leno 两个解释器各跑一遍、逐行比对，把"哪一侧结果不对"直接暴露出来
# （配套文档：docs/寄存式与栈式的差异清单.md）。
#
# 用法（PowerShell）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\diff_vs_stack.ps1 "<目标>" [超时秒数] [-Reg <exe>] [-Stk <exe>]
#
#   <目标> 可以是目录（递归所有 *.leno）或单个 .leno 文件。
#   例：
#     ... -File tools\diff_vs_stack.ps1 "examples/async await"
#     ... -File tools\diff_vs_stack.ps1 examples\minilang\minilang.leno 20
#
# 说明：
#   * 每个样例有**超时保护**（默认 20s）—— 协程/定时器样例可能挂住；
#     两个解释器都超时的不算差异，会单独标记 TIMEOUT。
#   * 输出先做**噪声归一化**：指针地址 / 耗时(ms,s) / 时间戳 / 空行，
#     避免把随机值当成差异。
#   * 退出码 = 差异个数（0 = 全一致），方便挂到自动化里。
#   * 明细写入 build\_diff_report.txt。
# ============================================================================
param(
    [Parameter(Mandatory = $true)][string]$Target,
    [int]$Timeout = 20,
    [string]$Reg = '',
    [string]$Stk = ''
)
$ErrorActionPreference = 'Continue'

$workspace = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $Reg) { $Reg = Join-Path $workspace 'build\lenoreg.exe' }
if (-not $Stk) { $Stk = 'D:\CLeno\Leno\build\leno.exe' }
$reportPath = Join-Path $workspace 'build\_diff_report.txt'

if (-not (Test-Path $Reg)) { Write-Output "[ERR] register exe not found: $Reg"; exit 2 }
if (-not (Test-Path $Stk)) { Write-Output "[ERR] stack exe not found: $Stk";    exit 2 }
if (-not (Test-Path $Target)) { Write-Output "[ERR] target not found: $Target"; exit 2 }

function Run-WithTimeout([string]$exe, [string]$file, [int]$timeoutSec) {
    $tag = [System.IO.Path]::GetRandomFileName()
    $out = Join-Path $env:TEMP "$tag.out"
    $err = Join-Path $env:TEMP "$tag.err"
    $p = Start-Process -FilePath $exe -ArgumentList @('--no-cache', $file) -PassThru -NoNewWindow `
                       -RedirectStandardOutput $out -RedirectStandardError $err
    $done = $p.WaitForExit($timeoutSec * 1000)
    if (-not $done) { try { $p.Kill() } catch {}; Start-Sleep -Milliseconds 200 }
    $text = ''
    if (Test-Path $out) { $text += (Get-Content -Encoding UTF8 $out -Raw) }
    if (Test-Path $err) { $text += (Get-Content -Encoding UTF8 $err -Raw) }
    Remove-Item $out, $err -Force -ErrorAction SilentlyContinue
    return @{ text = $text; timeout = (-not $done) }
}

function Norm([string]$s) {
    if (-not $s) { return '' }
    $s = $s -replace '\r', ''
    $s = $s -replace '0x[0-9a-fA-F]+', '0xPTR'                      # 指针地址
    $s = $s -replace '[0-9]+\s*ms', 'T'                             # 毫秒
    $s = $s -replace '\d{4}-\d{2}-\d{2}[ T]\d{2}:\d{2}:\d{2}(\.\d+)?', 'TS'  # 时间戳
    # ⚠ 本文件必须**保持 ASCII**：PowerShell 读脚本走 GBK，脚本里的中文字面量会被读坏，
    #   正则就永远匹配不上（第一版就是这么写的，PID/内存行仍然逐个报差异）。
    $s = $s -replace 'PID\s*[:=]\s*\d+', 'PID=N'                    # 进程号（每次运行都不同）
    $s = $s -replace 'ID\s*[:=]\s*\d+', 'ID: N'                     # 「当前进程ID: 1234」等
    $s = $s -replace '\d+\s*MB', 'N MB'                             # 内存波动
    $s = $s -replace '\d+\s*KB', 'N KB'
    $s = $s -replace '<ptr [0-9A-Fa-f]+>', '<ptr P>'                # 指针（形如 <ptr 000001B8...>）
    $s = $s -replace '\d+(\.\d+)?\s*n[sm]\b', 'T'                    # ns / us 级耗时
    $s = $s -replace '\(\d+(\.\d+)?[^)]*\)', '(T)'                   # 「耗时=T (4.93s)」里的秒数
    $s = $s -replace '\d+\.\d{6,}', 'F'                             # 高精度浮点（计时/时间戳类输出）
    $s = $s -replace '[A-Za-z]:\\[^\s"]*\\leno(reg)?\.exe', 'leno.exe'  # 可执行文件全路径
    $s = $s -replace 'lenoreg\.exe', 'leno.exe'                     # 可执行名不同（分析对象本身）
    $s = $s -replace '(?m)^\s*$', ''                                # 空行
    return $s.Trim()
}

$files = @()
if ((Get-Item $Target).PSIsContainer) {
    $files = Get-ChildItem -Path $Target -Filter *.leno -File -Recurse | Sort-Object FullName
} else {
    $files = @(Get-Item $Target)
}
if ($files.Count -eq 0) { Write-Output "[ERR] no .leno found under $Target"; exit 2 }

Write-Output ("target: {0}" -f $Target)
Write-Output ("samples: {0}   timeout: {1}s" -f $files.Count, $Timeout)
Write-Output ("reg: {0}" -f $Reg)
Write-Output ("stk: {0}" -f $Stk)
Write-Output '------------------------------------------------------------------------'

$report = @()
$diffCount = 0
foreach ($f in $files) {
    $r = Run-WithTimeout $Reg $f.FullName $Timeout
    $s = Run-WithTimeout $Stk $f.FullName $Timeout
    $rn = Norm $r.text
    $sn = Norm $s.text

    $status = 'OK'
    if ($rn -eq $sn -and -not $r.timeout -and -not $s.timeout) {
        $status = 'OK'
    } elseif ($r.timeout -and $s.timeout) {
        $status = 'BOTH-TIMEOUT'
    } elseif ($r.timeout -or $s.timeout) {
        $status = 'TIMEOUT'
    } else {
        $status = 'DIFF'
    }
    if ($status -eq 'DIFF') { $diffCount++ }

    Write-Output ("[{0}] {1}" -f $status, $f.Name)
    if ($status -ne 'OK') {
        $report += ("===== {0} : {1} =====" -f $status, $f.FullName)
        if ($status -eq 'TIMEOUT' -or $status -eq 'BOTH-TIMEOUT') {
            $report += ("  reg timeout={0} chars={1} | stk timeout={2} chars={3}" -f `
                        $r.timeout, $rn.Length, $s.timeout, $sn.Length)
        }
        $rl = $rn -split "`n"
        $sl = $sn -split "`n"
        $max = [Math]::Max($rl.Count, $sl.Count)
        $shown = 0
        for ($i = 0; $i -lt $max -and $shown -lt 20; $i++) {
            $x = if ($i -lt $rl.Count) { $rl[$i] } else { '<none>' }
            $y = if ($i -lt $sl.Count) { $sl[$i] } else { '<none>' }
            if ($x -ne $y) {
                $report += ("  L{0}: reg=|{1}|  stk=|{2}|" -f ($i + 1), $x, $y)
                $shown++
            }
        }
        $report += '  --- reg full ---'
        $report += ($rl | ForEach-Object { '  R| ' + $_ })
        $report += '  --- stk full ---'
        $report += ($sl | ForEach-Object { '  S| ' + $_ })
    }
}
$report | Set-Content -Encoding UTF8 $reportPath
Write-Output '------------------------------------------------------------------------'
Write-Output ("diffs: {0}   report -> {1}" -f $diffCount, $reportPath)
exit $diffCount
