# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

# ============================================================================
#  generators.ps1 - run every generated-file check of tools/capture (plan E1 review I2)
#
#  Usage (PowerShell, from anywhere):
#      .\scripts\generators.ps1                       # every gen_*.py --check whose inputs are at hand; exit 1 on a change or a failure
#      .\scripts\generators.ps1 -Picks F -Scan D -Cc D                # the research inputs (else the environment, below)
#      .\scripts\generators.ps1 -Only gen_region,gen_tooltips        # a subset (by script name, without .py)
#      .\scripts\generators.ps1 -Strict                              # a generator whose inputs are missing fails instead of being skipped
#
#  The generators and their inputs (each script's docstring). None is in the repository; each is named once here:
#      -RetailDir   AOC_RETAIL_JL      (unused: the generators that read the decoded retail captures are not in this repository)
#      -Picks       AOC_COMBAT_PICKS   the combat research's picks.json (gen_npc_kits)
#      -Scan        AOC_ACTORS_SCAN    the research's per-capture actor scan folder (gen_npc_appearance)
#      -Cc          AOC_RESEARCH_CC    the research's decoded captures, flat/<id>.jsonl (gen_equip)
#      -ContentDb   AOC_CONTENT_DB     dist\content\content.db (gen_tooltips, gen_gm_garden, gen_pop_golden, gen_node_pop_golden, gen_zoi_surface, gen_equip)
#      -ClientConfig AOC_CLIENT_CONFIG the Cache.db export's AOC\Config directory (gen_node_constants: DefaultAoCRecordConstants.ini); without it that generator is skipped
#      -ZoiChunks   AOC_ZOI_CHUNKS     the ZOI chunk folder of the export (gen_zoi_surface); without it that generator is skipped
#  gen_region.py (G1 stage 1) reads fixtures\region\*.json only;
#  gen_gm_garden.py and gen_pop_golden.py (G1 stage 1, Part B) read content.db (the second also npc_motion.tsv and spawns\area_spawns.h); gen_node_pop_golden.py (Nodes 1, stage 3b) the same
#  for the towns' people (tests\node_pop_golden.h).
#  `contentc roads --check` (mounts review B I4) is the one generator that is not Python: dist\content\roads.tsv must be what contentc resolves from content.db and
#  roads_raw.tsv (cheap: no map; `mapc roads --check`, a map read, is not run here).  It needs go on PATH.  `contentc roads --levels --check` (Nodes 1, stage 3b) does the same for
#  dist\content\roads_levels.tsv, every road at all seven node levels with its owner zone.
#  Every check writes nothing and exits 1 when a header would change.
# ============================================================================
[CmdletBinding()]
param(
    [string]   $RetailDir = $env:AOC_RETAIL_JL,
    [string]   $Picks     = $env:AOC_COMBAT_PICKS,
    [string]   $Scan      = $env:AOC_ACTORS_SCAN,
    [string]   $Cc        = $env:AOC_RESEARCH_CC,
    [string]   $ContentDb = $env:AOC_CONTENT_DB,
    [string]   $ClientConfig = $env:AOC_CLIENT_CONFIG,
    [string]   $ZoiChunks = $env:AOC_ZOI_CHUNKS,
    [int]      $Jobs      = 1,
    [string[]] $Only,
    [switch]   $Strict
)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
if (-not $ContentDb) { $ContentDb = Join-Path $root 'dist\content\content.db' }
# Nodes 1: the research inputs outside the repository (nothing is searched for: set the variable or pass the parameter)
$python = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $python) { throw "python is not on PATH" }
# ~ CoA :: C | Y | B ~

