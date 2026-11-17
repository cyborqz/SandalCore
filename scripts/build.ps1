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
#      $env:AOC_VCPKG_SHARED = '<folder>'; .\scripts\build.ps1 -Configure
#                                          # one Release-only vcpkg install, in <folder>, for every build folder that is configured with it (see the configure below)
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
# the VS2022-integrated vcpkg.
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
        'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\vcpkg'
    )
    foreach ($p in $candidates) {
        if (Test-Path (Join-Path $p 'scripts\buildsystems\vcpkg.cmake')) { return $p }
    }
    return $null
}

$vcpkgRoot = Find-Vcpkg
if ($vcpkgRoot) {
    Write-Host "vcpkg   : $vcpkgRoot"
} else {
    Write-Warning "vcpkg not found. Install from https://github.com/microsoft/vcpkg or set VCPKG_ROOT."
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
# ── The shared vcpkg install (AOC_VCPKG_SHARED), see the configure below ───
# The folder of one shared install is named by the hash of vcpkg.json (with vcpkg-configuration.json and the triplet).
$sharedTriplet = 'x64-windows-release'
function Get-ManifestHash([string] $triplet) {
    $manifest = @(Join-Path $repoRoot 'vcpkg.json'; Join-Path $repoRoot 'vcpkg-configuration.json') | Where-Object { Test-Path $_ }
    $sha = [Security.Cryptography.SHA256]::Create()
    $bytes = @(); foreach ($m in $manifest) { $bytes += [IO.File]::ReadAllBytes($m) }; $bytes += [Text.Encoding]::ASCII.GetBytes($triplet)
    return (($sha.ComputeHash([byte[]]$bytes) | ForEach-Object { $_.ToString('x2') }) -join '').Substring(0, 12)
}
# a relative AOC_VCPKG_SHARED is the folder of the caller (worked out here, before the configure moves into the build folder), so that every build folder gets the same install
$sharedRoot = $null
if ($env:AOC_VCPKG_SHARED) { $sharedRoot = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($env:AOC_VCPKG_SHARED) }
# A build folder that holds a shared install follows vcpkg.json.  CMake's configure installs the packages that vcpkg.json names into the install folder it has in its cache, and it runs again by itself
# when vcpkg.json changes: in a shared install that would change the libraries under every other build folder that uses it.  So a changed vcpkg.json is configured here, into the install the new file
# names, before anything builds.  The shared install has no Debug libraries: a Debug build of such a folder is refused.
$cachedInstall = $null; $cachedTriplet = $null
if (Test-Path $cacheFile) {
    foreach ($line in Get-Content -LiteralPath $cacheFile) {
        if ($line -match '^VCPKG_INSTALLED_DIR:[A-Z]+=(.+)$') { $cachedInstall = $Matches[1].Trim() }
        elseif ($line -match '^VCPKG_TARGET_TRIPLET:[A-Z]+=(.+)$') { $cachedTriplet = $Matches[1].Trim() }
    }
}
$followShared = $false
if ($vcpkgRoot -and $cachedInstall -and $cachedTriplet -eq $sharedTriplet) {
    if ($Config -eq 'Debug') { throw "This build folder holds the shared vcpkg install, which is Release-only (it has no Debug libraries): use another -Config, or a build folder without AOC_VCPKG_SHARED." }
    if ((Split-Path -Leaf $cachedInstall) -ne "$sharedTriplet-$(Get-ManifestHash $sharedTriplet)") {
        $followShared = $true
        Write-Host "vcpkg   : vcpkg.json changed since this build folder was configured; configuring again into the shared install that it names."
    }
}
$needConfigure = $Configure -or (-not (Test-Path $cacheFile)) -or $followShared
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
            $vcpkgTriplet = 'x64-windows'
            if ($sharedRoot -or $followShared) {
                # One shared, Release-only install of the libraries for every build folder on this PC (set AOC_VCPKG_SHARED to the folder that holds it).
                # It is named by the hash of vcpkg.json (with the triplet), so a changed dependency list or baseline gets its own install, and a build folder
                # holds no vcpkg_installed of its own (about 11 GB each, 5.9 GB of it Debug libraries that no build uses).  Release, RelWithDebInfo and MinSizeRel
                # builds only: there are no Debug libraries in it.  A build folder that already holds a shared install keeps to the folder the install is in when the variable is not set.
                if ($Config -eq 'Debug') { throw "AOC_VCPKG_SHARED is set: the shared vcpkg install is Release-only; use another -Config or unset it." }
                $vcpkgTriplet = $sharedTriplet
                $manifestHash = Get-ManifestHash $vcpkgTriplet
                $sharedFolder = if ($sharedRoot) { $sharedRoot } else { Split-Path -Parent $cachedInstall }
                $sharedInstall = Join-Path $sharedFolder "$vcpkgTriplet-$manifestHash"
                New-Item -ItemType Directory -Force -Path $sharedInstall | Out-Null
                $cmakeArgs += "-DVCPKG_INSTALLED_DIR=$sharedInstall"
                Write-Host "vcpkg   : shared Release-only install $sharedInstall"
            }
            $cmakeArgs += "-DVCPKG_TARGET_TRIPLET=$vcpkgTriplet"
            # the DLLs are copied once after the build (below), not after every program
            $cmakeArgs += "-DVCPKG_APPLOCAL_DEPS=OFF"
        }
        # Two configures at once on the one shared install would run vcpkg twice on the same folder: take a PC-wide lock named by the install (the second waits, then finds the libraries installed).
        $installLock = $null
        if ($sharedInstall) {
            $installLock = New-Object System.Threading.Mutex($false, "Global\AocVcpkgInstall_$manifestHash")
            $held = $false
            try { $held = $installLock.WaitOne(0) } catch [System.Threading.AbandonedMutexException] { $held = $true }
            if (-not $held) {
                Write-Host "vcpkg   : another configure is using the shared install; waiting for it (up to 60 minutes) ..."
                try { $held = $installLock.WaitOne([TimeSpan]::FromMinutes(60)) } catch [System.Threading.AbandonedMutexException] { $held = $true }
                if (-not $held) { throw "another configure kept the shared vcpkg install for 60 minutes." }
            }
        }
        try {
            & $cmake @cmakeArgs
        } finally {
            if ($installLock) { try { $installLock.ReleaseMutex() } catch { }; $installLock.Dispose() }
        }
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
