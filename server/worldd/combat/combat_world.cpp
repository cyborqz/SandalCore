// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/combat_world.cpp (see combat_world.h)
// ============================================================================
#include "combat/combat_world.h"

#include "combat/npc_kits.h"
#include "combat/npc_replicator.h"
#include "content/content_db.h"
#include "spawns/generic_npc.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace aoc::worldd::combat {

namespace {

nav::Vec3 to_nav(const Vec3& v) { return nav::Vec3{v.x, v.y, v.z}; }

/// The kit of a creature kind: by the Blueprint's leaf without "_C"; else the kit whose name is the longest prefix of the leaf (or the leaf of it).
const npc_kits::Kit* kit_of(const std::string& leaf_in) {
    std::string leaf = leaf_in;
    if (leaf.size() > 2 && leaf.compare(leaf.size() - 2, 2, "_C") == 0) leaf.resize(leaf.size() - 2);
    const npc_kits::Kit* best = nullptr;
    size_t best_len = 0;
    for (size_t i = 0; i < npc_kits::kKitCount; ++i) {
        const npc_kits::Kit& k = npc_kits::kKits[i];
        const std::string name = k.blueprint;
        if (name == leaf) return &k;
        const bool related = leaf.rfind(name + "_", 0) == 0 || name.rfind(leaf + "_", 0) == 0;
        if (related && name.size() > best_len) {
            best = &k;
            best_len = name.size();
        }
    }
    return best;
}

}  // namespace
// [CoA ~ C:Y:B]

// ---- one NPC's combat state ----------------------------------------------------------------------------------------------------------

class CombatWorld::Npc final : public Fighter {
public:
    Npc(CombatWorld& w, uint64_t actor, npc::MotionStats bp_in, std::string leaf_in, int level_in)
        : world(w), actor_id(actor), bp(std::move(bp_in)), leaf(std::move(leaf_in)), lvl(level_in) {}

    uint64_t id() const override { return actor_id; }
    bool     is_player() const override { return false; }
    int      level() const override { return lvl; }
    Vec3     position() const override {
        const world::WorldActor* a = world.registry_.find(actor_id);
        return a ? Vec3{a->x, a->y, a->z} : Vec3{};
    }
    double radius_cm() const override { return bp.radius; }
    bool   alive() const override { return !dead; }
    const std::vector<int64_t>& alignments() const override { return aligns; }
    stats::CharacterStats&      stats() override { return *cs; }

    double health() const {
        const stats::StatValue* v = cs ? cs->find(content::kGuidHealth) : nullptr;
        return v ? v->value : 0.0;
    }
    double max_health() const {
        const stats::StatValue* v = cs ? cs->find(content::kGuidMaxHealth) : nullptr;
        return v ? v->value : 0.0;
    }

    CombatWorld&                           world;
    uint64_t                               actor_id;
    npc::MotionStats                       bp;
    std::string                            leaf;
    int                                    lvl;
    std::vector<int64_t>                   aligns;
    std::unique_ptr<stats::CharacterStats> cs;
    npc::ai::NpcAi                         ai;
    bool                                   hostile = false;
    bool                                   dead = false;
    bool                                   flags_in_combat = false;
    uint64_t                               flags_target = 0;
    bool                                   returning = false;
    const npc_kits::Kit*                   kit = nullptr;
    CombatWorld::Attack                    next;
    double                                 next_attack_at = 0;
    uint64_t                               killer = 0;
    std::set<uint64_t>                     attackers;
    struct Shield {
        double amount = 0;
        double until = 0;   // 0: until used up
    };
    std::vector<Shield>                    shields;

    /// A loss goes through the shields first (the oldest first); what is left reaches Health.
    double absorb(double loss, double now) {
        shields.erase(std::remove_if(shields.begin(), shields.end(), [&](const Shield& s) { return s.amount <= 0 || (s.until > 0 && s.until <= now); }), shields.end());
        for (Shield& s : shields) {
            const double take = std::min(s.amount, loss);
            s.amount -= take;
            loss -= take;
        }
        return loss;
    }
};

