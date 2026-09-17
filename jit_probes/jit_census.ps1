# ============================================================================
# jit_census.ps1 -- merged view of the "two disjoint measurements" of JIT coverage
#                  (§8.92)
#
# Why: JIT coverage has two mutually-exclusive observation channels:
#   * scan-time rejects  (`LENO_JIT_GAPS=1`): objects rejected BEFORE compiling
#     (loop / func / inline, de-duplicated per compiled object -- see §8.77);
#   * runtime bailouts   (the stats `Bailout:` lines): objects that DID compile but
#     fail at run time and end up blacklisted -- each line carries the **triggering
#     instruction name** (§8.89 permanent diagnostic).
# Only the first misses "compiled but bails every time" (bound-method calls before
# R6-k); only the second misses "never even tried". Together they answer:
# "which hot loops / functions / callees still do not enter the JIT, and why".
#
# NOTE: do NOT use `LENO_JIT_DEBUG=1` for this -- on real apps it produces 8MB+ of
# stderr and drags the run until it never finishes (§8.71 / §8.89 each cost a round).
# Both switches used here print once at exit.
#
# Usage:
#   powershell -File jit_probes\jit_census.ps1                # 3 real apps x 300 frames
#   powershell -File jit_probes\jit_census.ps1 -Frames 60     # hot loops need >= 50 back-edges
#
# WARNING (keep this file ASCII-only): Windows PowerShell 5.1 reads a BOM-less file
# as ANSI, so any CJK string literal in here would be mangled and break parsing
# (measured). CJK directory names are therefore *located* via ASCII file names
# instead of being embedded.
# ============================================================================
param(
    [int]$Frames = 300
)

$ErrorActionPreference = 'Stop'
$root   = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$exe    = Join-Path $root 'build\leno.exe'
$exBase = Join-Path $root 'leno_module\LenoSDL3\examples'
$targets = @('file_manager.leno', 'cache_cleaner.leno', 'gomoku.leno')

$env:SDL_VIDEODRIVER = 'dummy'
$env:LENO_SDL_FRAMES = "$Frames"
$env:LENO_JIT_GAPS   = '1'
try {
    foreach ($t in $targets) {
        $f = Get-ChildItem -Path $exBase -Recurse -Filter $t -File -ErrorAction SilentlyContinue |
             Select-Object -First 1
        if (-not $f) {
            Write-Output ("===== {0}  [SKIP: not found under examples\] =====" -f $t)
            continue
        }

        $err = Join-Path $env:TEMP ('jit_census_' + $t + '.err')
        $out = Join-Path $env:TEMP ('jit_census_' + $t + '.out')

        $p = Start-Process -FilePath $exe -ArgumentList $f.Name -WorkingDirectory $f.DirectoryName `
             -RedirectStandardOutput $out -RedirectStandardError $err -PassThru
        $finished = $p.WaitForExit(300000)
        # NOTE: a -PassThru process object needs a second WaitForExit() before
        # ExitCode is populated; without it the field prints empty.
        if (-not $finished) { $p.Kill(); $code = 'KILLED' }
        else { $p.WaitForExit(); $code = $p.ExitCode }

        Write-Output ('===== {0}  (exit={1}, frames={2}) =====' -f $t, $code, $Frames)

        Write-Output ('  [runtime] ' + (((Select-String -Path $err `
              -Pattern 'Compiled:|Executed:|Bailouts:|FuncCompiled:|FuncExecuted:' |
              ForEach-Object { $_.Line.Trim() }) -join '  ')))

        $bl = @(Select-String -Path $err -Pattern '^\s+Bailout: ')
        if ($bl.Count -gt 0) {
            foreach ($l in $bl) { Write-Output ('  [runtime bailout] ' + $l.Line.Trim()) }
        } else {
            Write-Output '  [runtime bailout] (none)'
        }

        $gaps = @(Select-String -Path $err -Pattern '^\s+\d+\s+(loop|func|inline)\|')
        if ($gaps.Count -gt 0) {
            Write-Output '  [scan rejects]'
            foreach ($g in $gaps) { Write-Output ('    ' + $g.Line.Trim()) }
        } else {
            Write-Output '  [scan rejects] (none)'
        }

        $bench = @(Select-String -Path $out -Pattern 'SDL-BENCH')
        if ($bench.Count -gt 0) { Write-Output ('  [bench] ' + $bench[0].Line.Trim()) }
        Write-Output ''
    }
} finally {
    Remove-Item Env:SDL_VIDEODRIVER -ErrorAction SilentlyContinue
    Remove-Item Env:LENO_SDL_FRAMES -ErrorAction SilentlyContinue
    Remove-Item Env:LENO_JIT_GAPS   -ErrorAction SilentlyContinue
}
