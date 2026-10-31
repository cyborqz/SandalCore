# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

# ============================================================================
#  build.ps1 - portable build driver for this repository
#
#  Uses vswhere to help locate cmake, configures CMake if needed, then
#  builds the requested target(s) via `cmake --build`.  Paths are derived
#  from the script's location, so this script works regardless of where
#  the repo is cloned.
#
#  Usage:
#      .\scripts\build.ps1                  # build all default targets
#      .\scripts\build.ps1 -Target worldd
#      .\scripts\build.ps1 -Config Debug
#      .\scripts\build.ps1 -Clean           # clean first
#      .\scripts\build.ps1 -Test            # build, then run scripts\run_tests.ps1
#      .\scripts\build.ps1 -Jobs 6          # at most 6 projects and at most 6 compiler processes at a time
#                                          # (default: every core for projects, and as many compilers as the free memory allows)
#      .\scripts\build.ps1 -Configure      # run CMake again (this also switches an older build folder to the settings below)
#
#  What makes it fast:
#   - MSBuild's MultiToolTask compiles the files of one project in parallel (without it a project is one compiler call, one file after the other).
#     CL_MPCount holds the compiler processes at -Jobs; EnforceProcessCountAcrossBuilds asks MSBuild to share that limit between builds that run together.
#   - vcpkg's own step that copies the DLLs next to every program (a PowerShell run after each program) is off: the build folder is configured
#     with -DVCPKG_APPLOCAL_DEPS=OFF and the DLLs are copied once, after the build, into dist\<Config> (only the ones that are missing or older).
#     A build folder made earlier keeps the old step and works as before until -Configure; its libraries compile once more on the first build
#     with these settings.
# ============================================================================

[CmdletBinding()]
param(
    [string]   $Target  = 'all',
    [ValidateSet('Release','Debug','RelWithDebInfo','MinSizeRel')]
    [string]   $Config  = 'Release',
    [switch]   $Clean,
    [switch]   $Test,
    [switch]   $Configure,
    [int]      $Jobs = 0
)

$ErrorActionPreference = 'Stop'

# ── Resolve paths from script location ──────────────────────────────────
$repoRoot   = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$buildDir   = Join-Path $repoRoot 'build\x64'
$distDir    = Join-Path $repoRoot "dist\$Config"

Write-Host "Repo root: $repoRoot"
Write-Host "Build dir: $buildDir"
Write-Host "Config   : $Config"

# ── Find vswhere (used below to help locate cmake) ──────────────────────
$vswhere = "$env:ProgramFiles (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    $vswhere = "$env:ProgramFiles\Microsoft Visual Studio\Installer\vswhere.exe"
}
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found - Visual Studio 2022 (Build Tools) required."
}

# Find cmake (VS2022 ships it under Common7\IDE\CommonExtensions\...)
$cmake = Get-Command cmake -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source
if (-not $cmake) {
    $cmakeCandidate = & $vswhere -latest -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' |
                      Select-Object -First 1
    if ($cmakeCandidate -and (Test-Path $cmakeCandidate)) { $cmake = $cmakeCandidate }
}
if (-not $cmake) {
    throw "cmake.exe not found. Install CMake or use the VS2022 CMake component."
}
Write-Host "cmake    : $cmake"

# ── Clean ────────────────────────────────────────────────────────────────
if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Removing $buildDir ..."
    Remove-Item -Recurse -Force $buildDir
}
# == C | O | A | Cyb ==

# ── Locate vcpkg ────────────────────────────────────────────────────────
# Check $env:VCPKG_ROOT first, then fall back to common locations including
# the VS-integrated vcpkg.
function Find-Vcpkg {
    if ($env:VCPKG_ROOT -and (Test-Path (Join-Path $env:VCPKG_ROOT 'scripts\buildsystems\vcpkg.cmake'))) {
        return $env:VCPKG_ROOT
    }
    $drive = Split-Path $repoRoot -Qualifier
    $candidates = @(
        (Join-Path $repoRoot '..\vcpkg'),
        (Join-Path $drive '\vcpkg'),
        'C:\vcpkg',
        "$env:USERPROFILE\vcpkg",
        "$env:USERPROFILE\source\repos\vcpkg",
        'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\vcpkg',
        'C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\vcpkg',
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\vcpkg',
        'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\vcpkg',
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\Community\VC\vcpkg',
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\Professional\VC\vcpkg',
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\Enterprise\VC\vcpkg',
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\vcpkg'
    )
    foreach ($p in $candidates) {
        if (Test-Path (Join-Path $p 'scripts\buildsystems\vcpkg.cmake')) { return $p }
    }
    # Every Visual Studio instance may carry the integrated vcpkg, and where it lives
    # depends on the edition, the version and whether VS installed itself under
    # Program Files or Program Files (x86). Ask vswhere for the instances instead of
    # guessing fixed paths.
    if ($vswhere -and (Test-Path $vswhere)) {
        $instances = & $vswhere -all -products * -property installationPath
        foreach ($inst in $instances) {
            if (-not $inst) { continue }
            $p = Join-Path $inst 'VC\vcpkg'
            if (Test-Path (Join-Path $p 'scripts\buildsystems\vcpkg.cmake')) { return $p }
        }
    }
    return $null
}

