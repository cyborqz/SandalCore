# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

<#
.SYNOPSIS
  Installs Sandal Core on this PC: the tools, the game data and the programs, in that order. Safe to run again.

.DESCRIPTION
  install.bat runs this script. It stops at the first step that fails and exits with that step's exit code:

    1  The tools      scripts\install_dependencies.ps1 checks Visual Studio 2022 Build Tools (C++), CMake, Git, vcpkg, Go, the .NET 8 SDK,
                      Python 3.10 or newer and Docker, and offers to install what is missing with winget, asking before each item.
    2  The game data  scripts\extract_game_data.ps1 builds the game data from your own Ashes of Creation install. It skips every step that is up to date.
    3  The programs   scripts\build.ps1 -Target all builds tether_server, launcher and worldd into dist\<Config>. It builds only what changed.

  Run it again after a game patch or an update: it does only what changed. launch.bat starts the server once this has finished.

  With -CheckOnly it only reports the three (the tools, the steps of the game data that would run, whether the programs are built) and changes nothing.
  Its exit code is then 0 when nothing is left to do and 1 when something is.

.PARAMETER CheckOnly
  Only report; change nothing.
.PARAMETER GameRoot
  The game's "Game" folder (the one that holds AOCClient.exe and AOC\), for a game in an unusual place. It counts for that run only; GAME_ROOT keeps it for good.
.PARAMETER Yes
  Install every missing tool without asking.
.PARAMETER SkipDocker
  Do not offer Docker Desktop (you run PostgreSQL and NATS some other way).
.PARAMETER Config
  The build configuration (default Release; launch.bat starts the Release build).
.PARAMETER Clean
  Build from scratch (removes build\x64 first).
.PARAMETER Configure
  Run CMake again.
.PARAMETER Test
  Run scripts\run_tests.ps1 after the build.
.PARAMETER Jobs
  Build at most this many projects and this many compiler processes at a time (default: every core, and as many compilers as the free memory allows).
#>
[CmdletBinding()]
param(
    [switch]$CheckOnly,
    [string]$GameRoot,
    [switch]$Yes,
    [switch]$SkipDocker,
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Config = 'Release',
    [switch]$Clean,
    [switch]$Configure,
    [switch]$Test,
    [int]$Jobs = 0
)

$ErrorActionPreference = 'Stop'
$repoRoot  = Split-Path $PSScriptRoot -Parent
$hostExe   = (Get-Process -Id $PID).Path          # the PowerShell that runs this script runs the steps too
$stepCount = 3
$clock     = [Diagnostics.Stopwatch]::StartNew()
$script:rc = 0
if ($GameRoot) { $GameRoot = $GameRoot.Trim().Trim('"').TrimEnd('\', '/') }
$distDir  = Join-Path $repoRoot "dist\$Config"
$programs = @('tether_server.exe', 'launcher.exe', 'worldd.exe')

function Write-Step([int]$n, [string]$title) {
    Write-Host ''
    Write-Host ("Step {0} of {1}: {2}" -f $n, $stepCount, $title) -ForegroundColor Cyan
    Write-Host ('-' * 60)
}

# Runs a script that sits next to this one in a new PowerShell that shares this console, so its questions, colours and exit code come through.
function Invoke-Script([string]$name, [string[]]$scriptArgs = @()) {
    & $hostExe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot $name) @scriptArgs
    $script:rc = $LASTEXITCODE
}

# The same, but returns what the script printed (the caller shows it).
function Invoke-ScriptCaptured([string]$name, [string[]]$scriptArgs = @()) {
    $lines = @(& $hostExe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot $name) @scriptArgs)
    $script:rc = $LASTEXITCODE
    return $lines
}

