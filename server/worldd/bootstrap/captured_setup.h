// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  bootstrap/captured_setup.h
//
//  The capture's character as a PlayerSetup (the golden tests' input), and the
//  captured Bard's level-1 stats and abilities that stand in for the ones the
//  character service and content.db do not compute yet (spec section 5).
//
//  The capture's character (transcript section 0): a level-1 Kaelar male Bard
//  named RandomChar, Guid D8B2FE19 95FB4DF9 9B1E3958 6C2B2BD4, class record
//  108640158570105, 442 stats, 57 abilities, spawned at (-777664.5, 616578.6875,
//  15952.0498) cm facing 90.9 degrees, in the session ServerId 60, Randomizer
//  1860730596 where the PlayerController is GUID 10341530 (the pawn 10341538 ...).
// ============================================================================
#pragma once

#include "bootstrap/player_setup.h"

#include <optional>

namespace aoc::worldd::bootstrap {

/// The capture's ServerId and GUID randomizer (every dynamic GUID of the session).
inline constexpr uint32_t kCapturedServerId = 60;
inline constexpr uint32_t kCapturedRandomizer = 1860730596u;

/// The captured Bard's 442 level-1 stats, as sent (captured_stats.h): the primary attributes and the derived stats.
StatsSetup captured_bard_level1_stats();

/// The captured Bard's KnownAbilities (57) and ReplicatedRecordsToLoad (58).
std::vector<int64_t>       captured_bard_known_abilities();
std::vector<AbilityRecord> captured_bard_records_to_load();

/// Everything the original server sent for its character: the golden tests' PlayerSetup. The world time is the
/// PlayerController's (1593462.375: SummonCooldownTimer, and the PlayerState's whole-second StartTime 1593462);
/// the GameState's was 80 ms later (kGameStateWorldTimeBits). nullopt when the captured appearance does not read.
std::optional<PlayerSetup> captured_character_setup();

}  // namespace aoc::worldd::bootstrap
