// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  stats/gather_host.h: the gathering reads of an ability expression (Gathering stage 2, "Gathering data")
//
//  The Artisan equations of the design data (the speed, rarity, quantity and experience of a harvest, the ItemVariations' Quality formulas and the Profession
//  experience rewards) ask the harvest in progress and the player five things (stats/stat_program.h kHostArtisanStat ... kHostGatherBaseSecs). They are host
//  functions over numbers: the host fills a GatherContext and answers from it with gather_host_fn. services/internal/statprog (gather.go GatherContext) is the Go
//  twin, and the shared corpus (stats/testdata/corpus.txt, the `gatherhost` blocks) checks both give the same answers.
//
//    GetArtisanshipModifiedStat(actor, stat)     the rating of the stat for the caster (ratings, by the stat's guid); 0 for a stat with none and for any other actor
//    GetRolledItemQuality()                      quality: what the harvest's rarity equation rolled (0 before one ran)
//    GetItemRarity()                             item_rarity: the EItemRarity ordinal of the harvest's item (Common 0 ... Artifact 6)
//    GetSourceCertificationTier()                source_tier: the CertificationTier of the interaction (Novice 0 ... GrandMaster 4)
//    GetGatheringInteractionBaseDuration()       base_duration: the interaction's BaseGatheringDuration, seconds
//
//  The harvest host of the Harvesting part fills the context from the node's interaction (gather_def), the roll and the player's stat engine (the rating stats'
//  settled values), and runs the equations through the stat program's ExprEvaluator.
// ============================================================================
#pragma once

#include "stats/stat_program.h"

#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>

namespace aoc::worldd::stats {

struct GatherContext {
    double quality = 0;
    int item_rarity = 0;
    int source_tier = 0;
    double base_duration = 0;
    std::map<int64_t, double> ratings;  // by the artisanship rating stat's guid
};
// ~ CoA - C-Y-B ~

/// Answers a gathering read; false (and out untouched) for a function that is no gathering read. name is the stat's guid in decimal for kHostArtisanStat and args[0]
/// the role of the actor (n numbers were given).
inline bool gather_host_fn(const GatherContext& c, int fn, const std::string& name, const double* args, int n, double& out) {
    switch (fn) {
        case kHostArtisanStat: {
            out = 0;
            if (n >= 1 && static_cast<int>(args[0]) != kRoleCaster) return true;   // only the instigator, the harvesting player, has artisanship ratings
            if (name.empty()) return true;
            char* end = nullptr;
            const long long guid = std::strtoll(name.c_str(), &end, 10);
            if (end != name.c_str() + name.size()) return true;
            const auto it = c.ratings.find(static_cast<int64_t>(guid));
            if (it != c.ratings.end()) out = it->second;
            return true;
        }
        case kHostRolledItemQuality: out = c.quality; return true;
        case kHostItemRarity: out = c.item_rarity; return true;
        case kHostSourceCertTier: out = c.source_tier; return true;
        case kHostGatherBaseSecs: out = c.base_duration; return true;
        default: return false;
    }
}

}  // namespace aoc::worldd::stats
