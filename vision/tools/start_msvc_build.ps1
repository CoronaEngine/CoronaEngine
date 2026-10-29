param(
    [string]$BuildDir = "D:\yzy\code\cpp\Vision\cmake-build-debug-msvc",
    [string]$Target = "vision-base",
    [int]$TimeoutSec = 900,
    [int]$Jobs = 4
)

$ErrorActionPreference = "Stop"

function Remove-StaleNinjaLock {
    param([string]$ResolvedBuildDir)
    $lockPath = Join-Path $ResolvedBuildDir ".ninja_lock"
    if (Test-Path -LiteralPath $lockPath) {
        cmd /d /c "del /f /q `"$lockPath`"" | Out-Null
    }
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$tmpDir = Join-Path $repoRoot ".agent-os\tmp"
if (!(Test-Path -LiteralPath $tmpDir)) {
    New-Item -ItemType Directory -Path $tmpDir | Out-Null
}

$resolvedBuildDir = (Resolve-Path $BuildDir).Path
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$stdoutLog = Join-Path $tmpDir "$Target-build-$stamp.out.log"
$stderrLog = Join-Path $tmpDir "$Target-build-$stamp.err.log"
$exitFile = Join-Path $tmpDir "$Target-build-$stamp.exit.txt"
$stateFile = Join-Path $tmpDir "$Target-build-$stamp.state.json"
$watchdogOutLog = Join-Path $tmpDir "$Target-build-$stamp.watchdog.out.log"
$watchdogErrLog = Join-Path $tmpDir "$Target-build-$stamp.watchdog.err.log"

$vcvars = "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat"
$ninja = "D:\Program Files (x86)\CLion 2025.3.1.1\bin\ninja\win\x64\ninja.exe"

Remove-StaleNinjaLock -ResolvedBuildDir $resolvedBuildDir

$command = "set VSCMD_DEBUG=0 && call `"$vcvars`" >nul && `"$ninja`" -C `"$resolvedBuildDir`" $Target -j $Jobs 1> `"$stdoutLog`" 2> `"$stderrLog`" & set BUILD_EXIT=!errorlevel! & echo !BUILD_EXIT! > `"$exitFile`""
$process = Start-Process -FilePath "cmd.exe" `
    -ArgumentList "/v:on", "/d", "/c", $command `
    -WorkingDirectory $repoRoot `
    -PassThru

$state = [ordered]@{
    pid = $process.Id
    target = $Target
    build_dir = $resolvedBuildDir
    timeout_sec = $TimeoutSec
    start_time_utc = (Get-Date).ToUniversalTime().ToString("o")
    stdout_log = $stdoutLog
    stderr_log = $stderrLog
    exit_file = $exitFile
}
$state | ConvertTo-Json | Set-Content -LiteralPath $stateFile -Encoding UTF8

$watcher = Start-Process -FilePath "powershell.exe" `
    -ArgumentList "-ExecutionPolicy", "Bypass", "-File", (Join-Path $PSScriptRoot "watch_async_step.ps1"), "-StateFile", $stateFile `
    -WindowStyle Hidden `
    -PassThru

$state.watchdog_pid = $watcher.Id
$state.watchdog_stdout_log = $watchdogOutLog
$state.watchdog_stderr_log = $watchdogErrLog
$state | ConvertTo-Json | Set-Content -LiteralPath $stateFile -Encoding UTF8

Write-Output "state_file=$stateFile"
Write-Output "pid=$($process.Id)"
Write-Output "watchdog_pid=$($watcher.Id)"
Write-Output "stdout=$stdoutLog"
Write-Output "stderr=$stderrLog"
Write-Output "exit_file=$exitFile"