CombatWorld::CombatWorld(world::ActorRegistry& registry, npc::NpcWorld* npcs, const nav::AreaNav* nav, const CombatData& data, const abilities::Catalog& catalog,
                         const NpcStatBook& book, const npc::MotionTable& motion, CombatWorldConfig config)
    : registry_(registry), npcs_world_(npcs), nav_(nav), data_(data), catalog_(catalog), book_(book), motion_(motion), cfg_(std::move(config)),
      resolver_(catalog, data, cfg_.seed ? cfg_.seed : 1) {
    rng_.state = cfg_.seed * 0x9E3779B97F4A7C15ull + 7;
}

CombatWorld::~CombatWorld() = default;

// ---- players ----------------------------------------------------------------------------------------------------------------------

void CombatWorld::add_player(PlayerPort* port) {
    if (port && std::find(players_.begin(), players_.end(), port) == players_.end()) players_.push_back(port);
}

void CombatWorld::remove_player(PlayerPort* port) {
    players_.erase(std::remove(players_.begin(), players_.end(), port), players_.end());
    if (port) player_combat_until_.erase(port->actor_id());
}

PlayerPort* CombatWorld::player(uint64_t actor_id) {
    for (PlayerPort* p : players_) {
        if (p->actor_id() == actor_id) return p;
    }
    return nullptr;
}

void CombatWorld::player_exchange(uint64_t player_id, double now) {
    const double until = now + data_.in_combat_timer();
    const auto it = player_combat_until_.find(player_id);
    const bool was = it != player_combat_until_.end() && it->second > now;
    player_combat_until_[player_id] = until;
    if (!was) {
        if (PlayerPort* p = player(player_id)) p->set_in_combat(true, now);
    }
}

// ---- combatants -----------------------------------------------------------------------------------------------------------------------

CombatWorld::Npc* CombatWorld::make_combatant(const world::WorldActor& a) {
    if (a.kind != world::ActorKind::Npc) return nullptr;
    const std::string leaf = spawns::blueprint_leaf(a.npc.archetype);
    const npc::MotionStats bp = motion_.get(a.npc.archetype);
    auto n = std::make_unique<Npc>(*this, a.id, bp, leaf, a.npc.level);
    std::string err;
    n->cs = book_.make(bp, a.npc.level, err);
    if (!n->cs) {
        spdlog::warn("[combat] {} ({}): its stats do not run: {}", a.id, leaf, err);
        return nullptr;
    }
    n->aligns.assign(bp.alignments.begin(), bp.alignments.end());
    n->hostile = data_.relation(n->aligns, CombatData::player_alignments()) == Relation::Hostile;
    n->kit = kit_of(leaf);
    npc::ai::Params p;
    p.aggro_radius_cm = a.npc.aggro_radius_cm > 0 ? a.npc.aggro_radius_cm : cfg_.default_aggro_cm;
    // who may start a fight by noticing a player: a creature hostile to players whose data does not switch its aggro off (retest 3: a Timid Grem, whose instance overrides the radius with 0, and a
    // huntable, whose alignment is Friendly, never attack first)
    p.proximity_aggro = notices_players(n->hostile, a.npc);
    p.leash_radius_cm = a.npc.leash_radius_cm > 0 ? a.npc.leash_radius_cm : std::max(cfg_.default_leash_cm, p.aggro_radius_cm * 3.0);
    const double run = std::max({bp.max_run_speed, bp.max_walk_speed, 300.0});
    p.chase_speed_cm_s = run;
    p.run_home_speed_cm_s = run;
    n->ai.set_params(p);
    n->next = choose_attack(*n);
    // the client sees the creature's own numbers from now on
    write_stats(registry_, a.id, *n->cs);
    write_elite_rank(registry_, a.id, static_cast<int>(n->cs->npc().elite_rank));
    Npc* raw = n.get();
    npcs_[a.id] = std::move(n);
    return raw;
}
// (C_Y_B | C.O.A)

