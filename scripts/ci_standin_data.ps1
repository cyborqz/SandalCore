# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

# ============================================================================
#  ci_standin_data.ps1 - write placeholders for the generated game-data files, for the data-free build of the CI job
#
#  Usage (PowerShell):
#      .\scripts\ci_standin_data.ps1                    # in the CI job (GITHUB_ACTIONS=true): write the 8 placeholder files
#      .\scripts\ci_standin_data.ps1 -AllowOutsideCI    # the same in a clone that has no game data, for a local check of the CI build
#      .\scripts\ci_standin_data.ps1 -Remove            # delete the placeholder files again (only files this script wrote)
#
#  The root CMakeLists.txt stops when the files that scripts\extract_game_data.ps1 makes from a player's own game install are missing.
#  A CI machine has no game install, so this script writes small placeholders at the same places (they are git-ignored). They compile
#  and hold no game data: empty tables, zero constants, and a first line "aoc-standin" that marks every file as a placeholder.
#  The test programs that need the real tables do not run in CI (scripts\ci_needs_game_data.txt lists them); the rest build and run on the placeholders.
#
#  Safety: the script REFUSES, and writes nothing, when
#   - it does not run in the CI job and -AllowOutsideCI is not given, or
#   - dist\content exists and holds files (this clone has game data), or
#   - any of the 8 files exists and is not one of its own placeholders (a real file made from a game install).
#  A player's data is never overwritten. The script never deletes anything but its own placeholders (-Remove).
#
#  Exit codes: 0 done, 1 refused or failed.
# ============================================================================
[CmdletBinding()]
param(
    [string] $Root = '',
    [switch] $AllowOutsideCI,
    [switch] $Remove
)
$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = Split-Path -Parent (Split-Path -Parent $PSCommandPath) }   # the repository root: the folder above this script's folder

$marker = 'aoc-standin'
$utf8 = New-Object System.Text.UTF8Encoding($false)
$files = New-Object System.Collections.Generic.List[object]
function Add-File([string] $path, [string] $text) { $files.Add([pscustomobject]@{ Path = $path; Text = ($text -replace "`r`n", "`n") }) }

# The first line of a file, or $null when there is none.
function Get-FirstLine([string] $full) {
    $l = Get-Content -LiteralPath $full -TotalCount 1 -ErrorAction SilentlyContinue
    if ($null -eq $l) { return $null }
    return [string]($l | Select-Object -First 1)
}
function Test-Placeholder([string] $full) {
    $l = Get-FirstLine $full
    return ($null -ne $l -and $l.Contains($marker))
}

# ---- the placeholders -------------------------------------------------------------------------------------------------------------
Add-File 'server/worldd/world/zoi_surface_table.h' @'
// aoc-standin: a placeholder that scripts/ci_standin_data.ps1 writes for the data-free CI build. It holds no game data.
// The real file is made from the player's own game install by scripts/extract_game_data.ps1.
#pragma once

#include <cstdint>

#define AOC_STANDIN_DATA 1

namespace aoc::worldd::world::zoi_surface {

// One empty chunk: no position is inside a zone of influence.
inline constexpr int kChunkCm = 1;
inline constexpr int kCellCm = 1;
inline constexpr int kCells = 1;
inline constexpr int kMinCx = 0;
inline constexpr int kMinCy = 0;
inline constexpr int kColumns = 1;
inline constexpr int kRowsOfChunks = 1;
inline constexpr uint32_t kNone = 0xFFFFFFFFu;
inline constexpr uint32_t kUniform = 0x80000000u;

inline constexpr uint64_t kPalette[1] = {0ULL};
inline constexpr uint32_t kIndex[1] = {kNone};
inline constexpr uint8_t kRuns[1] = {0};

}  // namespace aoc::worldd::world::zoi_surface
'@

Add-File 'server/worldd/world/node_constants.h' @'
// aoc-standin: a placeholder that scripts/ci_standin_data.ps1 writes for the data-free CI build. It holds no game data.
// The real file is made from the player's own game install by scripts/extract_game_data.ps1.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#define AOC_STANDIN_DATA 1

