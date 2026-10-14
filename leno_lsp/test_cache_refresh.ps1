# leno_lsp/test_cache_refresh.ps1 —— LSP「改完被 import 的模块，波浪线要立刻更新」自检
#
# 为什么单独立它（2026-10-14 实录）：LSP 是**长驻进程**，而编译器里有两处**进程内缓存**
#   （`g_sym_memo` 符号表记忆化 + `g_ast_export_cache` 导出名清单），它们最初都是按"一次性进程"
#   设计的 ⇒ 在 LSP 里会复用旧数据，症状就是：
#     「改了被 import 的模块并保存，依赖它的那个文件**波浪线还是旧错** ✗」
#   这个修复**跑不进 assert 套件** —— 套件跑的是 CLI（一次性进程），
#   复现不出"同一进程内、跨请求、中间改文件"这个场景 ⇒ 单独立这个脚本 ✓
#
# 用法：  pwsh -File leno_lsp/test_cache_refresh.ps1 [leno_lsp.exe 路径]
#         （默认取 <本脚本目录>/build/leno_lsp.exe）
# 判据：给 a.leno 加上 world() 之后，b.leno 的诊断数**必须下降**（1 → 0）。
#       仍为 1 ⇒ 缓存没失效（回归）⇒ 退出码 1 ✗
#       另：若 LSP 因被 IDE 占用而无法写 build/leno_lsp.exe，请先关闭编辑器（本脚本只**运行**它 ✓）

param([string]$Exe = (Join-Path $PSScriptRoot "build\leno_lsp.exe"))

if (-not (Test-Path $Exe)) { Write-Host "找不到 LSP 可执行文件: $Exe" -ForegroundColor Red; exit 2 }

$dir = Join-Path ([System.IO.Path]::GetTempPath()) "leno_lsp_cache_refresh_probe"
Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $dir | Out-Null

$a = Join-Path $dir "a.leno"
$b = Join-Path $dir "b.leno"
# a 的第一版：只有 hello（b 调用的 world 还不存在 ⇒ 第一次诊断应当报 1 个错）
$aV1 = "export func hello(): int {`n    return 1`n}`n"
# a 的第二版：补上 world ⇒ 改完之后 b 应当**不再报错**
$aV2 = "export func hello(): int {`n    return 1`n}`nexport func world(): int {`n    return 2`n}`n"
$bSrc = "import `"a.leno`" as a`n`nmain() {`n    print(a.world())`n}`n"

Set-Content -Path $a -Value $aV1 -NoNewline -Encoding UTF8
Set-Content -Path $b -Value $bSrc -NoNewline -Encoding UTF8

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.UseShellExecute = $false
$p = [System.Diagnostics.Process]::Start($psi)

function Send-Lsp([string]$m) {
    $p.StandardInput.Write("Content-Length: " + [System.Text.Encoding]::UTF8.GetByteCount($m) + "`r`n`r`n" + $m)
    $p.StandardInput.Flush()
}
# 等一次诊断完成：LSP 每发一次诊断都会在 stderr 打 `[DIAG] publish END count=N` ✓
#   （用这个做同步点，就不必自己去解 stdout 的 LSP 帧）
function Wait-Diag {
    $g = 0
    while ($g -lt 5000) {
        $g++
        $l = $p.StandardError.ReadLine()
        if ($null -eq $l) { return -1 }
        if ($l -match "publish END count=(\d+)") { return [int]$Matches[1] }
    }
    return -2
}

$uriB = "file:///" + ($b -replace '\\', '/' -replace ' ', '%20')
$rootUri = "file:///" + ($dir -replace '\\', '/' -replace ' ', '%20')
$bJson = $bSrc.Replace('\', '\\').Replace('"', '\"').Replace("`r`n", '\n').Replace("`n", '\n')

Send-Lsp ('{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":"' + $rootUri + '","capabilities":{}}}')
Send-Lsp '{"jsonrpc":"2.0","method":"initialized","params":{}}'
Send-Lsp ('{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"' + $uriB + '","languageId":"leno","version":1,"text":"' + $bJson + '"}}}')
$n1 = Wait-Diag

# 关键动作：**改被 import 的模块**（b 的内容一字未动 ⇒ 考的就是"依赖变了要不要重读"）
Set-Content -Path $a -Value $aV2 -NoNewline -Encoding UTF8

Send-Lsp ('{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"' + $uriB + '","version":2},"contentChanges":[{"text":"' + $bJson + '"}]}}')
$n2 = Wait-Diag

try { $p.Kill() } catch { }
Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue

Write-Host ("改依赖前 count={0} ; 给 a.leno 补上 world() 之后 count={1}" -f $n1, $n2)
if ($n1 -eq 1 -and $n2 -eq 0) {
    Write-Host "OK：依赖改动已即时反映到诊断 ✓" -ForegroundColor Green
    exit 0
}
Write-Host "FAIL：期望 1 → 0。若 n2 仍为 1 ⇒ 进程内缓存没失效（回归）；若 n1 不是 1 ⇒ 诊断链路本身有问题" -ForegroundColor Red
exit 1
