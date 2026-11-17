// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/ability_resurrect.h  (Social, seeing other players: their death)
//
//  What a hit that raises the dead names.  Another player's pawn is a target of the combat world like an NPC's: CombatWorld::inspect reads his Fighter (his
//  place, whether he lives, his side of the caster), and the pipeline and the session use that one way.  The resurrect records' values and the hit's life
//  states are here.
//
//  The ResurrectRecordBase rows are not exported by contentc (the record table's data is compressed and no table copies it), so the game's values are here: the
//  Cleric's resurrect record (6064630894416297986) and the NPCs' (6064633231498281195) both say 0.25 of Health, 0.25 of Mana, all of the Stamina and a prompt
//  (read from the design data; a data change would not reach the server without an edit of this header).
// ============================================================================
#pragma once

#include "combat/combat_world.h"
#include "session/ability_catalog.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace aoc::worldd::abilities {

/// What a ResurrectRecordBase says: the shares of the maxima the revived player stands up with, and whether he is asked first.
struct ResurrectValues {
    double health_ratio = 0.25, mana_ratio = 0.25, stamina_ratio = 1.0;
    bool   prompt = true;
};

/// The values of the resurrect record `record` (the game's two records hold the same ones).
inline ResurrectValues resurrect_values(int64_t record) {
    (void)record;
    return ResurrectValues{};
}

/// The hit of `def` that raises the dead: the one that names a ResurrectId and whose life states include the dead (ApplicableLifeStates' bit 2).  null when the
/// ability raises nobody (every ability but the Cleric's Resurrection, and every other Resurrect ability's hits, which name no record).
inline const HitDef* resurrect_hit_of(const Catalog& catalog, const AbilityDef& def) {
    for (const content::AbilityHitRow& h : def.hits) {
        const HitDef* hit = catalog.hit(h.hit_guid);
        if (hit && hit->resurrect != 0 && (hit->life_states & 2) != 0) return hit;
    }
    return nullptr;
}

/// The life states the ability's resurrect hit reaches (1 alive, 2 dead; 0 when the ability raises nobody): a Resurrection record that names only the dead may not
/// be cast at a living ally.
inline int resurrect_life_states(const Catalog& catalog, const AbilityDef& def) {
    const HitDef* hit = resurrect_hit_of(catalog, def);
    return hit ? hit->life_states : 0;
}

/// (another player is a target of an ability through CombatWorld::inspect: combat/combat_players.cpp)

}  // namespace aoc::worldd::abilities
