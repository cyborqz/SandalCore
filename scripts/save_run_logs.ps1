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
$ToolsDir = Join-Path $RepoRoot "tools"
$PrivateLogsRoot = if ($env:AOC_MIRROR_LOGS_ROOT) { $env:AOC_MIRROR_LOGS_ROOT } else { Join-Path $RepoRoot "logs\mirror" }   # a second copy of every run

# Determine RunId if not specified
if (-not $RunId) {
    # Check if aoc_server is running
    $aocProc = Get-Process -Name "aoc_server" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($aocProc) {
        $RunId = "run_" + $aocProc.StartTime.ToString("yyyyMMdd_HHmmss")
    } else {
        # Check latest emu log
        $latestEmu = Get-ChildItem -Path $DistLogsDir -Filter "emu-*.log" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($latestEmu -and $latestEmu.Name -match "emu-(\d{8})-(\d{6})\.log") {
            $RunId = "run_" + $matches[1] + "_" + $matches[2]
        } else {
            $RunId = "run_" + (Get-Date).ToString("yyyyMMdd_HHmmss")
        }
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

# 1. Game Server
$latestEmu = Get-ChildItem -Path $DistLogsDir -Filter "emu-*.log" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($latestEmu) {
    Save-Artifact $latestEmu.FullName "game_server.txt"
    # Update server_last.log in the mirror folder
    $lastLog = Join-Path $PrivateLogsRoot "server_last.log"
    try { Copy-Item -LiteralPath $latestEmu.FullName -Destination $lastLog -Force } catch {}
}

# 2. Tether Server
$latestTether = Get-ChildItem -Path $DistLogsDir -Filter "tether-*.log" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($latestTether) {
    Save-Artifact $latestTether.FullName "tether.txt"
}

# 3. Mock Auth Server
$authLogContent = ""
$mockAuthFile = Join-Path $DistLogsDir "mock_auth.log"
if (Test-Path $mockAuthFile) {
    $authLogContent = Get-Content -LiteralPath $mockAuthFile -Raw -ErrorAction SilentlyContinue
}

# Try dumping console if auth_server is running
$authProc = Get-Process -Name "auth_server" -ErrorAction SilentlyContinue | Select-Object -First 1
$dumpExe = Join-Path $ToolsDir "dump_console.exe"
if ($authProc -and (Test-Path $dumpExe)) {
    $tempDump = [System.IO.Path]::GetTempFileName()
    try {
        & $dumpExe $authProc.Id $tempDump | Out-Null
        if (Test-Path $tempDump) {
            $dumpText = Get-Content -LiteralPath $tempDump -Raw -ErrorAction SilentlyContinue
            if ($dumpText -and $dumpText.Trim()) {
                if ($authLogContent) {
                    $authLogContent += "`r`n" + $dumpText.Trim()
                } else {
                    $authLogContent = $dumpText.Trim()
                }
            }
        }
    } finally {
        if (Test-Path $tempDump) { Remove-Item -LiteralPath $tempDump -Force -ErrorAction SilentlyContinue }
    }
}

if ($authLogContent) {
    Save-TextArtifact $authLogContent "mock_auth.txt"
}

# 4. Game Client
$clientLogPath = Join-Path $env:LOCALAPPDATA "AOC\Saved\Logs\AOC.log"
if (Test-Path $clientLogPath) {
    Save-Artifact $clientLogPath "game_client.txt"
}

# 5. Additional Artifacts
$bunchesLog = Join-Path $DistLogsDir "bunches_liveworld.log"
if (Test-Path $bunchesLog) {
    Save-Artifact $bunchesLog "bunches_liveworld.txt"
}

$latestCapture = Get-ChildItem -Path $DistLogsDir -Filter "capture-*.jsonl" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($latestCapture) {
    Save-Artifact $latestCapture.FullName "capture.jsonl"
}

$cefLogPath = Join-Path $env:LOCALAPPDATA "AOC\Saved\Logs\cef3.log"
if (Test-Path $cefLogPath) {
    Save-Artifact $cefLogPath "cef3.txt"
}

# 6. Generate Run Summary
$summary = [System.Collections.Generic.List[string]]::new()
$summary.Add("================================================================")
$summary.Add("  AoC Private Server Run Summary: $RunId")
$summary.Add("  Captured At: " + (Get-Date).ToString("yyyy-MM-dd HH:mm:ss"))
$summary.Add("================================================================")
$summary.Add("")

# Check Game Server
$primaryDest = $DestDirs[0]
$gsTxt = Join-Path $primaryDest "game_server.txt"
if (Test-Path $gsTxt) {
    $gsContent = Get-Content -LiteralPath $gsTxt
    $handshakeOk = ($gsContent | Select-String "HANDSHAKE COMPLETE|CLIENT CONNECTED").Count -gt 0
    $welcomeOk = ($gsContent | Select-String "NMT_Welcome").Count -gt 0
    $replayLines = $gsContent | Select-String "Sent:\s+(\d+)\s+/\s+(\d+)"
    $warnCount = ($gsContent | Select-String "\[warning\]").Count
    $errCount = ($gsContent | Select-String "\[error\]").Count
# C.O.A - C Y B

    $summary.Add("[Game Server]")
    $summary.Add("  Handshake: " + ($(if ($handshakeOk) { "YES" } else { "NO" })))
    $summary.Add("  Welcome Sent: " + ($(if ($welcomeOk) { "YES" } else { "NO" })))
    if ($replayLines) {
        $summary.Add("  " + $replayLines[-1].Line.Trim())
    }
    $summary.Add("  Warnings: $warnCount, Errors: $errCount")
    $summary.Add("")
}

# Check Tether
$tetherTxt = Join-Path $primaryDest "tether.txt"
if (Test-Path $tetherTxt) {
    $tContent = Get-Content -LiteralPath $tetherTxt
    $sessionOk = ($tContent | Select-String "Session \d+ established").Count -gt 0
    $summary.Add("[Tether Server]")
    $summary.Add("  Session Established: " + ($(if ($sessionOk) { "YES" } else { "NO" })))
    $summary.Add("")
}

# Check Mock Auth
$authTxt = Join-Path $primaryDest "mock_auth.txt"
if (Test-Path $authTxt) {
    $aContent = Get-Content -LiteralPath $authTxt
    $summary.Add("[Auth Server]")
    foreach ($line in ($aContent | Select-Object -First 10)) {
        if ($line.Trim()) { $summary.Add("  " + $line.Trim()) }
    }
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