# name, the arguments, the inputs it needs (a list of parameter names that must be set and exist)
$gens = @(
    @{ name = 'gen_region';          args = @('--check');                                                    needs = @() },
    @{ name = 'gen_gm_garden';       args = @('--content-db', $ContentDb, '--check');                        needs = @('ContentDb') },
    @{ name = 'gen_pop_golden';      args = @('--content-db', $ContentDb, '--check');                        needs = @('ContentDb') },
    @{ name = 'gen_node_pop_golden'; args = @('--content-db', $ContentDb, '--check');                        needs = @('ContentDb') },
    @{ name = 'gen_npc_kits';        args = @('--picks', $Picks, '--check');                                 needs = @('Picks') },
    @{ name = 'gen_npc_appearance';  args = @('--scan', $Scan, '--check');                                   needs = @('Scan') },
    @{ name = 'gen_tooltips';        args = @('--db', $ContentDb, '--check');                                needs = @('ContentDb') },
    @{ name = 'gen_equip';           args = @((Join-Path $root 'server\worldd\tests\capture_equip.h'), $ContentDb, '--cc', $Cc, '--check'); needs = @('Cc', 'ContentDb') },
    # Nodes 1, stage 1: the city node constants from the ini, and the world's ZOI surface table from the chunk grid (both inputs are outside the repository)
    @{ name = 'gen_node_constants';  args = @('--ini', $(if ($ClientConfig) { Join-Path $ClientConfig 'DefaultAoCRecordConstants.ini' } else { '' }), '--check'); needs = @('ClientConfig') },
    @{ name = 'gen_zoi_surface';     args = @('--chunks', $ZoiChunks, '--content-db', $ContentDb, '--check');  needs = @('ZoiChunks', 'ContentDb') },
    @{ name = 'contentc roads';      go = @('run', './cmd/contentc', 'roads', '--content', $ContentDb, '--check');                       needs = @('ContentDb', 'RoadsRaw', 'RoadsTsv', 'Go') },
    @{ name = 'contentc roads --levels'; go = @('run', './cmd/contentc', 'roads', '--levels', '--content', $ContentDb, '--check');          needs = @('ContentDb', 'RoadsRaw', 'RoadsLevelsTsv', 'Go') }
)
$goExe = (Get-Command go -ErrorAction SilentlyContinue).Source
if (-not $goExe -and (Test-Path 'C:\Program Files\Go\bin\go.exe')) { $goExe = 'C:\Program Files\Go\bin\go.exe' }
$values = @{ RetailDir = $RetailDir; Picks = $Picks; Scan = $Scan; Cc = $Cc; ContentDb = $ContentDb; Go = $goExe; ClientConfig = $ClientConfig; ZoiChunks = $ZoiChunks
             RoadsRaw = (Join-Path $root 'dist\content\roads_raw.tsv'); RoadsTsv = (Join-Path $root 'dist\content\roads.tsv')
             RoadsLevelsTsv = (Join-Path $root 'dist\content\roads_levels.tsv') }

$failed = @(); $skipped = @(); $passed = @()
Push-Location $root
try {
    foreach ($g in $gens) {
        if ($Only -and ($Only -notcontains $g.name)) { continue }
        $missing = @($g.needs | Where-Object { -not $values[$_] -or -not (Test-Path $values[$_]) })
        if ($missing.Count -gt 0) {
            $what = ($missing | ForEach-Object { "-$_" }) -join ', '
            if ($Strict) { Write-Host "== $($g.name): FAIL: $what not given or not found"; $failed += $g.name }
            else { Write-Host "== $($g.name): SKIP: $what not given or not found"; $skipped += $g.name }
            continue
        }
        $t0 = Get-Date
        if ($g.go) {
            Write-Host ("== $($g.name) (go " + ($g.go -join ' ') + ")")
            Push-Location (Join-Path $root 'services')
            try { & $goExe @($g.go) 2>&1 | ForEach-Object { "$_" } | Write-Host; $code = $LASTEXITCODE } finally { Pop-Location }
        } else {
            $script = Join-Path $root ("tools\capture\" + $(if ($g.script) { $g.script } else { $g.name }) + ".py")
            Write-Host ("== $($g.name) " + ($g.args -join ' '))
            & $python $script @($g.args) 2>&1 | ForEach-Object { "$_" } | Write-Host
            $code = $LASTEXITCODE
        }
        Write-Host ("   exit={0} ({1:F0} s)" -f $code, ((Get-Date) - $t0).TotalSeconds)
        if ($code -eq 0) { $passed += $g.name } else { $failed += $g.name }
    }
} finally { Pop-Location }

Write-Host ("{0} generators unchanged, {1} skipped ({2}), {3} failed ({4})" -f $passed.Count, $skipped.Count, ($skipped -join ', '), $failed.Count, ($failed -join ', '))
if ($failed.Count -gt 0) { exit 1 }
exit 0