CombatWorld::Npc* CombatWorld::combatant(uint64_t actor_id) {
    if (const auto it = npcs_.find(actor_id); it != npcs_.end()) return it->second.get();
    const world::WorldActor* a = registry_.find(actor_id);
    if (!a || a->kind != world::ActorKind::Npc) return nullptr;
    return make_combatant(*a);
}

Fighter* CombatWorld::fighter_of(uint64_t actor_id) { return combatant(actor_id); }

uint64_t CombatWorld::actor_of_object(uint64_t object_id) const {
    if (registry_.find(object_id)) return object_id;
    // the components the registry indexes: the cylinder (what the client names once the server exported it) and the interactable
    // [E1 stage 3 fix 3: no arithmetic on the id; the draft's "actor + 24" was another actor's component]
    if (const world::WorldActor* a = registry_.find_by_component(object_id)) return a->id;
    return 0;
}

uint64_t CombatWorld::hit_component_of(uint64_t actor_id) const {
    const world::WorldActor* a = registry_.find(actor_id);
    return a ? a->npc.collision_cylinder : 0;
}

TargetInfo CombatWorld::inspect(uint64_t target_actor, const Fighter& from) {
    TargetInfo t;
    const world::WorldActor* a = registry_.find(target_actor);
    if (!a || a->kind != world::ActorKind::Npc) return t;
    t.exists = true;
    t.position = {a->x, a->y, a->z};
    const npc::MotionStats bp = motion_.get(a->npc.archetype);
    t.radius_cm = bp.radius;
    const auto it = npcs_.find(target_actor);
    t.alive = it == npcs_.end() ? true : !it->second->dead;
    std::vector<int64_t> aligns(bp.alignments.begin(), bp.alignments.end());
    t.relation = data_.relation(from.alignments(), aligns);
    const Vec3 me = from.position();
    t.surface_distance_cm = std::max(0.0, distance_xy_cm(me, t.position) - from.radius_cm() - t.radius_cm);
    t.in_line_of_sight = true;
    if (nav_) {
        const nav::Vec3 p = to_nav(me), q = to_nav(t.position);
        if (nav_->on_navmesh(p, 150, 300) && nav_->on_navmesh(q, 150, 300)) t.in_line_of_sight = nav_->line_of_sight(p, q);
    }
    return t;
}

std::optional<npc::ai::State> CombatWorld::ai_state(uint64_t actor_id) const {
    const auto it = npcs_.find(actor_id);
    if (it == npcs_.end()) return std::nullopt;
    return it->second->ai.state();
}

const npc::ai::NpcAi* CombatWorld::ai_of(uint64_t actor_id) const {
    const auto it = npcs_.find(actor_id);
    return it == npcs_.end() ? nullptr : &it->second->ai;
}

double CombatWorld::health_of(uint64_t actor_id) const {
    const auto it = npcs_.find(actor_id);
    return it == npcs_.end() ? -1.0 : it->second->health();
}

double CombatWorld::max_health_of(uint64_t actor_id) const {
    const auto it = npcs_.find(actor_id);
    return it == npcs_.end() ? -1.0 : it->second->max_health();
}

bool CombatWorld::dead(uint64_t actor_id) const {
    const auto it = npcs_.find(actor_id);
    return it != npcs_.end() && it->second->dead;
}

std::string CombatWorld::next_attack_name(uint64_t actor_id) const {
    const auto it = npcs_.find(actor_id);
    if (it == npcs_.end() || !it->second->next.ability) return {};
    return it->second->next.ability->row.name;
}

void CombatWorld::forget(uint64_t actor_id) {
    npcs_.erase(actor_id);
    huntable_stats_.erase(actor_id);
}

