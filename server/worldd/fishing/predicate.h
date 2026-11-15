// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  fishing/predicate.h  (Fishing, milestone 1)
//
//  The reward tables of the fishing waters gate each fish on the player's certification and rod:
//
//      (GetPlayerProfessionCertificationLevel(GetPlayerGUID(), "Fishing") >= 1) && (GetEquipmentCertificationTier(GetInstigator(), EEquipSlot::FishingRod) == 1)
//
//  The stat programs (services/internal/statprog) do not have these two functions, so those predicates do not compile (ab_expr's error column says "unsupported function") and the reward roller,
//  which counts a predicate that does not run as false, would hand out no fish at all.  This is a small evaluator for exactly the shapes the data uses: comparisons (>=, <=, ==, !=, >, <) of the
//  two functions with numbers, joined by &&, || and !, with parentheses and // comments.  Player_HasObjective(...) (the world events' extra fish) answers false: no objective is tracked here.
//  Any other function makes the whole text "not evaluable" and the roller counts it as it did before.
//
//  The two functions answer from facts the caller gives: the character's certification tier in a profession (Novice 0 ... GrandMaster 4) and the tier of the tool worn in an equipment slot
//  (the tool data's certification: the same number gather_tool holds), -1 for a slot with no tool.
// ============================================================================
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace aoc::worldd::fishing {

struct Facts {
    /// The character's certification tier in the profession of that name ("Fishing"); -1 for a profession it has none in.
    std::function<int(std::string_view profession)> certification;
    /// The certification tier of the tool worn in the equipment slot of that name (the part after EEquipSlot::, "FishingRod"); -1 when no tool is worn.
    std::function<int(std::string_view slot)>       equipment_tier;
};

/// The value of the predicate text (1 or 0), or nullopt when it holds anything but the shapes above (or does not parse).
std::optional<double> evaluate_predicate(std::string_view text, const Facts& facts);

}  // namespace aoc::worldd::fishing
