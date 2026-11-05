@echo off
setlocal
title Download Mitsuba Scenes
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0download_mitsuba_scenes.ps1" %*
set "download_exit=%ERRORLEVEL%"
echo.
if not "%download_exit%"=="0" echo Download failed. Run this launcher again to retry and resume.
pause
exit /b %download_exit%
