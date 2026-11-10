# generate benchmark .leno (ASCII only) - dispatch cost
$L = New-Object System.Collections.Generic.List[string]
function W([string]$s) { $script:L.Add($s) }

W '// auto-generated: dispatch cost (if-chain vs switch, int / enum / payload enum)'
W 'import times'
W ''

# 2026-11-10：加 100 —— 二叉比较树就是为"大分支"做的，30 个 case 看不出差距
$Ks = @(3, 10, 30, 100)
foreach ($k in $Ks) {
    W ('func if{0}(int v): int {{' -f $k)
    for ($i = 0; $i -lt $k; $i++) {
        if ($i -eq 0) { $kw = 'if' } else { $kw = 'eif' }
        W ('    {0} v == {1} {{ return {2} }}' -f $kw, $i, ($i + 1))
    }
    W '    return 0'
    W '}'
    W ''
    W ('func sw{0}(int v): int {{' -f $k)
    W '    switch v {'
    for ($i = 0; $i -lt $k; $i++) { W ('        case {0} {{ return {1} }}' -f $i, ($i + 1)) }
    W '        default { return 0 }'
    W '    }'
    W '}'
    W ''
}

W 'enum E10 { a0, a1, a2, a3, a4, a5, a6, a7, a8, a9 }'
W ''
W 'func enIf(E10 e): int {'
for ($i = 0; $i -lt 10; $i++) {
    if ($i -eq 0) { $kw = 'if' } else { $kw = 'eif' }
    W ('    {0} e == E10.a{1} {{ return {2} }}' -f $kw, $i, ($i + 1))
}
W '    return 0'
W '}'
W ''
W 'func enSw(E10 e): int {'
W '    switch e {'
for ($i = 0; $i -lt 10; $i++) { W ('        case E10.a{0} {{ return {1} }}' -f $i, ($i + 1)) }
W '        default { return 0 }'
W '    }'
W '}'
W ''

W 'enum P10 {'
for ($i = 0; $i -lt 10; $i++) { W ('    m{0}(int v)' -f $i) }
W '}'
W ''
W 'func pSw(P10 p): int {'
W '    switch p {'
for ($i = 0; $i -lt 10; $i++) { W ('        case P10.m{0}(x) {{ return x + {0} }}' -f $i) }
W '        default { return 0 }'
W '    }'
W '}'
W ''

W 'const ITERS = 300000'
W ''
W 'main() {'
W '    Array[E10] es = [E10.a0, E10.a1, E10.a2, E10.a3, E10.a4, E10.a5, E10.a6, E10.a7, E10.a8, E10.a9]'
W '    Array[P10] ps = []'
for ($i = 0; $i -lt 10; $i++) { W ('    ps.add(P10.m{0}({0}))' -f $i) }
W ''
W '    // 基线：同样的循环 + 同样的"取输入"，只是不调被测函数'
W '    { var acc = 0'
W '      var t0 = times.us()'
W '      for ITERS to i { acc = acc + i }'
W '      var t1 = times.us()'
W '      print("baseline_loop = " + _str(_int((t1 - t0) / 1000.0)) + " ms  acc=" + _str(acc)) }'
W ''

function Bench([string]$name, [string]$expr) {
    $script:L.Add('    // ---- ' + $name + ' ----')
    $script:L.Add('    { var acc = 0')
    $script:L.Add('      var t0 = times.us()')
    $script:L.Add('      for ITERS to i { acc = acc + ' + $expr + ' }')
    $script:L.Add('      var t1 = times.us()')
    $script:L.Add('      print("' + $name + ' = " + _str(_int((t1 - t0) / 1000.0)) + " ms  acc=" + _str(acc)) }')
    $script:L.Add('')
}

# int：命中第 1 个 / 最后 1 个 / 混合
foreach ($k in $Ks) {
    Bench ("if{0}_first" -f $k)   ("if{0}(0)" -f $k)
    Bench ("if{0}_last" -f $k)    ("if{0}({1})" -f $k, ($k - 1))
    Bench ("sw{0}_first" -f $k)   ("sw{0}(0)" -f $k)
    Bench ("sw{0}_last" -f $k)    ("sw{0}({1})" -f $k, ($k - 1))
    Bench ("sw{0}_mixed" -f $k)   ("sw{0}(i % {1})" -f $k, $k)
}
Bench 'enum_if_first' 'enIf(E10.a0)'
Bench 'enum_if_last' 'enIf(E10.a9)'
Bench 'enum_sw_first' 'enSw(E10.a0)'
Bench 'enum_sw_last' 'enSw(E10.a9)'
Bench 'enum_sw_mixed' 'enSw(es[i % 10])'
Bench 'payload_sw_first' 'pSw(ps[0])'
Bench 'payload_sw_last' 'pSw(ps[9])'
Bench 'payload_sw_mixed' 'pSw(ps[i % 10])'
W '}'

Set-Content -Path 'd:\CLeno\Leno\tmp_bench.leno' -Value $L -Encoding ascii
Write-Output ('lines = ' + $L.Count)
