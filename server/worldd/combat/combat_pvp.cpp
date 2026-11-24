// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// PvP: contributed by MrJambix in #23.
// ============================================================================
//  combat/combat_pvp.cpp: the world's side of PvP (the rules are combat/pvp.h; the player's side is session/player_pvp.cpp)
//
//  A hit between two players is judged here by the rules between them (combat/combat_players.cpp asks pvp_pair and pvp::permissions), and what it leads to
//  is kept here:
//    PvP combat     a harmful hit between two players puts both in combat and in PvP combat (CombatInfo.bInPVPCombat) for the in-combat time
//                   (pvp_exchange); the flag ends with that time (expire_pvp_combat) or with the player's death;
//    the kill       the player whose harmful hit was the last blow another player took before he fell hears of the kill, with the victim as he was when he
//                   fell (his flag, his level, a duel between them, one party): what the killer's corruption reads (pvp_player_died);
//    a bleed's tick an effect one player left on another (a bleed, a burn) ticks as its maker's hit, resolved from his stats as he is now, judged by the rules,
//                   and confirmed to his client as a hit he dealt (player_effect_hit).
// ============================================================================
#include "combat/combat_world.h"

#include "content/content_db.h"

#include <spdlog/spdlog.h>

namespace aoc::worldd::combat {

bool CombatWorld::pvp_pair(PlayerPort& self, PlayerPort& target, pvp::Side& a, bool& force, pvp::Side& b, pvp::Pair& pair) {
    if (&self == &target) return false;
    bool target_force = false;
    if (!self.pvp_side(a, force) || !target.pvp_side(b, target_force)) return false;
    const uint64_t group = self.group_key();
    pair.group = group != 0 && group == target.group_key() ? (self.squad_key() != 0 && self.squad_key() == target.squad_key() ? 2 : 1) : 0;   // one raid: 1, one squad: 2
    return true;
}

void CombatWorld::pvp_exchange(uint64_t a, uint64_t b, double now) {
    const double until = now + data_.in_combat_timer();
    for (const uint64_t id : {a, b}) {
        player_exchange(id, now);
        const auto it = player_pvp_until_.find(id);
        const bool was = it != player_pvp_until_.end() && it->second > now;
        player_pvp_until_[id] = until;
        if (!was) {
            if (PlayerPort* p = player(id)) p->set_in_pvp_combat(true, now);
        }
    }
}

void CombatWorld::expire_pvp_combat(double now) {
    for (auto it = player_pvp_until_.begin(); it != player_pvp_until_.end();) {
        if (now >= it->second) {
            if (PlayerPort* p = player(it->first)) p->set_in_pvp_combat(false, now);
            it = player_pvp_until_.erase(it);
        } else {
            ++it;
        }
    }
}

void CombatWorld::pvp_player_died(uint64_t victim_id, uint64_t killer, double now) {
    PlayerPort* victim = player_pawn(victim_id);
    PlayerPort* by = killer != 0 && killer != victim_id ? player_pawn(killer) : nullptr;
    if (victim && by) {
        pvp::Side a, b;
        pvp::Pair pair;
        bool force = false;
        if (pvp_pair(*by, *victim, a, force, b, pair)) {
            PvpKill k;
            k.victim = victim_id;
            k.victim_guid = victim->character_guid();
            k.victim_flag = b.flag;
            k.victim_level = victim->fighter().level();
            k.duel = pvp::deciding_tier(a, b, pair) == pvp::Tier::Duel;
            k.grouped = pair.group >= 1;
            by->pvp_kill(k, now);
        }
    }
    if (player_pvp_until_.erase(victim_id) != 0 && victim) victim->set_in_pvp_combat(false, now);
}

void CombatWorld::player_effect_hit(uint64_t from, uint64_t target, int64_t hit_guid, int64_t effect_guid, double now, bool pvp_force) {
    PlayerPort* by = from != target ? player_pawn(from) : nullptr;
    PlayerPort* port = player_pawn(target);
    const abilities::HitDef* hit = catalog_.hit(hit_guid);
    if (!by || !port || !hit || !by->fighter().alive() || !port->fighter().alive() || (hit->mods.empty() && hit->effects.empty())) return;
    HitMeta meta;
    meta.source_type = content::kSourceTypeEffect;
    meta.source_guid = effect_guid;
    Resolved r;
    std::string why;
    if (!resolver_.resolve(by->fighter(), port->fighter(), *hit, meta, r, why)) return;
    HitOrigin origin;
    origin.hit = hit;
    origin.source_type = meta.source_type;
    origin.source_guid = meta.source_guid;
    origin.landed_here = true;
    origin.pvp_force = pvp_force;   // (the key the effect was put with, not the last trigger's: #23 review)
    const Delivery d = apply_hit_to_player(target, from, r, now, &origin);
    if (!d.applied) return;
    DealtHit dealt{hit->row.guid, meta.source_type, meta.source_guid, 0, target, d.flags, {}, {}};
    for (size_t i = 0; i < r.mods.size(); ++i) {
        if (data_.ids().is_status_stat(r.mods[i].stat_guid)) continue;
        dealt.mods.push_back(r.mods[i]);
        dealt.effective.push_back(i < d.effective.size() ? d.effective[i] : r.mods[i].value);
    }
    if (!dealt.mods.empty() || hit->row.has_fx || r.miss()) by->hit_dealt(dealt, now);
}

}  // namespace aoc::worldd::combat