# ── Windows SDK: make sure the linker can run rc.exe ────────────────────
# The linker needs rc.exe (and mt.exe) from the Windows SDK. Both the Visual Studio
# generator and vcpkg pick the newest installed Windows SDK, and when that SDK is
# installed without its bin tools - the headers and the libs are there, rc.exe is not -
# the link fails with "LNK1158: cannot run 'rc.exe'". CMake is only testing the
# compiler at that point, so it reports "No CMAKE_CXX_COMPILER could be found", which
# hides the real cause.
#
# vcvarsall removes the Windows Kits folders from PATH when it sets the environment
# up, so putting the SDK's own bin folder on PATH does not survive the toolchain
# setup. The tools are copied into a folder outside the Kits tree instead, and that
# one stays on PATH.
function Get-WindowsSdk {
    $kitsRoot = $null
    try {
        $kitsRoot = (Get-ItemProperty -Path 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots' -ErrorAction Stop).KitsRoot10
    } catch { }
    if (-not $kitsRoot) { $kitsRoot = 'C:\Program Files (x86)\Windows Kits\10' }
    $kitsRoot = $kitsRoot.TrimEnd('\')
    $binRoot = Join-Path $kitsRoot 'bin'

    $newest = $null
    $completeBin = $null
    if (Test-Path $binRoot) {
        $versions = Get-ChildItem $binRoot -Directory -ErrorAction SilentlyContinue | ForEach-Object {
            $parsed = $null
            if ([version]::TryParse($_.Name, [ref]$parsed)) {
                [pscustomobject]@{ Name = $_.Name; Version = $parsed }
            }
        } | Sort-Object Version -Descending
        foreach ($cand in $versions) {
            if (-not $newest) { $newest = $cand.Name }
            if (-not $completeBin) {
                $dir = Join-Path $binRoot "$($cand.Name)\x64"
                if (Test-Path (Join-Path $dir 'rc.exe')) { $completeBin = $dir }
            }
        }
    }
    return [pscustomobject]@{ KitsRoot = $kitsRoot; Newest = $newest; CompleteBin = $completeBin }
}

# Copy the SDK tools that are needed for linking into a folder outside the Kits tree.
# rc.exe needs rcdll.dll and mt.exe needs midlrtmd.dll next to them.
function Get-SdkTools {
    $sdk = Get-WindowsSdk
    if (-not $sdk.CompleteBin) { return $null }
    $tools = Join-Path $env:LOCALAPPDATA 'WindowsKitsTools'
    New-Item -ItemType Directory -Force -Path $tools | Out-Null
    foreach ($name in 'rc.exe','rcdll.dll','mt.exe','midlrtmd.dll','signtool.exe') {
        $from = Join-Path $sdk.CompleteBin $name
        $to = Join-Path $tools $name
        if ((Test-Path $from) -and -not (Test-Path $to)) {
            Copy-Item -Path $from -Destination $to
        }
    }
    if (-not (Test-Path (Join-Path $tools 'rc.exe'))) { return $null }
    return $tools
}

$vcpkgRoot = Find-Vcpkg
if ($vcpkgRoot) {
    Write-Host "vcpkg   : $vcpkgRoot"
} else {
    Write-Warning "vcpkg not found. Install from https://github.com/microsoft/vcpkg or set VCPKG_ROOT."
}

# The linker needs rc.exe on PATH. If the Windows SDK that the toolchain picks has no
# bin tools, use the tools of an SDK that has them, instead of failing with a
# confusing "No CMAKE_CXX_COMPILER could be found".
$rcOnPath = $false
foreach ($dir in ($env:PATH -split ';')) {
    if ($dir -and (Test-Path (Join-Path $dir 'rc.exe'))) { $rcOnPath = $true; break }
}
if (-not $rcOnPath) {
    $sdkTools = Get-SdkTools
    if ($sdkTools) {
        # Outside a Visual Studio developer prompt the SDK tools are not on PATH, and
        # the linker needs rc.exe and mt.exe from there.
        Write-Host "SDK tools: $sdkTools (added to PATH for the build)"
        $env:PATH = "$sdkTools;$env:PATH"
    } else {
        Write-Warning "rc.exe is not on PATH and no installed Windows SDK has its bin tools. Reinstall the Windows SDK with the 'SDK for Windows desktop development' component."
    }
}

# Report the SDKs that are installed without their bin tools: the build fails with
# "LNK1158: cannot run 'rc.exe'" and "CMAKE_MT-NOTFOUND" when the toolchain picks one
# of them, and the message names neither the SDK nor the missing file.
$sdk = Get-WindowsSdk
$sdkWithoutTools = @()
# The SDKs that are installed are the ones with headers; their bin folder with
# rc.exe and mt.exe may be missing.
foreach ($dir in (Get-ChildItem (Join-Path $sdk.KitsRoot 'Include') -Directory -ErrorAction SilentlyContinue)) {
    $parsed = $null
    if (-not [version]::TryParse($dir.Name, [ref]$parsed)) { continue }
    if (-not (Test-Path (Join-Path $sdk.KitsRoot "bin\$($dir.Name)\x64\mt.exe"))) { $sdkWithoutTools += $dir.Name }
}
if ($sdkWithoutTools) {
    Write-Warning "These Windows SDKs are installed without their bin tools (no rc.exe/mt.exe): $($sdkWithoutTools -join ', '). The libraries that vcpkg builds need an SDK whose tools are there - repair the Windows SDK in the Visual Studio Installer, or install one that has them."
}

# ── Configure (CMake) if needed ─────────────────────────────────────────
# A build folder copied along with the repo still points at the old folder; start it fresh.
$cacheFile = Join-Path $buildDir 'CMakeCache.txt'
if (Test-Path $cacheFile) {
    $homeLine = Select-String -Path $cacheFile -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$' | Select-Object -First 1
    if ($homeLine) {
        $cachedRoot  = [IO.Path]::GetFullPath($homeLine.Matches[0].Groups[1].Value.Trim()).TrimEnd('\')
        $currentRoot = [IO.Path]::GetFullPath($repoRoot).TrimEnd('\')
        if ($cachedRoot -ne $currentRoot) {
            Write-Warning "Build folder was set up for '$cachedRoot'; reconfiguring for '$currentRoot'."
            Remove-Item $cacheFile -Force
            Remove-Item (Join-Path $buildDir 'CMakeFiles') -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}
$needConfigure = $Configure -or (-not (Test-Path $cacheFile))
if ($needConfigure) {
    Write-Host "Running CMake configure ..."
    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
    Push-Location $buildDir
    try {
        $cmakeArgs = @(
            '-G', 'Visual Studio 17 2022',
            '-A', 'x64',
            $repoRoot
        )
        if ($vcpkgRoot) {
            $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$vcpkgRoot\scripts\buildsystems\vcpkg.cmake"
            $cmakeArgs += "-DVCPKG_TARGET_TRIPLET=x64-windows"
            # the DLLs are copied once after the build (below), not after every program
            $cmakeArgs += "-DVCPKG_APPLOCAL_DEPS=OFF"
            # The overlay triplet builds the libraries with the same toolset the
            # project itself uses (the generator above is Visual Studio 17 2022 =
            # v143). vcpkg would otherwise pick the newest Visual Studio on the
            # machine, and the libraries would then need STL internals that the
            # programs' STL does not have (LNK2019 __std_*).
            $overlayTriplets = Join-Path $repoRoot 'vcpkg-triplets'
            if (Test-Path $overlayTriplets) {
                $cmakeArgs += "-DVCPKG_INSTALL_OPTIONS=--overlay-triplets=$overlayTriplets"
            }
        }
        & $cmake @cmakeArgs
        if ($LASTEXITCODE -ne 0) {
            throw "CMake configure failed."
        }
    } finally { Pop-Location }
}

if (-not $needConfigure -and $vcpkgRoot -and (Test-Path $cacheFile) -and
    (Select-String -Path $cacheFile -Pattern '^VCPKG_APPLOCAL_DEPS:BOOL=ON$' -Quiet)) {
    Write-Host "Note: this build folder still runs vcpkg's DLL copy after every program; build.ps1 -Configure switches it off (it works either way)."
}

# ── Build target(s) ──────────────────────────────────────────────────────
$targets = if ($Target -eq 'all') {
    @('tether_server','launcher','worldd')
} else {
    @($Target)
}

# -Jobs N: N projects at a time (/m:N) and at most N compiler processes at a time (builds that run together share that limit). Without -Jobs: every core
# for projects, and for the compilers as many as the free memory allows (about 1.5 GB each), at most one per processor.
$mArg = if ($Jobs -gt 0) { "/m:$Jobs" } else { '/m' }
$clCount = $Jobs
if ($clCount -le 0) {
    $freeGB = 0.0
    try { $freeGB = (Get-CimInstance -ClassName Win32_OperatingSystem -ErrorAction Stop).FreePhysicalMemory / 1MB } catch { }
    $clCount = [int][math]::Max(2, [math]::Min([Environment]::ProcessorCount, [math]::Floor($freeGB / 1.5)))
}
$msbuildArgs = @($mArg, '/p:UseMultiToolTask=true', '/p:EnforceProcessCountAcrossBuilds=true', "/p:CL_MPCount=$clCount", '/nr:false', '/v:minimal', '/nologo')
Write-Host "Projects at a time: $(if ($Jobs -gt 0) { $Jobs } else { 'all cores' }); compiler processes at a time: $clCount"
foreach ($t in $targets) {
    Write-Host ""
    Write-Host "Building $t ..."
    & $cmake --build $buildDir --config $Config --target $t -- @msbuildArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed for target: $t"
    }
}

# ── Runtime DLLs ─────────────────────────────────────────────────────────
# The programs load vcpkg's DLLs from their own folder. With vcpkg's per-program copy off (see the header), copy them once here: the ones that
# are missing in dist\<Config>, older than the vcpkg copy, or of another size. The folder is read from the CMake cache.
$vcpkgInstalled = $null
$triplet = 'x64-windows'
if (Test-Path $cacheFile) {
    foreach ($line in Get-Content -LiteralPath $cacheFile) {
        if ($line -match '^VCPKG_INSTALLED_DIR:[A-Z]+=(.+)$') { $vcpkgInstalled = $Matches[1].Trim() }
        elseif ($line -match '^VCPKG_TARGET_TRIPLET:[A-Z]+=(.+)$') { $triplet = $Matches[1].Trim() }
    }
}
$dllSource = if ($vcpkgInstalled) { Join-Path (Join-Path $vcpkgInstalled $triplet) $(if ($Config -eq 'Debug') { 'debug\bin' } else { 'bin' }) } else { $null }
if ($dllSource -and (Test-Path $dllSource)) {
    New-Item -ItemType Directory -Force -Path $distDir | Out-Null
    $copied = 0
    $kept = 0
    foreach ($dll in Get-ChildItem -LiteralPath $dllSource -Filter '*.dll' -File) {
        $dest = Join-Path $distDir $dll.Name
        $have = Get-Item -LiteralPath $dest -ErrorAction SilentlyContinue
        if ($have -and $have.Length -eq $dll.Length -and $have.LastWriteTimeUtc -ge $dll.LastWriteTimeUtc) { $kept++; continue }
        try { Copy-Item -LiteralPath $dll.FullName -Destination $dest -Force; $copied++ }
        catch { Write-Warning "Could not copy $($dll.Name) to $distDir (a running program uses it?): $($_.Exception.Message)" }
    }
    Write-Host "Runtime DLLs: $copied copied, $kept already up to date ($dllSource)"
}

# ── Run tests if requested ──────────────────────────────────────────────
# Delegate to run_tests.ps1: it builds every CMake target (including the
# aocnet library and every libs/aocnet/tests + src/tools test executable,
# not just the target(s) built above) and then runs every dist\<Config>\
# test_*.exe.  Called without -NoBuild so it does its own full build.
if ($Test) {
    Write-Host ""
    Write-Host "Running scripts\run_tests.ps1 ..."
    & (Join-Path $PSScriptRoot 'run_tests.ps1') -Config $Config -Jobs $Jobs
    exit $LASTEXITCODE
}

Write-Host ""
Write-Host "Build completed successfully."
Write-Host "Binaries: $distDir"
