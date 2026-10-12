# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

param(
    [int]$IntervalMs = 1000
)

$ErrorActionPreference = "SilentlyContinue"

# Single-instance check
try {
    $myPid = $PID
    Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object {
        $_.ProcessId -ne $myPid -and $_.CommandLine -like "*auto_logger.ps1*"
    } | ForEach-Object {
        Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
    }
} catch {}

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $ScriptDir
$DistDir = Join-Path $RepoRoot "dist\Release"
$DistLogsDir = Join-Path $DistDir "logs"
$ToolsDir = Join-Path $RepoRoot "tools"
$PrivateLogsRoot = if ($env:AOC_MIRROR_LOGS_ROOT) { $env:AOC_MIRROR_LOGS_ROOT } else { Join-Path $RepoRoot "logs\mirror" }   # a second copy of every run
$ClientLogPath = Join-Path $env:LOCALAPPDATA "AOC\Saved\Logs\AOC.log"
$CefLogPath = Join-Path $env:LOCALAPPDATA "AOC\Saved\Logs\cef3.log"
$DumpExe = Join-Path $ToolsDir "dump_console.exe"
# launch_go.bat sets AOC_STACK=go for launch_all.bat, which starts this script.
$GoStack = ($env:AOC_STACK -eq 'go')
$GoLogsDir = Join-Path $RepoRoot "dist\services\logs"

function Sync-FileSafe($srcPath, $dstPath) {
    if (-not (Test-Path -LiteralPath $srcPath)) { return $false }
    try {
        $in = [System.IO.File]::Open($srcPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        $out = [System.IO.File]::Open($dstPath, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::ReadWrite)
        $in.CopyTo($out)
        $out.Flush()
        $out.Close()
        $in.Close()
        return $true
    } catch {
        return $false
    }
}
# [CoA :: C Y B]

function Write-TextSafe($text, $dstPath) {
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($text)
        $out = [System.IO.File]::Open($dstPath, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::ReadWrite)
        $out.Write($bytes, 0, $bytes.Length)
        $out.Flush()
        $out.Close()
    } catch {}
}