void CombatWorld::fill_huntable_stats(const world::WorldActor& a) {
    if (!huntable_stats_.insert(a.id).second) return;
    const npc::MotionStats bp = motion_.get(a.npc.archetype);
    std::string err;
    const auto cs = book_.make(bp, a.npc.level, err);
    if (!cs) {
        spdlog::warn("[combat] {} ({}): a huntable whose stats do not run keeps its template's: {}", a.id, spawns::blueprint_leaf(a.npc.archetype), err);
        return;
    }
    write_stats(registry_, a.id, *cs);
}

void CombatWorld::add_shield(uint64_t npc_actor, double amount, double seconds, double now) {
    Npc* n = combatant(npc_actor);
    if (!n || n->dead || !std::isfinite(amount) || amount <= 0) return;
    if (n->shields.size() >= 16) n->shields.erase(n->shields.begin());
    n->shields.push_back({amount, seconds > 0 ? now + seconds : 0.0});
}

double CombatWorld::shield_of(uint64_t npc_actor, double now) const {
    const auto it = npcs_.find(npc_actor);
    if (it == npcs_.end()) return 0;
    double total = 0;
    for (const Npc::Shield& s : it->second->shields) {
        if (s.amount > 0 && (s.until <= 0 || s.until > now)) total += s.amount;
    }
    return total;
}

void CombatWorld::add_threat(uint64_t npc_actor, uint64_t attacker, double amount, double now) {
    Npc* n = combatant(npc_actor);
    if (!n || n->dead) return;
    n->ai.notice(attacker, amount, now);
    n->attackers.insert(attacker);
}

// ---- attacks ----------------------------------------------------------------------------------------------------------------------

namespace {
// C_Y_B.C.O.A

/// Whether an ability does harm to enemies: some hit of it is aimed at enemies and moves Health.
bool is_attack(const abilities::AbilityDef& d, const abilities::Catalog& catalog, int64_t health_stat) {
    for (const content::AbilityHitRow& h : d.hits) {
        const abilities::HitDef* hit = catalog.hit(h.hit_guid);
        if (!hit || (hit->row.applicable_targets & 2) == 0) continue;
        for (const content::ModRow& m : hit->mods) {
            if (m.stat_guid == health_stat && m.value_expr != 0 && catalog.program(m.value_expr) >= 0) return true;
        }
    }
    return false;
}

}  // namespace

CombatWorld::Attack CombatWorld::choose_attack(Npc& n) {
    Attack a;
    a.windup_ms = static_cast<int>(cfg_.default_windup * 1000);
    a.range_cm = static_cast<int>(cfg_.default_attack_range_cm);
    if (n.kit) {
        struct Cand {
            const abilities::AbilityDef* def;
            const npc_kits::KitAbility*  k;
        };
        std::vector<Cand> cands;
        double total = 0;
        for (size_t i = 0; i < n.kit->count; ++i) {
            const npc_kits::KitAbility& k = n.kit->abilities[i];
            const abilities::AbilityDef* d = catalog_.ability_named(k.name);
            if (!d || !is_attack(*d, catalog_, data_.ids().health)) continue;
            cands.push_back({d, &k});
            total += std::max(1, k.uses);
        }
        if (!cands.empty()) {
            double r = rng_.rand_double() * total;
            for (const Cand& c : cands) {
                r -= std::max(1, c.k->uses);
                if (r <= 0 || &c == &cands.back()) {
                    a.ability = c.def;
                    a.windup_ms = c.k->windup_ms;
                    a.range_cm = c.k->range_cm;
                    return a;
                }
            }
        }
    }
    if (const abilities::AbilityDef* d = catalog_.ability_named(cfg_.fallback_ability)) {
        if (is_attack(*d, catalog_, data_.ids().health)) a.ability = d;
    }
    return a;
}

