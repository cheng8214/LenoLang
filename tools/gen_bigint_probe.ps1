# gen_bigint_probe.ps1 - T26 probe generator (see docs roadmap T26)
#
# NOTE: keep this file ASCII-only. PowerShell 5.1 parses BOM-less .ps1 with the
# active code page (same trap as build.bat; mojibake eats quotes => parse errors).
#
# The semantic whitelist allows bigint->int/float and bool->numeric into native
# params (visit_module.inc implicit-conversion table), but modules that strictly
# check val_is_int/val_is_float reject legal programs at runtime => latent bugs.
# This script scans src/module/*/*.c registration tables, enumerates methods with
# int/float params, and generates assert/probe_bigint_widen.leno: one call per
# (method, int/float param slot) with a negative bigint and a bool argument,
# wrapped in try/catch. Classification: error text complaining about the TYPE
# (while the argument IS type-compatible) => bug list; range/IO errors => noise.
#
# Safety:
#   - bigint is NEGATIVE (-1e20): if saturated, negative triggers a range error,
#     not a giant allocation (positive saturates to INT64_MAX => rep()-style OOM)
#   - method blacklist: side-effectful names (write/delete/mkdir/net/process/sleep)
#   - param kinds without a safe dummy (PTR/CSTRUCT/FUNC...) skip the whole method
#   - variadic (arity=-1) and spec-length-mismatch methods are skipped
#
# Usage: powershell -File tools\gen_bigint_probe.ps1
#   then: build\leno.exe assert\probe_bigint_widen.leno
#   BUG lines = fix list; noise = value-range/IO (ignorable).

$ErrorActionPreference = "Stop"
$root    = Split-Path $PSScriptRoot -Parent                          # tools\.. = repo root
$srcDir  = Join-Path $root "src\module"
$outFile = Join-Path $root "assert\test_bigint_widen.leno"

# v2 module tiers:
#   full-probe (both _BIG and true): strings maths rands times dirs sys regexs jsons sockets
#     - sockets: only pure byte-swap fns survive the blacklist (connect/listen/recv... blocked)
#   true-only (int params there are sizes/offsets - a huge/negative value could
#     giant-alloc or wild-ptr): files ffi
$moduleWhitelist = @("strings", "maths", "rands", "times", "dirs", "sys", "regexs", "jsons", "sockets", "files", "io", "ffi")
$noBigModules    = @("files", "ffi")
$methodBlacklist = "(?i)(delete|remove|write|clear|unlink|rmdir|mkdir|create|touch|chdir|exec|run|connect|send|kill|exit|sleep|wait|delay|pause|ints|floats|spawn|launch|accept|listen|bind|recv|select|resolve|shutdown|timeout|malloc|calloc|realloc|alloc|free|copy|rename|move|load|dlsym|open|seek)"

$calls   = New-Object System.Collections.Generic.List[string]
$mods    = New-Object System.Collections.Generic.List[string]
$skipped = New-Object System.Collections.Generic.List[string]

