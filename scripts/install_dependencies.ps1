# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

<#
.SYNOPSIS
  Checks for everything Sandal Core needs to build and run, and installs what is missing.

.DESCRIPTION
  Checks: winget, Visual Studio 2022 Build Tools (C++ workload), CMake, Git, vcpkg, Go, the .NET 8 SDK (mapc), Python 3.10 or newer (the
  generators of extract_game_data.bat) and Docker Desktop (PostgreSQL + NATS for the Go services).
  Missing items are installed with winget after you confirm each one (or all at once with -Yes).
  vcpkg is cloned to %USERPROFILE%\vcpkg and VCPKG_ROOT is set for your user.
  The C++ libraries themselves (grpc, protobuf, openssl, ...) are installed by vcpkg during the first build;
  that first build takes a long time (grpc compiles from source).

.PARAMETER CheckOnly
  Only report what is installed and what is missing; change nothing.
.PARAMETER Yes
  Install everything missing without asking.
.PARAMETER SkipDocker
  Do not offer Docker Desktop (you run PostgreSQL and NATS some other way).
#>
[CmdletBinding()]
param(
    [switch]$CheckOnly,
    [switch]$Yes,
    [switch]$SkipDocker
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent

function Find-Winget {
    $c = Get-Command winget -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($c) { return $c.Source }
    $alias = Join-Path $env:LOCALAPPDATA 'Microsoft\WindowsApps\winget.exe'
    if (Test-Path $alias) { return $alias }
    return $null
}

function Update-SessionPath {
    $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' +
                [Environment]::GetEnvironmentVariable('Path', 'User')
}

function Get-VsWhere {
    foreach ($p in @("${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe",
                     "$env:ProgramFiles\Microsoft Visual Studio\Installer\vswhere.exe")) {
        if (Test-Path $p) { return $p }
    }
    return $null
}

function Find-VcTools {
    $vswhere = Get-VsWhere
    if (-not $vswhere) { return $null }
    $path = & $vswhere -latest -products * -version '[17.0,18.0)' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if ($path) { return ($path | Select-Object -First 1) }
    return $null
}

function Find-CMake {
    $c = Get-Command cmake -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($c) { return $c.Source }
    $vs = Find-VcTools
    if ($vs) {
        $p = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        if (Test-Path $p) { return $p }
    }
    return $null
}

# -- C-Y-B :: C.O.A --

function Find-Vcpkg {
    # Same search order as scripts\build.ps1
    if ($env:VCPKG_ROOT -and (Test-Path (Join-Path $env:VCPKG_ROOT 'scripts\buildsystems\vcpkg.cmake'))) {
        return $env:VCPKG_ROOT
    }
    $userRoot = [Environment]::GetEnvironmentVariable('VCPKG_ROOT', 'User')
    if ($userRoot -and (Test-Path (Join-Path $userRoot 'scripts\buildsystems\vcpkg.cmake'))) { return $userRoot }
    $drive = Split-Path $repoRoot -Qualifier
    $candidates = @(
        (Join-Path $repoRoot '..\vcpkg'),
        (Join-Path $drive '\vcpkg'),
        'C:\vcpkg',
        "$env:USERPROFILE\vcpkg",
        "$env:USERPROFILE\source\repos\vcpkg"
    )
    $vs = Find-VcTools
    if ($vs) { $candidates += (Join-Path $vs 'VC\vcpkg') }
    foreach ($p in $candidates) {
        if (Test-Path (Join-Path $p 'scripts\buildsystems\vcpkg.cmake')) { return (Resolve-Path $p).Path }
    }
    return $null
}

function Get-GoVersion {
    $go = Get-Command go -ErrorAction SilentlyContinue
    if (-not $go) { return $null }
    $v = & go env GOVERSION 2>$null
    if ($v) { return $v.Trim() }
    return 'unknown'
}

function Get-GoRequired {
    $mod = Join-Path $repoRoot 'services\go.mod'
    if (Test-Path $mod) {
        $line = Select-String -Path $mod -Pattern '^go\s+(\S+)' | Select-Object -First 1
        if ($line) { return $line.Matches[0].Groups[1].Value }
    }
    return $null
}

function Find-DotnetSdk8 {
    $dn = Get-Command dotnet -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $dn) { return $null }
    foreach ($s in @(& $dn.Source --list-sdks 2>$null)) { if ($s -match '^(\d+)\.' -and [int]$Matches[1] -eq 8) { return $dn.Source } }
    return $null
}

