# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

# ============================================================================
#  audit_node_log.ps1 - what a run's client log says about city nodes (Nodes 1, stage 2)
#
#  Counts the lines the node work is meant to remove from the game client's log (E1's audit found about 980 and 30 per run):
#      Record not found: CityNodeRecord        the zone banner asking for a node the GameState does not list
#      LogAoC_NodeAudio ... Error/Warning      a node audio component finding no node for its zone
#      Requesting Node Data with invalid Node ID   the login's 49 requests not sent
#  and the lines that show the node system working:
#      LogAoC_NodeSpawning                     the replicator's node instantiation (stage 3)
#
#  Usage:
#      .\scripts\audit_node_log.ps1                      # the newest logs\run_*\game_client.txt under the repository
#      .\scripts\audit_node_log.ps1 -Log C:\path\game_client.txt
#  Exit code 0 when there is no "Record not found: CityNodeRecord" and no NodeAudio error, 1 otherwise (2: no log).
# ============================================================================
[CmdletBinding()]
param([string] $Log)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)

if (-not $Log) {
    $newest = Get-ChildItem -Path (Join-Path $repoRoot 'logs') -Recurse -Filter 'game_client.txt' -ErrorAction SilentlyContinue |
              Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($newest) { $Log = $newest.FullName }
}
if (-not $Log -or -not (Test-Path $Log)) {
    Write-Output 'no client log found (pass -Log, or run the game once so logs\run_*\game_client.txt exists)'
    exit 2
}

$text = Get-Content -Path $Log
function Count([string] $pattern) { return @($text | Select-String -Pattern $pattern).Count }

$record = Count 'Record not found: CityNodeRecord'
$audio = Count 'LogAoC_NodeAudio.*(Error|Warning)|Couldn.t find node guid for node audio'
$invalid = Count 'Requesting Node Data with invalid Node ID'
$spawning = Count 'LogAoC_NodeSpawning'

Write-Output "client log: $Log ($($text.Count) lines)"
Write-Output ("  Record not found: CityNodeRecord          {0,6}   (want 0)" -f $record)
Write-Output ("  NodeAudio errors and warnings             {0,6}   (want 0)" -f $audio)
Write-Output ("  Requesting Node Data with invalid Node ID {0,6}   (want 0)" -f $invalid)
Write-Output ("  LogAoC_NodeSpawning lines                 {0,6}   (the towns' scenery, stage 3)" -f $spawning)
if ($record -eq 0 -and $audio -eq 0) {
    Write-Output 'OK: the client finds its nodes.'
    exit 0
}
Write-Output 'NOT OK: the client still misses node records or node audio.'
exit 1
