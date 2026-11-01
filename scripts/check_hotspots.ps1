# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

# ============================================================================
#  check_hotspots.ps1 - the files that must not grow
#
#  The biggest files of the world server were split into one file per feature, so that parallel work does not collide in them (server/worldd/session/rpc_table.h and
#  server/worldd/tests/README.md say how a new feature registers itself instead).  scripts/hotspot_limits.txt lists the files that were split and the most lines each may
#  have.  A file over its limit is a finding: the new code goes into a file of its own (CONTRIBUTING.md, "Where New Code Goes").  A limit is raised by hand, in the commit
#  that needs it, so that a reviewer sees it.
#
#  Usage:
#      .\scripts\check_hotspots.ps1                    # prints the findings and exits 0 (warnings only)
#      .\scripts\check_hotspots.ps1 -Mode Fail         # exits 1 when a file is over its limit
#      .\scripts\check_hotspots.ps1 -Root <dir> -Limits <file>
#
#  A line of the limits file is "<most lines> <path from the repository root>"; empty lines and lines that start with # are ignored.  A listed file that does not exist is
#  a finding too (the list names a file that was renamed or removed).  On GitHub's machines each finding is also written as a workflow annotation.
#  Exit codes: 0 clean, or findings in Warn mode; 1 findings in Fail mode; 2 the limits file cannot be read.
# ============================================================================
[CmdletBinding()]
param(
    [string] $Root = '',
    [string] $Limits = '',
    [ValidateSet('Warn', 'Fail')] [string] $Mode = 'Warn'
)
$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = Split-Path -Parent $PSScriptRoot }
if (-not $Limits) { $Limits = Join-Path $PSScriptRoot 'hotspot_limits.txt' }
if (-not (Test-Path -LiteralPath $Limits -PathType Leaf)) {
    Write-Output "ERROR: the limits file '$Limits' does not exist"
    exit 2
}
$onGitHub = ($env:GITHUB_ACTIONS -eq 'true')
$annotation = if ($Mode -eq 'Fail') { 'error' } else { 'warning' }
$findings = 0
$checked = 0
foreach ($raw in [System.IO.File]::ReadAllLines($Limits)) {
    $line = $raw.Trim()
    if ($line -eq '' -or $line.StartsWith('#')) { continue }
    if ($line -notmatch '^(\d+)\s+(\S+)$') {
        Write-Output "ERROR: a line of $Limits is not '<most lines> <path>': $line"
        exit 2
    }
    $max = [int]$Matches[1]
    $path = $Matches[2]
    $full = Join-Path $Root ($path -replace '/', [IO.Path]::DirectorySeparatorChar)
    $checked++
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
        $findings++
        Write-Output "HOTSPOT ${path}: the file does not exist; take it out of the limits file"
        if ($onGitHub) { Write-Output "::${annotation} file=${path},title=Listed file is missing::${path} is listed in scripts/hotspot_limits.txt and does not exist" }
        continue
    }
    $count = [System.IO.File]::ReadAllLines($full).Length
    if ($count -gt $max) {
        $findings++
        $over = $count - $max
        Write-Output "HOTSPOT ${path}: $count lines, $over over its limit of ${max}: put the new code into a file of its own"
        if ($onGitHub) { Write-Output "::${annotation} file=${path},title=File over its limit::${path} has $count lines, $over over its limit of ${max}. Put the new code into a file of its own (CONTRIBUTING.md, Where New Code Goes)." }
    }
}
if ($findings -eq 0) {
    Write-Output "CLEAN: $checked file(s) within their limits"
    exit 0
}
Write-Output "FINDINGS: $findings of $checked file(s) ($Mode mode)"
if ($Mode -eq 'Fail') { exit 1 }
exit 0