function Find-Python310 {
    foreach ($try in @('python', 'py')) {
        $c = Get-Command $try -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $c) { continue }
        [string[]]$prefix = @()
        if ($try -eq 'py') { $prefix = @('-3') }
        $oldPref = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $pyArgs = @($prefix) + @('-c', 'import sys; print(sys.version_info[0], sys.version_info[1])')
            $res = @(& $c.Source @pyArgs 2>$null)
            $code = $LASTEXITCODE
            $v = if ($res.Count -gt 0) { [string]$res[0] } else { '' }
            if ($code -eq 0 -and $v -match '^(\d+) (\d+)$' -and ([int]$Matches[1] -gt 3 -or ([int]$Matches[1] -eq 3 -and [int]$Matches[2] -ge 10))) { return $c.Source }
        } catch { } finally { $ErrorActionPreference = $oldPref }
    }
    return $null
}

function Test-VersionAtLeast([string]$have, [string]$need) {
    try {
        $h = [version](($have -replace '^go', '') -replace '[^0-9.].*$', '')
        $n = [version]($need -replace '[^0-9.].*$', '')
        return $h -ge $n
    } catch { return $true }
}

function Confirm-Install([string]$what) {
    if ($Yes) { return $true }
    $a = Read-Host "Install $what now? [Y/n]"
    return ($a -eq '' -or $a -match '^[Yy]')
}

function Install-Winget([string]$id, [string]$name, [string[]]$extra = @()) {
    Write-Host "Installing $name (winget id $id) ..." -ForegroundColor Cyan
    $wingetArgs = @('install', '--id', $id, '-e', '--source', 'winget',
              '--accept-package-agreements', '--accept-source-agreements') + $extra
    & $script:winget @wingetArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "winget could not install $name (exit $LASTEXITCODE). Install it by hand, then run this script again."
        return $false
    }
    Update-SessionPath
    return $true
}

# ---------------------------------------------------------------- check
# ~ CoA x C | Y | B ~
$goNeed = Get-GoRequired
$items = [ordered]@{}
$script:winget = Find-Winget
$items['VS 2022 C++']   = [bool](Find-VcTools)
$items['CMake']         = [bool](Find-CMake)
$items['Git']           = [bool](Get-Command git -ErrorAction SilentlyContinue)
$items['vcpkg']         = [bool](Find-Vcpkg)
$goHave = Get-GoVersion
$items['Go']            = [bool]($goHave -and (-not $goNeed -or (Test-VersionAtLeast $goHave $goNeed)))
$items['.NET 8 SDK']    = [bool](Find-DotnetSdk8)
$items['Python 3.10+']  = [bool](Find-Python310)
$items['Docker']        = [bool](Get-Command docker -ErrorAction SilentlyContinue)

Write-Host ''
Write-Host 'Sandal Core dependency check' -ForegroundColor White
Write-Host '----------------------------'
foreach ($k in $items.Keys) {
    $state = if ($items[$k]) { 'OK' } else { 'MISSING' }
    $color = if ($items[$k]) { 'Green' } else { 'Yellow' }
    $note = ''
    if ($k -eq 'Go' -and $goHave) { $note = "  (have $goHave, need $goNeed or newer)" }
    if ($k -eq 'Docker' -and $SkipDocker) { $note = '  (skipped: -SkipDocker)' }
    Write-Host ("  {0,-14} {1}{2}" -f $k, $state, $note) -ForegroundColor $color
}
Write-Host ''

