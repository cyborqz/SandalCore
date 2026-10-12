# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

# ============================================================================
#  e2e_replay.ps1 - replay a captured client session at a throwaway aoc_server
#
#  Starts dist\Release\aoc_server.exe on game port 17777 / gRPC 4443 with its
#  working directory under build\e2e (so logs stay out of dist\), replays the
#  capture with tools\replay_capture.py, stops the server, and prints a report:
#  packets sent, packets received after the last one, key log-line counts, and
#  a digest of the decoded C>S lines (so wrong decoded values show up too).
#
#  Usage (PowerShell):
#      .\scripts\e2e_replay.ps1 -Capture FILE                              # print report
#      .\scripts\e2e_replay.ps1 -Capture FILE -CompareTo BASELINE          # the gate
#      .\scripts\e2e_replay.ps1 -Capture FILE -SaveBaseline BASELINE       # record a baseline
#  -Capture FILE is a capture.jsonl a live run wrote (logs\run_*\capture.jsonl);
#  no capture ships with the repository.
# ============================================================================
[CmdletBinding()]
param(
    [string] $Capture,
    [string[]] $ServerArgs = @(),
    [switch] $Realtime,
    [string] $SaveBaseline,
    [string] $CompareTo
)
$ErrorActionPreference = 'Stop'
if ($SaveBaseline -and $CompareTo) { throw "use -SaveBaseline or -CompareTo, not both" }
if ($CompareTo -and -not (Test-Path -LiteralPath $CompareTo)) { throw "baseline not found: $CompareTo" }
# A busy port does not make aoc_server exit, it just splits or loses the traffic.
if (Get-NetUDPEndpoint -LocalPort 17777 -ErrorAction SilentlyContinue) { throw "UDP 17777 is already in use (leftover aoc_server?)" }
if (Get-NetTCPConnection -LocalPort 4443 -State Listen -ErrorAction SilentlyContinue) { throw "TCP 4443 is already in use" }
$root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
if (-not $Capture) { throw "pass -Capture FILE (a capture.jsonl from logs\run_*\): no capture ships with the repository" }
$work = Join-Path $root 'build\e2e'
$logs = Join-Path $work 'logs'
$capturePath = (Resolve-Path -LiteralPath $Capture).Path
New-Item -ItemType Directory -Force $logs | Out-Null
$before = @(Get-ChildItem $logs -Filter 'emu-*.log' | ForEach-Object Name)

$serverArgv = @('--use-embedded-bootstrap','--enable-live-world','--port','4443','--game-port','17777') + $ServerArgs
$p = Start-Process -FilePath (Join-Path $root 'dist\Release\aoc_server.exe') `
     -ArgumentList $serverArgv -WorkingDirectory $work -WindowStyle Hidden -PassThru
try {
    Start-Sleep -Seconds 3
    if ($p.HasExited) {
        # Say why: the reason is only in the new emulator log (e.g. a fresh
        # checkout without OpenSSL on PATH cannot make its TLS certificate).
        $emu = Get-ChildItem $logs -Filter 'emu-*.log' | Where-Object { $before -notcontains $_.Name } |
            Sort-Object LastWriteTime | Select-Object -Last 1
        $tail = if ($emu) { (Get-Content $emu.FullName -Tail 12) -join "`n" } else { '(it wrote no log)' }
        throw "aoc_server exited during startup (exit $($p.ExitCode)). The end of its log:`n$tail"
    }
    $h = @($capturePath, '--port', '17777')
    if ($Realtime) { $h += '--realtime' }
    # stdout is captured for parsing; the harness's stderr goes straight to the console.
    $harness = @(python (Join-Path $root 'tools\replay_capture.py') @h)
    $harnessExit = $LASTEXITCODE
    $diedDuringReplay = $p.HasExited
} finally {
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; $null = $p.WaitForExit(5000) }
}
$harness | ForEach-Object { Write-Host $_ }
if ($diedDuringReplay) { throw "aoc_server exited during the replay (exit $($p.ExitCode))" }
if ($harnessExit -ne 0) { throw "replay_capture.py failed (exit $harnessExit)" }
# -- C-Y-B . C | O | A --

$summary = $harness | Select-String 'sent (\d+) C>S packets, .* (\d+) after last packet' | Select-Object -Last 1
if (-not $summary) { throw "replay_capture.py printed no summary line" }
$newLogs = @(Get-ChildItem $logs -Filter 'emu-*.log' | Where-Object { $before -notcontains $_.Name })
if ($newLogs.Count -ne 1) { throw "expected one new emu-*.log under $logs, found $($newLogs.Count)" }
$log = $newLogs[0]
Write-Host "server log: $($log.FullName)"

$report = @(
    '  {0,-34} {1}' -f 'C>S packets sent', $summary.Matches[0].Groups[1].Value
    '  {0,-34} {1}' -f 'S>C packets after last', $summary.Matches[0].Groups[2].Value
)
foreach ($pat in 'unparsed bits','did not decode','SERVER UPDATE LEVEL VISIBILITY','\[C>S-RPC\]','ServerMove parsed','closed the connection','\[error\]') {
    $n = @(Select-String -LiteralPath $log.FullName -Pattern $pat).Count
    $report += '  {0,-34} {1}' -f $pat, $n
}
# Counts miss a decoder that still logs its line but with wrong values, so also
# hash the decoded lines (timestamp and client address removed).
$decoded = @(Select-String -LiteralPath $log.FullName -Pattern 'SERVER UPDATE LEVEL VISIBILITY','\[C>S-RPC\]','ServerMove parsed','unparsed bits','closed the connection' |
             ForEach-Object { ($_.Line -replace '^\[[^\]]*\] ', '') -replace '127\.0\.0\.1:\d+', 'ADDR' })
$sha = [Security.Cryptography.SHA256]::Create()
$digest = -join ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($decoded -join "`n")) | ForEach-Object { $_.ToString('x2') })
$report += '  {0,-34} {1}' -f 'decoded-line digest', $digest.Substring(0, 16)
$report | ForEach-Object { Write-Host $_ }

if ($SaveBaseline) { $report | Set-Content -Encoding utf8 $SaveBaseline }
if ($CompareTo) {
    $diff = Compare-Object @(Get-Content -LiteralPath $CompareTo) $report
    if ($diff) {
        Write-Host 'DIFFERS FROM BASELINE:'
        $diff | ForEach-Object { Write-Host ("  {0} {1}" -f $_.SideIndicator, $_.InputObject) }
        exit 1
    }
    Write-Host 'Matches baseline.'
}
exit 0
