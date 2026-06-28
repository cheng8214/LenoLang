@echo off
setlocal enabledelayedexpansion

echo Building LenoLang VM Runtime (no compiler)...

if not exist build mkdir build

set SOURCES=
set SOURCES=!SOURCES! src\vm_main.c
set SOURCES=!SOURCES! src\error.c
set SOURCES=!SOURCES! src\scope.c
set SOURCES=!SOURCES! src\gc.c
set SOURCES=!SOURCES! src\value.c
set SOURCES=!SOURCES! src\string_table.c
set SOURCES=!SOURCES! src\object\object_string.c
set SOURCES=!SOURCES! src\object\object_array.c
set SOURCES=!SOURCES! src\object\object_dict.c
set SOURCES=!SOURCES! src\object\object_number.c
set SOURCES=!SOURCES! src\object\object_file.c
set SOURCES=!SOURCES! src\object\object_struct.c
set SOURCES=!SOURCES! src\object\object_face.c
set SOURCES=!SOURCES! src\object\object_cstruct.c
set SOURCES=!SOURCES! src\object\object_thread.c
set SOURCES=!SOURCES! src\object\object_socket.c
set SOURCES=!SOURCES! src\bound_method.c
set SOURCES=!SOURCES! src\coroutine.c
set SOURCES=!SOURCES! src\vm\vm.c
set SOURCES=!SOURCES! src\debug.c
set SOURCES=!SOURCES! src\type.c
set SOURCES=!SOURCES! src\native.c
set SOURCES=!SOURCES! src\bigint.c
set SOURCES=!SOURCES! src\module_loader.c
set SOURCES=!SOURCES! src\module_dispatch.c
set SOURCES=!SOURCES! src\module_symbol_table.c
set SOURCES=!SOURCES! src\module.c
set SOURCES=!SOURCES! src\module\io\io.c
set SOURCES=!SOURCES! src\module\types\types.c
set SOURCES=!SOURCES! src\module\times\times.c
set SOURCES=!SOURCES! src\module\arrays\arrays.c
set SOURCES=!SOURCES! src\module\strings\strings.c
set SOURCES=!SOURCES! src\module\maths\maths.c
set SOURCES=!SOURCES! src\module\dicts\dicts.c
set SOURCES=!SOURCES! src\module\structs\structs.c
set SOURCES=!SOURCES! src\module\cstructs\cstructs.c
set SOURCES=!SOURCES! src\module\rands\rands.c
set SOURCES=!SOURCES! src\module\files\files.c
set SOURCES=!SOURCES! src\module\asyncs\asyncs.c
set SOURCES=!SOURCES! src\module\dirs\dirs.c
set SOURCES=!SOURCES! src\module\jsons\jsons.c
set SOURCES=!SOURCES! src\module\sockets\sockets.c
set SOURCES=!SOURCES! src\module\ffi\ffi.c
set SOURCES=!SOURCES! src\module\ffi\leno_ffi_win64.c
set SOURCES=!SOURCES! src\module\threads\threads.c
set SOURCES=!SOURCES! src\module\assert\assert.c
set SOURCES=!SOURCES! src\module\sys\sys.c
set SOURCES=!SOURCES! src\module\regexs\regexs.c
set SOURCES=!SOURCES! src\platform\platform_thread.c
set SOURCES=!SOURCES! src\serialize\serialize.c

gcc -o build\leno_vm.exe !SOURCES! -Isrc -Wall -Wextra -std=c99 -O2 -lm -municode -lws2_32

if %ERRORLEVEL% neq 0 (
    echo VM build failed
    exit /b 1
)

echo VM build successful
echo.
echo Usage: build\leno_vm.exe ^<file.lenb^>
echo Self:  build\leno_vm.exe --self
