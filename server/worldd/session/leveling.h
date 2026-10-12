// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/leveling.h
//
//  The character's own level (plan D1, stage 1) as pure logic: the Adventuring level curve of content.db (LevelingTypeDef
//  `LevelUpCurve`: the XP total at which each level is reached, 0 at level 1, 300 at 2, 4,000 at 3 ... 14,295,400 at 25,
//  16,563,400 at 26) and its cap (LevelCapOverride 25), the XP a level scroll grants, and what a grant of XP does: the new
//  total, the levels it crosses and the ranks of the Universal tree those levels grant by themselves (SkillTreeNodes with
//  bAutoGranted and a LevelRequirement: Sprint Training at 5, 20 and 35, Endurance Training at 10, 30 and 50, Guard and
//  Dodge Training at 15, 25, 40 and 45).
//
//  What the retail captures show [R: scratchpad dcodecs/levelup_evidence.md]: XP is replicated as absolute totals in the
//  XPTracker's ExperienceFastMap, with the ExperienceDebtFastMap item of Adventuring whenever XP comes in; a level-up of an
//  artisan or weapon type is the XP item plus BroadcastPlayLevelUpEffects on the pawn, a TempHealth float-array item and, for a
//  level-gated node, the auto-grant sent like a purchase's reply. No Adventuring level-up and no GM item use were captured:
//  the numbers below are the design data's, the behaviour is modelled on those captures (the part-2 spec, "Settled in D1").
//
//  Modelled, not captured:
//    - the level is the highest level of the curve whose XP total the character's total reaches, held at the cap; the XP
//      total is held just below the threshold of the level past the cap (the client derives the level from the same curve);
//    - the level scrolls' ExperienceValue (contentc resolved it to its parameters and checked the shape):
//      (curve(target) - curve(level) + debt_ratio * xp_of_level) / max(0.001, 1 + AdventuringXPAddMult) + 10, where
//      GetPlayerXPForCurrentLevel is read as the XP span of the current level (curve(level + 1) - curve(level); the
//      GameStatProfile's debt cap is that function, a death's debt 2% of it) and GetXPDebtRatio as debt / that span: so the
//      debt is paid back by the grant (xpDebt in the formula equals the debt);
//    - a grant pays the debt first, the rest raises the total (retail's debt item shrank as XP came in);
//    - the ranks: every node whose LevelRequirement the new level reaches and the old did not, in level order.
// ============================================================================
#pragma once

#include "content/content_db.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace aoc::worldd::leveling {
// ~ C-O-A & Cyb ~

/// The Adventuring curve with its cap.
class LevelCurve {
public:
    /// From content.db's leveling type; nullopt (with `error`) for a curve with no levels, one that does not start at level 1,
    /// one whose XP totals are not non-decreasing, or a cap past the curve.
    static std::optional<LevelCurve> from(const content::LevelingType& type, std::string& error);

    int     cap() const { return cap_; }
    int     max_level() const { return static_cast<int>(thresholds_.size()); }   // the curve's last level
    /// The XP total at which `level` is reached (level 1: 0); the last threshold for a level past the curve.
    int64_t xp_for(int level) const;
    /// The level a total gives: the highest level whose threshold it reaches, at most the cap.
    int     level_of(int64_t xp) const;
    /// The most XP the tracker holds: one below the threshold of the level past the cap (the curve's last total when the cap
    /// is its last level).
    int64_t max_xp() const;
    /// GetPlayerXPForCurrentLevel: the XP span of `level` (to the next level; the span below it at the curve's last level).
    int64_t span_of(int level) const;
    int64_t guid() const { return guid_; }

private:
    std::vector<int64_t> thresholds_;   // index level - 1
    int                  cap_ = 1;
    int64_t              guid_ = 0;
};

/// The character's Adventuring progress.
struct Progress {
    int     level = 1;
    int64_t xp = 0;
    int64_t debt = 0;
    bool operator==(const Progress&) const = default;
};

/// The XP a level scroll grants (the ExperienceValue as contentc resolved it): nullopt when `target` is not a level of the
/// curve, 0 when the character is at or past it (the table's predicate GetPlayerLevel() < N refuses the use before this).
std::optional<int64_t> scroll_xp(const LevelCurve& curve, int target_level, const Progress& p, double xp_add_mult, double margin);

/// What a grant of XP did.
struct XpResult {
    Progress                        before, after;
    std::vector<const content::AutoGrant*> granted;   // the ranks the new levels grant, in level order
    bool leveled() const { return after.level != before.level; }
    bool changed() const { return !(after == before); }
};

/// Adds `delta` XP (0 or more: the debt is paid first, the rest raises the total, held at max_xp) and finds the new level and
/// the nodes of `grants` (the Universal tree's, any order) whose level requirement the new level reaches and the old did not.
XpResult add_xp(const LevelCurve& curve, std::span<const content::AutoGrant> grants, const Progress& p, int64_t delta);

/// What a death costs (plan E1, stage 3, part 2): the experience debt grows by `share` of the XP span of the level (the part-2 spec's "Settled in D1": a death's
/// debt 2 % of GetPlayerXPForCurrentLevel), at most the span (the GameStatProfile's cap).  The total and the level are untouched.
Progress with_death_debt(const LevelCurve& curve, const Progress& p, double share = 0.02);

/// The ranks a level has granted so far (level requirement <= level), in level order: the SkillTreePurchasesFastMap's entry.
std::vector<const content::AutoGrant*> granted_by(std::span<const content::AutoGrant> grants, int level);

/// The progress a record gives: the row's XP when it is at least the level's threshold, else the threshold (a record saved
/// before XP existed, or a level set by hand); the debt is the row's.
Progress progress_of(const LevelCurve& curve, int level, std::optional<int64_t> xp, int64_t debt);

}  // namespace aoc::worldd::leveling
