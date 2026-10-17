# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

<#
.SYNOPSIS
  Builds the game data Sandal Core needs from YOUR OWN Ashes of Creation install: content.db, the tables, the navigation mesh and the generated sources.

.DESCRIPTION
  Sandal Core ships no game data. This script reads your installed client (the design database CacheDB.dbc and the game's .pak/.utoc containers; they are
  only read) and writes, next to the code and in dist\content (both git-ignored):

    dist\content\content.db                    the game numbers                                  (contentc, from CacheDB.dbc)
    dist\content\artisan_curves.tsv,           profession curves, mount speeds and riders        (mapc, from the game's containers)
      mount_speeds.tsv, mount_riders.tsv
    dist\content\npc_motion.tsv, region_foliage.tsv, roads_raw.tsv, roads.tsv, roads_levels.tsv
    dist\content\area_geom.bin, area_nav.bin   the navigation mesh (combat needs it)             (mapc and aoc_navbuild)
    dist\content\client_config\, zoi_chunks\   the game's Config files and its zone-of-influence grid (read by the generators below)
    the sources the build includes             world\zoi_surface_table.h, node_constants.h, constants_gen.go, region_data.h, garden_defs.h,
                                               region_rects.tsv, the Riverlands' rectangles, and the test fixtures (listed in .gitignore)
    dist\content\aoc.usmap                     a copy of layouts\aoc.usmap, where the tools look for it

  It needs: the game (Steam, or GAME_ROOT / -GameRoot), the .NET 8 SDK (mapc), Go (contentc), Python 3.10+ (the generators), and Visual Studio 2022 Build Tools
  with CMake and vcpkg (aoc_navbuild, built by scripts\build.ps1). The first run fetches CUE4Parse (tools\mapc\fetch_cue4parse.ps1) and restores mapc's NuGet
  packages, so it needs the network once. A full run takes about 10 minutes on a fast PC (the longest step is the map read for the navigation mesh) and up to
  4 GB of memory for any one program; the first build of the C++ libraries (vcpkg) comes on top and can take a long time.

  Every step is skipped when what it reads is unchanged since the last run (a stamp per step in dist\content\.extract), so build_all.bat and the launchers can call
  this every time. The supported client build is the one of 2026-01-29 (AOCClient-Win64-Shipping.exe 235,606,624 bytes, CacheDB.dbc 144,085,052 bytes); with another
  build the script warns and goes on.

.PARAMETER GameRoot
  The game's "Game" folder (the one that holds AOCClient.exe and AOC\). Default: GAME_ROOT, config\paths.json, Steam's libraries, the usual folders.
.PARAMETER Only
  Run only these steps: tools, config, curves, database, region, headers, fixtures, navbuild, roads, navmesh.
.PARAMETER Skip
  Leave these steps out (for example -Skip fixtures when you do not build the tests).
.PARAMETER Force
  Run the selected steps again even if their stamps are current.
.PARAMETER CheckOnly
  Find the game and the tools, say which steps would run, and change nothing.
.PARAMETER Jobs
  Parallel jobs of the C++ build (default: half of the logical processors, at most 8).
.PARAMETER NavThreads
  Threads of the navigation mesh build (default: half of the logical processors, at most 8; the result does not depend on it).
#>
[CmdletBinding()]
param(
    [string]   $GameRoot,
    [string[]] $Only,
    [string[]] $Skip,
    [switch]   $Force,
    [switch]   $CheckOnly,
    [int]      $Jobs = 0,
    [int]      $NavThreads = 0
)

$ErrorActionPreference = 'Stop'
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })     # powershell -File hands over "a,b" as one string
$Skip = @($Skip | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$repoRoot  = Split-Path $PSScriptRoot -Parent
$distDir   = Join-Path $repoRoot 'dist'
$outDir    = Join-Path $distDir 'content'
$stampDir  = Join-Path $outDir '.extract'
$toolsDir  = Join-Path $distDir 'tools'
$usmapSrc  = Join-Path $repoRoot 'layouts\aoc.usmap'
$mapcDll   = Join-Path $repoRoot 'tools\mapc\bin\Release\net8.0\mapc.dll'
$contentc  = Join-Path $toolsDir 'contentc.exe'
$statprog  = Join-Path $toolsDir 'statprog.exe'
$navbuild  = Join-Path $distDir 'Release\aoc_navbuild.exe'
$logFile   = Join-Path $stampDir 'current.log'
$lastLog   = Join-Path $stampDir 'last_run.log'

# The client build this code was made for (the player's own files are only compared by size).
$Supported = @{ ExeBytes = 235606624; DbcBytes = 144085052; Date = '2026-01-29' }
$ExeRel  = 'AOC\Binaries\Win64\AOCClient-Win64-Shipping.exe'
$DbcRel  = 'AOC\Plugins\DesignDataPlugin\Generated\Client\CacheDB.dbc'
$PaksRel = 'AOC\Content\Paks'

$cores = [Environment]::ProcessorCount
if ($Jobs -le 0)       { $Jobs = [Math]::Max(2, [Math]::Min(8, [int]($cores / 2))) }
if ($NavThreads -le 0) { $NavThreads = [Math]::Max(2, [Math]::Min(8, [int]($cores / 2))) }

# ---------------------------------------------------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------------------------------------------------
function Write-Log([string]$text) {
    if (Test-Path -LiteralPath $stampDir) { try { Add-Content -LiteralPath $logFile -Value $text -Encoding ASCII } catch { } }
}
function Say([string]$text, [string]$color = '') {
    if ($color) { Write-Host $text -ForegroundColor $color } else { Write-Host $text }
    Write-Log $text
}
function Fail([string]$text, [string[]]$hint = @()) {
    Say "ERROR: $text" 'Red'
    foreach ($h in $hint) { Say "       $h" 'Yellow' }
    exit 1
}
function Format-Seconds([double]$s) { if ($s -ge 90) { '{0:N1} min' -f ($s / 60) } else { '{0:N1} s' -f $s } }

# ---------------------------------------------------------------------------------------------------------------------
# Hashes (the stamps)
# ---------------------------------------------------------------------------------------------------------------------
$sha = [System.Security.Cryptography.SHA256]::Create()
function Get-TextHash([string]$text) {
    ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text))) -replace '-', '').ToLower()
}
function Get-FileSha([string]$path) {
    ([BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes($path))) -replace '-', '').ToLower()
}
$codeHashCache = @{}
# One hash over the files under the given repo-relative folders or files. Build products, tests, notes and the files this script writes are left out.
function Get-CodeHash([string[]]$paths) {
    $key = $paths -join '|'
    if ($codeHashCache.ContainsKey($key)) { return $codeHashCache[$key] }
    $lines = New-Object System.Collections.Generic.List[string]
    foreach ($p in $paths) {
        $full = Join-Path $repoRoot $p
        if (-not (Test-Path -LiteralPath $full)) { $lines.Add("$p missing"); continue }
        $files = if ((Get-Item -LiteralPath $full).PSIsContainer) {
            Get-ChildItem -LiteralPath $full -Recurse -File | Where-Object { $_.FullName -notmatch '\\(bin|obj|__pycache__)\\' -and $_.Name -notmatch '_test\.go$|\.md$|\.pyc$|^region_rects\.tsv$' }
        } else { Get-Item -LiteralPath $full }
        foreach ($f in ($files | Sort-Object FullName)) {
            $rel = $f.FullName.Substring($repoRoot.Length).TrimStart('\')
            $lines.Add("$rel $(Get-FileSha $f.FullName)")
        }
    }
    $h = Get-TextHash ($lines -join "`n")
    $codeHashCache[$key] = $h
    return $h
}

# ---------------------------------------------------------------------------------------------------------------------
# The game
# ---------------------------------------------------------------------------------------------------------------------
# $candidate may be the "Game" folder, its parent, or the client's exe.
function Resolve-GameRoot([string]$candidate) {
    if (-not $candidate) { return $null }
    $c = $candidate.Trim().Trim('"').TrimEnd('\', '/')
    if ($c -match '(?i)\.exe$') {            # Game\AOC\Binaries\Win64\x.exe -> Game
        $c = Split-Path (Split-Path (Split-Path (Split-Path $c -Parent) -Parent) -Parent) -Parent
    }
    foreach ($try in @($c, (Join-Path $c 'Game'))) {
        if ($try -and (Test-Path -LiteralPath (Join-Path $try $PaksRel)) -and (Test-Path -LiteralPath (Join-Path $try $DbcRel))) { return [IO.Path]::GetFullPath($try).TrimEnd('') }
    }
    return $null
}

function Get-RegistryString([string]$path, [string]$name) {
    try { return [string](Get-ItemProperty -LiteralPath $path -Name $name -ErrorAction Stop).$name } catch { return '' }
}

function Find-GameRoot {
    if ($GameRoot) {
        $r = Resolve-GameRoot $GameRoot
        if (-not $r) {
            Fail "-GameRoot '$GameRoot' is not the game's folder (it has no $PaksRel and no $DbcRel)." @(
                'Give the folder that holds AOCClient.exe and the AOC folder (the Game folder of the install), not the launcher folder.',
                'If the install is incomplete, let Steam verify the game files, then run this again.',
                'More: README.md, "Build and run", "If something goes wrong".')
        }
        return [pscustomobject]@{ Path = $r; Via = '-GameRoot' }
    }
    $candidates = New-Object System.Collections.Generic.List[object]
    function Add-Candidate([string]$path, [string]$via) { $candidates.Add([pscustomobject]@{ Path = $path; Via = $via }) }
    if ($env:GAME_ROOT) { Add-Candidate $env:GAME_ROOT 'GAME_ROOT' }
    foreach ($cfg in @((Join-Path $repoRoot 'config\paths.json'), (Join-Path $repoRoot 'dist\Release\config\paths.json'))) {
        if (Test-Path -LiteralPath $cfg) {
            try { $exe = (Get-Content -LiteralPath $cfg -Raw | ConvertFrom-Json).game_executable; if ($exe) { Add-Candidate ([string]$exe) 'game_executable in paths.json' } } catch { }
        }
    }
    # Steam: its install folder and every library in libraryfolders.vdf (the launcher's Auto-Detect looks in the same places)
    $libs = New-Object System.Collections.Generic.List[string]
    foreach ($pair in @(@('HKCU:\Software\Valve\Steam', 'SteamPath'), @('HKLM:\SOFTWARE\Valve\Steam', 'InstallPath'), @('HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'InstallPath'))) {
        $v = Get-RegistryString $pair[0] $pair[1]
        if (-not $v) { continue }
        $v = $v -replace '/', '\'
        $libs.Add($v)
        $vdf = Join-Path $v 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $vdf) {
            foreach ($line in (Get-Content -LiteralPath $vdf)) {
                if ($line -match '"path"\s+"([^"]+)"') { $libs.Add(($Matches[1] -replace '\\', '\')) }
            }
        }
    }
    foreach ($l in $libs) { Add-Candidate (Join-Path $l 'steamapps\common\Ashes of Creation\Game') 'a Steam library' }
    # the usual folders on every fixed drive
    foreach ($d in [IO.DriveInfo]::GetDrives()) {
        if ($d.DriveType -ne 'Fixed') { continue }
        $root = $d.Name.TrimEnd('\')
        foreach ($sub in @('', '\Program Files (x86)\Steam', '\Steam', '\Games', '\Ashes of Creation', '\AshesOfCreation')) {
            Add-Candidate "$root$sub\steamapps\common\Ashes of Creation\Game" 'a usual folder'
            Add-Candidate "$root$sub\Game" 'a usual folder'
            Add-Candidate "$root$sub" 'a usual folder'
        }
    }
    foreach ($c in $candidates) {
        $r = Resolve-GameRoot $c.Path
        if ($r) { return [pscustomobject]@{ Path = $r; Via = $c.Via } }
    }
    return $null
}

# ---------------------------------------------------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------------------------------------------------
function Find-Exe([string]$name, [string[]]$extra = @()) {
    $c = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($c) { return $c.Source }
    foreach ($e in $extra) { if (Test-Path -LiteralPath $e) { return $e } }
    return $null
}

function Find-Python {
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
            if ($code -eq 0 -and $v -match '^(\d+) (\d+)$' -and ([int]$Matches[1] -gt 3 -or ([int]$Matches[1] -eq 3 -and [int]$Matches[2] -ge 10))) {
                return [pscustomobject]@{ Exe = $c.Source; Prefix = $prefix; Version = ($v -replace ' ', '.') }
            }
        } catch { } finally { $ErrorActionPreference = $oldPref }
    }
    return $null
}

function Find-DotnetSdk8 {
    $dn = Find-Exe 'dotnet' @("$env:ProgramFiles\dotnet\dotnet.exe")
    if (-not $dn) { return $null }
    foreach ($s in @(& $dn --list-sdks 2>$null)) { if ($s -match '^(\d+)\.' -and [int]$Matches[1] -eq 8) { return $dn } }
    return $null
}

# Quote one argument for a Windows command line.
function ConvertTo-ArgText([string]$a) {
    if ($a -eq '') { return '""' }
    if ($a -notmatch '[\s"]') { return $a }
    $q = $a -replace '(\\*)"', '$1$1\"'
    $q = $q -replace '(\\+)$', '$1$1'
    return '"' + $q + '"'
}

function Show-NewText([string]$path, [long]$pos) {
    if (-not (Test-Path -LiteralPath $path)) { return $pos }
    $fs = $null
    try {
        $fs = New-Object IO.FileStream($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        if ($fs.Length -le $pos) { return $pos }
        [void]$fs.Seek($pos, [IO.SeekOrigin]::Begin)
        $buf = New-Object byte[] ([int]($fs.Length - $pos))
        $n = $fs.Read($buf, 0, $buf.Length)
        $text = [Text.Encoding]::UTF8.GetString($buf, 0, $n)
        if ($text) { Write-Host -NoNewline $text; Write-Log $text.TrimEnd("`r", "`n") }
        return $pos + $n
    } finally { if ($fs) { $fs.Dispose() } }
}

$script:stepPeak = [long]0
# Runs a program, shows its output as it comes, returns its exit code; the peak working set of the biggest program of a step is kept for the summary.
function Invoke-Tool {
    param([string]$Exe, [string[]]$Arguments = @(), [string]$WorkDir = $repoRoot, [string]$Name = 'tool')
    $so = Join-Path $stampDir "$Name.out.txt"
    $se = Join-Path $stampDir "$Name.err.txt"
    Remove-Item -Force -ErrorAction SilentlyContinue $so, $se
    $argText = ($Arguments | ForEach-Object { ConvertTo-ArgText $_ }) -join ' '
    Write-Log ("> $Exe $argText")
    $p = Start-Process -FilePath $Exe -ArgumentList $argText -WorkingDirectory $WorkDir -RedirectStandardOutput $so -RedirectStandardError $se -NoNewWindow -PassThru
    $null = $p.Handle                 # keeps the exit code readable (Windows PowerShell 5.1)
    $peak = [long]0
    $po = [long]0; $pe = [long]0
    while (-not $p.HasExited) {
        Start-Sleep -Milliseconds 500
        try { $p.Refresh(); if ($p.PeakWorkingSet64 -gt $peak) { $peak = $p.PeakWorkingSet64 } } catch { }
        $po = Show-NewText $so $po
        $pe = Show-NewText $se $pe
    }
    $p.WaitForExit()
    $po = Show-NewText $so $po
    $pe = Show-NewText $se $pe
    $code = $p.ExitCode
    $p.Dispose()
    if ($peak -gt $script:stepPeak) { $script:stepPeak = $peak }
    return [int]$code
}

function Invoke-Checked([string]$Exe, [string[]]$Arguments, [string]$Name, [string]$WorkDir = $repoRoot) {
    $code = Invoke-Tool -Exe $Exe -Arguments $Arguments -WorkDir $WorkDir -Name $Name
    if ($code -ne 0) { throw "$Name failed (exit code $code)" }
}

function Assert-Files([string[]]$rel, [string]$why) {
    foreach ($r in $rel) {
        if (-not (Test-Path -LiteralPath (Join-Path $repoRoot $r))) { throw "$r is missing ($why)" }
    }
}

# ---------------------------------------------------------------------------------------------------------------------
# Start
# ---------------------------------------------------------------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $stampDir | Out-Null
Set-Content -LiteralPath $logFile -Value ("extract_game_data.ps1 " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')) -Encoding ASCII

$found = Find-GameRoot
if (-not $found) {
    Fail 'The Ashes of Creation client was not found on this PC.' @(
        'Sandal Core builds its game data from your own installed copy of the game (the game files are only read).',
        'It looked at: -GameRoot, the GAME_ROOT variable, game_executable in config\paths.json, the Steam libraries (libraryfolders.vdf) and the usual folders on every fixed drive.',
        'Install the game (Steam), or tell the script where it is and run this again:',
        '    extract_game_data.bat -GameRoot "D:\...\steamapps\common\Ashes of Creation\Game"',
        '-GameRoot counts for that run only. To keep it for build_all.bat and the launchers, set GAME_ROOT once and open a new window:',
        '    setx GAME_ROOT "D:\...\steamapps\common\Ashes of Creation\Game"',
        '(the folder that holds AOCClient.exe and the AOC folder).',
        'More: README.md, "Build and run", "If something goes wrong".')
}
$game = $found.Path
$gameExe  = Join-Path $game $ExeRel
$gameDbc  = Join-Path $game $DbcRel
$gamePaks = Join-Path $game $PaksRel
$exeLen = -1; $exeTicks = 0
if (Test-Path -LiteralPath $gameExe) { $it = Get-Item -LiteralPath $gameExe; $exeLen = $it.Length; $exeTicks = $it.LastWriteTimeUtc.Ticks }
$dbcItem = Get-Item -LiteralPath $gameDbc
$dbcLen = $dbcItem.Length
Say "Game folder : $game (found through $($found.Via))"
if (($exeLen -eq $Supported.ExeBytes) -and ($dbcLen -eq $Supported.DbcBytes)) {
    Say ("Client build: supported ({0}; exe {1:N0} bytes, CacheDB.dbc {2:N0} bytes)" -f $Supported.Date, $exeLen, $dbcLen)
} else {
    Say ("WARNING: this client is not the build this code was made for ({0}: exe {1:N0} bytes, CacheDB.dbc {2:N0} bytes; yours: exe {3:N0}, CacheDB.dbc {4:N0})." -f `
        $Supported.Date, $Supported.ExeBytes, $Supported.DbcBytes, $exeLen, $dbcLen) 'Yellow'
    Say '         The data may be wrong or a step may fail (README.md, "After a game patch").' 'Yellow'
}

# what identifies the player's game files: names, sizes and write times of the containers, the exe and the design database
$paks = Get-ChildItem -LiteralPath $gamePaks -File | Sort-Object Name | ForEach-Object { '{0} {1} {2}' -f $_.Name, $_.Length, $_.LastWriteTimeUtc.Ticks }
$gameId = Get-TextHash (($paks -join "`n") + "`n" + ('exe {0} {1}' -f $exeLen, $exeTicks) + "`n" + ('dbc {0} {1}' -f $dbcLen, $dbcItem.LastWriteTimeUtc.Ticks))

$python = Find-Python
$dotnet = Find-DotnetSdk8
$go     = Find-Exe 'go' @('C:\Program Files\Go\bin\go.exe')
$missing = @()
if (-not $python) { $missing += 'Python 3.10 or newer (python on PATH)' }
if (-not $dotnet) { $missing += 'the .NET 8 SDK (dotnet)' }
if (-not $go)     { $missing += 'Go (go on PATH)' }
if (-not (Test-Path -LiteralPath $usmapSrc)) { $missing += 'layouts\aoc.usmap (part of the repository)' }
if ($missing.Count -gt 0) {
    Fail ('Missing: ' + ($missing -join '; ') + '.') @(
        'Run install_dependencies.bat to install what is missing, open a new window (so the new PATH applies), then run this again.',
        'More: README.md, "Build and run", "What to install".')
}
Say ("Tools       : Python {0}, .NET SDK, Go; {1} jobs for C++, {2} threads for the navigation mesh" -f $python.Version, $Jobs, $NavThreads)

# ---------------------------------------------------------------------------------------------------------------------
# The steps
# ---------------------------------------------------------------------------------------------------------------------
function Get-CodeMapc     { Get-CodeHash @('tools\mapc') }
function Get-CodeContentc { Get-CodeHash @('services\cmd\contentc', 'services\internal\content', 'services\internal\designdb', 'services\internal\usmap', 'services\internal\aocexpr', 'services\internal\chardef', 'services\internal\statprog', 'services\go.mod', 'services\go.sum') }
function Get-CodeGens     { Get-CodeHash @('tools\capture\gen_zoi_surface.py', 'tools\capture\gen_region_rects.py', 'tools\capture\gen_region.py', 'tools\capture\gen_node_constants.py', 'tools\capture\gen_gm_garden.py', 'tools\capture\gen_tooltips.py', 'tools\worldd\gen_gear_fixture.py', 'tools\worldd\gen_talent_fixture.py') }
function Get-CodeNav      { Get-CodeHash @('server\worldd\nav', 'third_party\recastnavigation', 'CMakeLists.txt', 'server\worldd\CMakeLists.txt', 'vcpkg.json') }
$ids = @{}

# A step's id says what its output is a function of: the player's game files, the code that makes it and the ids of the steps it reads.
function Get-Id([string]$step) {
    if ($ids.ContainsKey($step)) { return $ids[$step] }
    $usmap = Get-FileSha $usmapSrc
    $v = switch ($step) {
        'config'   { Get-TextHash ("config $gameId $usmap " + (Get-CodeMapc)) }
        'curves'   { Get-TextHash ("curves $gameId $usmap " + (Get-CodeMapc)) }
        'database' { Get-TextHash ("database $gameId $usmap " + (Get-CodeContentc) + (Get-Id 'config') + (Get-Id 'curves')) }
        'region'   { Get-TextHash ("region $gameId $usmap " + (Get-CodeMapc) + (Get-CodeGens) + (Get-Id 'database')) }
        'headers'  { Get-TextHash ("headers " + (Get-CodeGens) + (Get-Id 'config') + (Get-Id 'region')) }
        'fixtures' { Get-TextHash ("fixtures " + (Get-CodeGens) + (Get-CodeContentc) + (Get-Id 'database')) }
        'navbuild' { Get-TextHash ("navbuild " + (Get-CodeNav)) }
        'roads'    { Get-TextHash ("roads $gameId $usmap " + (Get-CodeMapc) + (Get-CodeContentc) + (Get-Id 'region')) }
        'navmesh'  { Get-TextHash ("navmesh $gameId $usmap " + (Get-CodeMapc) + (Get-Id 'region') + (Get-Id 'navbuild')) }
    }
    $ids[$step] = $v
    return $v
}

function Get-Stamp([string]$step) {
    $f = Join-Path $stampDir "$step.json"
    if (-not (Test-Path -LiteralPath $f)) { return $null }
    try { return Get-Content -LiteralPath $f -Raw | ConvertFrom-Json } catch { return $null }
}

function Test-Current($s) {
    if ($s.Always) { return $false }
    $st = Get-Stamp $s.Name
    if (-not $st -or $st.inputs -ne (Get-Id $s.Name)) { return $false }
    foreach ($o in $s.Outputs) {
        $p = Join-Path $repoRoot $o
        if (-not (Test-Path -LiteralPath $p)) { return $false }
        $it = Get-Item -LiteralPath $p
        if (-not $it.PSIsContainer -and $it.Length -eq 0) { return $false }
    }
    return $true
}

$steps = @(
    @{ Name = 'tools'; Title = 'Build mapc (.NET) and contentc (Go)'; Always = $true; Outputs = @(); Run = {
        $fetch = Join-Path $repoRoot 'tools\mapc\fetch_cue4parse.ps1'
        if (-not (Test-Path -LiteralPath (Join-Path $repoRoot 'third_party\CUE4Parse\CUE4Parse\CUE4Parse.csproj'))) {
            if ($env:SANDAL_CUE4PARSE_DIR) { Say "Copying CUE4Parse from $($env:SANDAL_CUE4PARSE_DIR) ..." } else { Say 'Fetching CUE4Parse (the pinned commit; needs git and the network) ...' }
            Invoke-Checked 'powershell.exe' @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $fetch) 'fetch_cue4parse'
        }
        $env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'; $env:DOTNET_NOLOGO = '1'
        Invoke-Checked $dotnet @('build', '-c', 'Release', (Join-Path $repoRoot 'tools\mapc'), '-v', 'q', '-nologo', '-nodeReuse:false', '-p:UseSharedCompilation=false') 'dotnet_build'
        Assert-Files @('tools\mapc\bin\Release\net8.0\mapc.dll') 'the mapc build did not produce it'
        New-Item -ItemType Directory -Force -Path $toolsDir | Out-Null
        $svc = Join-Path $repoRoot 'services'
        Invoke-Checked $go @('build', '-trimpath', '-buildvcs=false', '-o', $contentc, './cmd/contentc') 'go_build_contentc' $svc
        Invoke-Checked $go @('build', '-trimpath', '-buildvcs=false', '-o', $statprog, './internal/statprog/cmd/statprog') 'go_build_statprog' $svc
    } },
    @{ Name = 'config'; Title = "Export the game's Config files and copy aoc.usmap"; Outputs = @('dist\content\client_config\DefaultCombat.ini', 'dist\content\client_config\DefaultAoCRecordConstants.ini', 'dist\content\aoc.usmap'); Run = {
        Assert-Files @('tools\mapc\bin\Release\net8.0\mapc.dll') 'run the tools step first'
        Copy-Item -Force -LiteralPath $usmapSrc -Destination (Join-Path $outDir 'aoc.usmap')
        Invoke-Checked $dotnet @($mapcDll, 'export-config', '--paks', $gamePaks, '--usmap', $usmapSrc, '--out-dir', $outDir) 'mapc_export_config'
    } },
    @{ Name = 'curves'; Title = 'Read the profession curves and the mounts from the game containers (mapc)'; Outputs = @('dist\content\artisan_curves.tsv', 'dist\content\mount_speeds.tsv', 'dist\content\mount_riders.tsv'); Run = {
        Assert-Files @('tools\mapc\bin\Release\net8.0\mapc.dll') 'run the tools step first'
        $common = @('--paks', $gamePaks, '--usmap', $usmapSrc, '--out-dir', $outDir)
        Invoke-Checked $dotnet (@($mapcDll, 'artisan-curves') + $common) 'mapc_artisan_curves'
        Invoke-Checked $dotnet (@($mapcDll, 'mount-speeds') + $common) 'mapc_mount_speeds'
    } },
    @{ Name = 'database'; Title = 'Build content.db from CacheDB.dbc (contentc)'; Outputs = @('dist\content\content.db'); Run = {
        Assert-Files @('dist\tools\contentc.exe', 'dist\content\client_config\DefaultCombat.ini', 'dist\content\artisan_curves.tsv') 'run the tools, config and curves steps first'
        $savedLimit = $env:GOMEMLIMIT
        if (-not $env:GOMEMLIMIT) { $env:GOMEMLIMIT = '3GiB' }     # contentc peaks at 4.4 GB without it and at 2.9 GB with it, in the same time
        try {
            Invoke-Checked $contentc @('--dbc', $gameDbc, '--usmap', (Join-Path $outDir 'aoc.usmap'), '--config', (Join-Path $outDir 'client_config'),
                '--artisan-curves', (Join-Path $outDir 'artisan_curves.tsv'), '--out', (Join-Path $outDir 'content.db')) 'contentc'
        } finally { $env:GOMEMLIMIT = $savedLimit }
    } },
    @{ Name = 'region'; Title = "Read the zone-of-influence grid and cut the Riverlands' rectangles"; Outputs = @('dist\content\zoi_chunks', 'fixtures\region\riverlands_rects.json', 'fixtures\region\riverlands_zoi_rects.json', 'server\worldd\spawns\region_data.h', 'tools\mapc\region_rects.tsv'); Run = {
        Assert-Files @('dist\content\content.db', 'tools\mapc\bin\Release\net8.0\mapc.dll') 'run the earlier steps first'
        Invoke-Checked $dotnet @($mapcDll, 'export-zoi', '--paks', $gamePaks, '--usmap', $usmapSrc, '--out-dir', $outDir) 'mapc_export_zoi'
        $gen = Join-Path $repoRoot 'tools\capture'
        Invoke-Checked $python.Exe ($python.Prefix + @('-I', '-u', (Join-Path $gen 'gen_region_rects.py'), '--chunks', (Join-Path $outDir 'zoi_chunks'), '--content-db', (Join-Path $outDir 'content.db'))) 'gen_region_rects'
        Invoke-Checked $python.Exe ($python.Prefix + @('-I', '-u', (Join-Path $gen 'gen_region.py'))) 'gen_region'
    } },
    @{ Name = 'headers'; Title = "Generate worldd's tables and the citynode constants"; Outputs = @('server\worldd\world\zoi_surface_table.h', 'server\worldd\world\node_constants.h', 'services\internal\citynode\constants_gen.go', 'server\worldd\spawns\garden_defs.h'); Run = {
        Assert-Files @('dist\content\content.db', 'dist\content\zoi_chunks', 'dist\content\client_config\DefaultAoCRecordConstants.ini') 'run the earlier steps first'
        $gen = Join-Path $repoRoot 'tools\capture'
        $db = Join-Path $outDir 'content.db'
        Invoke-Checked $python.Exe ($python.Prefix + @('-I', '-u', (Join-Path $gen 'gen_zoi_surface.py'), '--chunks', (Join-Path $outDir 'zoi_chunks'), '--content-db', $db)) 'gen_zoi_surface'
        Invoke-Checked $python.Exe ($python.Prefix + @('-I', '-u', (Join-Path $gen 'gen_node_constants.py'), '--ini', (Join-Path $outDir 'client_config\DefaultAoCRecordConstants.ini'))) 'gen_node_constants'
        Invoke-Checked $python.Exe ($python.Prefix + @('-I', '-u', (Join-Path $gen 'gen_gm_garden.py'), '--content-db', $db)) 'gen_gm_garden'
    } },
    @{ Name = 'fixtures'; Title = 'Generate the test fixtures'; Outputs = @('server\worldd\stats\testdata\corpus.txt', 'server\worldd\tests\talent_fixture.sql', 'server\worldd\tests\gear_fixture.h', 'server\worldd\tests\tooltip_key.h'); Run = {
        Assert-Files @('dist\content\content.db', 'dist\tools\statprog.exe') 'run the earlier steps first'
        $db = Join-Path $outDir 'content.db'
        New-Item -ItemType Directory -Force -Path (Join-Path $repoRoot 'server\worldd\stats\testdata') | Out-Null
        Invoke-Checked $statprog @('corpus', '--db', $db, '--out', (Join-Path $repoRoot 'server\worldd\stats\testdata\corpus.txt')) 'statprog_corpus'
        Invoke-Checked $python.Exe ($python.Prefix + @('-I', '-u', (Join-Path $repoRoot 'tools\worldd\gen_talent_fixture.py'), $db)) 'gen_talent_fixture'
        Invoke-Checked $python.Exe ($python.Prefix + @('-I', '-u', (Join-Path $repoRoot 'tools\worldd\gen_gear_fixture.py'), $db)) 'gen_gear_fixture'
        Invoke-Checked $python.Exe ($python.Prefix + @('-I', '-u', (Join-Path $repoRoot 'tools\capture\gen_tooltips.py'), '--db', $db)) 'gen_tooltips'
    } },
    @{ Name = 'navbuild'; Title = 'Build aoc_navbuild, the navigation mesh builder (C++; the first build also builds the C++ libraries with vcpkg)'; Outputs = @('dist\Release\aoc_navbuild.exe'); Run = {
        Assert-Files @('server\worldd\world\zoi_surface_table.h', 'server\worldd\spawns\region_data.h', 'server\worldd\spawns\garden_defs.h') 'run the earlier steps first'
        Invoke-Checked 'powershell.exe' @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $repoRoot 'scripts\build.ps1'), '-Target', 'aoc_navbuild', '-Jobs', "$Jobs") 'build_navbuild'
    } },
    @{ Name = 'roads'; Title = 'Read the road splines of the Riverlands (mapc) and resolve them (contentc)'; Outputs = @('dist\content\roads_raw.tsv', 'dist\content\roads.tsv', 'dist\content\roads_levels.tsv'); Run = {
        Assert-Files @('dist\content\content.db', 'tools\mapc\region_rects.tsv', 'dist\tools\contentc.exe', 'tools\mapc\bin\Release\net8.0\mapc.dll') 'run the earlier steps first'
        Invoke-Checked $dotnet @($mapcDll, 'roads', '--paks', $gamePaks, '--usmap', $usmapSrc, '--out-dir', $outDir) 'mapc_roads'
        $db = Join-Path $outDir 'content.db'
        $raw = Join-Path $outDir 'roads_raw.tsv'
        Invoke-Checked $contentc @('roads', '--content', $db, '--raw', $raw, '--out', (Join-Path $outDir 'roads.tsv')) 'contentc_roads'
        Invoke-Checked $contentc @('roads', '--levels', '--content', $db, '--raw', $raw, '--out', (Join-Path $outDir 'roads_levels.tsv')) 'contentc_roads_levels'
    } },
    @{ Name = 'navmesh'; Title = 'Read the map and build the navigation mesh, the NPC motion table and the foliage (mapc; the longest step)'; Outputs = @('dist\content\npc_motion.tsv', 'dist\content\region_foliage.tsv', 'dist\content\area_geom.bin', 'dist\content\area_nav.bin'); Run = {
        Assert-Files @('tools\mapc\region_rects.tsv', 'dist\Release\aoc_navbuild.exe', 'tools\mapc\bin\Release\net8.0\mapc.dll') 'run the earlier steps first'
        $savedHeap = $env:DOTNET_GCHeapHardLimit
        if (-not $env:DOTNET_GCHeapHardLimit) { $env:DOTNET_GCHeapHardLimit = '0x100000000' }     # the NPC Blueprint pass peaks at 7.7 GB without this limit and at 4.0 GB with it, in the same time; the map read's child programs set their own
        try {
            Invoke-Checked $dotnet @($mapcDll, 'build', '--paks', $gamePaks, '--usmap', $usmapSrc, '--out-dir', $outDir, '--navbuild', $navbuild, '--threads', "$NavThreads") 'mapc_build'
        } finally { $env:DOTNET_GCHeapHardLimit = $savedHeap }
    } }
)

$names = @($steps | ForEach-Object { $_.Name })
foreach ($n in @($Only) + @($Skip)) {
    if ($n -and $names -notcontains $n) { Fail "Unknown step '$n'. The steps are: $($names -join ', ')." }
}
$selected = @($steps | Where-Object { ($Only.Count -eq 0 -or $Only -contains $_.Name) -and ($Skip.Count -eq 0 -or $Skip -notcontains $_.Name) })

# ---------------------------------------------------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------------------------------------------------
function Test-Todo($s) { $Force -or $s.Always -or -not (Test-Current $s) }

if ($CheckOnly) {
    Say ''
    foreach ($s in $selected) {
        $state = if ($s.Always) { 'builds the tools when a later step runs' } elseif (Test-Todo $s) { 'would run' } else { 'up to date' }
        Say ('  {0,-9} {1}' -f $s.Name, $state)
    }
    exit 0
}

$lockFile = Join-Path $stampDir 'running.lock'
if (Test-Path -LiteralPath $lockFile) {
    $other = 0
    try { $other = [int](Get-Content -LiteralPath $lockFile -TotalCount 1) } catch { }
    if ($other -gt 0 -and $other -ne $PID -and (Get-Process -Id $other -ErrorAction SilentlyContinue)) {
        Fail "Another extract_game_data.ps1 is running (process $other)." 'Wait for it to finish.'
    }
}
Set-Content -LiteralPath $lockFile -Value $PID -Encoding ASCII

$summary = New-Object System.Collections.Generic.List[object]
$total = [Diagnostics.Stopwatch]::StartNew()
$anyWork = (@($selected | Where-Object { -not $_.Always -and (Test-Todo $_) }).Count -gt 0) -or ($Only -contains 'tools')
$index = 0
$ran = 0
try {
    foreach ($s in $selected) {
        $index++
        if ($s.Always -and -not $anyWork) { continue }
        if (-not (Test-Todo $s)) { $summary.Add([pscustomobject]@{ Step = $s.Name; State = 'up to date'; Seconds = 0.0; PeakGB = 0.0 }); continue }
        Say ''
        Say ("[{0}/{1}] {2}" -f $index, $selected.Count, $s.Title) 'Cyan'
        $script:stepPeak = [long]0
        $sw = [Diagnostics.Stopwatch]::StartNew()
        & $s.Run
        $sw.Stop()
        if ($s.Outputs.Count -gt 0) {
            Assert-Files $s.Outputs "the $($s.Name) step finished without writing it"
            $stamp = [ordered]@{ step = $s.Name; inputs = (Get-Id $s.Name); finished = (Get-Date).ToUniversalTime().ToString('s') + 'Z'; seconds = [Math]::Round($sw.Elapsed.TotalSeconds, 1); outputs = $s.Outputs }
            ($stamp | ConvertTo-Json) | Set-Content -LiteralPath (Join-Path $stampDir "$($s.Name).json") -Encoding ASCII
        }
        $peakGb = [Math]::Round($script:stepPeak / 1GB, 2)
        Say ("    done in {0}; the biggest program held {1:N2} GB at most" -f (Format-Seconds $sw.Elapsed.TotalSeconds), $peakGb) 'Green'
        $summary.Add([pscustomobject]@{ Step = $s.Name; State = 'ran'; Seconds = [Math]::Round($sw.Elapsed.TotalSeconds, 1); PeakGB = $peakGb })
        $ran++
    }
} catch {
    Say ''
    Say "ERROR in step '$($selected[$index - 1].Name)': $($_.Exception.Message)" 'Red'
    Say "       What each program printed is in $stampDir (last_run.log has all of it). Steps that finished keep their stamps; run this again to continue (README.md, 'If something goes wrong')." 'Yellow'
    Copy-Item -Force -LiteralPath $logFile -Destination $lastLog
    Remove-Item -Force -ErrorAction SilentlyContinue $lockFile
    exit 1
}
Remove-Item -Force -ErrorAction SilentlyContinue $lockFile
$total.Stop()

Say ''
if ($ran -eq 0) {
    Write-Host 'Game data: up to date.' -ForegroundColor Green
} else {
    foreach ($r in $summary) { Say ('  {0,-9} {1,-10} {2,-10} {3}' -f $r.Step, $r.State, $(if ($r.State -eq 'ran') { Format-Seconds $r.Seconds } else { '' }), $(if ($r.State -eq 'ran') { '{0:N2} GB' -f $r.PeakGB } else { '' })) }
    Say ("Game data ready in {0}: {1}" -f (Format-Seconds $total.Elapsed.TotalSeconds), $outDir) 'Green'
    Copy-Item -Force -LiteralPath $logFile -Destination $lastLog
}
exit 0