void CombatWorld::start_attack(Npc& n, uint64_t target, double now) {
    const Attack a = n.next;
    const double interval = (n.kit && n.kit->interval_ms > 0 ? n.kit->interval_ms / 1000.0 : cfg_.default_attack_interval);
    n.next_attack_at = now + interval * (1.0 + cfg_.attack_variance * (2.0 * rng_.rand_double() - 1.0));
    n.next = choose_attack(n);
    if (!a.ability) {
        ++stats_.refused_no_attack;
        return;
    }
    Pending p;
    p.due = now + a.windup_ms / 1000.0;
    p.npc = n.actor_id;
    p.target = target;
    p.ability = a.ability;
    std::set<int64_t> seen;
    for (const content::AbilityHitRow& h : a.ability->hits) {
        if ((h.source == "hit" || h.source == "projectile") && seen.insert(h.hit_guid).second) p.hits.push_back(h.hit_guid);
    }
    pending_.push_back(std::move(p));
    ++stats_.npc_attacks;
}

void CombatWorld::resolve_pending(double now) {
    for (size_t i = 0; i < pending_.size();) {
        Pending p = pending_[i];
        if (p.due > now) {
            ++i;
            continue;
        }
        pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
        const auto it = npcs_.find(p.npc);
        PlayerPort* port = player(p.target);
        if (it == npcs_.end() || it->second->dead || !port || !port->fighter().alive()) continue;
        Npc& n = *it->second;
        // the target must still be near the swing (it may have run out of range during the windup)
        const double reach = static_cast<double>(n.next.range_cm) * 1.5 + 300.0;
        const double d = distance_xy_cm(n.position(), port->fighter().position()) - n.radius_cm() - port->fighter().radius_cm();
        if (d > std::max(reach, 600.0)) continue;
        for (const int64_t guid : p.hits) {
            const abilities::HitDef* hit = catalog_.hit(guid);
            if (!hit || (hit->row.applicable_targets & 2) == 0 || hit->mods.empty()) continue;
            HitMeta meta;
            meta.source_type = content::kSourceTypeAbility;
            meta.source_guid = p.ability->row.guid;
            meta.ability_tags = p.ability->row.tags;
            Resolved r;
            std::string why;
            if (!resolver_.resolve(n, port->fighter(), *hit, meta, r, why)) {
                spdlog::debug("[combat] {} ({}): {}", n.actor_id, n.leaf, why);
                continue;
            }
            IncomingHit in;
            in.instigator = n.actor_id;
            in.hit_guid = guid;
            in.source_type = meta.source_type;
            in.source_guid = meta.source_guid;
            in.flags = r.flags;
            in.mods = r.mods;
            in.at = now;
            r.miss() ? ++stats_.npc_misses : ++stats_.npc_hits;
            port->receive_hit(in, now);
            player_exchange(port->actor_id(), now);
        }
    }
}

// ---- hits from a player -------------------------------------------------------------------------------------------------------------
// -- C.Y.B & CoA --

Delivery CombatWorld::apply_hit_to_npc(uint64_t npc_actor, uint64_t attacker, const Resolved& r, double now) {
    Delivery d;
    d.target_object = hit_component_of(npc_actor);
    Npc* n = combatant(npc_actor);
    if (!n || n->dead) return d;
    d.applied = true;
    d.flags = r.flags;
    d.effective.assign(r.mods.size(), 0.0);
    n->attackers.insert(attacker);
    ++stats_.player_hits;
    if (!r.miss()) {
        const CombatStatIds& id = data_.ids();
        for (size_t i = 0; i < r.mods.size(); ++i) {
            const ResolvedMod& m = r.mods[i];
            if ((m.stat_guid != id.health && m.stat_guid != id.mana) || !std::isfinite(m.value)) continue;
            const stats::StatValue* cur = n->cs->find(m.stat_guid);
            const stats::StatValue* max = n->cs->find(m.stat_guid == id.health ? id.max_health : id.max_mana);
            if (!cur || !max) continue;
            const double before = cur->value;
            double value = m.value;
            if (m.stat_guid == id.health && value < 0) value = -n->absorb(-value, now);   // the shields take it first
            const double after = std::clamp(before + value, 0.0, static_cast<double>(max->value));
            d.effective[i] = after - before;
            n->cs->set_current(m.stat_guid, static_cast<float>(after));
            write_stat(registry_, npc_actor, m.stat_guid, static_cast<float>(after));
        }
    }
    const bool lethal = n->health() <= 0.0;
    // the threat: what the hit says, else what it took
    double threat = r.threat;
    if (threat <= 0 && !r.miss()) {
        for (const double e : d.effective) threat += e < 0 ? -e : 0;
    }
    if (!lethal) n->ai.notice(attacker, std::max(threat, 1.0), now);
    player_exchange(attacker, now);
    if (lethal) {
        d.lethal = true;
        d.flags |= stats::kHitLethal;
        kill(*n, attacker, now);
    } else {
        think(*n, now);   // the fight starts in this step: the flags go up with the first exchange
    }
    return d;
}