$missing = @($items.Keys | Where-Object { -not $items[$_] -and -not ($_ -eq 'Docker' -and $SkipDocker) })
$wingetNote = if ($script:winget) { 'winget: OK' } else { 'winget: MISSING (needed only to install missing items)' }
Write-Host "  $wingetNote" 
if ($CheckOnly) {
    if ($missing.Count) { Write-Host "Missing: $($missing -join ', ')"; exit 1 }
    Write-Host 'Everything required is installed.'; exit 0
}
if (-not $missing.Count) {
    Write-Host 'Everything is installed. Next: build_all.bat, then launch_worldd.bat.' -ForegroundColor Green
    exit 0
}

# ---------------------------------------------------------------- install
if (-not $script:winget) {
    Write-Warning 'winget is missing. Install "App Installer" from the Microsoft Store, then run this script again.'
    exit 1
}

if (-not $items['VS 2022 C++'] -and (Confirm-Install 'Visual Studio 2022 Build Tools with the C++ workload (several GB)')) {
    [void](Install-Winget 'Microsoft.VisualStudio.2022.BuildTools' 'Visual Studio 2022 Build Tools' @(
        '--override', '--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'))
}
if (-not (Find-CMake) -and (Confirm-Install 'CMake')) {
    [void](Install-Winget 'Kitware.CMake' 'CMake')
}
if (-not (Get-Command git -ErrorAction SilentlyContinue) -and (Confirm-Install 'Git')) {
    [void](Install-Winget 'Git.Git' 'Git')
}

# -- C.O.A / Cyb --
if (-not (Find-Vcpkg) -and (Confirm-Install "vcpkg (cloned to $env:USERPROFILE\vcpkg)")) {
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        Write-Warning 'Git is needed to get vcpkg. Install Git, then run this script again.'
    } else {
        $dest = Join-Path $env:USERPROFILE 'vcpkg'
        if (-not (Test-Path $dest)) { & git clone https://github.com/microsoft/vcpkg $dest }
        & (Join-Path $dest 'bootstrap-vcpkg.bat') -disableMetrics
        if ($LASTEXITCODE -eq 0) {
            [Environment]::SetEnvironmentVariable('VCPKG_ROOT', $dest, 'User')
            $env:VCPKG_ROOT = $dest
            Write-Host "vcpkg ready at $dest (VCPKG_ROOT set for your user)." -ForegroundColor Green
        } else {
            Write-Warning 'vcpkg bootstrap failed; see the output above.'
        }
    }
}

if (-not $items['Go'] -and (Confirm-Install "Go $goNeed or newer")) {
    [void](Install-Winget 'GoLang.Go' 'Go')
}
if (-not $SkipDocker -and -not $items['Docker'] -and
    (Confirm-Install 'Docker Desktop (runs PostgreSQL and NATS for the Go services; may need WSL2 and a reboot)')) {
    [void](Install-Winget 'Docker.DockerDesktop' 'Docker Desktop')
}
if (-not $items['.NET 8 SDK'] -and (Confirm-Install '.NET 8 SDK (builds mapc, which reads your game files)')) {
    [void](Install-Winget 'Microsoft.DotNet.SDK.8' '.NET 8 SDK')
}
if (-not $items['Python 3.10+'] -and (Confirm-Install 'Python 3.12 (runs the generators of extract_game_data.bat)')) {
    [void](Install-Winget 'Python.Python.3.12' 'Python 3.12')
}

Write-Host ''
Write-Host 'Done. Open a NEW terminal (so PATH changes apply) and run this script with -CheckOnly to confirm.' -ForegroundColor Green
Write-Host 'Then: build_all.bat (it builds the game data from your own Ashes of Creation install, then the programs; the first build downloads and compiles the C++ libraries and can take a long time),'
Write-Host 'then: launch_worldd.bat'
exit 0
