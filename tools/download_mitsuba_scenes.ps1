<#
.SYNOPSIS
Downloads every Mitsuba ZIP linked from Benedikt Bitterli's resource page.
.EXAMPLE
Double-click tools\download_mitsuba_scenes.cmd.
.EXAMPLE
.\tools\download_mitsuba_scenes.ps1 -ListOnly
.EXAMPLE
.\tools\download_mitsuba_scenes.ps1 -DestinationDirectory D:\Scenes\Mitsuba
#>
[CmdletBinding()]
param(
    [string]$DestinationDirectory,
    [switch]$ListOnly
)

$ErrorActionPreference = 'Stop'

function Get-MitsubaArchiveUrls {
    param([string]$Html, [uri]$PageUri)

    $seen = New-Object 'System.Collections.Generic.HashSet[string]'
    foreach ($link in [regex]::Matches($Html, '(?is)\bhref\s*=\s*["''](?<url>[^"'']+)["'']')) {
        $uri = [uri]::new($PageUri, [Net.WebUtility]::HtmlDecode($link.Groups['url'].Value))
        if ($uri.Scheme -in @('http', 'https') -and $uri.Host -eq $PageUri.Host -and
            $uri.AbsolutePath -match '/mitsuba/[a-z0-9][a-z0-9_-]*\.zip$' -and
            $seen.Add($uri.AbsoluteUri)) {
            $uri.AbsoluteUri
        }
    }
}

function Save-MitsubaArchive {
    param([uri]$Uri, [string]$DestinationDirectory)

    New-Item -ItemType Directory -Path $DestinationDirectory -Force | Out-Null
    $name = [IO.Path]::GetFileName($Uri.AbsolutePath)
    $destination = Join-Path $DestinationDirectory $name
    $partial = "$destination.part"
    if ((Test-Path -LiteralPath $destination -PathType Leaf) -and
        (Get-Item -LiteralPath $destination).Length -gt 0) {
        Write-Host "[SKIP] $name already downloaded."
        return 'skipped'
    }

    Write-Host "[DOWNLOAD] $name"
    & curl.exe --fail --location --retry 3 --retry-delay 2 --connect-timeout 20 `
        --continue-at - --output $partial --url $Uri.AbsoluteUri
    if ($LASTEXITCODE -ne 0) {
        throw "curl failed for $name (exit $LASTEXITCODE). Run again to resume the .part file."
    }
    if (-not (Test-Path -LiteralPath $partial -PathType Leaf) -or
        (Get-Item -LiteralPath $partial).Length -eq 0) {
        throw "No archive data received for $name."
    }
    Move-Item -LiteralPath $partial -Destination $destination -Force
    return 'downloaded'
}

function Invoke-MitsubaSceneDownloads {
    param([string]$DestinationDirectory, [switch]$ListOnly)

    $pageUri = [uri]'https://benedikt-bitterli.me/resources/'
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor
        [Net.SecurityProtocolType]::Tls12
    Write-Host "Reading scene list: $pageUri"
    $page = Invoke-WebRequest -Uri $pageUri -UseBasicParsing -TimeoutSec 60
    $archives = @(Get-MitsubaArchiveUrls -Html $page.Content -PageUri $pageUri)
    if ($archives.Count -eq 0) {
        throw 'No Mitsuba ZIP links found. Check the resource page before retrying.'
    }
    Write-Host "Found $($archives.Count) Mitsuba scene archives (including legacy Mitsuba scenes)."
    if ($ListOnly) {
        $archives
        return
    }
    if (-not (Get-Command curl.exe -ErrorAction SilentlyContinue)) {
        throw 'curl.exe is required. It is included with current Windows 10 and Windows 11.'
    }

    $DestinationDirectory = [IO.Path]::GetFullPath($DestinationDirectory)
    Write-Host "Download folder: $DestinationDirectory"
    Write-Host 'Keep this window open. Run again after an interruption to resume.'
    $downloaded = 0
    $skipped = 0
    $failed = 0
    for ($index = 0; $index -lt $archives.Count; $index++) {
        Write-Host "`n[$($index + 1)/$($archives.Count)] $($archives[$index])"
        try {
            $result = Save-MitsubaArchive -Uri $archives[$index] -DestinationDirectory $DestinationDirectory
            if ($result -eq 'skipped') { $skipped++ } else { $downloaded++ }
        } catch {
            $failed++
            Write-Warning $_.Exception.Message
        }
    }
    Write-Host "`nFinished: $downloaded downloaded, $skipped skipped, $failed failed."
    Write-Host "ZIP archives saved in: $DestinationDirectory"
    if ($failed -gt 0) {
        throw 'Some downloads failed. Double-click the launcher again to retry and resume.'
    }
}

if ($MyInvocation.InvocationName -ne '.') {
    try {
        if (-not $DestinationDirectory) {
            $DestinationDirectory = Join-Path (Split-Path $PSScriptRoot -Parent) '.workspace\downloads\mitsuba'
        }
        Invoke-MitsubaSceneDownloads -DestinationDirectory $DestinationDirectory -ListOnly:$ListOnly
        exit 0
    } catch {
        Write-Host "`n[ERROR] $($_.Exception.Message)" -ForegroundColor Red
        exit 1
    }
}
