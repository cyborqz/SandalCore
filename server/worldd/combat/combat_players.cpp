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

#include <spdlog/spdlog.h>

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
        // PvP: what the rules between players let the caster do to him (a pet, which keeps no side, stays on its owner's side as before)
        pvp::Side a, b;
        pvp::Pair pair;
        bool force = false;
        if (pvp_pair(*me, target, a, force, b, pair)) {
            t.pvp = true;
            t.pvp_permissions = pvp::permissions(a, b, pair, force);
            t.relation = t.pvp_permissions == pvp::kHarm ? Relation::Hostile : t.pvp_permissions == (pvp::kHarm | pvp::kHelp) ? Relation::Neutral : Relation::Friendly;
        }
    }
    return t;
}

Delivery CombatWorld::apply_hit_to_player(uint64_t target, uint64_t attacker, const Resolved& r, double now, const HitOrigin* origin) {
    Delivery d;
    d.target_object = hit_component_of(target);
    PlayerPort* p = player_pawn(target);
    const abilities::HitDef* hit = origin ? origin->hit : nullptr;
    if (!p || (!p->fighter().alive() && !(hit && hit->resurrect != 0))) return d;
    // PvP: a hit between two players is harmful when its mask has Enemy and the rules let the attacker harm him (or, a hit that names no mask, when it takes
    // Health); a harmful hit the rules do not allow lands nowhere (the client would not have sent it: a forged or a stale one)
    bool hostile = false;
    PlayerPort* by = attacker != target ? player_pawn(attacker) : nullptr;
    pvp::Side side_a, side_b;
    pvp::Pair pair;
    bool force = false;
    const bool between_players = by && pvp_pair(*by, *p, side_a, force, side_b, pair);
    if (between_players) {
        const uint8_t perms = pvp::permissions(side_a, side_b, pair, force);
        bool takes = false;
        for (const ResolvedMod& m : r.mods) takes = takes || (m.stat_guid == content::kGuidHealth && m.value < 0);
        const int mask = hit ? hit->row.applicable_targets : 0;
        const bool harmful = mask != 0 ? (mask & 2) != 0 && (perms & pvp::kHarm) != 0 : takes;
        if ((harmful || takes) && (perms & pvp::kHarm) == 0) {
            spdlog::debug("[pvp] {} -> {}: a harmful hit the rules do not allow is dropped", attacker, target);
            return d;
        }
        hostile = harmful;
    }
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
    in.hostile = hostile;
    p->receive_hit(in, now);
    if (between_players) {
        // the attacker's flag: harming a non-combatant or a combatant, or supporting a combatant, makes him a combatant (InterServerHandleFlaggingOnHitInstigated)
        const uint8_t action = hostile ? pvp::kAttack : pvp::kSupport;
        if (pvp::flags(side_a, side_b, pair, action)) by->pvp_flagged(target, hostile ? pvp::kIntentionHarm : pvp::kIntentionSupport, now);
        if (hostile) pvp_exchange(attacker, target, now);
    }
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
    // PvP: a non-combatant is immune to another player's crowd control (the NonCombatant alignment's bGrantNonCombatantCrowdControlImmunity) where the rules between
    // players decide; a duel and a party are not that tier [modelled: the data holds the flag, no recording shows where retail reads it]
    bool immune = false;
    if (PlayerPort* other = attacker != target ? player_pawn(attacker) : nullptr) {
        pvp::Side a, b;
        pvp::Pair pair;
        bool force = false;
        immune = pvp_pair(*other, *p, a, force, b, pair) && b.flag == pvp::Flag::NonCombatant && pvp::deciding_tier(a, b, pair) == pvp::Tier::Players;
    }
    for (const effects::Applied& a : list) {
        if (immune && a.def && a.def->row.crowd_control_guid != 0) {
            ++pvp_cc_immunities_;
            spdlog::debug("[pvp] {} -> {}: a non-combatant is immune to another player's {}", attacker, target, a.def->row.name);
            continue;
        }
        in.effects.push_back(a);
    }
    if (in.effects.empty()) return;
    for (const effects::Applied& a : in.effects) {
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
