// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/combat_players.cpp: abilities between players (Social, seeing other players: "a player cannot be targeted, healed or buffed by another
//  player's abilities")
//
//  Another player is a target the way a creature is.  His client names the pawn the other players are shown (the registry's Player actor,
//  session/player_presence.cpp), which carries his own pawn's GUID, so the actor id a caster's Targets name is the id his PlayerPort joined with.
//
//    inspect               where he is, whether he lives, on the caster's side (Friendly: no fight between players is served here), and how the two are
//                          grouped: a party is one squad, so a hit whose mask is Party or Raid reaches a member (combat_world.h mask_reaches);
//    apply_hit_to_player   the caster's pipeline resolved the hit for the two (the caster's stats, the target's as the target is now); it goes to the target's
//                          session as an IncomingHit (from_player), which moves his stats through his shields and confirms the hit to his client.  What it
//                          should take effect for (a heal up to the missing Health) is worked out here for the caster's confirmation;
//    apply_effects_...     the effects the hit puts on him, their numbers worked out by the caster (HitResolver::resolve_effects_with), and the shield an
//                          effect's TempHealth mod gives (HitResolver::effect_shield): his session puts them on as effects of its own;
//    remove_effects_...    what the hit takes off him (its removal list, or a cleanse's filter): his session takes them off;
//    resurrect_player      a Resurrect (a hit whose record names a ResurrectId) on a dead player: his session asks him (ClientPromptResurrect).
//
//  Modelled: no recording shows a heal's threat on the creatures that fight the healed player, so a heal on another player draws none.
// ============================================================================
#include "combat/combat_world.h"

#include "content/content_db.h"

#include <algorithm>
#include <cmath>

namespace aoc::worldd::combat {

PlayerPort* CombatWorld::player_pawn(uint64_t actor_id) {
    const world::WorldActor* a = registry_.find(actor_id);
    return a && a->kind == world::ActorKind::Player ? player(actor_id) : nullptr;
}

TargetInfo CombatWorld::inspect_player(PlayerPort& target, const Fighter& from) {
    TargetInfo t;
    Fighter& f = target.fighter();
    t.exists = true;
    t.is_player = true;
    t.other_player = true;
    t.position = f.position();
    t.radius_cm = f.radius_cm();
    t.alive = f.alive();
    t.relation = Relation::Friendly;   // players are on one side
    t.surface_distance_cm = std::max(0.0, distance_xy_cm(from.position(), t.position) - from.radius_cm() - t.radius_cm);
    t.in_line_of_sight = true;         // (the client keeps its own sight rules, as for a creature: E1 review I4)
    if (PlayerPort* me = player(from.id())) {
        const uint64_t group = me->group_key();
        if (group != 0 && group == target.group_key()) t.group = 2;   // one party: one squad
    }
    return t;
}

Delivery CombatWorld::apply_hit_to_player(uint64_t target, uint64_t attacker, const Resolved& r, double now, const HitOrigin* origin) {
    Delivery d;
    d.target_object = hit_component_of(target);
    PlayerPort* p = player_pawn(target);
    const abilities::HitDef* hit = origin ? origin->hit : nullptr;
    if (!p || (!p->fighter().alive() && !(hit && hit->resurrect != 0))) return d;
    d.applied = true;
    d.flags = r.flags;
    d.effective.assign(r.mods.size(), 0.0);
    IncomingHit in;
    in.instigator = attacker;
    in.from_player = true;
    in.flags = r.flags;
    in.mods = r.mods;
    in.at = now;
    if (origin) {
        in.source_type = origin->source_type;
        in.source_guid = origin->source_guid;
    }
    if (hit) in.hit_guid = hit->row.guid;
    // what the caster's confirmation draws: a heal up to what the target is missing, a loss as it is (his session takes it through his shields at its next tick)
    if (!r.miss()) {
        stats::CharacterStats& s = p->fighter().stats();
        for (size_t i = 0; i < r.mods.size(); ++i) {
            const int64_t stat = r.mods[i].stat_guid;
            const stats::StatValue* cur = s.find(stat);
            const stats::StatValue* max = stat == content::kGuidHealth ? s.find(content::kGuidMaxHealth) : stat == content::kGuidMana ? s.find(content::kGuidMaxMana) : nullptr;
            const double v = r.mods[i].value;
            d.effective[i] = cur && max && v > 0 ? std::clamp(v, 0.0, std::max(0.0, static_cast<double>(max->value) - cur->value)) : v;
        }
    }
    p->receive_hit(in, now);
    return d;
}

void CombatWorld::apply_effects_to_player(uint64_t target, uint64_t attacker, const std::vector<effects::Applied>& list, double now) {
    PlayerPort* p = player_pawn(target);
    if (!p || !p->fighter().alive() || list.empty()) return;
    PlayerPort* by = player(attacker);
    IncomingHit in;
    in.instigator = attacker;
    in.from_player = true;
    in.at = now;
    in.effects = list;
    for (const effects::Applied& a : list) {
        double shield = 0;
        if (by && a.def) {
            const bool has = std::any_of(a.def->mods.begin(), a.def->mods.end(), [](const content::ModRow& m) {
                return m.stat_guid == content::kGuidTempHealth && !m.shield_absorb && m.value_expr != 0;
            });
            if (has) shield = resolver_.effect_shield(by->fighter(), p->fighter(), *a.def);
        }
        in.effect_shields.push_back(std::isfinite(shield) && shield > 0 ? shield : 0.0);
    }
    p->receive_hit(in, now);
}

void CombatWorld::remove_effects_from_player(uint64_t target, uint64_t attacker, const abilities::HitDef& hit, double now) {
    PlayerPort* p = player_pawn(target);
    if (!p || !p->fighter().alive()) return;
    IncomingHit in;
    in.instigator = attacker;
    in.from_player = true;
    in.at = now;
    in.removes = &hit;
    p->receive_hit(in, now);
}

void CombatWorld::resurrect_player(uint64_t target, uint64_t caster, double now) {
    PlayerPort* p = player_pawn(target);
    PlayerPort* by = player(caster);
    if (p && by && !p->fighter().alive()) p->prompt_resurrect(by->character_guid(), now);
}

}  // namespace aoc::worldd::combat