namespace aoc::worldd::world::node_constants {

inline constexpr std::array<int64_t, 7> kExpToLevel = {0LL, 0LL, 0LL, 0LL, 0LL, 0LL, 0LL};
inline constexpr std::array<int64_t, 5> kReputationToLevel = {0LL, 0LL, 0LL, 0LL, 0LL};
inline constexpr int kMaxNodeLevel = 0;
inline constexpr int kMinimumOperationalNodeLevel = 0;
inline constexpr int kNodeCitizenshipPlayerLevelRequirement = 0;
inline constexpr int kNodePassiveIncomeSeconds = 0;
inline constexpr int kMaxNumberNodeNotifications = 0;
inline constexpr std::array<int64_t, 1> kMayoralCommissionHandRefreshSeconds = {0LL};
inline constexpr bool kVassalshipSystemEnabled = false;
inline constexpr bool kVassalshipProtectionEnabled = false;
inline constexpr int64_t kVassalshipDefinitionGuid = 0LL;
inline constexpr int64_t kBuyOrderConstantsGuid = 0LL;
inline constexpr int64_t kElectionConstantsGuid = 0LL;
inline constexpr int64_t kWarConstantsGuid = 0LL;
inline constexpr int64_t kCitizenshipRewardGuid = 0LL;
inline constexpr int64_t kNodeCurrencyGuid = 0LL;
inline constexpr int64_t kNodeAlignmentGuid = 0LL;
inline constexpr int64_t kNodeWarDeclaringAlignmentGuid = 0LL;
inline constexpr int64_t kNodeWarTargetAlignmentGuid = 0LL;
inline constexpr int64_t kNodeSiegeDeclaringAlignmentGuid = 0LL;
inline constexpr int64_t kNodeSiegeTargetAlignmentGuid = 0LL;
inline constexpr int64_t kNodeLevelUpCountdownToastGuid = 0LL;
inline constexpr int64_t kNodeCitizenshipGainToastGuid = 0LL;
inline constexpr int kServiceBuildingDefaultRepairSeconds = 0;
inline constexpr int kServiceBuildingDefaultDemolitionSeconds = 0;
inline constexpr bool kServiceBuildingDemolitionVotingEnabled = false;
inline constexpr int kServiceBuildingMaintenanceFrequencyWeeks = 0;
inline constexpr std::string_view kServiceBuildingMaintenanceDayOfWeek = "";
inline constexpr int kZoiPlayerCountRefreshSeconds = 0;
inline constexpr int kMayorMessageCooldownSeconds = 0;
inline constexpr int kMaxMayorMessageLength = 0;
inline constexpr int kStaticHouseUpgradeSeconds = 0;
inline constexpr int kSiegeJoinRequestCooldownSeconds = 0;
inline constexpr std::string_view kSiegeInactiveTag = "";
inline constexpr std::string_view kSiegePrepTag = "";
inline constexpr std::string_view kSiegeActiveTag = "";
inline constexpr std::string_view kSiegePillageTag = "";

}  // namespace aoc::worldd::world::node_constants
'@

Add-File 'server/worldd/spawns/region_data.h' @'
// aoc-standin: a placeholder that scripts/ci_standin_data.ps1 writes for the data-free CI build. It holds no game data.
// The real file is made from the player's own game install by scripts/extract_game_data.ps1.
#pragma once

#include <cstddef>
#include <cstdint>

#define AOC_STANDIN_DATA 1

namespace aoc::worldd::spawns::region {

struct Rect {
    int32_t x0, y0, x1, y1;   // world cm, [x0, x1) x [y0, y1)
};

struct ZoiRows {
    uint64_t guid;            // the zone's record guid
    const char* name;         // its record name
    uint32_t first, count;    // its rows of kZoiRects
    uint32_t cells;           // its area in cells of kCellCm
};

inline constexpr int32_t kCellCm = 1;
inline constexpr uint32_t kRiverlandsAreaCells = 0;

// No rectangles and no zones: every count is 0. Each array keeps one empty row, because C++ has no zero-size arrays.
inline constexpr Rect kRiverlandsRects[] = {{0, 0, 0, 0}};
inline constexpr size_t kRiverlandsRectCount = 0;

inline constexpr Rect kZoiRects[] = {{0, 0, 0, 0}};
inline constexpr size_t kZoiRectCount = 0;

inline constexpr ZoiRows kZois[] = {{0ull, "", 0, 0, 0}};
inline constexpr size_t kZoiCount = 0;

}  // namespace aoc::worldd::spawns::region
'@

Add-File 'server/worldd/spawns/garden_defs.h' @'
// aoc-standin: a placeholder that scripts/ci_standin_data.ps1 writes for the data-free CI build. It holds no game data.
// The real file is made from the player's own game install by scripts/extract_game_data.ps1.
#pragma once

#include <cstddef>
#include <cstdint>

#define AOC_STANDIN_DATA 1

namespace aoc::worldd::spawns::garden {

struct Def {
    const char* profession;
    int         tier;
    const char* tier_name;
    uint64_t    guid;
    uint64_t    type_id;
    uint64_t    interaction;
    const char* name;
    const char* resource;
    const char* tool;
    double      base_duration;
    int         candidates;
};

struct Gap {
    const char* profession;
    int         tier;
    const char* tier_name;
};

// No definitions and no gaps: both counts are 0. Each array keeps one empty row, because C++ has no zero-size arrays.
inline constexpr Def kGardenDefs[] = {{"", 0, "", 0ull, 0ull, 0ull, "", "", "", 0.0, 0}};
inline constexpr size_t kGardenDefCount = 0;

inline constexpr Gap kGardenGaps[] = {{"", 0, ""}};
inline constexpr size_t kGardenGapCount = 0;

}  // namespace aoc::worldd::spawns::garden
'@

Add-File 'server/worldd/stats/testdata/corpus.txt' @'
# aoc-standin: a placeholder that scripts/ci_standin_data.ps1 writes for the data-free CI build. It holds no game data.
# The real file is written by the Go stat compiler from the player's own game install.
'@

