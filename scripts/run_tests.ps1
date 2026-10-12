# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

# ============================================================================
#  run_tests.ps1 - build all targets and run every dist\<Config>\test_*.exe
#
#  Usage (PowerShell):
#      .\scripts\run_tests.ps1                         # build + run
#      .\scripts\run_tests.ps1 -NoBuild                # run only
#      .\scripts\run_tests.ps1 -SaveBaseline FILE      # write summary to FILE
#      .\scripts\run_tests.ps1 -CompareTo FILE         # exit 1 if results differ
#      .\scripts\run_tests.ps1 -Heavy                  # also run the heavy tests (below)
#
#  The heavy tests (test_worldd_region_load: 1 GB of ground, about a minute, wall-clock checks) are NOT in the default gate: on a PC that is also building or
#  playing a timing check can flip, and the PC hard-resets under load.  -Heavy runs them (G1 review B I1); the plan's last task and the lead do, alone, under the
#  heavy-job lock.  fixtures/test-baseline.txt does not list them.
#
#  A test that skipped part or all of its checks (no content.db, no ground file: it prints a "[skip]" line or ends a line with ": skipped", and exits 0) is reported in a NOTE after
#  the summary; the compared lines stay "PASS exit=0" (review B I1).
#
#  -NoBuild runs whatever test_*.exe is already present in dist\<Config>,
#  including leftovers from a test target since dropped from CMake.
# ============================================================================
[CmdletBinding()]
param(
    [ValidateSet('Release','Debug')] [string] $Config = 'Release',
    [switch] $NoBuild,
    [string] $SaveBaseline,
    [string] $CompareTo,
    [switch] $Heavy
)
$ErrorActionPreference = 'Stop'
if ($SaveBaseline -and $CompareTo) { throw "use -SaveBaseline or -CompareTo, not both" }
$root  = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$build = Join-Path $root 'build\x64'
$dist  = Join-Path $root "dist\$Config"

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) { $cmake = 'C:\Program Files\CMake\bin\cmake.exe' }
# ~ CoA :: C Y B ~

if (-not $NoBuild) {
    # Drop stale test exes first: a test target dropped from CMake in a later
    # change would otherwise keep "passing" using its old binary. Test exes
    # link straight into dist\<Config> via CMAKE_RUNTIME_OUTPUT_DIRECTORY, so
    # MSBuild relinks the current ones below.
    Remove-Item (Join-Path $dist 'test_*.exe') -ErrorAction SilentlyContinue
    if (-not (Test-Path (Join-Path $build 'CMakeCache.txt'))) {
        & (Join-Path $PSScriptRoot 'build.ps1') -Config $Config -Configure
    }
    & $cmake --build $build --config $Config -- /m /v:minimal /nologo
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

# Windows PowerShell 5.1 turns a native program's redirected stderr into a
# terminating NativeCommandError when $ErrorActionPreference is 'Stop'.
# Failing tests write to stderr, so run them under 'Continue' and keep only
# the exit code.
$ErrorActionPreference = 'Continue'
Push-Location $root
try {
    $heavyTests = @('test_worldd_region_load')
    $skippers = New-Object System.Collections.Generic.List[string]
    $lines = @(foreach ($exe in Get-ChildItem $dist -Filter 'test_*.exe' | Where-Object { $Heavy -or ($heavyTests -notcontains $_.BaseName) } | Sort-Object Name) {
        # test_worldd_node_pop has a half that is opt-in because it reads the baked ground (AOC_REAL_GROUND=1: the five city nodes' bowls on the real heightfield, 0.2 s): it runs whenever
        # dist\content\area_nav.bin is there (Nodes 1 review B, M7: the town floor's constants are tuned on exactly that data)
        $realGround = $exe.BaseName -eq 'test_worldd_node_pop' -and (Test-Path -LiteralPath (Join-Path $root 'dist\content\area_nav.bin'))
        if ($realGround) { $env:AOC_REAL_GROUND = '1' }
        $text = & $exe.FullName 2>&1 | Out-String
        $code = $LASTEXITCODE
        if ($realGround) { Remove-Item Env:AOC_REAL_GROUND -ErrorAction SilentlyContinue }
        if ($code -eq 0 -and $text -match '(?m)\[skip\]|:\s+skipped\s*$') { $skippers.Add($exe.BaseName) }
        $status = if ($code -eq 0) { 'PASS' } else { 'FAIL' }
        '{0,-36} {1} exit={2}' -f $exe.BaseName, $status, $code
    })
} finally { Pop-Location; $ErrorActionPreference = 'Stop' }

if ($lines.Count -eq 0) { throw "no test_*.exe in $dist" }

$lines | ForEach-Object { Write-Host $_ }
if (-not $Heavy) { Write-Host ("NOTE: not run (heavy, -Heavy): " + ($heavyTests -join ', ')) }
if ($skippers.Count -gt 0) { Write-Host ("NOTE: skipped part or all of their checks (a skip line, exit 0): " + ($skippers -join ', ')) }
$failed = @($lines | Where-Object { $_ -match ' FAIL ' }).Count
Write-Host ("{0} tests, {1} failing" -f $lines.Count, $failed)

if ($SaveBaseline) { $lines | Set-Content -Encoding utf8 $SaveBaseline }
if ($CompareTo) {
    $diff = Compare-Object @(Get-Content -LiteralPath $CompareTo) $lines
    if ($diff) {
        Write-Host 'DIFFERS FROM BASELINE:'
        $diff | ForEach-Object { Write-Host ("  {0} {1}" -f $_.SideIndicator, $_.InputObject) }
        exit 1
    }
    Write-Host 'Matches baseline.'
}
exit 0
