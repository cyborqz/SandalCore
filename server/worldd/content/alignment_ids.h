// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  content/alignment_ids.h: the two alignment records the PvP flag swaps on a player (Social, seeing other players: their combat stances)
//
//  A player carries the NonCombatant (the white name) or the Combatant (the purple name, a 60 s removal timer in the record) in the AlignmentComponent; both records override the
//  nameplate colour.  The game's own flagging rules put the Combatant on a player at his first harmful action; until that action exists here the PvP flag stands for it
//  (PlayerBootstrap::set_flag_alignment).  Header only: the guids are the records' own (content.db's `record` table, the names Combatant and NonCombatant).
// ============================================================================
#pragma once

#include <cstdint>

namespace aoc::worldd::content {

inline constexpr int64_t kAlignmentCombatant = 6064629443707731976ll;
inline constexpr int64_t kAlignmentNonCombatant = 6064629443712188425ll;

}  // namespace aoc::worldd::content
