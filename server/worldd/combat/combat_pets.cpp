// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// Summoner pets: contributed by MrJambix in #13.
// ============================================================================
//  combat/combat_pets.cpp (see combat_world.h): the fights of a player's pet, the abilities it plays, and what reacts to a hit on what a creature holds.
//
//  A pet is an NPC combatant that stands in players_ through a PetPort, so the creatures' code sees a body on their tables that takes their hits; its AI does not run, its fight is its owner's order.
//  What a pet holds is an effects::Book like any creature's, and what its hits leave goes through land_effect: a slow, a root, a stun, a trip and a sleep bind the creature, but a knockback, a pull or a
//  silence is left off it and does not land.  The ability still plays, and its other hits and effects do.
//
//  Modelled, no recording shows them: both ways a pet's ability can be asked for, a pet's cooldowns, Lunge's leap (the record's 600 cm), a Taunt that puts the creature on the Guardian, the reach of a
//  melee and of a ranged attack, its regeneration, and that its numbers are its owner's.
// ============================================================================
#include "combat/combat_world.h"

#include "bootstrap/ability_replication.h"
#include "bootstrap/constants.h"
#include "combat/combat_world_npc.h"
#include "combat/npc_ability_replicator.h"
#include "combat/npc_replicator.h"
#include "content/content_db.h"
#include "session/summoner_pets.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string_view>

