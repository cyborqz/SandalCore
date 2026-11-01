# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

# ============================================================================
#  ci_run_tests.ps1 - run the test programs that need no game data (the test step of the CI job)
#
#  Usage (PowerShell):
#      .\scripts\ci_run_tests.ps1                      # run every dist\Release\test_*.exe except the ones listed in scripts\ci_needs_game_data.txt
#      .\scripts\ci_run_tests.ps1 -Config Debug
#      .\scripts\ci_run_tests.ps1 -LimitSeconds 120    # a time limit for each program (default 600)
#
#  The programs run one after the other from the repository root. A program passes when it exits with 0. A program that skipped part or all of
#  its checks (it prints a "[skip]" line or ends a line with ": skipped", and exits 0) passes and is listed. A program that exits with anything
#  else, or runs past the time limit, fails the run (exit 1), and the end of its output is printed.
#  The programs named in scripts\ci_needs_game_data.txt are not run: they need the game data (the content database, the stat corpus, the zone
#  tables), which a CI machine does not have, and the end of the output lists them with their reasons.
#  scripts\run_tests.ps1 runs everything, on a machine that has the game data. Build first (scripts\build.ps1 -Target ALL_BUILD).
#
#  Exit codes: 0 every program that ran passed, 1 a program failed or timed out, or none ran.
# ============================================================================
[CmdletBinding()]
param(
    [ValidateSet('Release','Debug')] [string] $Config = 'Release',
    [int]    $LimitSeconds = 600,
    [string] $ListFile = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
if (-not $ListFile) { $ListFile = Join-Path $root 'scripts\ci_needs_game_data.txt' }
$dist = Join-Path $root "dist\$Config"

# ---- the programs that need the game data --------------------------------------------------------------------------------------
$needsData = [ordered]@{}
foreach ($line in Get-Content -LiteralPath $ListFile) {
    $t = $line.Trim()
    if (-not $t -or $t.StartsWith('#')) { continue }
    $name = $t; $why = ''
    $i = $t.IndexOf('#')
    if ($i -ge 0) { $name = $t.Substring(0, $i).Trim(); $why = $t.Substring($i + 1).Trim() }
    if ($name -notmatch '^test_[A-Za-z0-9_]+$') { throw "bad line in ${ListFile}: $line" }
    $needsData[$name] = $why
}

$exes = @(Get-ChildItem -LiteralPath $dist -Filter 'test_*.exe' -ErrorAction SilentlyContinue | Sort-Object Name)
if ($exes.Count -eq 0) { Write-Host "no test_*.exe in $dist (build first: scripts\build.ps1 -Target ALL_BUILD)"; exit 1 }

# ---- the run -------------------------------------------------------------------------------------------------------------------
$env:Path = "$dist;$env:Path"      # the DLLs of the packages sit next to the programs
$passed = New-Object System.Collections.Generic.List[string]
$skippers = New-Object System.Collections.Generic.List[string]
$failed = New-Object System.Collections.Generic.List[string]
$notRun = New-Object System.Collections.Generic.List[string]
Push-Location $root
try {
    foreach ($exe in $exes) {
        $name = $exe.BaseName
        if ($needsData.Contains($name)) { $notRun.Add($name); continue }
        $outFile = [IO.Path]::GetTempFileName()
        $errFile = [IO.Path]::GetTempFileName()
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $p = Start-Process -FilePath $exe.FullName -WorkingDirectory $root -RedirectStandardOutput $outFile -RedirectStandardError $errFile -PassThru -WindowStyle Hidden
        $null = $p.Handle          # keeps the process handle, so that ExitCode is known after the program ends
        $timedOut = -not $p.WaitForExit($LimitSeconds * 1000)
        if ($timedOut) { try { $p.Kill() } catch { } ; $code = -1 } else { $code = $p.ExitCode }
        $sw.Stop()
        $text = [IO.File]::ReadAllText($outFile) + [IO.File]::ReadAllText($errFile)
        Remove-Item -LiteralPath $outFile, $errFile -Force -ErrorAction SilentlyContinue
        $seconds = [Math]::Round($sw.Elapsed.TotalSeconds, 1)
        if ($code -eq 0) {
            $skipped = $text -match '(?m)\[skip\]|:\s+skipped\s*$'
            if ($skipped) { $skippers.Add($name) } else { $passed.Add($name) }
            Write-Host ('{0,-6} {1,-34} {2,6} s' -f $(if ($skipped) { 'SKIP' } else { 'PASS' }), $name, $seconds)
        } else {
            $failed.Add($name)
            Write-Host ('{0,-6} {1,-34} {2,6} s  exit={3}' -f $(if ($timedOut) { 'TIMEOUT' } else { 'FAIL' }), $name, $seconds, $code)
            $tail = @($text -split "`r?`n" | Where-Object { $_.Trim() } | Select-Object -Last 40)
            foreach ($l in $tail) { Write-Host "         $l" }
        }
    }
} finally { Pop-Location }

# ---- the summary ---------------------------------------------------------------------------------------------------------------
foreach ($n in $needsData.Keys) {
    if (-not (Test-Path -LiteralPath (Join-Path $dist "$n.exe"))) { Write-Host "NOTE: $n is in scripts\ci_needs_game_data.txt but was not built" }
}
$ran = $passed.Count + $skippers.Count + $failed.Count
Write-Host ''
Write-Host ('{0} test programs ran: {1} passed, {2} of them skipped part or all of their checks, {3} failed' -f $ran, ($passed.Count + $skippers.Count), $skippers.Count, $failed.Count)
if ($skippers.Count -gt 0) { Write-Host ('skipped part or all of their checks (a skip line, exit 0): ' + ($skippers -join ', ')) }
Write-Host ('{0} test programs not run, they need the game data (scripts\ci_needs_game_data.txt): {1}' -f $notRun.Count, ($notRun -join ', '))

if ($env:GITHUB_STEP_SUMMARY) {
    $md = New-Object System.Collections.Generic.List[string]
    $md.Add('### Tests without game data')
    $md.Add(('{0} test programs ran: {1} passed ({2} of them skipped part or all of their checks), {3} failed.' -f $ran, ($passed.Count + $skippers.Count), $skippers.Count, $failed.Count))
    if ($failed.Count -gt 0) { $md.Add(''); $md.Add('Failed: ' + ($failed -join ', ')) }
    $md.Add('')
    $md.Add(('{0} test programs were not run because they need the game data:' -f $notRun.Count))
    $md.Add('')
    $md.Add('| Program | Needs |')
    $md.Add('| --- | --- |')
    foreach ($n in $notRun) { $md.Add(('| {0} | {1} |' -f $n, $needsData[$n])) }
    Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Value $md -Encoding ascii
}
if ($ran -eq 0) { Write-Host 'no test program ran'; exit 1 }
if ($failed.Count -gt 0) { exit 1 }
exit 0
