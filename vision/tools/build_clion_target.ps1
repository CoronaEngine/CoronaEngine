param(
    [string]$BuildDir = "D:\yzy\code\cpp\Vision\cmake-build-debug-msvc",
    [string]$Target = "vision-base",
    [int]$TimeoutSec = 600,
    [int]$Jobs = 4
)

$ErrorActionPreference = "Stop"

function Remove-StaleNinjaLock {
    param([string]$ResolvedBuildDir)
    $lockPath = Join-Path $ResolvedBuildDir ".ninja_lock"
    if (Test-Path -LiteralPath $lockPath) {
        Remove-Item -LiteralPath $lockPath -Force -ErrorAction SilentlyContinue
    }
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$tmpDir = Join-Path $repoRoot ".agent-os\tmp"
if (!(Test-Path -LiteralPath $tmpDir)) {
    New-Item -ItemType Directory -Path $tmpDir | Out-Null
}

$resolvedBuildDir = (Resolve-Path $BuildDir).Path
Remove-StaleNinjaLock -ResolvedBuildDir $resolvedBuildDir

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$cmdFile = Join-Path $tmpDir "build-clion-target-$stamp.cmd"
$vcvars = "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat"
$cmake = "D:\Program Files (x86)\CLion 2025.3.1.1\bin\cmake\win\x64\bin\cmake.exe"

$lines = @(
    "@echo off",
    "set VSCMD_DEBUG=0",
    "call `"$vcvars`" >nul",
    "`"$cmake`" --build `"$BuildDir`" --target $Target -j $Jobs",
    "if errorlevel 1 exit /b %errorlevel%"
)
Set-Content -LiteralPath $cmdFile -Value $lines -Encoding ASCII

$stdoutLog = Join-Path $tmpDir "build-clion-target-$stamp.out.log"
$stderrLog = Join-Path $tmpDir "build-clion-target-$stamp.err.log"
$argList = @("/d", "/c", $cmdFile)
$timeoutArgs = @{
    FilePath = "cmd.exe"
    ArgumentList = $argList
    TimeoutSec = $TimeoutSec
    WorkingDirectory = $repoRoot
    StdoutLog = $stdoutLog
    StderrLog = $stderrLog
}

& (Join-Path $PSScriptRoot "run_with_timeout.ps1") @timeoutArgs

$exitCode = $LASTEXITCODE
if ($exitCode -ne 0) {
    Remove-StaleNinjaLock -ResolvedBuildDir $resolvedBuildDir
}

exit $exitCode
