# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

param(
    [string]$RunId = "",
    [switch]$Quiet
)

$ErrorActionPreference = "Continue"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $ScriptDir
$DistDir = Join-Path $RepoRoot "dist\Release"
$DistLogsDir = Join-Path $DistDir "logs"
$PrivateLogsRoot = if ($env:AOC_MIRROR_LOGS_ROOT) { $env:AOC_MIRROR_LOGS_ROOT } else { Join-Path $RepoRoot "logs\mirror" }   # a second copy of every run

# Determine RunId if not specified
if (-not $RunId) {
    # The Go services and worldd from this checkout, if they run: the run is named after the first of them to start
    $goProc = Get-Process -Name worldd, gateway -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($RepoRoot, [System.StringComparison]::OrdinalIgnoreCase) } |
        Sort-Object StartTime | Select-Object -First 1
    if ($goProc) {
        $RunId = "run_" + $goProc.StartTime.ToString("yyyyMMdd_HHmmss")
    } else {
        # Not running any more: the run the auto-logger saved last, else the time now
        $lastRun = Get-ChildItem -Path $DistLogsDir -Directory -Filter "run_*" -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
        if ($lastRun) { $RunId = $lastRun.Name } else { $RunId = "run_" + (Get-Date).ToString("yyyyMMdd_HHmmss") }
    }
}

$DestDirs = @(
    (Join-Path $RepoRoot "logs\$RunId"),
    (Join-Path $PrivateLogsRoot $RunId),
    (Join-Path $DistLogsDir $RunId)
)
# -- C.O.A & C_Y_B --

foreach ($dir in $DestDirs) {
    if (-not (Test-Path $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
}

$collected = [System.Collections.Generic.List[PSObject]]::new()

function Save-Artifact($srcPath, $destFileName) {
    if (-not (Test-Path $srcPath)) { return }
    $fi = Get-Item $srcPath
    foreach ($dir in $DestDirs) {
        $dest = Join-Path $dir $destFileName
        Copy-Item -LiteralPath $srcPath -Destination $dest -Force
    }
    $collected.Add([PSCustomObject]@{
        Name = $destFileName
        Size = $fi.Length
        Source = $srcPath
    })
}

function Save-TextArtifact($content, $destFileName) {
    if (-not $content) { return }
    foreach ($dir in $DestDirs) {
        $dest = Join-Path $dir $destFileName
        Set-Content -LiteralPath $dest -Value $content -Encoding UTF8 -Force
    }
    $collected.Add([PSCustomObject]@{
        Name = $destFileName
        Size = $content.Length
        Source = "<in-memory>"
    })
}

# 1. Tether Server
$latestTether = Get-ChildItem -Path $DistLogsDir -Filter "tether-*.log" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($latestTether) {
    Save-Artifact $latestTether.FullName "tether.txt"
}

# 2. The Go services and worldd
$GoLogsDir = Join-Path $RepoRoot "dist\services\logs"
foreach ($name in 'auth', 'character', 'citynode', 'mesh', 'gateway', 'worldd') {
    Save-Artifact (Join-Path $GoLogsDir "$name.log") "go-$name.txt"
}

# 3. Game Client
$clientLogPath = Join-Path $env:LOCALAPPDATA "AOC\Saved\Logs\AOC.log"
if (Test-Path $clientLogPath) {
    Save-Artifact $clientLogPath "game_client.txt"
}

# 4. The client's browser log
$cefLogPath = Join-Path $env:LOCALAPPDATA "AOC\Saved\Logs\cef3.log"
if (Test-Path $cefLogPath) {
    Save-Artifact $cefLogPath "cef3.txt"
}

# 5. Generate Run Summary
$summary = [System.Collections.Generic.List[string]]::new()
$summary.Add("================================================================")
$summary.Add("  AoC Private Server Run Summary: $RunId")
$summary.Add("  Captured At: " + (Get-Date).ToString("yyyy-MM-dd HH:mm:ss"))
$summary.Add("================================================================")
$summary.Add("")

$primaryDest = $DestDirs[0]

# Check Tether
$tetherTxt = Join-Path $primaryDest "tether.txt"
if (Test-Path $tetherTxt) {
    $tContent = Get-Content -LiteralPath $tetherTxt
    $sessionOk = ($tContent | Select-String "Session \d+ established").Count -gt 0
    $summary.Add("[Tether Server]")
    $summary.Add("  Session Established: " + ($(if ($sessionOk) { "YES" } else { "NO" })))
    $summary.Add("")
}

# Check Client
$gcTxt = Join-Path $primaryDest "game_client.txt"
if (Test-Path $gcTxt) {
    $gcContent = Get-Content -LiteralPath $gcTxt
    $loadDrop = ($gcContent | Select-String "HideLoadingScreen").Count -gt 0
    $cleanExit = ($gcContent | Select-String "FPlatformMisc::RequestExit").Count -gt 0
    $gcWarns = ($gcContent | Select-String '"severity":"Warning"').Count
    $gcErrs = ($gcContent | Select-String '"severity":"Error"').Count

    $summary.Add("[Game Client]")
    $summary.Add("  World Load Screen Dropped: " + ($(if ($loadDrop) { "YES" } else { "NO" })))
    $summary.Add("  Clean Exit (Window Close): " + ($(if ($cleanExit) { "YES" } else { "NO" })))
    $summary.Add("  Client Warnings: $gcWarns, Errors: $gcErrs")
    $summary.Add("")
}

$summaryText = $summary -join "`r`n"
Save-TextArtifact $summaryText "run_summary.txt"

if (-not $Quiet) {
    Write-Host ""
    Write-Host "================================================================" -ForegroundColor Cyan
    Write-Host "  Logs successfully collected for: $RunId" -ForegroundColor Cyan
    Write-Host "================================================================" -ForegroundColor Cyan
    Write-Host "Primary location: $primaryDest" -ForegroundColor Green
    Write-Host "Mirrored to:     $($DestDirs[1])" -ForegroundColor Green
    Write-Host ""
    Write-Host "Captured Files:" -ForegroundColor Yellow
    $collected | Sort-Object Name -Unique | Format-Table -Property @{Label="File Name"; Expression={$_.Name}}, @{Label="Size (Bytes)"; Expression={$_.Size.ToString("N0")}; Alignment="Right"}
    Write-Host $summaryText
}