void CombatWorld::kill(Npc& n, uint64_t killer, double now) {
    if (n.dead) return;
    n.dead = true;
    n.killer = killer;
    n.ai.kill();
    n.cs->set_in_combat(false);
    n.cs->set_current(content::kGuidHealth, 0.0f);
    write_stat(registry_, n.actor_id, content::kGuidHealth, 0.0f);
    write_combat(registry_, n.actor_id, false, true, 0);
    n.flags_in_combat = false;
    n.flags_target = 0;
    if (npcs_world_) npcs_world_->halt(n.actor_id);
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const Pending& p) { return p.npc == n.actor_id; }), pending_.end());
    ++stats_.kills;
    if (death_hook_) {
        const world::WorldActor* a = registry_.find(n.actor_id);
        DeathEvent e;
        e.npc = n.actor_id;
        e.killer = killer;
        e.at = now;
        e.level = n.lvl;
        e.position = n.position();
        e.blueprint = n.leaf;
        e.info = a ? &a->npc : nullptr;
        e.motion = &n.bp;
        e.attackers = n.attackers;
        death_hook_(e);
    }
}

// ---- thinking -------------------------------------------------------------------------------------------------------------------

void CombatWorld::set_flags(Npc& n, bool in_combat, uint64_t target_actor) {
    const uint64_t object = target_actor;
    if (n.flags_in_combat == in_combat && n.flags_target == object) return;
    n.flags_in_combat = in_combat;
    n.flags_target = object;
    n.cs->set_in_combat(in_combat);
    write_combat(registry_, n.actor_id, in_combat, false, object);
}

void CombatWorld::begin_reset(Npc& n, double now) {
    (void)now;
    ++stats_.resets;
    n.returning = true;
    n.attackers.clear();
    // it heals to full as it walks home
    if (const stats::StatValue* max = n.cs->find(data_.ids().max_health)) {
        n.cs->set_current(content::kGuidHealth, max->value);
        write_stat(registry_, n.actor_id, content::kGuidHealth, max->value);
    }
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const Pending& p) { return p.npc == n.actor_id; }), pending_.end());
    set_flags(n, false, 0);
    if (npcs_world_) npcs_world_->order_return_home(n.actor_id);
}

