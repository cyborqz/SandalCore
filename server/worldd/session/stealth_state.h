// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/stealth_state.h
//
//  The stealth a player carries, as a creature's sight reads it (combat/stealth.h): which of the three stealth effects he holds, and the two stats the rule compares
//  (his StealthLevel, which Camouflage lowers by 5, and his StealthPerceptionLevel).  One function, so that the session and the tests read it the same way.
// ============================================================================
#pragma once

#include "bootstrap/stealth_effects.h"
#include "combat/stealth.h"
#include "session/ability_pipeline.h"
#include "stats/character_stats.h"

namespace aoc::worldd {

inline combat::StealthState stealth_state_of(const AbilityPipeline* pipeline, const stats::CharacterStats* stats) {
    combat::StealthState st;
    if (pipeline) {
        st.camouflage = pipeline->stacks_of(bootstrap::kEffectRangerCamouflage) > 0;
        st.active = st.camouflage || pipeline->stacks_of(bootstrap::kEffectRogueStealth) > 0 || pipeline->stacks_of(bootstrap::kEffectSummonerPhaseOut) > 0;
    }
    if (stats) {
        if (const stats::StatValue* v = stats->find(bootstrap::kStatStealthLevel)) st.level = v->value;
        if (const stats::StatValue* v = stats->find(bootstrap::kStatStealthPerceptionLevel)) st.perception = v->value;
    }
    return st;
}

}  // namespace aoc::worldd
