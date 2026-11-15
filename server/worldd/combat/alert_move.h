// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/alert_move.h
//
//  What kind of move a creature's ability is: one that hurts its enemies (an attack), only a show (an alert: a growl, a yawp, a rally, a rear-up, a retreat), or a state the creature
//  enters (an empty hit of the tree and the effects the ability itself lists on the creature, such as the Skeletons' Block).  An alert plays its montage with no target in the cast, and
//  nothing lands but the effects its harmless hits put on its own side.  A state plays its montage with no target too, and the ability's own effects go on the creature.  Header only.
// ============================================================================
#pragma once

#include "content/ability_content.h"
#include "session/ability_catalog.h"

#include <cstdint>

namespace aoc::worldd::combat {

/// Whether an ability does harm to enemies: some hit of it is aimed at enemies and moves Health.
inline bool hurts_enemies(const abilities::AbilityDef& d, const abilities::Catalog& catalog, int64_t health_stat) {
    for (const content::AbilityHitRow& h : d.hits) {
        const abilities::HitDef* hit = catalog.hit(h.hit_guid);
        if (!hit || (hit->row.applicable_targets & 2) == 0) continue;
        for (const content::ModRow& m : hit->mods) {
            if (m.stat_guid == health_stat && m.value_expr != 0 && catalog.program(m.value_expr) >= 0) return true;
        }
    }
    return false;
}

/// Whether a hit of a show does nothing the world cannot show: a plain hit record (not a projectile or a zone) that reaches only the creature itself or its allies and changes no stat,
/// takes nothing off anyone and spawns no zone.  The effects it puts on its own side are the alert effects the world serves.
inline bool quiet_show_hit(const content::AbilityHitRow& r, const abilities::Catalog& catalog) {
    if (r.source != "hit") return false;
    const abilities::HitDef* hit = catalog.hit(r.hit_guid);
    if (!hit || (hit->row.applicable_targets & 2) != 0) return false;
    return hit->mods.empty() && !hit->effects.empty() && hit->remove_effects.empty() && hit->zones.empty();
}

/// Whether an ability is only a show: it hurts nobody, the ability itself opens no zone, and every hit it has is one the tables can say is harmless to model: a plain hit (not a
/// projectile or a zone) that reaches only the creature itself or its allies, changes no stat, takes nothing off anyone and puts effects on them.  An ability with no hit at all is
/// one (a growl), unless it lists effects of its own that the show would leave out (a fortification, a burrow).  A hit that reaches enemies is not (what it does to them, a debuff, is
/// not built), nor is a hit the tables hold nothing of (a zone's, a spray's), a heal or a cleanse.
inline bool is_alert_move(const abilities::AbilityDef& d, const abilities::Catalog& catalog, int64_t health_stat) {
    if (hurts_enemies(d, catalog, health_stat)) return false;
    if (!d.zones.empty()) return false;   // the ability itself opens a zone: the show would leave it out
    // an ability with no hit at all that lists effects of its own is a self-buff or a state, not a show (a fortification, a burrow): its effect would be left out
    if (!d.effects.empty() && d.hits.empty()) return false;
    for (const content::AbilityHitRow& r : d.hits) {
        if (!quiet_show_hit(r, catalog)) return false;
    }
    return true;
}

/// Whether an ability is a state the creature enters: it hurts nobody, its hits are empty markers (no stat, no effect, no zone; the effect removals a state's own cleanup names are
/// part of it) and the effects the ability itself lists are the state it puts on the creature (the Skeletons' Block: its hit carries nothing and its own effect is the shield state).
/// The world plays it as a show with no target and puts the ability's own effects on the creature.
inline bool is_state_move(const abilities::AbilityDef& d, const abilities::Catalog& catalog, int64_t health_stat) {
    if (hurts_enemies(d, catalog, health_stat)) return false;
    if ((d.row.applicable_targets & 2) != 0 || d.effects.empty() || !d.zones.empty()) return false;
    if (d.hits.empty()) return false;
    for (const content::AbilityHitRow& r : d.hits) {
        if (r.source != "hit") return false;
        const abilities::HitDef* hit = catalog.hit(r.hit_guid);
        if (!hit || (hit->row.applicable_targets & 2) != 0) return false;
        if (!hit->mods.empty() || !hit->effects.empty() || !hit->zones.empty()) return false;
    }
    return true;
}

}  // namespace aoc::worldd::combat