# What a new window would see after the installs: the folders winget added to PATH, and the folder of vcpkg.
function Update-SessionEnv {
    $have  = @($env:Path -split ';' | Where-Object { $_ })
    $fresh = @(([Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')) -split ';' |
        Where-Object { $_ -and ($have -notcontains $_) })
    if ($fresh.Count -gt 0) { $env:Path = $env:Path.TrimEnd(';') + ';' + ($fresh -join ';') }
    $vcpkg = [Environment]::GetEnvironmentVariable('VCPKG_ROOT', 'User')
    if ($vcpkg) { $env:VCPKG_ROOT = $vcpkg }
}

function Format-Elapsed([TimeSpan]$t) {
    if ($t.TotalMinutes -ge 1.5) { return ('{0:N1} min' -f $t.TotalMinutes) }
    return ('{0:N0} s' -f $t.TotalSeconds)
}

function Stop-Install([int]$n, [string]$what, [int]$code, [string[]]$more = @()) {
    if ($code -eq 0) { $code = 1 }
    Write-Host ''
    Write-Host ("The install stopped at step {0} of {1} ({2}), exit code {3}." -f $n, $stepCount, $what, $code) -ForegroundColor Red
    foreach ($m in $more) { Write-Host $m -ForegroundColor Yellow }
    Write-Host 'Run install.bat again once the cause is fixed: what finished is kept, so it continues where it stopped.' -ForegroundColor Yellow
    exit $code
}

# ------------------------------------------------------------------------------------------------------------------------------------
# -CheckOnly: report the three steps, change nothing
# ------------------------------------------------------------------------------------------------------------------------------------
if ($CheckOnly) {
    Write-Host 'Sandal Core install check: this only reports, nothing is changed.' -ForegroundColor White

    Write-Step 1 'the tools'
    $depArgs = @('-CheckOnly')
    if ($SkipDocker) { $depArgs += '-SkipDocker' }
    Invoke-Script 'install_dependencies.ps1' $depArgs
    $toolsOk = ($script:rc -eq 0)

    Write-Step 2 'the game data'
    $exArgs = @('-CheckOnly')
    if ($GameRoot) { $exArgs += @('-GameRoot', $GameRoot) }
    $dataLines = @(Invoke-ScriptCaptured 'extract_game_data.ps1' $exArgs)
    $dataRc = $script:rc
    $wouldRun = New-Object System.Collections.Generic.List[string]
    foreach ($l in $dataLines) {
        Write-Host $l
        if ("$l" -match '^\s*(\S+)\s+would run\s*$') { $wouldRun.Add($Matches[1]) }
    }

    Write-Step 3 'the programs'
    $lacking = @($programs | Where-Object { -not (Test-Path -LiteralPath (Join-Path $distDir $_) -PathType Leaf) })
    if ($lacking.Count -eq 0) { Write-Host ("Present: dist\{0} has {1} (install.bat rebuilds what changed)." -f $Config, ($programs -join ', ')) }
    else { Write-Host ("Not built yet: dist\{0} lacks {1}." -f $Config, ($lacking -join ', ')) }

    $todo = New-Object System.Collections.Generic.List[string]
    Write-Host ''
    Write-Host 'Summary' -ForegroundColor White
    Write-Host ('-' * 60)
    if ($toolsOk) { Write-Host '  Tools       all installed' -ForegroundColor Green }
    else { Write-Host '  Tools       something is missing (listed above)' -ForegroundColor Yellow; $todo.Add('install the missing tools') }
    if ($dataRc -ne 0) { Write-Host '  Game data   could not be checked (see above)' -ForegroundColor Yellow; $todo.Add('check the game data') }
    elseif ($wouldRun.Count -gt 0) { Write-Host ("  Game data   {0} step(s) would run: {1}" -f $wouldRun.Count, ($wouldRun -join ', ')) -ForegroundColor Yellow; $todo.Add('make the game data') }
    else { Write-Host '  Game data   up to date' -ForegroundColor Green }
    if ($lacking.Count -gt 0) { Write-Host ("  Programs    not built (dist\{0} lacks {1})" -f $Config, ($lacking -join ', ')) -ForegroundColor Yellow; $todo.Add('build the programs') }
    else { Write-Host ("  Programs    present (dist\{0}); install.bat rebuilds what changed" -f $Config) -ForegroundColor Green }
    Write-Host ''
    if ($todo.Count -eq 0) {
        Write-Host 'Nothing is missing. After a git pull or a game patch run install.bat again: it rebuilds what changed. Next: launch.bat' -ForegroundColor Green
        exit 0
    }
    Write-Host ("Left to do: {0}. install.bat does all of it." -f ($todo -join ', ')) -ForegroundColor Yellow
    exit 1
}

# ------------------------------------------------------------------------------------------------------------------------------------
# Step 1: the tools
# ------------------------------------------------------------------------------------------------------------------------------------
Write-Step 1 'the tools'
$depArgs = @('-NoHints')
if ($Yes) { $depArgs += '-Yes' }
if ($SkipDocker) { $depArgs += '-SkipDocker' }
Invoke-Script 'install_dependencies.ps1' $depArgs
if ($script:rc -ne 0) { Stop-Install 1 'the tools' $script:rc }

# The build needs every tool but Docker (Docker only runs the servers). Check again with what a new window would see: an item that was
# declined, or one that needs a new window or a restart, stops the install here and not in the middle of a long build.
Update-SessionEnv
$verify = @(Invoke-ScriptCaptured 'install_dependencies.ps1' @('-CheckOnly', '-SkipDocker'))
if ($script:rc -ne 0) {
    $missingLine = @($verify | Where-Object { "$_" -match '^Missing:' }) | Select-Object -First 1
    Stop-Install 1 'the tools' $script:rc @(
        $(if ($missingLine) { "$missingLine" } else { 'Some tools are still missing.' }),
        'Install them (install.bat offers to), open a NEW window so that the new PATH applies, then run install.bat again.')
}
Write-Host 'The tools are in place.' -ForegroundColor Green

# ------------------------------------------------------------------------------------------------------------------------------------
# Step 2: the game data
# ------------------------------------------------------------------------------------------------------------------------------------
Write-Step 2 'the game data (built from your own Ashes of Creation install; what is up to date is skipped)'
$exArgs = @()
if ($GameRoot) { $exArgs += @('-GameRoot', $GameRoot) }
Invoke-Script 'extract_game_data.ps1' $exArgs
if ($script:rc -ne 0) {
    Stop-Install 2 'the game data' $script:rc @('The game data could not be built - see the messages above.',
        'If a tool is reported missing, run install.bat again in a new window.')
}

# ------------------------------------------------------------------------------------------------------------------------------------
# Step 3: the programs
# ------------------------------------------------------------------------------------------------------------------------------------
Write-Step 3 ("the programs ({0} x64; the first build also compiles the C++ libraries and can take a long time)" -f $Config)
$buildArgs = @('-Target', 'all', '-Config', $Config)
if ($Clean) { $buildArgs += '-Clean' }
if ($Configure) { $buildArgs += '-Configure' }
if ($Test) { $buildArgs += '-Test' }
if ($Jobs -gt 0) { $buildArgs += @('-Jobs', "$Jobs") }
Invoke-Script 'build.ps1' $buildArgs
if ($script:rc -ne 0) {
    Stop-Install 3 'the programs' $script:rc @($(if ($Test) { 'The build or its tests failed - see the messages above (the tests run after the build).' } else { 'The build failed - see the messages above.' }),
        'If a tool is reported missing, run install.bat again in a new window.')
}

Write-Host ''
Write-Host ("Install finished in {0}: the build tools are in place, the game data is up to date and the programs are built (dist\{1})." -f (Format-Elapsed $clock.Elapsed), $Config) -ForegroundColor Green
if (-not $SkipDocker -and -not (Get-Command docker -ErrorAction SilentlyContinue)) {
    Write-Host 'Docker Desktop is not installed, so nothing here starts PostgreSQL and NATS: run them yourself, or install Docker Desktop and run install.bat again.' -ForegroundColor Yellow
}
if ($Config -ne 'Release') { Write-Host 'launch.bat starts the Release build; this was a different configuration.' -ForegroundColor Yellow }
Write-Host 'Next: launch.bat' -ForegroundColor Green
exit 0