void CombatWorld::think(Npc& n, double now) {
    if (n.dead) return;
    const world::WorldActor* a = registry_.find(n.actor_id);
    if (!a) return;
    npc::ai::Sense s;
    s.now = now;
    s.self = {a->x, a->y, a->z};
    s.self_radius_cm = n.bp.radius;
    s.home = {a->npc.home_x, a->npc.home_y, a->npc.home_z};
    s.attack_range_cm = n.next.range_cm;
    const double far = n.ai.params().leash_radius_cm * 2.0 + n.ai.params().aggro_radius_cm;
    for (PlayerPort* p : players_) {
        const Fighter& f = p->fighter();
        if (distance_xy_cm(f.position(), s.self) > far) continue;
        npc::ai::Candidate c;
        c.id = p->actor_id();
        c.pos = f.position();
        c.radius_cm = f.radius_cm();
        c.alive = f.alive();
        c.hostile = true;
        s.known.push_back(c);
    }
    if (nav_) {
        s.line_of_sight = [this](const Vec3& from, const Vec3& to) {
            const nav::Vec3 p = to_nav(from), q = to_nav(to);
            if (!nav_->on_navmesh(p, 150, 300) || !nav_->on_navmesh(q, 150, 300)) return true;   // off the navmesh the ground says nothing
            return nav_->line_of_sight(p, q);
        };
    }
    const npc::ai::State before = n.ai.state();
    const npc::ai::Orders o = n.ai.think(s);
    if (o.began_fight) ++stats_.aggros;
    if (o.began_reset) begin_reset(n, now);
    if (o.reset_done) {
        n.returning = false;
        if (npcs_world_) npcs_world_->release(n.actor_id);
    }
    if (o.in_combat) set_flags(n, true, o.target);
    else if (n.ai.state() == npc::ai::State::Idle || n.ai.state() == npc::ai::State::Reset) set_flags(n, false, 0);
    if (npcs_world_ && !o.began_reset) {
        const nav::Vec3 here{a->x, a->y, a->z};
        switch (o.move) {
            case npc::ai::Orders::Move::ChaseTo: npcs_world_->order_move(n.actor_id, to_nav(o.dest), o.speed_cm_s, o.stop_distance_cm); break;
            case npc::ai::Orders::Move::Stand:
                if (before != npc::ai::State::Attack) npcs_world_->order_move(n.actor_id, here, 0.0, 0.0);
                break;
            default: break;
        }
    }
    if (o.attack && now >= n.next_attack_at) start_attack(n, o.target, now);
}
// Cyb + C-O-A

void CombatWorld::tick(double now) {
    now_ = now;
    if (!cfg_.enabled) return;
    resolve_pending(now);
    if (now < next_think_) return;
    next_think_ = now + cfg_.think_seconds;

    // the players' combat flags end a while after the last exchange
    for (auto it = player_combat_until_.begin(); it != player_combat_until_.end();) {
        if (now >= it->second) {
            if (PlayerPort* p = player(it->first)) p->set_in_combat(false, now);
            it = player_combat_until_.erase(it);
        } else {
            ++it;
        }
    }

    std::set<uint64_t> todo;
    for (PlayerPort* p : players_) {
        const Vec3 pos = p->fighter().position();
        std::vector<uint64_t> ids;
        registry_.query(pos.x, pos.y, cfg_.scan_radius_cm, ids);
        for (const uint64_t id : ids) {
            const world::WorldActor* a = registry_.find(id);
            if (!a || a->kind != world::ActorKind::Npc) continue;
            const auto known = npcs_.find(id);
            if (known != npcs_.end()) {
                todo.insert(id);
                continue;
            }
            if (huntable_ && huntable_(a->npc)) {   // a huntable is no fighter: its stats, nothing more
                fill_huntable_stats(*a);
                continue;
            }
            auto h = hostile_by_archetype_.find(a->npc.archetype);
            if (h == hostile_by_archetype_.end()) {
                const npc::MotionStats bp = motion_.get(a->npc.archetype);
                const std::vector<int64_t> al(bp.alignments.begin(), bp.alignments.end());
                h = hostile_by_archetype_.emplace(a->npc.archetype, data_.relation(al, CombatData::player_alignments()) == Relation::Hostile).first;
            }
            if (h->second) todo.insert(id);
        }
    }
    for (const auto& [id, n] : npcs_) {
        if (!n->dead && n->ai.state() != npc::ai::State::Idle) todo.insert(id);
    }
    for (const uint64_t id : todo) {
        Npc* n = combatant(id);
        if (n) think(*n, now);
    }
}

}  // namespace aoc::worldd::combat