$files = Get-ChildItem $srcDir -Recurse -Filter *.c
foreach ($f in $files) {
    $text = Get-Content $f.FullName -Raw
    $text = $text -replace "(?m)//.*$", ""       # line comments
    $text = $text -replace "(?s)/\*.*?\*/", ""   # block comments
    $flat = $text -replace "\r?\n\s*", " "       # flatten (registrations may wrap)

    # param spec tables: TypeKind xxx_params[] = {TYPE_A, TYPE_B};
    $specs = @{}
    foreach ($m in [regex]::Matches($flat, "TypeKind\s+(\w+)\s*\[\s*\]\s*=\s*\{([^}]*)\}")) {
        $specs[$m.Groups[1].Value] = @($m.Groups[2].Value -split "," | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    }

    # registrations: native_register_module_method_spec("mod", "meth", fn, arity, min, max, &RET_SPEC, params)
    # capture groups: 1=mod 2=meth 3=arity 4=min 5=max  (return spec &?\w+ NOT captured) 6=params
    foreach ($m in [regex]::Matches($flat, 'native_register_module_method_spec\(\s*"(\w+)"\s*,\s*"(\w+)"\s*,\s*\w+\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*&?\w+\s*,\s*(\w+)\s*\)')) {
        $mod   = $m.Groups[1].Value
        $meth  = $m.Groups[2].Value
        $arity = [int]$m.Groups[3].Value
        $pname = $m.Groups[6].Value
        if ($moduleWhitelist -notcontains $mod) { continue }
        if ($meth -match $methodBlacklist)      { continue }
        if ($arity -le 0)                       { continue }   # variadic: skip in v1
        if (-not $specs.ContainsKey($pname))    { continue }
        $ptypes = $specs[$pname]
        if ($ptypes.Count -ne $arity) {
            $skipped.Add("$mod.$meth (spec $($ptypes.Count) != arity $arity)")
            continue
        }
        # all params must have a safe dummy, else skip the whole method
        $safe = $true
        foreach ($t in $ptypes) {
            if (@("TYPE_STRING","TYPE_INT","TYPE_FLOAT","TYPE_BOOL","TYPE_ARRAY","TYPE_DICT","TYPE_ANY") -notcontains $t) { $safe = $false; break }
        }
        if (-not $safe) { $skipped.Add("$mod.$meth (param kind without safe dummy)"); continue }

        if (-not $mods.Contains($mod)) { $mods.Add($mod) }

        for ($i = 0; $i -lt $arity; $i++) {
            $t = $ptypes[$i]
            if ($t -ne "TYPE_INT" -and $t -ne "TYPE_FLOAT") { continue }
            # _BIG policy: sizes/offsets modules (files/ffi) get true only -
            # a huge/negative value could giant-alloc or wild-pointer there
            $probeArgs = if ($noBigModules -contains $mod) { @("true") } else { @("_BIG", "true") }
            foreach ($arg in $probeArgs) {
                $list = @()
                for ($j = 0; $j -lt $arity; $j++) {
                    if ($j -eq $i) { $list += $arg; continue }
                    switch ($ptypes[$j]) {
                        "TYPE_STRING" { $list += '"a"' }
                        "TYPE_FLOAT"  { $list += "1.5" }
                        "TYPE_BOOL"   { $list += "true" }
                        "TYPE_ARRAY"  { $list += "[1]" }
                        "TYPE_DICT"   { $list += "{}" }
                        default       { $list += "1" }   # INT / ANY / numeric family
                    }
                }
                $call = "$mod.$meth(" + ($list -join ", ") + ")"
                $tag  = "$mod.$meth[$t#$i<$arg>]"
                $calls.Add("    try { $call } catch e { probe_hit(`"$tag`", e.msg) }")
            }
        }
    }
}

if ($calls.Count -eq 0) { Write-Error "no probeable calls parsed - check registration format"; exit 1 }

$imports = ($mods | Sort-Object) | ForEach-Object { "import $_" }

# classification keywords (build from code points: keep THIS file ASCII-only)
$kwType   = [string]::Join("", @([char]0x7C7B, [char]0x578B))                        # "leixing" (type)
$kwMustBe = [string]::Join("", @([char]0x5FC5, [char]0x987B, [char]0x662F))          # "bixushi" (must be)
$kwExpect = [string]::Join("", @([char]0x671F, [char]0x671B))                        # "qiwang" (expect)

# regression message (code points: keep THIS file ASCII-only)
$regMsg = [string]::Join("", @(
    [char]0x52A0, [char]0x5BBD, [char]0x5B9E, [char]0x53C2,          # widened args
    0x20, 0x3C, 0x3E, 0x20,                                          # " <> "
    [char]0x7C7B, [char]0x578B, [char]0x62B1, [char]0x6028,          # type complaint
    0x20, 0x2D, 0x2D, 0x3E, 0x20, 0x42, 0x55, 0x47, 0x20,            # " --> BUG "
    [char]0x884C, [char]0x89C1, [char]0x4E0A                          # "lines above"
))
$NL = [Environment]::NewLine
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("// probe_bigint_widen.leno - T26 probe (generated by tools/gen_bigint_probe.ps1, do not edit)")
$lines.Add("// Each line is a call the whitelist ALLOWS (widening arg). If the runtime error")
$lines.Add("// complains about the TYPE => latent bug (printed as BUG). Range/IO errors => noise.")
$lines.Add(($imports -join $NL))
$lines.Add("")
$lines.Add("int probe_cnt = 0")
$lines.Add("int bug_cnt = 0")
$lines.Add("int noise_cnt = 0")
$lines.Add("")
$lines.Add("func probe_hit(string tag, string msg) {")
$lines.Add("    probe_cnt = probe_cnt + 1")
$lines.Add("    string m = if msg == null then """" else msg")
$lines.Add("    if strings.has(m, ""$kwType"") or strings.has(m, ""$kwMustBe"") or strings.has(m, ""$kwExpect"") {")
$lines.Add("        bug_cnt = bug_cnt + 1")
$lines.Add("        print(""BUG  "" + tag + ""  <--  "" + m)")
$lines.Add("    } else {")
$lines.Add("        noise_cnt = noise_cnt + 1")
$lines.Add("        print(""NOISE  "" + tag + ""  <--  "" + m)")
$lines.Add("    }")
$lines.Add("}")
$lines.Add("")
$lines.Add("main() {")
$lines.Add("    var _BIG = -99999999999999999999   // negative: saturated value is still negative => range error, not a giant alloc")
$lines.Add("")
foreach ($c in $calls) { $lines.Add($c) }
$lines.Add("")
$lines.Add("    print(""--- probed="" + probe_cnt + "" bugs="" + bug_cnt + "" noise="" + noise_cnt)")
$lines.Add("    // regression: any TYPE complaint on a widened arg => whitelist/runtime mismatch (T26)")
$lines.Add("    assert_eq(bug_cnt, 0, ""$regMsg"")")
$lines.Add("}")

$dir = Split-Path $outFile -Parent
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
[System.IO.File]::WriteAllLines($outFile, $lines)
Write-Output ("generated " + $outFile + " : calls " + $calls.Count + ", modules [" + ($mods -join ",") + "], skipped " + $skipped.Count)
if ($skipped.Count -gt 0) { $skipped | ForEach-Object { Write-Output ("  skip: " + $_) } }
