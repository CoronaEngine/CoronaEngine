@echo off
setlocal

:: ============================================================
::  Vision Dev Verify — one-click pipeline
::  Usage:
::    dev_verify.bat build test         Multiple stages
:: ============================================================

set "OPENCV_IO_ENABLE_OPENEXR=1"
set "PYTHON=python"
set "SCRIPT=%~dp0dev_verify\dev_verify.py"

:: If no arguments, run all stages
if "%~1"=="" (
    "%PYTHON%" "%SCRIPT%"
) else (
    "%PYTHON%" "%SCRIPT%" --stage %*
)

if %ERRORLEVEL% neq 0 (
    echo.
    echo [FAILED] Some stages failed.
    pause
    exit /b 1
)