function Generate-Summary($destDir, $runId) {
    $summary = [System.Collections.Generic.List[string]]::new()
    $summary.Add("================================================================")
    $summary.Add("  AoC Private Server Run Summary: $runId")
    $summary.Add("  Updated At: " + (Get-Date).ToString("yyyy-MM-dd HH:mm:ss"))
    $summary.Add("================================================================")
    $summary.Add("")

    $gsTxt = Join-Path $destDir "game_server.txt"
    if (Test-Path $gsTxt) {
        $gsContent = Get-Content -LiteralPath $gsTxt -ErrorAction SilentlyContinue
        $handshakeOk = ($gsContent | Select-String "HANDSHAKE COMPLETE|CLIENT CONNECTED").Count -gt 0
        $welcomeOk = ($gsContent | Select-String "NMT_Welcome").Count -gt 0
        $replayLines = $gsContent | Select-String "Sent:\s+(\d+)\s+/\s+(\d+)"
        $warnCount = ($gsContent | Select-String "\[warning\]").Count
        $errCount = ($gsContent | Select-String "\[error\]").Count

        $summary.Add("[Game Server]")
        $summary.Add("  Handshake: " + ($(if ($handshakeOk) { "YES" } else { "NO" })))
        $summary.Add("  Welcome Sent: " + ($(if ($welcomeOk) { "YES" } else { "NO" })))
        if ($replayLines) { $summary.Add("  " + $replayLines[-1].Line.Trim()) }
        $summary.Add("  Warnings: $warnCount, Errors: $errCount")
        $summary.Add("")
    }

    $tetherTxt = Join-Path $destDir "tether.txt"
    if (Test-Path $tetherTxt) {
        $tContent = Get-Content -LiteralPath $tetherTxt -ErrorAction SilentlyContinue
        $sessionOk = ($tContent | Select-String "Session \d+ established").Count -gt 0
        $summary.Add("[Tether Server]")
        $summary.Add("  Session Established: " + ($(if ($sessionOk) { "YES" } else { "NO" })))
        $summary.Add("")
    }

    $authTxt = Join-Path $destDir "mock_auth.txt"
    if (Test-Path $authTxt) {
        $aContent = Get-Content -LiteralPath $authTxt -ErrorAction SilentlyContinue
        $summary.Add("[Auth Server]")
        foreach ($line in ($aContent | Select-Object -First 10)) {
            if ($line.Trim()) { $summary.Add("  " + $line.Trim()) }
        }
        $summary.Add("")
    }

    $gcTxt = Join-Path $destDir "game_client.txt"
    if (Test-Path $gcTxt) {
        $gcContent = Get-Content -LiteralPath $gcTxt -ErrorAction SilentlyContinue
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

    return ($summary -join "`r`n")
}

$currentRunId = ""
$lastSizes = @{}
$sawProcessesRunning = $false
$idleSecondsAfterStop = 0

Write-Host "[AutoLogger] Background auto-logger active. Streaming all 4 application logs live..." -ForegroundColor Cyan

while ($true) {
    $aocProc = Get-Process -Name "aoc_server" -ErrorAction SilentlyContinue | Select-Object -First 1
    $clientProc = Get-Process -Name "AOCClient-Win64-Shipping" -ErrorAction SilentlyContinue | Select-Object -First 1
    $authProc = Get-Process -Name "auth_server" -ErrorAction SilentlyContinue | Select-Object -First 1
    $tetherProc = Get-Process -Name "tether_server" -ErrorAction SilentlyContinue | Select-Object -First 1
    # The Go stack (launch_go.bat): its services and worldd from this checkout.
    # With worldd there is no aoc_server, so the run is named after them.
    $goProc = $null
    if ($GoStack) {
        $goProc = Get-Process -Name worldd, gateway -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -and $_.Path.StartsWith($RepoRoot, [System.StringComparison]::OrdinalIgnoreCase) } |
            Sort-Object StartTime | Select-Object -First 1
    }
    $runProc = if ($aocProc) { $aocProc } else { $goProc }

    $anyRunning = ($aocProc -or $clientProc -or $authProc -or $tetherProc -or $goProc)
    if ($anyRunning) {
        $sawProcessesRunning = $true
        $idleSecondsAfterStop = 0
    } elseif ($sawProcessesRunning) {
        $idleSecondsAfterStop += [math]::Max(1, [int]($IntervalMs / 1000))
    }

    # Find active run ID
    $runId = ""
    if ($runProc) {
        $runId = "run_" + $runProc.StartTime.ToString("yyyyMMdd_HHmmss")
    } else {
        $latestEmu = Get-ChildItem -Path $DistLogsDir -Filter "emu-*.log" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($latestEmu -and $latestEmu.Name -match "emu-(\d{8})-(\d{6})\.log") {
            $runId = "run_" + $matches[1] + "_" + $matches[2]
        }
    }

    if ($runId) {
        if ($runId -ne $currentRunId) {
            $currentRunId = $runId
            $lastSizes.Clear()
            Write-Host "[AutoLogger] Tracking run: $currentRunId" -ForegroundColor Green

            $targetDirs = @(
                (Join-Path $RepoRoot "logs\$currentRunId"),
                (Join-Path $PrivateLogsRoot $currentRunId),
                (Join-Path $DistLogsDir $currentRunId)
            )
            foreach ($td in $targetDirs) {
                if (-not (Test-Path $td)) { New-Item -ItemType Directory -Path $td -Force | Out-Null }
            }
        }

        $repoDest = Join-Path $RepoRoot "logs\$currentRunId"
        $privateDest = Join-Path $PrivateLogsRoot $currentRunId
        $distDest = Join-Path $DistLogsDir $currentRunId
        $allDests = @($repoDest, $privateDest, $distDest)

        $needsSummaryUpdate = $false

        # 1. Game Server
        $latestEmu = Get-ChildItem -Path $DistLogsDir -Filter "emu-*.log" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($latestEmu) {
            $szKey = "gs_" + $latestEmu.FullName
            $curSz = $latestEmu.Length
            if ($lastSizes[$szKey] -ne $curSz) {
                $lastSizes[$szKey] = $curSz
                foreach ($d in $allDests) {
                    Sync-FileSafe $latestEmu.FullName (Join-Path $d "game_server.txt") | Out-Null
                }
                Sync-FileSafe $latestEmu.FullName (Join-Path $PrivateLogsRoot "server_last.log") | Out-Null
                $needsSummaryUpdate = $true
            }
        }
# built by Cyb with C O A

        # 2. Tether Server
        $latestTether = Get-ChildItem -Path $DistLogsDir -Filter "tether-*.log" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($latestTether) {
            $szKey = "tether_" + $latestTether.FullName
            $curSz = $latestTether.Length
            if ($lastSizes[$szKey] -ne $curSz) {
                $lastSizes[$szKey] = $curSz
                foreach ($d in $allDests) {
                    Sync-FileSafe $latestTether.FullName (Join-Path $d "tether.txt") | Out-Null
                }
                $needsSummaryUpdate = $true
            }
        }

        # 3. Mock Auth Server (legacy stack; in the Go stack its log is a stale one)
        $authLog = Join-Path $DistLogsDir "mock_auth.log"
        if ($GoStack) {
            # 3b. The Go services and worldd: dist\services\logs\<name>.log
            foreach ($name in 'auth', 'character', 'citynode', 'mesh', 'gateway', 'worldd') {
                $svcLog = Join-Path $GoLogsDir "$name.log"
                if (-not (Test-Path $svcLog)) { continue }
                $fi = Get-Item $svcLog
                if ($lastSizes["go-$name"] -ne $fi.Length) {
                    $lastSizes["go-$name"] = $fi.Length
                    foreach ($d in $allDests) {
                        Sync-FileSafe $svcLog (Join-Path $d "go-$name.txt") | Out-Null
                    }
                    $needsSummaryUpdate = $true
                }
            }
        } elseif (Test-Path $authLog) {
            $fi = Get-Item $authLog
            $szKey = "auth"
            if ($lastSizes[$szKey] -ne $fi.Length) {
                $lastSizes[$szKey] = $fi.Length
                foreach ($d in $allDests) {
                    Sync-FileSafe $authLog (Join-Path $d "mock_auth.txt") | Out-Null
                }
                $needsSummaryUpdate = $true
            }
        } elseif ($authProc -and (Test-Path $DumpExe)) {
            $tempDump = [System.IO.Path]::GetTempFileName()
            try {
                & $DumpExe $authProc.Id $tempDump | Out-Null
                if (Test-Path $tempDump) {
                    $fi = Get-Item $tempDump
                    if ($fi.Length -gt 0 -and $lastSizes["auth_dump"] -ne $fi.Length) {
                        $lastSizes["auth_dump"] = $fi.Length
                        foreach ($d in $allDests) {
                            Sync-FileSafe $tempDump (Join-Path $d "mock_auth.txt") | Out-Null
                        }
                        $needsSummaryUpdate = $true
                    }
                }
            } finally {
                if (Test-Path $tempDump) { Remove-Item -LiteralPath $tempDump -Force -ErrorAction SilentlyContinue }
            }
        }

        # 4. Game Client
        if (Test-Path $ClientLogPath) {
            $fi = Get-Item $ClientLogPath
            $szKey = "client"
            if ($lastSizes[$szKey] -ne $fi.Length) {
                $lastSizes[$szKey] = $fi.Length
                foreach ($d in $allDests) {
                    Sync-FileSafe $ClientLogPath (Join-Path $d "game_client.txt") | Out-Null
                }
                $needsSummaryUpdate = $true
            }
        }

        # 5. Extras
        $bunchesLog = Join-Path $DistLogsDir "bunches_liveworld.log"
        if (Test-Path $bunchesLog) {
            $fi = Get-Item $bunchesLog
            if ($lastSizes["bunches"] -ne $fi.Length) {
                $lastSizes["bunches"] = $fi.Length
                foreach ($d in $allDests) {
                    Sync-FileSafe $bunchesLog (Join-Path $d "bunches_liveworld.txt") | Out-Null
                }
            }
        }

        $latestCapture = Get-ChildItem -Path $DistLogsDir -Filter "capture-*.jsonl" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($latestCapture) {
            if ($lastSizes["capture"] -ne $latestCapture.Length) {
                $lastSizes["capture"] = $latestCapture.Length
                foreach ($d in $allDests) {
                    Sync-FileSafe $latestCapture.FullName (Join-Path $d "capture.jsonl") | Out-Null
                }
            }
        }

        if (Test-Path $CefLogPath) {
            $fi = Get-Item $CefLogPath
            if ($lastSizes["cef"] -ne $fi.Length) {
                $lastSizes["cef"] = $fi.Length
                foreach ($d in $allDests) {
                    Sync-FileSafe $CefLogPath (Join-Path $d "cef3.txt") | Out-Null
                }
            }
        }

        # Update summary
        if ($needsSummaryUpdate) {
            $sumText = Generate-Summary $repoDest $currentRunId
            foreach ($d in $allDests) {
                Write-TextSafe $sumText (Join-Path $d "run_summary.txt")
            }
        }
    }

    # When all emulator/client processes have stopped for 15 seconds, exit cleanly
    if ($sawProcessesRunning -and $idleSecondsAfterStop -ge 15) {
        Write-Host "[AutoLogger] All processes stopped. Final logs saved to $currentRunId. Exiting." -ForegroundColor Cyan
        break
    }

    Start-Sleep -Milliseconds $IntervalMs
}
