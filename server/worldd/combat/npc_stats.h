// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/npc_stats.h  (plan E1, stage 3, part 1)
//
//  An NPC's stats: the stat engine run for the kind of NPC at its level.  A creature's StatsComponent starts from three initializer lists
//  (npc_motion.tsv's `lists` column: BaseStatInitializer CombatStats, StatInitializerList1 NPC_Base, StatInitializerList2 the elite scalar);
//  the stat program (statprog's NpcProfiles) holds them as the classes kNpcClassBase ("NPC": NPC_Elite_Scalar) and kNpcClassBase + 1
//  ("NPC_Guard": the Guard overtuned scalar), with no common lists.  The Blueprint's own numbers come with the run as the NPC state:
//
//    StatDistribution   eight weights (Constitution, Strength, Dexterity, Mentality, Intelligence, Wisdom, MaxHealth, MaxMana); the formulas read
//                       GetNpcStatDistribution(npc, stat) = the weight over the sum of the eight
//    EliteLevelRanks    (level, rank) pairs: the rank of the last pair whose level is not above the NPC's; GetNpcEliteRank
//
//  The 46 pawn Blueprints that name another scalar (the elder dragons, a pet, a target dummy) take the first profile.  The NPC's level is the
//  instance's (0 in the data is 1: the engine's levels start there).
// ============================================================================
#pragma once

#include "npc/motion_table.h"
#include "stats/character_stats.h"

#include <cstdint>
#include <memory>
#include <string>

namespace aoc::worldd::combat {

/// StatInitializerList_NPC_Elite_Scalar_Guard_Overtuned: the Guard profile's scalar.
inline constexpr int64_t kGuidGuardOvertunedList = 6064632280717066264ll;

class NpcStatBook {
public:
    /// `bundle` and `gear` must outlive the book.
    NpcStatBook(const stats::StatBundle& bundle, const stats::GearCatalog& gear) : bundle_(&bundle), gear_(&gear) {}

    /// The class enum of the profile a Blueprint's lists make (kNpcClassBase: NPC, +1: NPC_Guard); 0 when the stat program has no NPC profile.
    int profile_for(const npc::MotionStats& bp) const;
    /// The Blueprint's elite rank at a level and its stat distribution as shares.
    static stats::NpcShape shape_for(const npc::MotionStats& bp, int level);
    /// The NPC's stats at `level`, started: every held value (Health, Mana) at its maximum.  nullptr, with `err`, when the program has no NPC profile or the
    /// engine refuses.
    std::unique_ptr<stats::CharacterStats> make(const npc::MotionStats& bp, int level, std::string& err) const;

    /// The level the engine runs an NPC of the data's level at: 1 .. kMaxLevel.
    static int engine_level(int data_level);

private:
    const stats::StatBundle*   bundle_;
    const stats::GearCatalog*  gear_;
};

}  // namespace aoc::worldd::combat
