// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/npc_stats.cpp (see npc_stats.h)
// ============================================================================
#include "combat/npc_stats.h"

#include <algorithm>

namespace aoc::worldd::combat {

int NpcStatBook::engine_level(int data_level) { return std::clamp(data_level, 1, stats::kMaxLevel); }

int NpcStatBook::profile_for(const npc::MotionStats& bp) const {
    const int plain = stats::kNpcClassBase;
    if (!bundle_->class_by_enum(plain)) return 0;
    if (bp.list2 == kGuidGuardOvertunedList && bundle_->class_by_enum(plain + 1)) return plain + 1;
    return plain;
}

stats::NpcShape NpcStatBook::shape_for(const npc::MotionStats& bp, int level) {
    stats::NpcShape s;
    int rank = 0;
    int best = -1;
    for (const auto& [lv, rk] : bp.elite) {
        if (lv <= level && lv >= best) {
            best = lv;
            rank = rk;
        }
    }
    s.elite_rank = rank;
    double sum = 0;
    for (const int w : bp.dist) sum += w;
    for (size_t i = 0; i < 8; ++i) s.dist[i] = sum > 0 ? bp.dist[i] / sum : 0.0;
    return s;
}

std::unique_ptr<stats::CharacterStats> NpcStatBook::make(const npc::MotionStats& bp, int level, std::string& err) const {
    const int profile = profile_for(bp);
    if (profile == 0) {
        err = "the stat program has no NPC profile";
        return nullptr;
    }
    const int lv = engine_level(level);
    auto cs = std::make_unique<stats::CharacterStats>(*bundle_, *gear_, profile, lv);
    cs->set_npc(shape_for(bp, lv));
    if (!cs->start({}, err)) return nullptr;
    return cs;
}

}  // namespace aoc::worldd::combat