Add-File 'server/worldd/tests/talent_fixture.sql' @'
-- aoc-standin: a placeholder that scripts/ci_standin_data.ps1 writes for the data-free CI build. It holds no game data.
-- The real file is made from the player's own game install by scripts/extract_game_data.ps1.
'@

Add-File 'server/worldd/tests/gear_fixture.h' @'
// aoc-standin: a placeholder that scripts/ci_standin_data.ps1 writes for the data-free CI build. It holds no game data.
// The real file is made from the player's own game install by scripts/extract_game_data.ps1.
#pragma once

#include <cstdint>

#define AOC_STANDIN_DATA 1

namespace gear_fixture {

struct KitItem {
    int     slot;
    int64_t record;
};

// One empty statement, and kits without items (each array keeps one empty row, because C++ has no zero-size arrays).
inline constexpr const char* kStatements[] = {""};

inline constexpr KitItem kRangerKit[] = {{0, 0}};
inline constexpr KitItem kBardKit[] = {{0, 0}};
inline constexpr KitItem kClericKit[] = {{0, 0}};
inline constexpr KitItem kFighterKit[] = {{0, 0}};
inline constexpr KitItem kRogueKit[] = {{0, 0}};
inline constexpr KitItem kSummonerKit[] = {{0, 0}};
inline constexpr KitItem kTankKit[] = {{0, 0}};
inline constexpr KitItem kMageKit[] = {{0, 0}};

struct ClassKit {
    int            class_id;
    const char*    name;
    int64_t        guid;
    const KitItem* items;
    int            count;
};
inline constexpr ClassKit kClassKits[] = {{0, "", 0, kRangerKit, 0}};

}  // namespace gear_fixture
'@

Add-File 'server/worldd/tests/tooltip_key.h' @'
// aoc-standin: a placeholder that scripts/ci_standin_data.ps1 writes for the data-free CI build. It holds no game data.
// The real file is made by the tooltip generator from the player's own game install.
#pragma once

#include <cstdint>

#define AOC_STANDIN_DATA 1

namespace tooltip_key {

constexpr int kLevel = 0;

struct Key {
    const char* group;
    const char* name;
    int64_t     guid;
    double      cooldown_s;
    double      cooldown_all_s;
    const char* shared_tag;
    int         charges;
    int         charges_all;
    const char* costs;
    double      cast_s;
    double      range_cm;
    const char* range_source;
    int         target_mask;
    const char* target_requirement;
    const char* held;
    const char* weapon_requirement;
    const char* hits;
    const char* applies;
    const char* text;
};

// No keys: the count is 0. The array keeps one empty row, because C++ has no zero-size arrays.
const Key kKeys[] = {{"", "", 0ll, 0.0, 0.0, "", 0, 0, "", 0.0, 0.0, "", 0, "", "", "", "", "", ""}};

constexpr int kKeyCount = 0;

}  // namespace tooltip_key
'@

# ---- the run ----------------------------------------------------------------------------------------------------------------------
$rootFull = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\')
function Say([string] $m) { Write-Output "ci_standin_data: $m" }
function Refuse([string] $m) { Write-Output "ci_standin_data: REFUSED, nothing was written: $m"; exit 1 }

if ($Remove) {
    $removed = 0
    foreach ($f in $files) {
        $full = Join-Path $rootFull ($f.Path -replace '/', '\')
        if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { continue }
        if (Test-Placeholder $full) { Remove-Item -LiteralPath $full -Force; $removed++; Say "removed $($f.Path)" }
        else { Say "kept $($f.Path): it is not a placeholder" }
    }
    Say "removed $removed placeholder file(s)"
    exit 0
}

if ($env:GITHUB_ACTIONS -ne 'true' -and -not $AllowOutsideCI) {
    Refuse 'this script prepares the data-free build of the CI job; outside it, pass -AllowOutsideCI in a clone that has no game data'
}
$content = Join-Path $rootFull 'dist\content'
if ((Test-Path -LiteralPath $content -PathType Container) -and (Get-ChildItem -LiteralPath $content -Recurse -File -Force -ErrorAction SilentlyContinue | Select-Object -First 1)) {
    Refuse 'dist\content holds files: this clone has game data'
}
$real = New-Object System.Collections.Generic.List[string]
foreach ($f in $files) {
    $full = Join-Path $rootFull ($f.Path -replace '/', '\')
    if ((Test-Path -LiteralPath $full) -and -not (Test-Placeholder $full)) { $real.Add($f.Path) }
}
if ($real.Count -gt 0) {
    Refuse ("these files exist and are not placeholders (a game install made them): " + ($real -join ', '))
}

foreach ($f in $files) {
    $full = Join-Path $rootFull ($f.Path -replace '/', '\')
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $full) | Out-Null
    [System.IO.File]::WriteAllText($full, $f.Text, $utf8)
    Say "wrote $($f.Path)"
}
Say "done: $($files.Count) placeholder files, no game data"
exit 0
