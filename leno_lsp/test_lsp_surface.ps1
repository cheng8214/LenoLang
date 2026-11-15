# leno_lsp/test_lsp_surface.ps1 — LSP「表面能力」自检（ASCII 输出，PS 5.1 安全）
#
# 为什么需要它：LSP 是长驻进程，功能分散在 28 个 .c 里，之前**一条断言都没有**。
#   2026-11-15 实际发现的问题全是"静默漏掉"：
#     · 补全表里没有 defer / const / union / thread_local / extend（语言早就有）
#     · hover 没有 defer / thread_local / union，enum 文档还用逗号分隔成员（编译期直接报错）
#     · 大纲漏 `main()`（入口函数不写 func）、漏 `thread_local var`、漏 `union`；
#       `const int LIMIT` 被记成名字叫 "int"；带载荷 enum 的参数被当成成员（假的 int/total）
#     · `Msg.` 成员补全列出 tag/p0/p1（**编译器实现细节**）而不是 progress/finished
#     · 内置函数补全只有 10/37 个下划线全局内置
#
# 用法： powershell -NoProfile -ExecutionPolicy Bypass -File leno_lsp/test_lsp_surface.ps1 [leno_lsp.exe]
# 判据： 每行 OK / 任一 MISS ⇒ 退出码 1
param([string]$Exe = (Join-Path $PSScriptRoot "build\leno_lsp.exe"))
if (-not (Test-Path $Exe)) { Write-Host "LSP exe not found: $Exe" -ForegroundColor Red; exit 2 }

$dir = Join-Path $env:TEMP "leno_lsp_surface_probe"
Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $dir | Out-Null

function Invoke-LspProbe([string]$src, [string[]]$extraReqs) {
    $file = Join-Path $dir ("p_" + [guid]::NewGuid().ToString("N") + ".leno")
    [System.IO.File]::WriteAllText($file, $src, (New-Object System.Text.UTF8Encoding($false)))
    $uri = "file:///" + ($file -replace '\\', '/' -replace ' ', '%20')
    $root = "file:///" + ($dir -replace '\\', '/' -replace ' ', '%20')
    $json = $src -replace '\\', '\\' -replace '"', '\"' -replace "`n", '\n'

    $msgs = @()
    $msgs += '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":"' + $root + '","capabilities":{}}}'
    $msgs += '{"jsonrpc":"2.0","method":"initialized","params":{}}'
    $msgs += '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"' + $uri + '","languageId":"leno","version":1,"text":"' + $json + '"}}}'
    foreach ($r in $extraReqs) { $msgs += ($r -replace '@URI@', $uri) }
    $msgs += '{"jsonrpc":"2.0","id":99,"method":"shutdown","params":null}'
    $msgs += '{"jsonrpc":"2.0","method":"exit","params":null}'

    $enc = New-Object System.Text.UTF8Encoding($false)
    $sb = New-Object System.Text.StringBuilder
    foreach ($m in $msgs) {
        [void]$sb.Append("Content-Length: " + $enc.GetByteCount($m) + "`r`n`r`n")
        [void]$sb.Append($m)
    }
    $req = Join-Path $dir "req.txt"
    $resp = Join-Path $dir "resp.txt"
    $errf = Join-Path $dir "err.txt"
    [System.IO.File]::WriteAllText($req, $sb.ToString(), $enc)
    $proc = Start-Process -FilePath $Exe -RedirectStandardInput $req -RedirectStandardOutput $resp -RedirectStandardError $errf -PassThru -NoNewWindow
    if (-not $proc.WaitForExit(15000)) { $proc.Kill() }
    if (Test-Path $resp) { return [System.IO.File]::ReadAllText($resp) } else { return "" }
}

$script:fail = 0
function Check([string]$name, [bool]$ok) {
    if ($ok) { Write-Host ("OK   " + $name) -ForegroundColor Green }
    else { Write-Host ("MISS " + $name) -ForegroundColor Red; $script:fail++ }
}

# ---------- 探针文件：新语法全家桶 ----------
#   行号**按内容算**，不写死：`$lines` 里加/删一行不会让判据错位
$lines = @(
    'import ffi',
    'thread_local var hits = 0',
    'const int LIMIT = 3',
    'enum Msg {',
    '    progress(int done, int total)',
    '    finished(string path)',
    '}',
    'union Num {',
    '    i32 i',
    '    f32 f',
    '}',
    'cstruct Flags {',
    '    u32 visible : 1',
    '    u32 depth : 5',
    '}',
    'func handle(Msg m): int {',
    '    switch m {',
    '        case Msg.progress(d, t) { return d + t }',
    '        case Msg.finished(p)    { return p.len() }',
    '    }',
    '    return 0',
    '}',
    'main() {',
    '    var p = ffi.malloc(8)',
    '    defer ffi.free(p)',
    '    var m = Msg.progress(1, 2)',
    '    var n = Msg.',
    '    print(_str(handle(m)))',
    '}'
)
function LineOf([string]$pat) {
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match $pat) { return $i }
    }
    return -1
}
function ColOf([string]$pat, [string]$sub) {
    $i = LineOf $pat
    if ($i -lt 0) { return 0 }
    $c = $lines[$i].IndexOf($sub)
    if ($c -lt 0) { return 0 }
    return $c + $sub.Length
}

