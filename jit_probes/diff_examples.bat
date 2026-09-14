@echo off
setlocal enabledelayedexpansion
REM 基线二进制 vs 新版二进制的 stdout/退出码差分（结构体/函数/cstruct 密集示例）
REM 用法：在**仓库根目录**执行  jit_probes\diff_examples.bat
REM 依赖：build\leno.exe（新版）与 build\leno_base.exe（改动前的基线，见 README）
REM 注意：只比对 stdout 与退出码；含 <ptr ...> 地址或计时数字的行会有假差异
REM       （地址是 ASLR，同一二进制跑两次也不同 —— 先自查确定性再判定回归）。
set BASE=build\leno_base.exe
set NEW=build\leno.exe
set REPORT=%TEMP%\diff_report.txt
del /q "%REPORT%" 2>nul
if not exist "%BASE%" (
  echo [ERR] 缺少基线二进制 %BASE%
  exit /b 1
)
set COUNT=0
for %%f in (examples\struct\*.leno examples\func\*.leno examples\module_export_struct\*.leno examples\cstruct\*.leno) do (
  set /a COUNT+=1
  REM 注意：退出码必须在**加管道之前**取（管道会让 ERRORLEVEL 变成 findstr 的）
  "%BASE%" "%%f" > "%TEMP%\d_raw.txt" 2>&1
  set RA=!ERRORLEVEL!
  "%NEW%" "%%f" > "%TEMP%\d_raw2.txt" 2>&1
  set RB=!ERRORLEVEL!
  REM 过滤 ASLR 地址这类必然不同的行（<ptr 0x...>）
  findstr /V /C:"<ptr " "%TEMP%\d_raw.txt" > "%TEMP%\d_a.txt"
  findstr /V /C:"<ptr " "%TEMP%\d_raw2.txt" > "%TEMP%\d_b.txt"
  fc /b "%TEMP%\d_a.txt" "%TEMP%\d_b.txt" >nul 2>&1
  if errorlevel 1 (
    echo DIFF-STDOUT %%f  >> "%REPORT%"
  ) else (
    if not "!RA!"=="!RB!" echo DIFF-EXIT   %%f  ^(!RA! vs !RB!^) >> "%REPORT%"
  )
)
echo 共对比 !COUNT! 个文件
echo ---- 差异清单 ----
if exist "%REPORT%" (type "%REPORT%") else (echo 无差异)