namespace aoc::worldd::combat {

namespace {

nav::Vec3 to_nav(const Vec3& v) { return nav::Vec3{v.x, v.y, v.z}; }

const char* const kRemovePhases[] = {"Remove", "End"};

constexpr double kMeleeReachCm = 250.0, kRangedReachCm = 1500.0;   // [modelled: how near a pet's attack needs its target]
constexpr double kFirstHitSeconds = 0.4;                           // no notify to go by: a moment into the swing
constexpr double kBoltFlightSeconds = 0.3;
constexpr double kOwnerLeftCm = 5000.0;                            // the owner has run this far from the fight: the pet goes with it
constexpr double kLungeCm = 600.0;                                 // the targeting record's

}  // namespace

// ---- a pet as the creatures see it ------------------------------------------------------------------------------------------------------

/// A body on their tables that takes their hits.  One port per pet, in players_ while the pet is in the world.
class CombatWorld::PetPort final : public PlayerPort {
public:
    PetPort(CombatWorld& w, Npc& pet) : world_(w), pet_(pet) {}
    uint64_t actor_id() const override { return pet_.actor_id; }
    Fighter& fighter() override { return pet_; }
    void receive_hit(const IncomingHit& hit, double now) override {
        if (pet_.dead || (hit.flags & stats::kHitMiss) != 0) return;
        const CombatStatIds& id = world_.data_.ids();
        double taken = 0;
        for (const ResolvedMod& m : hit.mods) {
            if (m.stat_guid != id.health || !std::isfinite(m.value)) continue;
            const stats::StatValue* cur = pet_.cs->find(id.health);
            const stats::StatValue* max = pet_.cs->find(id.max_health);
            if (!cur || !max) continue;
            double value = m.value;
            if (value < 0) value = -pet_.absorb(-value, now);
            const double after = std::clamp(static_cast<double>(cur->value) + value, 0.0, static_cast<double>(max->value));
            taken += std::max(0.0, static_cast<double>(cur->value) - after);
            pet_.cs->set_current(id.health, static_cast<float>(after));
            write_stat(world_.registry_, pet_.actor_id, id.health, static_cast<float>(after));
        }
        world_.pet_was_hit(pet_, hit, taken, now);
        if (pet_.health() <= 0.0) {   // (no corpse and no reward: its session sees dead() and takes it away)
            pet_.dead = true;
            world_.pet_stop(pet_.actor_id);
        }
    }
    void set_in_combat(bool, double) override {}
    protocol::emit::FIntrepidNetworkGUID pawn_guid() const override {
        const world::WorldActor* a = world_.registry_.find(pet_.actor_id);
        return a ? a->actor.guid : protocol::emit::FIntrepidNetworkGUID{};
    }
    protocol::emit::FIntrepidNetworkGUID cylinder_guid() const override {
        protocol::emit::FIntrepidNetworkGUID g = pawn_guid();
        if (const uint64_t cylinder = world_.hit_component_of(pet_.actor_id)) g.ObjectId = cylinder;
        return g;
    }

private:
    CombatWorld& world_;
    Npc&         pet_;
};

// ---- what the session asks ---------------------------------------------------------------------------------------------------------------

uint64_t CombatWorld::attacker_of(uint64_t player_actor, uint64_t pet, double radius_cm, bool owner_too) const {
    const auto port = std::find_if(players_.begin(), players_.end(), [&](const PlayerPort* p) { return p->actor_id() == player_actor; });
    if (port == players_.end()) return 0;
    const Vec3 at = (*port)->fighter().position();
    uint64_t best = 0;
    double nearest = radius_cm;
    for (const auto& [id, n] : npcs_) {
        if (n->dead || id == pet || !n->flags_in_combat || (!(owner_too && n->flags_target == player_actor) && n->flags_target != pet)) continue;
        const double d = distance_xy_cm(n->position(), at);
        if (d <= nearest) {
            nearest = d;
            best = id;
        }
    }
    return best;
}

uint64_t CombatWorld::owner_attacker(uint64_t player_actor, uint64_t pet, double radius_cm) const {
    const auto port = std::find_if(players_.begin(), players_.end(), [&](const PlayerPort* p) { return p->actor_id() == player_actor; });
    if (port == players_.end()) return 0;
    const Vec3 at = (*port)->fighter().position();
    uint64_t best = 0;
    double nearest = radius_cm;
    for (const auto& [id, n] : npcs_) {
        if (n->dead || id == pet || is_pet(id) || n->ai.target() != player_actor) continue;
        const double d = distance_xy_cm(n->position(), at);
        if (d <= nearest) {
            nearest = d;
            best = id;
        }
    }
    return best;
}

bool CombatWorld::pet_can_act(uint64_t pet) const {
    const auto it = npcs_.find(pet);
    return it != npcs_.end() && !it->second->dead;
}

uint64_t CombatWorld::take_pet_stand_down(uint64_t pet) {
    const auto it = pet_stand_downs_.find(pet);
    if (it == pet_stand_downs_.end()) return 0;
    const uint64_t target = it->second;
    pet_stand_downs_.erase(it);
    return target;
}

bool CombatWorld::fought_by(uint64_t npc_actor, uint64_t player_actor) const {
    const auto it = npcs_.find(npc_actor);
    return it != npcs_.end() && !it->second->dead && it->second->attackers.count(player_actor) != 0;
}

uint64_t CombatWorld::creature_target(uint64_t npc_actor) const {
    const auto found = npcs_.find(npc_actor);
    return found == npcs_.end() || found->second->dead ? 0 : found->second->ai.target();
}

bool CombatWorld::holds_dispellable(uint64_t npc_actor) const {
    const auto found = npcs_.find(npc_actor);
    if (found == npcs_.end()) return false;
    for (const effects::Instance& e : found->second->book.instances()) {
        if (e.def->row.dispellable && e.def->row.category.find("Debuff") != std::string::npos) return true;
    }
    return false;
}

uint64_t CombatWorld::pet_of(uint64_t owner) const { return pet_of_owner(owner); }

uint64_t CombatWorld::pet_of_owner(uint64_t owner) const {
    for (const auto& [pet, who] : pet_owners_) {
        if (who == owner) return pet;
    }
    return 0;
}

double CombatWorld::flight_seconds(const content::AbilityHitRow& row, const Vec3& from, const Vec3& to) const {
    const content::ProjectileRow* flying = catalog_.projectile(row.owner_guid);
    return flying && flying->speed > 0 ? distance_cm(from, to) / flying->speed : kBoltFlightSeconds;
}

void CombatWorld::pet_effects_changed(Npc& pet) {
    std::vector<stats::StatValue> changed;
    stats::CharacterStats::Next next;
    std::string err;
    if (pet.cs->prepare(pet.worn(), changed, next, err)) {
        pet.cs->commit(std::move(next));
        write_stats(registry_, pet.actor_id, *pet.cs);
    }
}

// ---- what a hit takes off, and leaves on, what it lands on -----------------------------------------------------------------------------------

void CombatWorld::remove_effects_from_npc(uint64_t npc_actor, const abilities::HitDef& hit, double now) {
    const auto found = npcs_.find(npc_actor);
    if (found == npcs_.end() || found->second->book.empty()) return;
    Npc& n = *found->second;
    int left = hit.max_remove > 0 ? hit.max_remove : 1 << 20;
    const std::vector<effects::Instance> held = n.book.instances();
    for (const effects::Instance& e : held) {
        if (left <= 0 || n.dead) break;
        if (!hit.removes(*e.def)) continue;
        effects::Book::Changes changes;
        if (!n.book.remove(e.key, changes)) continue;
        send_effects(n, changes);
        run_effect_hits(n, e, kRemovePhases, 2, now);
        --left;
    }
}

void CombatWorld::remove_npc_effect(uint64_t npc_actor, int64_t effect_guid, double now) {
    const auto found = npcs_.find(npc_actor);
    if (found == npcs_.end()) return;
    Npc& n = *found->second;
    const std::vector<effects::Instance> held = n.book.instances();
    for (const effects::Instance& e : held) {
        if (e.def->row.guid != effect_guid || n.dead) continue;
        effects::Book::Changes changes;
        if (!n.book.remove(e.key, changes)) continue;
        send_effects(n, changes);
        run_effect_hits(n, e, kRemovePhases, 2, now);
    }
}

namespace {

/// What a creature holds, for the reads of a hit that lands on it (GetEffectRemainingDuration(GetTarget(), effect)).
class HeldView final : public TargetEffects {
public:
    HeldView(const CombatWorld& world, uint64_t holder, double at) : world_(world), holder_(holder), at_(at) {}
    double remaining(int64_t effect_guid) const override { return world_.effect_remaining(holder_, effect_guid, at_); }

private:
    const CombatWorld& world_;
    uint64_t           holder_;
    double             at_;
};

}  // namespace

/// Where a hit a player's pet or reaction lands comes from.
struct CombatWorld::LandSource {
    int64_t                      type = 0, guid = 0;
    const abilities::AbilityDef* ability = nullptr;
    uint64_t                     pet = 0;                // the pet that lands it: the creature's anger is its
    const HolderEvent*           event = nullptr;        // the hit a reaction answers
    bool                         children = true;        // it fires the hits it lists
    bool                         skip_requirement = false;
};

/// What a reaction's expressions and hits ask of the hit in progress.
class CombatWorld::EventHost final : public ContextHost {
public:
    EventHost(stats::SplitMix& rng, const HolderEvent& event, uint64_t instigator) : rng_(rng), event_(event), instigator_(instigator) {}
    double rand_double() override { return rng_.rand_double(); }
    double effect_count_by_instigator(const stats::RecordRef& r) override {
        double count = 0;
        for (const auto& [def, by] : event_.held) count += def->row.guid == r.guid && by == instigator_ ? 1.0 : 0.0;
        return count;
    }

protected:
    double other_fn(int fn, const std::string& name, const double* args, int) override {
        switch (fn) {
            case stats::kHostHitStatMod:
            case stats::kHostHitStatEffective: {
                const std::map<int64_t, double>& m = fn == stats::kHostHitStatMod ? event_.mods : event_.effective;
                const auto it = m.find(std::strtoll(name.c_str(), nullptr, 10));
                return it == m.end() ? 0.0 : it->second;
            }
            case stats::kHostHitRecordIs: return event_.origin && event_.origin->hit && event_.origin->hit->row.guid == std::strtoll(name.c_str(), nullptr, 10) ? 1.0 : 0.0;
            case stats::kHostHitRecordNameIs: return event_.origin && event_.origin->hit && event_.origin->hit->row.name == name ? 1.0 : 0.0;
            case stats::kHostRandIntRange: {
                const double lo = std::floor(args[0]), hi = std::floor(args[1]);
                return hi < lo ? lo : lo + std::floor(rng_.rand_double() * (hi - lo + 1.0));
            }
            default: return 0.0;
        }
    }

private:
    stats::SplitMix&   rng_;
    const HolderEvent& event_;
    uint64_t           instigator_;
};

void CombatWorld::land_effects_of_hit(Npc& n, PlayerPort& port, const abilities::HitDef& hit, const HitMeta& meta, double now) {
    Resolved gate;
    std::string why;
    // the expressions read what the creature holds from its stat engine (GetEffectCount(GetTarget(), effect): Taunt's second landing): send_effects keeps it there
    const bool ran = resolver_.resolve_effects(port.fighter(), n, hit, meta, gate, why);
    if (!ran) {
        spdlog::debug("[combat] the effects of {} on {}: {}", hit.row.name, n.actor_id, why);
        return;
    }
    for (const effects::Applied& a : gate.effects) {
        if (n.dead) return;
        const double shield = resolver_.effect_shield(port.fighter(), n, *a.def);
        if (shield > 0) add_shield(n.actor_id, shield, std::isinf(a.duration) ? 0.0 : a.duration, now);
        land_effect(n, a, port.actor_id(), now);
    }
}

void CombatWorld::land_for_player(uint64_t by, uint64_t target, const abilities::HitDef& hit, const LandSource& src, double now, int depth) {
    PlayerPort* port = player(by);
    const auto found = npcs_.find(target);
    if (!port || depth > 4 || found == npcs_.end() || found->second->dead) return;
    Npc& n = *found->second;
    HitMeta meta;
    meta.source_type = src.type;
    meta.source_guid = src.guid;
    if (src.ability) meta.ability_tags = src.ability->row.tags;
    const HeldView held_view(*this, target, now);
    meta.target_effects = &held_view;
    if (!src.skip_requirement && !resolver_.requirement_holds(port->fighter(), n, hit, meta)) return;
    Resolved r;
    std::string why;
    bool ok = false;
    if (src.event) {   // a reaction's hit reads the hit it answers (GetStatModValue(GetHitEvent(), ...))
        std::string err;
        auto a = port->fighter().stats().evaluation(port->fighter().worn(), err, true);
        auto t = n.stats().evaluation(n.worn(), err, true);
        if (a && t) {
            EventHost host(rng_, *src.event, by);
            const Relation rel = data_.relation(port->fighter().alignments(), n.alignments());
            ok = resolver_.resolve_with(*a, host, t.get(), distance_cm(port->fighter().position(), n.position()), rel == Relation::Hostile, rel == Relation::Neutral, hit, meta, r, why);
        }
    } else {
        ok = resolver_.resolve(port->fighter(), n, hit, meta, r, why);
    }
    if (!ok) {
        spdlog::debug("[combat] the hit {} does not run on {}: {}", hit.row.name, target, why);
        return;
    }
    // a pet's numbers are its owner's, times what its own effects add (Amplify)
    if (src.pet != 0) {
        double damage = 1.0, healing = 1.0;
        if (const auto pet = npcs_.find(src.pet); pet != npcs_.end()) {
            for (const effects::Instance& e : pet->second->book.instances()) {
                for (const stats::EffectMod& m : e.mods) {
                    if (m.stat == pets::kStatDamageMult) damage *= m.value;
                    if (m.stat == pets::kStatHealingMult) healing *= m.value;
                }
            }
        }
        for (ResolvedMod& m : r.mods) {
            if (m.stat_guid == data_.ids().health) m.value *= m.value < 0 ? damage : healing;
        }
    }
    HitOrigin origin;
    origin.landed_here = true;
    origin.hit = &hit;
    origin.source_type = src.type;
    origin.source_guid = src.guid;
    origin.ability_tags = meta.ability_tags;
    const Delivery d = land_hit(n, by, r, now, true, target == src.pet ? 0 : src.pet, &origin);
    if (d.applied) {
        // what the client draws is Health and Mana: a status stat's move (the burn's pool and seconds) is not shown
        DealtHit dealt{hit.row.guid, meta.source_type, meta.source_guid, src.pet, target, d.flags, {}, {}};
        for (size_t i = 0; i < r.mods.size(); ++i) {
            if (data_.ids().is_status_stat(r.mods[i].stat_guid)) continue;
            dealt.mods.push_back(r.mods[i]);
            dealt.effective.push_back(d.effective[i]);
        }
        if (!dealt.mods.empty() || hit.row.has_fx || r.miss()) port->hit_dealt(dealt, now);
    }
    if (d.applied && !r.miss() && !d.lethal) {
        if (hit.max_remove > 0 || !hit.remove_effects.empty()) remove_effects_from_npc(target, hit, now);
        land_effects_of_hit(n, *port, hit, meta, now);
    }
    if (src.children) land_children(by, target, hit, src, now, depth);
}

void CombatWorld::land_children(uint64_t by, uint64_t target, const abilities::HitDef& hit, const LandSource& src, double now, int depth) {
    PlayerPort* port = player(by);
    if (!port) return;
    const world::WorldActor* at = registry_.find(target);
    if (!at) return;
    const Vec3 centre{at->x, at->y, at->z};
    for (const content::RecursiveHitRow& row : hit.recursive) {
        const abilities::HitDef* child = catalog_.hit(row.child_hit);
        if (!child) continue;
        const int mask = child->row.applicable_targets;
        if ((mask & 2) != 0) {
            if (row.radius_cm > 0) {
                for (const GatheredTarget& g : enemies_around(port->fighter(), centre, row.radius_cm, 40)) {
                    if (!is_pet(g.actor)) land_for_player(by, g.actor, *child, src, now, depth + 1);
                }
            } else if (!is_pet(target) && target != by) {
                land_for_player(by, target, *child, src, now, depth + 1);
            }
        } else if ((mask & 4) != 0) {
            if (const uint64_t pet = pet_of_owner(by)) land_for_player(by, pet, *child, src, now, depth + 1);
        } else if ((mask & 1) != 0) {
            port->hit_from_pet(child->row.guid, src.guid, now);
        }
    }
}

void CombatWorld::effect_hit_fires(Npc& n, uint64_t by, const abilities::HitDef& hit, int64_t effect_guid, double now) {
    if (hit.recursive.empty() || hit.row.name.rfind("Summoner_", 0) != 0) return;
    LandSource src;
    src.type = content::kSourceTypeEffect;
    src.guid = effect_guid;
    land_children(by, n.actor_id, hit, src, now, 0);
}

// ---- what reacts to a hit -----------------------------------------------------------------------------------------------------------------------

CombatWorld::HolderEvent CombatWorld::event_of_hit(const Npc& n, uint64_t attacker, const Resolved& r, const Delivery& d, bool lethal, const HitOrigin* origin) const {
    HolderEvent event;
    event.attacker = attacker;
    event.origin = origin;
    event.flags = lethal ? static_cast<uint8_t>(r.flags | stats::kHitLethal) : r.flags;
    event.lethal = lethal;
    for (size_t i = 0; i < r.mods.size(); ++i) {
        event.mods[r.mods[i].stat_guid] += r.mods[i].value;
        event.effective[r.mods[i].stat_guid] += d.effective[i];
    }
    for (const effects::Instance& e : n.book.instances()) event.held.emplace_back(e.def, e.instigator);
    return event;
}

void CombatWorld::hit_reacts(uint64_t npc_actor, const HolderEvent& event, uint64_t threat_to, const HitOrigin* origin, double now) {
    holder_reacts(npc_actor, event, now);
    if (threat_to != 0 && is_pet(threat_to)) {
        pet_hit_reacts(threat_to, event, now);
    } else if (origin && origin->landed_here && origin->hit && !origin->hit->reactions.empty()) {
        // what a hit that is not the session's (an effect's tick: Spirit Shred's damage) carries on its own record; the session runs the reactions of its own hits
        HolderEvent own = event;
        own.held.clear();
        own.lethal = false;
        holder_reacts(npc_actor, own, now, 2 | 4, &origin->hit->reactions);
    }
}

void CombatWorld::pet_hit_reacts(uint64_t pet, const HolderEvent& event, double now) {
    const auto it = npcs_.find(pet);
    if (it == npcs_.end() || !event.origin || it->second->dead) return;
    HolderEvent own = event;
    own.lethal = false;
    own.held.clear();
    for (const effects::Instance& e : it->second->book.instances()) own.held.emplace_back(e.def, e.instigator);
    holder_reacts(pet, own, now, 4, event.origin->hit ? &event.origin->hit->reactions : nullptr);
}

void CombatWorld::holder_reacts(uint64_t holder, const HolderEvent& event, double now, int hit_bit, const std::vector<int64_t>* carried) {
    if (event.origin && event.origin->source_type == content::kSourceTypeReaction) return;   // a hit a reaction made raises no reaction [the game's code; ability_reactions.cpp]
    if (!event.origin && !event.lethal) return;
    std::vector<std::pair<const std::vector<int64_t>*, uint64_t>> lists;
    for (const auto& [def, left_by] : event.held) lists.emplace_back(&def->reactions, left_by);
    if (carried) lists.emplace_back(carried, event.attacker);
    for (const auto& [list, by] : lists) {
        for (const int64_t guid : *list) {
            const abilities::ReactionDef* reaction = catalog_.reaction(guid);
            if (!reaction) continue;
            const content::ReactionRow& row = reaction->row;
            const bool on_hit = event.origin && (row.bind_hit & hit_bit) != 0;
            const bool on_death = hit_bit == 2 && event.lethal && (row.bind_stat & 2) != 0;
            if (!on_hit && !on_death) continue;
            // who casts it: the one who left the effect or the one whose hit this is; a creature's hit on a player's effect has no fighter to run from
            const uint64_t caster = row.instigator == "HitInstigator" ? event.attacker : by;
            PlayerPort* port = player(caster);
            if (!port || is_pet(caster)) continue;
            const auto holder_it = npcs_.find(holder);
            if (holder_it == npcs_.end()) return;
            if (row.trigger_expr != 0) {
                const content::ExprRow* e = catalog_.expr(row.trigger_expr);
                if (!e || e->program < 0) {
                    if (reaction_noted_.insert(guid).second) spdlog::info("[abilities] the reaction {} on a creature stays inert: its trigger does not compile", row.name);
                    continue;
                }
                // GetEffectInstigator() reads as the caster: the trigger's comparison of the two is made here
                if (e->text.find("GetEffectInstigator") != std::string::npos && caster != by) continue;
                std::string err;
                auto a = port->fighter().stats().evaluation(port->fighter().worn(), err, true);
                auto t = holder_it->second->stats().evaluation(holder_it->second->worn(), err, true);
                if (!a || !t || !a->set_actor(stats::kRoleTarget, *t, err)) continue;
                HitContext ctx;
                ctx.profile = &data_.profile();
                if (event.origin) {
                    if (event.origin->hit) ctx.tags = event.origin->hit->tags;
                    ctx.tags.insert(ctx.tags.end(), event.origin->ability_tags.begin(), event.origin->ability_tags.end());
                    ctx.source_type = event.origin->source_type;
                }
                ctx.hit.flags = event.flags;
                EventHost host(rng_, event, caster);
                host.set_context(&ctx);
                double v = 0;
                const bool ran = a->eval(e->program, host, v, err);
                host.set_context(nullptr);
                a->clear_actors();
                if (!ran) {
                    if (reaction_noted_.insert(guid).second) spdlog::info("[abilities] the reaction {} on a creature stays inert: {}", row.name, err);
                    continue;
                }
                if (v == 0.0) continue;
            }
            LandSource src;
            src.type = content::kSourceTypeReaction;
            src.guid = guid;
            src.event = &event;
            src.skip_requirement = true;
            for (const int64_t hit_guid : row.hits) {
                const abilities::HitDef* hit = catalog_.hit(hit_guid);
                if (!hit) continue;
                if ((row.targets_mask & (1 | 8 | 16)) != 0) land_for_player(caster, holder, *hit, src, now);   // the holder, the hit's target, what the hit was for
                if ((row.targets_mask & 2) != 0) port->hit_from_pet(hit_guid, guid, now);                       // the effect's instigator
                if ((row.targets_mask & 4) != 0 && event.attacker != caster) land_for_player(caster, event.attacker, *hit, src, now);   // who struck the holder (Brace)
                if ((row.targets_mask & 8192) != 0) {   // the hit instigator's pet
                    if (const uint64_t pet = pet_of_owner(caster)) land_for_player(caster, pet, *hit, src, now);
                }
            }
        }
    }
}

void CombatWorld::pet_was_hit(Npc& pet, const IncomingHit& hit, double taken, double now) {
    if (pet.book.empty()) return;
    HitOrigin origin;
    origin.hit = catalog_.hit(hit.hit_guid);
    origin.source_type = hit.source_type;
    origin.source_guid = hit.source_guid;
    HolderEvent event;
    event.attacker = hit.instigator;
    event.origin = &origin;
    event.flags = hit.flags;
    for (const ResolvedMod& m : hit.mods) event.mods[m.stat_guid] += m.value;
    event.effective[data_.ids().health] = -taken;
    for (const effects::Instance& e : pet.book.instances()) event.held.emplace_back(e.def, e.instigator);
    holder_reacts(pet.actor_id, event, now);
}

// ---- a player's pet ------------------------------------------------------------------------------------------------------------------------------

bool CombatWorld::pet_join(uint64_t pet, uint64_t owner) {
    PlayerPort* port = player(owner);
    Npc* n = port ? combatant(pet) : nullptr;
    if (!n) return false;
    n->aligns = port->fighter().alignments();   // (a creature's hits resolve against it as they do against its owner)
    pet_owners_[pet] = owner;
    if (pet_ports_.count(pet) == 0) {
        auto made = std::make_unique<PetPort>(*this, *n);
        add_player(made.get());
        pet_ports_[pet] = std::move(made);
    }
    return true;
}

bool CombatWorld::pet_use(uint64_t pet, uint64_t owner, int64_t ability, uint64_t target, double reach_cm) {
    const abilities::AbilityDef* def = catalog_.ability(ability);
    if (!def || !pet_join(pet, owner)) return false;
    Npc* n = combatant(pet);
    if (!n || n->dead) return false;
    PetCast cast;
    cast.owner = owner;
    cast.target = target;
    cast.ability = def;
    cast.reach_cm = reach_cm;
    if (const auto fight = pet_fights_.find(pet); fight != pet_fights_.end()) {   // the swing under way gives way; the fight goes on after the ability
        fight->second.swinging = false;
        fight->second.due.clear();
        fight->second.pursuing = false;
    }
    stop_cast(*n);
    pet_casts_[pet] = std::move(cast);
    return true;
}

bool CombatWorld::pet_can_use(uint64_t pet, uint64_t owner, int64_t ability, uint64_t target, std::string* why) {
    const auto refuse = [&](const char* text) {
        if (why) *why = text;
        return false;
    };
    const abilities::AbilityDef* def = catalog_.ability(ability);
    PlayerPort* port = player(owner);
    if (!def || !port) return refuse("the ability or its owner is not there");
    if (!pet_can_act(pet)) return refuse("the pet cannot act");
    constexpr int kFriendly = 1 | 4 | 8 | 16;   // Self, Ally, Party, Raid
    const int mask = def->row.applicable_targets;
    if (target == 0) return true;   // none named: it is played where the pet stands or on its owner, as the pipeline casts a player's ability at the air when it has no target
    if (target == pet || target == owner) {   // the friendly side
        if (mask != 0 && (mask & kFriendly) == 0) return refuse("it does not help allies");
        return true;
    }
    const world::WorldActor* t = registry_.find(target);
    if (!t || t->kind != world::ActorKind::Npc) return refuse("the target is not there");
    const auto fighting = npcs_.find(target);
    if (fighting != npcs_.end() && fighting->second->dead) return refuse("the target is dead");
    const npc::MotionStats bp = motion_.get(t->npc.archetype);
    const bool hostile = data_.relation(port->fighter().alignments(), std::vector<int64_t>(bp.alignments.begin(), bp.alignments.end())) == Relation::Hostile;
    if (hostile ? (mask & 2) == 0 : mask != 0 && (mask & kFriendly) == 0) return refuse(hostile ? "it does not harm enemies" : "it does not help allies");
    if (distance_xy_cm(port->fighter().position(), Vec3{t->x, t->y, t->z}) > kOwnerLeftCm) return refuse("the target is too far from its owner");
    return true;
}

void CombatWorld::pet_start_cast(Npc& pet, PetCast& cast, double now) {
    const abilities::AbilityDef& def = *cast.ability;
    PlayerPort* owner = player(cast.owner);
    const MontageData* montage = nullptr;
    if (anim_ && owner) {
        // the first of its montages whose choice holds (a Hunter's melee or ranged one) and that the tables read
        for (const content::MontageRow& m : def.montages) {
            if (m.soft_path.empty() || !resolver_.holds(owner->fighter(), m.select_expr, true)) continue;
            montage = anim_->montage(m.soft_path);
            if (montage && montage->length > 0 && montage->rate_scale > 0) break;
            montage = nullptr;
        }
    }
    double seconds = 1.0;
    bool timed = false;
    bool played = false;
    if (montage) {
        seconds = std::max(seconds, montage->seconds(1.0));
        const world::WorldActor* me = registry_.find(pet.actor_id);
        const auto play = bootstrap::montage_for(montage->path, montage->guid, montage->package_guid);
        if (me && npc_knows_ability(*me, def.row.guid) && play) {
            NpcCast shown;
            shown.ability = def.row.guid;
            shown.target = cast.target;
            const world::WorldActor* aim = cast.target ? registry_.find(cast.target) : nullptr;
            const Vec3 at = aim ? Vec3{aim->x, aim->y, aim->z} : pet.position();
            shown.tx = at.x;
            shown.ty = at.y;
            shown.tz = at.z;
            shown.montage = montage->guid;
            start_npc_cast(registry_, pet.actor_id, pet.cast, shown, play->chain);
            played = true;
        }
        const auto add = [&](double delay, int key, const char* source) {
            for (const content::AbilityHitRow& r : def.hits) {   // (every hit the key names lands at its notify)
                if (r.source != source || r.key != key) continue;
                cast.due.emplace_back(now + delay, r.hit_guid);
                timed = true;
            }
        };
        for (const MontageData::Hit& h : montage->hits) add(h.time / montage->rate_scale, static_cast<int>(h.key), "hit");
        const world::WorldActor* aim_at = cast.target ? registry_.find(cast.target) : nullptr;
        for (const MontageData::Fire& f : montage->fires) {
            // a bolt lands when it has flown: the montage's notify launches it on the clients
            double flying = kBoltFlightSeconds;
            if (aim_at) {
                for (const content::AbilityHitRow& r : def.hits) {
                    if (r.source == "projectile" && r.key == f.key) flying = flight_seconds(r, pet.position(), Vec3{aim_at->x, aim_at->y, aim_at->z});
                }
            }
            add(f.time / montage->rate_scale + flying, static_cast<int>(f.key), "projectile");
        }
    }
    if (!timed) {
        // no notify to go by: a moment into it, every hit of the pet's present stance (a Hunter's hits are named for theirs)
        const bool ranged = cast.reach_cm >= 1000.0;
        std::set<int64_t> done;
        for (const content::AbilityHitRow& r : def.hits) {
            const abilities::HitDef* hit = catalog_.hit(r.hit_guid);
            if (!hit || !done.insert(r.hit_guid).second) continue;
            const std::string& name = hit->row.name;
            if (ranged ? name.find("Melee") != std::string::npos : name.find("Ranged") != std::string::npos) continue;
            if (name.ends_with("_Redirect") || name.ends_with("_Remove")) continue;   // (these answer something later, they are not part of the swing)
            cast.due.emplace_back(now + kFirstHitSeconds, r.hit_guid);
        }
    }
    cast.end_at = now + seconds;
    cast.started = true;
    ++stats_.npc_attacks;
    spdlog::info("[pets] the pet (actor {}) starts {} at actor {}: {:.2f} s, {} hit(s) due, montage {}", pet.actor_id, def.row.name, cast.target, seconds, cast.due.size(),
                 played ? "shown" : montage ? "known to no list: not shown" : "none in the tables");
    // Lunge: the melee Hunter leaps at its target, the ranged one away from it [the montages are Engage and Disengage; the distance is the targeting record's]
    if (def.row.guid == pets::kHunterLunge && cast.target != 0 && npcs_world_) {
        if (const world::WorldActor* aim = registry_.find(cast.target)) {
            const Vec3 from = pet.position();
            const double dx = aim->x - from.x, dy = aim->y - from.y, length = std::max(1.0, std::hypot(dx, dy));
            const bool ranged = cast.reach_cm >= 1000.0;
            const nav::Vec3 to = ranged ? nav::Vec3{from.x - dx / length * kLungeCm, from.y - dy / length * kLungeCm, from.z} : nav::Vec3{aim->x, aim->y, aim->z};
            npcs_world_->order_move(pet.actor_id, to, 2500.0, ranged ? 0.0 : 150.0);
        }
    }
}

void CombatWorld::pet_land_on_self(Npc& pet, uint64_t owner, const abilities::HitDef& hit, double now) {
    PlayerPort* port = player(owner);
    if (!port || pet.dead) return;
    if (hit.max_remove > 0 || !hit.remove_effects.empty()) remove_effects_from_npc(pet.actor_id, hit, now);
    Resolved r;
    std::string why;
    HitMeta meta;
    meta.source_type = content::kSourceTypeAbility;
    if (!hit.mods.empty() && resolver_.resolve(port->fighter(), pet, hit, meta, r, why)) {
        for (const ResolvedMod& m : r.mods) {
            if (m.stat_guid != data_.ids().health || !std::isfinite(m.value)) continue;
            const double after = std::clamp(pet.health() + m.value, 0.0, pet.max_health());
            pet.cs->set_current(m.stat_guid, static_cast<float>(after));
            write_stat(registry_, pet.actor_id, m.stat_guid, static_cast<float>(after));
        }
    }
    land_effects_of_hit(pet, *port, hit, meta, now);
}

void CombatWorld::pet_land(Npc& pet, const PetCast& cast, const abilities::HitDef& hit, double now, int depth) {
    if (depth > 3 || hit.row.name.ends_with("_OwnerCost")) return;   // (the cost was paid when the ability was given)
    const int mask = hit.row.applicable_targets;
    PlayerPort* owner = player(cast.owner);
    if (!owner) return;
    if ((mask & 2) != 0) {
        const pets::HitArea area = pets::area_of_hit(hit.row.guid);
        const world::WorldActor* aim = cast.target ? registry_.find(cast.target) : nullptr;
        LandSource src;
        src.type = content::kSourceTypeAbility;
        src.guid = cast.ability->row.guid;
        src.ability = cast.ability;
        src.pet = pet.actor_id;
        src.children = false;
        src.skip_requirement = area.others_only;   // (its requirement is "not the owner's target", which is the test here)
        if (area.radius_cm > 0) {
            const Vec3 centre = area.around_pet || !aim ? pet.position() : Vec3{aim->x, aim->y, aim->z};
            for (const GatheredTarget& g : enemies_around(owner->fighter(), centre, area.radius_cm, 40)) {
                if (area.others_only && g.actor == cast.target) continue;
                if (!is_pet(g.actor)) land_for_player(cast.owner, g.actor, hit, src, now);
            }
        } else if (aim && !is_pet(cast.target) && cast.target != cast.owner) {
            land_for_player(cast.owner, cast.target, hit, src, now);
        }
    }
    if ((mask & 5) == 5) {   // one of the two: the pet when it was aimed at, else its owner
        if (cast.target == pet.actor_id) pet_land_on_self(pet, cast.owner, hit, now);
        else owner->hit_from_pet(hit.row.guid, cast.ability->row.guid, now);
    } else if ((mask & 1) != 0) {
        pet_land_on_self(pet, cast.owner, hit, now);
    } else if ((mask & (4 | 8)) != 0) {
        owner->hit_from_pet(hit.row.guid, cast.ability->row.guid, now);
    }
    if (cast.ability->row.guid == pets::kGuardianTaunt && cast.target != 0) {
        const auto taunted = npcs_.find(cast.target);
        if (taunted != npcs_.end() && !taunted->second->dead) {
            // it turns on the Guardian: more than anyone on its table by the margin a target changes at [modelled: the hit's threat is the design's own]
            double top = 0;
            for (const npc::ai::ThreatTable::Entry& e : taunted->second->ai.threat().entries()) top = std::max(top, e.threat);
            taunted->second->ai.notice(pet.actor_id, top * npc::ai::ThreatTable::kSwitchRatio + taunted->second->max_health(), now);
        }
    }
    if (hit.row.guid == pets::kHunterMeleeIncapHit && cast.target != 0) pet_stand_downs_[pet.actor_id] = cast.target;
    for (const content::RecursiveHitRow& r : hit.recursive) {
        if (const abilities::HitDef* child = catalog_.hit(r.child_hit)) pet_land(pet, cast, *child, now, depth + 1);
    }
}

void CombatWorld::tick_pet_casts(double now) {
    for (auto it = pet_casts_.begin(); it != pet_casts_.end();) {
        const uint64_t id = it->first;
        PetCast& cast = it->second;
        Npc* pet = combatant(id);
        PlayerPort* owner = player(cast.owner);
        bool over = !pet || pet->dead || !owner || !owner->fighter().alive();
        const bool at_enemy = cast.target != 0 && cast.target != id && cast.target != cast.owner && (cast.ability->row.applicable_targets & 2) != 0;
        const auto aimed = at_enemy ? npcs_.find(cast.target) : npcs_.end();
        if (!over && at_enemy && (aimed == npcs_.end() || aimed->second->dead)) over = true;
        if (over) {
            spdlog::info("[pets] the pet (actor {}): {} ends {} its hits: {}", id, cast.ability->row.name, cast.started ? "before all of" : "before any of",
                         !pet || pet->dead ? "the pet is gone" : !owner || !owner->fighter().alive() ? "its owner is gone" : "its target is gone");
            if (pet) stop_cast(*pet);
            it = pet_casts_.erase(it);
            continue;
        }
        if (!cast.started) {
            const double distance = at_enemy ? distance_xy_cm(pet->position(), aimed->second->position()) - pet->radius_cm() - aimed->second->radius_cm() : 0.0;
            if (distance > cast.reach_cm) {
                if (npcs_world_ && !cast.pursuing) {
                    const uint64_t who = cast.target;
                    cast.pursuing = npcs_world_->order_pursue(
                        id,
                        [this, who](nav::Vec3& out) {
                            const world::WorldActor* a = registry_.find(who);
                            if (!a) return false;
                            out = to_nav(Vec3{a->x, a->y, a->z});
                            return true;
                        },
                        720.0, std::max(100.0, cast.reach_cm * 0.8));
                }
                ++it;
                continue;
            }
            if (npcs_world_) npcs_world_->halt(id);
            pet_start_cast(*pet, cast, now);
        }
        std::vector<int64_t> landing;
        for (auto d = cast.due.begin(); d != cast.due.end();) {
            if (d->first <= now) {
                landing.push_back(d->second);
                d = cast.due.erase(d);
            } else {
                ++d;
            }
        }
        const PetCast held = cast;   // (a kill or a death in what lands can change the world's lists)
        for (const int64_t guid : landing) {
            if (const abilities::HitDef* hit = catalog_.hit(guid)) {
                spdlog::info("[pets] the pet (actor {}): the hit {} of {} lands", id, hit->row.name, held.ability->row.name);
                pet_land(*pet, held, *hit, now);
            }
        }
        // the ranged Incapacitate is a channel: the pet holds it while its target holds the effect (the sleep the cast puts on the creature)
        const bool channel = cast.ability->row.guid == pets::kHunterIncapacitate && cast.reach_cm >= 1000.0 && cast.target != 0 && effect_stacks(cast.target, pets::kHunterIncapRanged) > 0;
        if (cast.due.empty() && now >= cast.end_at && !channel) {
            stop_cast(*pet);
            it = pet_casts_.erase(it);
            continue;
        }
        ++it;
    }
}

bool CombatWorld::pet_attack(uint64_t pet, uint64_t owner, int64_t ability, uint64_t target) {
    if (pet == target || !player(owner)) return false;
    const abilities::AbilityDef* def = catalog_.ability(ability);
    if (!def) return false;
    const world::WorldActor* t = registry_.find(target);
    if (!t || t->kind != world::ActorKind::Npc) return false;
    Fighter& side = player(owner)->fighter();
    const npc::MotionStats bp = motion_.get(t->npc.archetype);
    if (data_.relation(side.alignments(), std::vector<int64_t>(bp.alignments.begin(), bp.alignments.end())) != Relation::Hostile) return false;
    Npc* other = combatant(target);
    if (!other || other->dead || !pet_join(pet, owner)) return false;
    PetFight fight;
    fight.owner = owner;
    fight.target = target;
    fight.ability = def;
    fight.reach_cm = launches_projectiles(*def) ? kRangedReachCm : kMeleeReachCm;
    pet_fights_[pet] = std::move(fight);
    return true;
}

void CombatWorld::pet_stop(uint64_t pet) {
    if (pet_fights_.erase(pet) == 0) return;
    const auto it = npcs_.find(pet);
    if (it != npcs_.end()) stop_cast(*it->second);
    if (npcs_world_) npcs_world_->halt(pet);
}

void CombatWorld::pet_cleanup(uint64_t pet) {
    pet_stop(pet);
    pet_stand_downs_.erase(pet);
    pet_casts_.erase(pet);
    pet_owners_.erase(pet);
    if (const auto it = pet_ports_.find(pet); it != pet_ports_.end()) {
        remove_player(it->second.get());
        pet_ports_.erase(it);
    }
}

void CombatWorld::pet_gone(uint64_t pet) {
    pet_cleanup(pet);
    forget(pet);
}

void CombatWorld::pet_swing(Npc& pet, PetFight& fight, const Npc& target, double now) {
    const abilities::AbilityDef& def = *fight.ability;
    double seconds = 1.5;
    bool timed = false;
    // the first of its montages the tables read: its notifies say when each hit lands.  A projectile's hit lands a moment after its notify (a pet's bolts do not fly in the world)
    const MontageData* montage = nullptr;
    if (anim_) {
        for (const content::MontageRow& m : def.montages) {
            if (m.soft_path.empty()) continue;
            montage = anim_->montage(m.soft_path);
            if (montage && montage->length > 0 && montage->rate_scale > 0) break;
            montage = nullptr;
        }
    }
    if (montage) {
        seconds = std::max(seconds, montage->seconds(1.0));
        const world::WorldActor* me = registry_.find(pet.actor_id);
        const auto play = bootstrap::montage_for(montage->path, montage->guid, montage->package_guid);
        if (me && npc_knows_ability(*me, def.row.guid) && play) {
            NpcCast cast;
            cast.ability = def.row.guid;
            cast.target = target.actor_id;
            const Vec3 at = target.position();
            cast.tx = at.x;
            cast.ty = at.y;
            cast.tz = at.z;
            cast.montage = montage->guid;
            start_npc_cast(registry_, pet.actor_id, pet.cast, cast, play->chain);
        }
        const auto add = [&](double delay, int key, const char* source) {
            for (const content::AbilityHitRow& r : def.hits) {
                if (r.source != source || r.key != key) continue;
                const abilities::HitDef* hit = catalog_.hit(r.hit_guid);
                if (!hit || (hit->row.applicable_targets & 2) == 0 || hit->mods.empty()) continue;
                fight.due.emplace_back(now + delay, r.hit_guid);   // (every hit the key names lands at its notify)
                timed = true;
            }
        };
        for (const MontageData::Hit& h : montage->hits) add(h.time / montage->rate_scale, static_cast<int>(h.key), "hit");
        for (const MontageData::Fire& f : montage->fires) {
            double flying = kBoltFlightSeconds;   // the bolt lands when it has flown
            for (const content::AbilityHitRow& r : def.hits) {
                if (r.source == "projectile" && r.key == f.key) flying = flight_seconds(r, pet.position(), target.position());
            }
            add(f.time / montage->rate_scale + flying, static_cast<int>(f.key), "projectile");
        }
    }
    if (!timed) {   // no notify the tables know: its first hit that hurts lands a moment into the swing
        for (const content::AbilityHitRow& r : def.hits) {
            const abilities::HitDef* hit = catalog_.hit(r.hit_guid);
            if (!hit || (hit->row.applicable_targets & 2) == 0 || hit->mods.empty()) continue;
            fight.due.emplace_back(now + kFirstHitSeconds, r.hit_guid);
            break;
        }
    }
    fight.end_at = now + seconds;
    fight.next_at = fight.end_at;
    fight.swinging = true;
    ++stats_.npc_attacks;
}

void CombatWorld::tick_pets(double now) {
    tick_pet_casts(now);
    // a pet's Health comes back as a player's does, once a second by its HealthRegeneration, whose formula gives an NPC no out-of-combat bonus (a creature resets instead; a pet does not)
    if (now >= pet_regen_at_) {
        pet_regen_at_ = now + 1.0;
        for (const auto& [id, owner] : pet_owners_) {
            Npc* pet = combatant(id);
            if (!pet || pet->dead || !pet->cs) continue;
            const stats::StatValue* regen = pet->cs->find(content::kGuidHealthRegeneration);
            const double max = pet->max_health();
            if (!regen || !(regen->value > 0) || !(pet->health() < max)) continue;
            const float after = static_cast<float>(std::min(max, pet->health() + static_cast<double>(regen->value)));
            pet->cs->set_current(data_.ids().health, after);
            write_stat(registry_, id, data_.ids().health, after);
        }
    }
    for (auto it = pet_fights_.begin(); it != pet_fights_.end();) {
        const uint64_t id = it->first;
        PetFight& fight = it->second;
        Npc* pet = combatant(id);
        if (pet_casts_.count(id) != 0) {   // the fight waits for its ability
            ++it;
            continue;
        }
        PlayerPort* owner = player(fight.owner);
        const auto found = npcs_.find(fight.target);
        const bool over = !pet || pet->dead || !owner || !owner->fighter().alive() || found == npcs_.end() || found->second->dead;
        if (over) {
            if (pet) stop_cast(*pet);
            if (npcs_world_) npcs_world_->halt(id);
            it = pet_fights_.erase(it);
            continue;
        }
        Npc& target = *found->second;
        if (distance_xy_cm(owner->fighter().position(), target.position()) > kOwnerLeftCm) {   // the owner has run off: the pet goes with it
            stop_cast(*pet);
            if (npcs_world_) npcs_world_->halt(id);
            it = pet_fights_.erase(it);
            continue;
        }
        // the hits that fall due; a kill can end the fight, seen at the next step
        std::vector<int64_t> landing;
        for (auto d = fight.due.begin(); d != fight.due.end();) {
            if (d->first <= now) {
                landing.push_back(d->second);
                d = fight.due.erase(d);
            } else {
                ++d;
            }
        }
        const uint64_t owner_id = fight.owner, target_id = fight.target;
        const abilities::AbilityDef* ability = fight.ability;
        for (const int64_t guid : landing) {
            const abilities::HitDef* hit = catalog_.hit(guid);
            const auto again = npcs_.find(target_id);
            if (!hit || again == npcs_.end() || again->second->dead) continue;
            HitMeta meta;
            meta.source_type = content::kSourceTypeAbility;
            meta.source_guid = ability->row.guid;
            meta.ability_tags = ability->row.tags;
            Resolved r;
            std::string why;
            // a pet's numbers are its owner's (the game scales a pet from its GetCreatureOwner stats; the generic creature stats a pet has here are many times a player's at its level)
            if (!resolver_.resolve(owner->fighter(), *again->second, *hit, meta, r, why)) {
                spdlog::debug("[pets] {}: {}", id, why);
                continue;
            }
            HitOrigin origin;
            origin.hit = hit;
            origin.source_type = meta.source_type;
            origin.source_guid = meta.source_guid;
            origin.ability_tags = meta.ability_tags;
            const Delivery d = apply_hit_to_npc(target_id, owner_id, r, now, id, &origin);
            if (d.applied) {
                // what the client draws is Health and Mana: a status stat's move is not told, as in land_for_player
                DealtHit dealt{guid, meta.source_type, meta.source_guid, id, target_id, d.flags, {}, {}};
                for (size_t i = 0; i < r.mods.size(); ++i) {
                    if (data_.ids().is_status_stat(r.mods[i].stat_guid)) continue;
                    dealt.mods.push_back(r.mods[i]);
                    dealt.effective.push_back(d.effective[i]);
                }
                if (!dealt.mods.empty() || hit->row.has_fx || r.miss()) owner->hit_dealt(dealt, now);
            }
        }
        if (fight.swinging && now >= fight.end_at) {
            fight.swinging = false;
            stop_cast(*pet);
        }
        if (!fight.swinging) {
            const double distance = distance_xy_cm(pet->position(), target.position()) - pet->radius_cm() - target.radius_cm();
            if (distance > fight.reach_cm) {
                if (npcs_world_ && !fight.pursuing) {
                    const uint64_t who = target_id;
                    fight.pursuing = npcs_world_->order_pursue(
                        id,
                        [this, who](nav::Vec3& out) {
                            const world::WorldActor* a = registry_.find(who);
                            if (!a) return false;
                            out = to_nav(Vec3{a->x, a->y, a->z});
                            return true;
                        },
                        720.0, std::max(100.0, fight.reach_cm * 0.8));
                }
            } else if (now >= fight.next_at) {
                if (npcs_world_) npcs_world_->halt(id);
                fight.pursuing = false;
                pet_swing(*pet, fight, target, now);
            }
        }
        ++it;
    }
}

}  // namespace aoc::worldd::combat
