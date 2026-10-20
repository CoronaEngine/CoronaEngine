@echo off
REM === Vision Project Build — Standalone MSVC Environment ===
REM Bypasses vcvars64.bat (fails in non-interactive shells).
REM Usage: build_standalone.cmd [target] [build_dir]
REM   Defaults: target=vision-eval, build_dir=cmake-build-codex-msvc3

setlocal

set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=vision-eval"

set "BUILD_DIR=%~2"
if "%BUILD_DIR%"=="" set "BUILD_DIR=D:\yzy\code\cpp\Vision\cmake-build-codex-msvc3"

set "MSVC_ROOT=C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Tools\MSVC\14.50.35615"
set "WINSDK_ROOT=C:\Program Files (x86)\Windows Kits\10"
set "WINSDK_VER=10.0.26100.0"
set "CMAKE_EXE=D:\Program Files (x86)\CLion 2025.3.1.1\bin\cmake\win\x64\bin\cmake.exe"

set "INCLUDE=%MSVC_ROOT%\include;%WINSDK_ROOT%\Include\%WINSDK_VER%\ucrt;%WINSDK_ROOT%\Include\%WINSDK_VER%\shared;%WINSDK_ROOT%\Include\%WINSDK_VER%\um;%WINSDK_ROOT%\Include\%WINSDK_VER%\winrt;%WINSDK_ROOT%\Include\%WINSDK_VER%\cppwinrt"
set "LIB=%MSVC_ROOT%\lib\x64;%WINSDK_ROOT%\Lib\%WINSDK_VER%\ucrt\x64;%WINSDK_ROOT%\Lib\%WINSDK_VER%\um\x64"
set "PATH=%MSVC_ROOT%\bin\Hostx64\x64;%WINSDK_ROOT%\bin\%WINSDK_VER%\x64;C:\Windows\System32;C:\Windows;C:\Windows\System32\WindowsPowerShell\v1.0;%PATH%"

echo [build] Target: %TARGET%
echo [build] BuildDir: %BUILD_DIR%
echo [build] Compiler: %MSVC_ROOT%\bin\Hostx64\x64\cl.exe

REM Remove stale ninja lock
if exist "%BUILD_DIR%\.ninja_lock" del /f "%BUILD_DIR%\.ninja_lock"

"%CMAKE_EXE%" --build "%BUILD_DIR%" --target %TARGET% -j 4
set BUILD_EXIT=%ERRORLEVEL%

if %BUILD_EXIT% neq 0 (
    echo [build] FAILED with exit code %BUILD_EXIT%
    if exist "%BUILD_DIR%\.ninja_lock" del /f "%BUILD_DIR%\.ninja_lock"
)
if %BUILD_EXIT% equ 0 echo [build] SUCCESS

exit /b %BUILD_EXIT%