# ❶ 完整合法的文件（不含未写完的 `Msg.`）⇒ 必须零 error 诊断
$valid = (($lines | Where-Object { $_ -notmatch 'var n = Msg\.' }) -join "`n") + "`n"
$outValid = Invoke-LspProbe $valid @()
Check "no error diagnostics on new syntax" (([regex]::Matches($outValid, '"severity":1')).Count -eq 0)

# ❷ 带 `Msg.` 的文件：补全 / hover / 大纲
$src = ($lines -join "`n") + "`n"
$reqs = @(
    ('{"jsonrpc":"2.0","id":2,"method":"textDocument/completion","params":{"textDocument":{"uri":"@URI@"},"position":{"line":' + (LineOf 'var m = Msg\.progress') + ',"character":30}}}'),
    ('{"jsonrpc":"2.0","id":3,"method":"textDocument/completion","params":{"textDocument":{"uri":"@URI@"},"position":{"line":' + (LineOf 'var n = Msg\.') + ',"character":' + (ColOf 'var n = Msg\.' '.') + '}}}'),
    ('{"jsonrpc":"2.0","id":4,"method":"textDocument/hover","params":{"textDocument":{"uri":"@URI@"},"position":{"line":' + (LineOf 'defer ffi\.free') + ',"character":6}}}'),
    ('{"jsonrpc":"2.0","id":5,"method":"textDocument/hover","params":{"textDocument":{"uri":"@URI@"},"position":{"line":' + (LineOf '^enum Msg \{') + ',"character":2}}}'),
    ('{"jsonrpc":"2.0","id":6,"method":"textDocument/hover","params":{"textDocument":{"uri":"@URI@"},"position":{"line":' + (LineOf '^thread_local var') + ',"character":6}}}'),
    ('{"jsonrpc":"2.0","id":7,"method":"textDocument/hover","params":{"textDocument":{"uri":"@URI@"},"position":{"line":' + (LineOf '^union Num \{') + ',"character":2}}}'),
    '{"jsonrpc":"2.0","id":8,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"@URI@"}}}'
)
$out = Invoke-LspProbe $src $reqs

Check "completion: defer"        ($out.Contains('"label":"defer"'))
Check "completion: const"        ($out.Contains('"label":"const"'))
Check "completion: union"        ($out.Contains('"label":"union"'))
Check "completion: thread_local" ($out.Contains('"label":"thread_local"'))
Check "completion: extend"       ($out.Contains('"label":"extend"'))
Check "hover: defer"             ($out.Contains('**defer**'))
Check "hover: enum payload doc"  ($out.Contains('progress(int done, int total)'))
Check "hover: thread_local"      ($out.Contains('**thread_local**'))
Check "hover: union"             ($out.Contains('**union**'))
Check "outline: thread_local var" ($out.Contains('"name":"hits","kind":13'))
Check "outline: const name"      ($out.Contains('"name":"LIMIT"'))
Check "outline: union type"      ($out.Contains('"name":"Num"'))
Check "outline: main()"          ($out.Contains('"name":"main"'))
Check "outline: payload members" ($out.Contains('"name":"progress"') -and -not $out.Contains('"name":"total"'))
Check "Msg. lists members"       ($out.Contains('"label":"progress"') -and $out.Contains('"label":"finished"'))
Check "Msg. hides p0/tag"        (-not $out.Contains('"label":"p0"') -and -not $out.Contains('"label":"tag"'))

# ---------- 内置函数补全（37 个下划线全局内置里最容易漏的几个）----------
$src2 = "main() {`n    var x = _int_or(`n}`n"
$out2 = Invoke-LspProbe $src2 @('{"jsonrpc":"2.0","id":2,"method":"textDocument/completion","params":{"textDocument":{"uri":"@URI@"},"position":{"line":1,"character":19}}}')
Check "builtin: _int_or" ($out2.Contains('"label":"_int_or"'))
Check "builtin: _exec"   ($out2.Contains('"label":"_exec"'))
Check "builtin: _spawn"  ($out2.Contains('"label":"_spawn"'))
Check "builtin: _pid"    ($out2.Contains('"label":"_pid"'))

if ($script:fail -eq 0) { Write-Host "`nALL LSP SURFACE CHECKS PASSED" -ForegroundColor Green; exit 0 }
Write-Host "`n$($script:fail) check(s) FAILED" -ForegroundColor Red
exit 1
