# 诊断质量压测：人类常见错误写法 ⇒ 编译器该报什么
# 判据：expect = error / warn / ok；实际取退出码 + 第一条诊断
$ErrorActionPreference = "Continue"
$leno = "d:\CLeno\Leno\build\leno.exe"
$dir  = Join-Path $env:TEMP "leno_diag"
if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
New-Item -ItemType Directory -Path $dir | Out-Null

$cases = @()

# ==================== struct ====================
$cases += @{ n = "s01 字段缺名字"; e = "error"; src = @'
struct S { int }
main() { print("x") }
'@ }
$cases += @{ n = "s02 重复字段名"; e = "error"; src = @'
struct S { int a = 0, int a = 1 }
main() { print("x") }
'@ }
$cases += @{ n = "s03 字段类型未定义"; e = "error"; src = @'
struct S { Nope x = null }
main() { print("x") }
'@ }
$cases += @{ n = "s04 字段默认值类型不符"; e = "error"; src = @'
struct S { int x = "hi" }
main() { print("x") }
'@ }
$cases += @{ n = "s05 字段未初始化（警告）"; e = "warn"; src = @'
struct S { int x }
main() { S s = new S(); print(_str(s.x)) }
'@ }
$cases += @{ n = "s06 new 时字段名拼错"; e = "error"; src = @'
struct S { int x = 0 }
main() { S s = new S(nope = 1); print(_str(s.x)) }
'@ }
$cases += @{ n = "s07 访问不存在的字段"; e = "error"; src = @'
struct S { int x = 0 }
main() { S s = new S(); print(_str(s.nope)) }
'@ }
$cases += @{ n = "s08 对 null 取字段（运行期报错即可）"; e = "ok"; src = @'
struct S { int x = 0 }
main() { S? s = null; print(_str(s.x)) }
'@ }
$cases += @{ n = "s09 方法缺返回类型（合法：void）"; e = "ok"; src = @'
struct S { int x = 0
    func go() { print("x") }
}
main() { print("y") }
'@ }
$cases += @{ n = "s10 struct 当值用（没 new）"; e = "error"; src = @'
struct S { int x = 0 }
main() { S s = S; print("x") }
'@ }

# ==================== enum ====================
$cases += @{ n = "e01 空 enum（2026-11-10 起告警）"; e = "warn"; src = @'
enum E { }
main() { print("x") }
'@ }
$cases += @{ n = "e01b enum 泛型参数（载荷 enum 不支持泛型）"; e = "error"; src = @'
enum R[T] { ok(T v), err(int c) }
main() { print("x") }
'@ }
$cases += @{ n = "e02 重复成员"; e = "error"; src = @'
enum E { a, a }
main() { print("x") }
'@ }
$cases += @{ n = "e03 引用不存在的成员"; e = "error"; src = @'
enum E { a, b }
main() { E v = E.nope; print(_str(v)) }
'@ }
$cases += @{ n = "e04 裸写成员名"; e = "error"; src = @'
enum E { a, b }
main() { E v = a; print(_str(v)) }
'@ }
$cases += @{ n = "e05 成员值不是常量"; e = "error"; src = @'
var g = 3
enum E { a = g }
main() { print("x") }
'@ }
$cases += @{ n = "e06 混用带载荷/不带载荷"; e = "error"; src = @'
enum E { a(int x), b }
main() { print("x") }
'@ }
$cases += @{ n = "e07 载荷参数类型未定义"; e = "error"; src = @'
enum E { a(Nope x) }
main() { print("x") }
'@ }
$cases += @{ n = "e08 带载荷 enum 与同名 struct 撞车"; e = "error"; src = @'
struct E { int x = 0 }
enum E { a(int v) }
main() { print("x") }
'@ }
$cases += @{ n = "e09 new 直接构造带载荷 enum"; e = "error"; src = @'
enum E { a(int v) }
main() { E e = new E(tag = 0); print(_str(e.tag)) }
'@ }
$cases += @{ n = "e10 裸写带载荷成员（不带括号）"; e = "error"; src = @'
enum E { a(int v) }
main() { E e = E.a; print("x") }
'@ }
$cases += @{ n = "e11 构造参数个数不符"; e = "error"; src = @'
enum E { a(int v) }
main() { E e = E.a(1, 2); print("x") }
'@ }
$cases += @{ n = "e12 带载荷 enum 写在函数里"; e = "error"; src = @'
main() {
    enum E { a(int v) }
    print("x")
}
'@ }
$cases += @{ n = "e13 普通 enum 成员超 255"; e = "error"; src = "enum E {`n" + (1..260 | ForEach-Object { "    m$_" } | Join-String -Separator ",`n") + "`n}`nmain() { print(`"x`") }" }
$cases += @{ n = "e14 case 用别的 enum 的成员"; e = "error"; src = @'
enum A { x, y }
enum B { x, y }
main() { A v = A.x
    switch v {
        case B.x { print("b") }
        default { print("d") }
    }
}
'@ }

# ==================== switch ====================
$cases += @{ n = "w01 普通 enum 漏 case（警告）"; e = "warn"; src = @'
enum E { a, b, c }
main() { E v = E.a
    switch v {
        case E.a { print("a") }
    }
}
'@ }
$cases += @{ n = "w02 带载荷 enum 漏成员（警告）"; e = "warn"; src = @'
enum E { a(int x), b(string s), c() }
main() { E v = E.c()
    switch v {
        case E.a(x) { print("a") }
    }
}
'@ }
$cases += @{ n = "w03 重复 case（警告）"; e = "warn"; src = @'
main() { int v = 1
    switch v {
        case 1 { print("1") }
        case 1 { print("again") }
        default { print("d") }
    }
}
'@ }
$cases += @{ n = "w04 case 值类型与主题不符（警告）"; e = "warn"; src = @'
main() { string v = "hi"
    switch v {
        case 1 { print("int") }
        case "hi" { print("str") }
        default { print("d") }
    }
}
'@ }
$cases += @{ n = "w05 case 绑定写了表达式"; e = "error"; src = @'
enum E { a(int x) }
main() { E e = E.a(1)
    switch e {
        case E.a(1 + 2) { print("x") }
    }
}
'@ }
$cases += @{ n = "w06 case 绑定个数不符"; e = "error"; src = @'
enum E { a(int x) }
main() { E e = E.a(1)
    switch e {
        case E.a(x, y) { print("x") }
    }
}
'@ }
$cases += @{ n = "w07 同一 case 写多个模式"; e = "error"; src = @'
enum E { a(int x), b(int y) }
main() { E e = E.a(1)
    switch e {
        case E.a(x), E.b(y) { print("x") }
    }
}
'@ }
$cases += @{ n = "w08 switch 主题是构造表达式"; e = "error"; src = @'
enum E { a(int x) }
main() {
    switch E.a(1) {
        case E.a(x) { print("x") }
    }
}
'@ }
$cases += @{ n = "w09 switch 少右括号"; e = "error"; src = @'
main() { int v = 1
    switch v {
        case 1 { print("1") }
}
'@ }
$cases += @{ n = "w10 default 写两次"; e = "error"; src = @'
main() { int v = 1
    switch v {
        case 1 { print("1") }
        default { print("d1") }
        default { print("d2") }
    }
}
'@ }
$cases += @{ n = "w11 case 缺冒号/值"; e = "error"; src = @'
main() { int v = 1
    switch v {
        case { print("1") }
        default { print("d") }
    }
}
'@ }

# ==================== 跑 ====================
$rows = @()
$i = 0
foreach ($c in $cases) {
    $i++
    $f = Join-Path $dir ("c{0:d2}.leno" -f $i)
    Set-Content -Path $f -Value $c.src -Encoding utf8
    $out = & $leno --check $f 2>&1 | Out-String
    $code = $LASTEXITCODE

    $kind = "ok"
    $msg = ""
    $line = ""
    if ($out -match 'error:') { $kind = "error" }
    elseif ($out -match 'warning:') { $kind = "warn" }

    # 第一条诊断（优先 error）
    $hit = ($out -split "`r?`n") | Where-Object { $_ -match 'error:|warning:' } | Select-Object -First 1
    if ($hit) {
        $line = $hit.Trim()
        $msg = $line -replace '^.*?\[[^\]]*\]\s*', ''
    }
    if ($code -ne 0 -and $kind -eq "ok") { $kind = "non-zero" }
    if ($out -match '内部错误|Segmentation|crash|assert') { $kind = "CRASH?" }

    $verdict = "OK"
    if ($c.e -eq "error" -and $kind -ne "error") { $verdict = "!! 该报错没报" }
    elseif ($c.e -eq "warn" -and $kind -eq "ok") { $verdict = "!! 该警告没警告" }
    elseif ($c.e -eq "error" -and $kind -eq "CRASH?") { $verdict = "!! 崩了" }
    elseif ($kind -eq "error" -and $c.e -eq "warn" -and $out -notmatch 'warning:') { $verdict = "!! 警告变错误" }
    elseif ($kind -eq "error" -and $c.e -eq "warn") { $verdict = "OK(都报了)" }

    $rows += [pscustomobject]@{ n = $c.n; want = $c.e; got = $kind; v = $verdict; msg = $msg }
}

$rows | ForEach-Object {
    Write-Output ("{0,-28} want={1,-5} got={2,-8} {3}" -f $_.n, $_.want, $_.got, $_.v)
    if ($_.msg) { Write-Output ("      → " + $_.msg) }
}
Write-Output ""
$bad = $rows | Where-Object { $_.v -like "!!*" }
Write-Output ("=== 共 " + $rows.Count + " 例；有问题 " + $bad.Count + " 例 ===")
$bad | ForEach-Object { Write-Output ("  " + $_.n + " : " + $_.v) }

