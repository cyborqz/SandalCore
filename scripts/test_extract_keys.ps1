# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

<#
.SYNOPSIS
  Checks which edits of server\worldd\CMakeLists.txt make the navigation mesh builder's step (and through it the navigation mesh) run again.

.DESCRIPTION
  scripts\extract_game_data.ps1 keys the navbuild step on the CMake lines that build aoc_navbuild (Get-NavCMakeLines), so that a test added to the file runs nothing again. This
  takes that function out of the script, runs it on the real file and on copies of it that carry one edit each, and checks that

    - an edit that cannot change the builder (a test registered, another program added, a library added to worldd_core, line ends) leaves the lines the same;
    - an edit that can change it changes them: a line before the builder's block, a line inside it, a command after it that names aoc_navbuild, worldd_nav or recast_detour (on one line or
      with the name on the next), and a command that sets a flag for every target of the file (add_compile_definitions, include_directories, set(CMAKE_CXX_FLAGS ...) and the like);
    - a file in which the end of the block is not found counts whole.

  It needs nothing but PowerShell (no game, no build) and takes a second.  Run: powershell -File scripts\test_extract_keys.ps1      Exit 0 when every check holds, 1 otherwise.
#>
$ErrorActionPreference = 'Stop'
$checks = 0
$failed = 0
function Check([bool]$ok, [string]$what) {
    $script:checks++
    if (-not $ok) { $script:failed++; Write-Host "FAIL: $what" -ForegroundColor Red } else { Write-Host "ok:   $what" }
}

$extract = Join-Path $PSScriptRoot 'extract_game_data.ps1'
$realCMake = Join-Path (Split-Path $PSScriptRoot -Parent) 'server\worldd\CMakeLists.txt'
$tokens = $null; $errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($extract, [ref]$tokens, [ref]$errors)
$def = $ast.Find({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Get-NavCMakeLines' }, $true)
if (-not $def) { throw 'Get-NavCMakeLines is not in extract_game_data.ps1' }
. ([scriptblock]::Create($def.Extent.Text))

$tmp = Join-Path $env:TEMP "aoc-extract-keys-test-$PID"
$repoRoot = $tmp          # (what Get-NavCMakeLines reads its file from)
New-Item -ItemType Directory -Force (Join-Path $tmp 'server\worldd') | Out-Null
$file = Join-Path $tmp 'server\worldd\CMakeLists.txt'
function Lines([string]$text, [string]$eol = "`n") {
    [IO.File]::WriteAllText($file, ($text -replace "`r`n", "`n").Replace("`n", $eol), (New-Object Text.UTF8Encoding($false)))
    return (Get-NavCMakeLines)
}
function After([string]$text, [string]$anchor, [string]$insert) {
    if (-not $text.Contains($anchor)) { throw "'$anchor' is not in the file" }
    return $text.Replace($anchor, $anchor + "`n" + $insert)
}
try {
    $base = ([IO.File]::ReadAllText($realCMake) -replace "`r`n", "`n").TrimEnd() + "`n"
    $key = Lines $base
    Check ($key.Length -gt 1000 -and $key.Length -lt $base.Length -and $key -match 'add_executable\(aoc_navbuild' -and $key -match 'set_target_properties\(aoc_navbuild') 'the real file: the key holds the builder''s block and is shorter than the file'
    Check ($key -notmatch 'worldd_add_test' -and $key -notmatch 'test_worldd_nav_window') 'the real file: the key holds no test registration'

    # edits that cannot change the builder
    Check ((Lines $base "`r`n") -ceq $key) 'the same file with CRLF line ends gives the same lines'
    Check ((Lines (After $base '    test_worldd_nav_window' '    test_worldd_speedprobe')) -ceq $key) 'a test registered after the block changes nothing'
    Check ((Lines ($base + "add_executable(zz_probe zz.cpp)`ntarget_link_libraries(zz_probe PRIVATE worldd_core worldd_nav)`ntarget_include_directories(zz_probe PRIVATE `"`${CMAKE_CURRENT_SOURCE_DIR}`")`n")) -ceq $key) 'another program added after the block (it links worldd_nav) changes nothing'
    Check ((Lines (After $base '    worldd_ability_content' '    zz_probe_lib')) -ceq $key) 'a library added to worldd_core''s link list (which names worldd_nav) changes nothing'
    Check ((Lines ($base + "if(MSVC)`n    target_compile_options(zz_probe PRIVATE /bigobj)`nendif()`n")) -ceq $key) 'a compile option of another target changes nothing'

    # edits that can change it
    Check ((Lines (After $base '    nav/town_geometry.h' '    nav/probe.h')) -cne $key) 'a source added to worldd_nav (inside the block) changes the lines'
    Check ((Lines $base.Replace('find_package(unofficial-sqlite3 CONFIG REQUIRED)', 'find_package(unofficial-sqlite3 CONFIG REQUIRED PROBE)')) -cne $key) 'a line before the block changes the lines'
    Check ((Lines ($base + "target_compile_definitions(worldd_nav PRIVATE PROBE=1)`n")) -cne $key) 'target_compile_definitions(worldd_nav ...) after the block changes the lines'
    Check ((Lines ($base + "target_link_libraries(aoc_navbuild PRIVATE probe_lib)`n")) -cne $key) 'target_link_libraries(aoc_navbuild ...) after the block changes the lines'
    Check ((Lines ($base + "target_link_libraries(`n    aoc_navbuild PRIVATE probe_lib)`n")) -cne $key) 'the same with the name on the next line changes the lines'
    Check ((Lines ($base + "set_property(TARGET recast_detour PROPERTY PROBE 1)`n")) -cne $key) 'set_property(TARGET recast_detour ...) after the block changes the lines'
    Check ((Lines ($base + "add_dependencies(aoc_navbuild zz_probe)`n")) -cne $key) 'add_dependencies(aoc_navbuild ...) after the block changes the lines'
    Check ((Lines ($base + "add_compile_definitions(PROBE=1)`n")) -cne $key) 'add_compile_definitions(...) after the block (it reaches every target of the file) changes the lines'
    Check ((Lines ($base + "add_definitions(-DPROBE)`n")) -cne $key) 'add_definitions(...) after the block changes the lines'
    Check ((Lines ($base + "include_directories(probe)`n")) -cne $key) 'include_directories(...) after the block changes the lines'
    Check ((Lines ($base + "set(CMAKE_CXX_FLAGS `"`${CMAKE_CXX_FLAGS} /DPROBE`")`n")) -cne $key) 'set(CMAKE_CXX_FLAGS ...) after the block changes the lines'
    Check ((Lines ($base + "function(zz_probe)`n    include_directories(probe)`nendfunction()`n")) -cne $key) 'include_directories(...) in a function after the block changes the lines'

    # a file whose block end is not found counts whole
    $noEnd = $base.Replace('set_target_properties(aoc_navbuild PROPERTIES FOLDER "Servers")', '# (no block end here)')
    Check ($noEnd -ne $base) '(the block end was taken out of the copy)'
    Check ((Lines (After $noEnd '    test_worldd_nav_window' '    test_worldd_speedprobe')) -cne (Lines $noEnd)) 'with no block end found an edit anywhere changes the lines'
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
Write-Host ("{0} checks, {1} failed" -f $checks, $failed)
if ($failed -gt 0) { exit 1 }
exit 0
