param(
    [string]$BuildDir = "D:\yzy\code\cpp\Vision\cmake-build-debug-msvc",
    [string]$Target = "vision-base",
    [int]$TimeoutSec = 900,
    [int]$Jobs = 4,
    [switch]$CleanLockOnly
)

$ErrorActionPreference = "Stop"

function Stop-ProcessTree {
    param([int]$ProcessId)
    if ($ProcessId -le 0) {
        return
    }
    $taskkill = Start-Process -FilePath "taskkill.exe" `
        -ArgumentList "/PID", $ProcessId, "/T", "/F" `
        -NoNewWindow `
        -PassThru `
        -Wait
    if ($taskkill.ExitCode -ne 0) {
        Write-Warning "taskkill exited with code $($taskkill.ExitCode) for PID $ProcessId"
    }
}

function Remove-StaleNinjaLock {
    param([string]$ResolvedBuildDir)
    $lockPath = Join-Path $ResolvedBuildDir ".ninja_lock"
    Get-Process cmd, cmake, ninja, cl, link -ErrorAction SilentlyContinue | Stop-Process -Force
    if (Test-Path -LiteralPath $lockPath) {
        Remove-Item -LiteralPath $lockPath -Force -ErrorAction SilentlyContinue
    }
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$tmpDir = Join-Path $repoRoot ".agent-os\\tmp"
if (!(Test-Path -LiteralPath $tmpDir)) {
    New-Item -ItemType Directory -Path $tmpDir | Out-Null
}

$resolvedBuildDir = (Resolve-Path $BuildDir).Path
if ($CleanLockOnly) {
    Remove-StaleNinjaLock -ResolvedBuildDir $resolvedBuildDir
    Write-Output "cleaned"
    exit 0
}

$vcvars = "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat"
$ninja = "D:\Program Files (x86)\CLion 2025.3.1.1\bin\ninja\win\x64\ninja.exe"
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$stdoutLog = Join-Path $tmpDir "$Target-build-$stamp.out.log"
$stderrLog = Join-Path $tmpDir "$Target-build-$stamp.err.log"

Remove-StaleNinjaLock -ResolvedBuildDir $resolvedBuildDir

$command = "set VSCMD_DEBUG=0 && call `"$vcvars`" >nul && `"$ninja`" -C `"$resolvedBuildDir`" $Target -j $Jobs"
$process = Start-Process -FilePath "cmd.exe" `
    -ArgumentList "/d", "/c", $command `
    -WorkingDirectory $repoRoot `
    -RedirectStandardOutput $stdoutLog `
    -RedirectStandardError $stderrLog `
    -PassThru

$finished = $process.WaitForExit($TimeoutSec * 1000)
if (!$finished) {
    Stop-ProcessTree -ProcessId $process.Id
    Remove-StaleNinjaLock -ResolvedBuildDir $resolvedBuildDir
    Write-Output "status=timeout"
    Write-Output "pid=$($process.Id)"
    Write-Output "stdout=$stdoutLog"
    Write-Output "stderr=$stderrLog"
    exit 124
}

$exitCode = $process.ExitCode
if ($exitCode -ne 0) {
    Remove-StaleNinjaLock -ResolvedBuildDir $resolvedBuildDir
}

Write-Output "status=completed"
Write-Output "exit_code=$exitCode"
Write-Output "stdout=$stdoutLog"
Write-Output "stderr=$stderrLog"
exit $exitCode
