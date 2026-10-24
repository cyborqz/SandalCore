// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/combat_world.cpp (see combat_world.h)
// ============================================================================
#include "combat/combat_world.h"

#include "bootstrap/constants.h"
#include "combat/npc_ability_replicator.h"
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
    std::string                            archetype;
    const std::vector<CreatureAbility>*    anim = nullptr;   // the Blueprint's use-ability tasks when the creature attacks with its own animation
    NpcCastState                           cast;             // the attack the client is shown (combat/npc_ability_replicator.h)
    std::map<std::string, double>          cooldown_until;   // the name of a task's cooldown -> when it is over
    double                                 hold_until = 0;   // the creature stands still, facing as it does, until then (the montage of an attack it stands for)
    double                                 fight_began = 0;  // when its fight began (the tree asks how long it has gone on)
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
    n->archetype = a.npc.archetype;
    n->anim = anim_ ? anim_->abilities_of(a.npc.archetype) : nullptr;
    // a creature whose tree has no attack it can play (a Blueprint with no open of its own, whose generic open lists none of the tree's abilities) keeps the single swing every creature had: the tree
    // would otherwise find nothing to use and the creature would stand and stare.  A tree that merely allows none right now (range, angle, cooldown, a roll) is not this: that creature waits.
    if (n->anim && !can_animate(*n)) n->anim = nullptr;
    if (n->anim) {   // its attacks start only where its tree's conditions hold: the reach is not stretched once a fight begins, and it walks up to just inside it (a chase ends at 96 % of the reach)
        p.attack_hysteresis = 1.0;
        p.chase_stop_fraction = 0.96;
    }
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

void CombatWorld::summaries(std::vector<Summary>& out) const {
    out.reserve(out.size() + npcs_.size());
    for (const auto& entry : npcs_) {
        const Npc& n = *entry.second;
        Summary s;
        s.id = entry.first;
        const double max = n.max_health(), now = n.health();
        const double pct = max > 0 ? std::clamp(now / max * 100.0, 0.0, 100.0) : 100.0;
        s.health_pct = static_cast<uint8_t>(std::lround(pct));
        if (!n.dead && now > 0 && s.health_pct == 0) s.health_pct = 1;   // alive is never shown as empty
        s.in_combat = n.flags_in_combat;
        s.dead = n.dead;
        out.push_back(s);
    }
}

std::string CombatWorld::next_attack_name(uint64_t actor_id) const {
    const auto it = npcs_.find(actor_id);
    if (it == npcs_.end() || !it->second->next.ability) return {};
    return it->second->next.ability->row.name;
}

void CombatWorld::stop_cast(Npc& n) {
    if (npcs_world_) {
        npcs_world_->end_root_motion(n.actor_id);
        npcs_world_->stop_tracking(n.actor_id);   // (the turning that ran with the ability ends with it)
    }
    end_npc_cast(registry_, n.actor_id, n.cast);
}

void CombatWorld::forget(uint64_t actor_id) {
    if (const auto it = npcs_.find(actor_id); it != npcs_.end()) stop_cast(*it->second);
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

namespace {

/// The host of a montage's selection expression ("RandIntRange(0,2) == 0"): a random number from the world's source and nothing else.
class SelectHost final : public stats::ExprHost {
public:
    explicit SelectHost(stats::SplitMix& rng) : rng_(rng) {}
    double rand_double() override { return rng_.rand_double(); }
    double host_fn(int fn, const std::string&, const double* args, int) override {
        if (fn != stats::kHostRandIntRange) return 0;
        const double lo = std::floor(args[0]), hi = std::floor(args[1]);
        return hi < lo ? lo : lo + std::floor(rng_.rand_double() * (hi - lo + 1.0));
    }

private:
    stats::SplitMix& rng_;
};

/// No hit lands sooner than this after its attack begins: the client has its animation by then, and the confirmation never overtakes it.
constexpr double kMinHitDelay = 0.1;

/// The angle between two directions in degrees, 0 to 180.
double angle_between_degrees(double a, double b) {
    double d = std::fmod(a - b, 360.0);
    if (d > 180.0) d -= 360.0;
    if (d < -180.0) d += 360.0;
    return std::fabs(d);
}

}  // namespace

CombatWorld::Attack CombatWorld::choose_attack(Npc& n) {
    Attack a;
    a.windup_ms = static_cast<int>(cfg_.default_windup * 1000);
    a.range_cm = static_cast<int>(cfg_.default_attack_range_cm);
    if (n.anim) {
        // a creature with its own animation uses what its Blueprint's tree says (the tasks with a condition the tables cannot say are left alone)
        const Attack b = choose_animated(n, nullptr);
        if (b.ability) return b;
    } else if (n.kit) {
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

CombatWorld::Attack CombatWorld::choose_animated(Npc& n, const Fighter* target, TreeWhy* why) {
    Attack a;
    a.windup_ms = static_cast<int>(cfg_.default_windup * 1000);
    a.range_cm = static_cast<int>(cfg_.default_attack_range_cm);
    if (!n.anim) return a;
    // how far the target is from the creature's surface (the AI measures its attack range so) and how far off the way the creature faces it stands: the tree's two conditions, asked every time
    double distance = -1, angle = 0;
    CondContext ctx;
    ctx.preview = target == nullptr;
    ctx.has_target = target != nullptr;
    if (target) {
        const world::WorldActor* w = registry_.find(n.actor_id);
        if (!w) return a;
        const Vec3 at = target->position();
        distance = distance_xy_cm(Vec3{w->x, w->y, w->z}, at) - n.radius_cm() - target->radius_cm();
        angle = angle_between_degrees(std::atan2(at.y - w->y, at.x - w->x) * 180.0 / 3.14159265358979323846, w->yaw_degrees);
        ctx.surface_distance = distance;
        ctx.angle = angle;
        ctx.centre_distance = std::sqrt((at.x - w->x) * (at.x - w->x) + (at.y - w->y) * (at.y - w->y) + (at.z - w->z) * (at.z - w->z));
        const Vec3 from{w->x, w->y, w->z};
        ctx.sees = [this, from, at]() { return line_clear(from, at); };
    }
    ctx.health_fraction = n.max_health() > 0 ? std::clamp(n.health() / n.max_health(), 0.0, 1.0) : 1.0;
    ctx.fight_seconds = std::max(0.0, now_ - n.fight_began);
    ctx.cooldown_over = [&n, this](const std::string& g) {
        const auto until = n.cooldown_until.find(g);
        return until == n.cooldown_until.end() || now_ >= until->second;
    };
    ctx.roll = [this]() { return rng_.rand_double(); };
    // the tree is tried in order and the first task whose conditions hold is used
    for (const CreatureAbility& t : *n.anim) {
        if (t.unknown || t.off || !t.fight) continue;
        const abilities::AbilityDef* d = catalog_.ability(t.ability);
        if (!d || !is_attack(*d, catalog_, data_.ids().health) || !animates(n, *d)) continue;
        bool ready = true;
        for (const std::string& g : t.gates) {
            const auto until = n.cooldown_until.find(g);
            if (until != n.cooldown_until.end() && now_ < until->second) ready = false;
        }
        if (!ready) continue;
        if (target) {
            if (t.max_distance >= 0 && distance > t.max_distance) {
                if (why) why->too_far = true;
                continue;
            }
            if (t.min_distance >= 0 && distance < t.min_distance) continue;
            if (t.max_angle >= 0 && angle > t.max_angle) {
                if (why) why->off_angle = true;
                continue;
            }
        } else if (t.min_distance > 0) {
            continue;   // a task for a target far away is not the attack the creature stands ready with
        }
        // the tree's other conditions (a roll, its health, how long the fight has gone on, whether it sees its target ...), asked afresh at every try
        bool holds = true;
        for (const CondGroup& g : t.groups) {
            if (!eval_group(g, ctx)) {
                holds = false;
                break;
            }
        }
        if (!holds) continue;
        a.ability = d;
        a.task = &t;
        if (t.max_distance >= 0) a.range_cm = static_cast<int>(t.max_distance);
        return a;
    }
    return a;
}

bool CombatWorld::line_clear(const Vec3& from, const Vec3& to) const {
    if (!nav_) return true;
    const nav::Vec3 p = to_nav(from), q = to_nav(to);
    if (!nav_->on_navmesh(p, 150, 300) || !nav_->on_navmesh(q, 150, 300)) return true;   // off the navmesh the ground says nothing
    return nav_->line_of_sight(p, q);
}

bool CombatWorld::hit_reaches(const Npc& n, const Fighter& body, const BiteArea& area, bool is_target) const {
    const world::WorldActor* w = registry_.find(n.actor_id);
    if (!w) return false;
    const Vec3 at = body.position();
    HitSubject s;
    s.cx = w->x;
    s.cy = w->y;
    s.yaw = w->yaw_degrees;
    s.tx = at.x;
    s.ty = at.y;
    s.tradius = body.radius_cm();
    const Vec3 from{w->x, w->y, w->z};
    return hit_lands(area, s, is_target, [this, from, at]() { return line_clear(from, at); });
}

uint64_t CombatWorld::aimed_at(const Npc& n, const BiteArea& area, uint64_t cast_target) {
    // the record finds its target the way it says: what the creature fights now, the one the cast was aimed at, or both in an order
    using D = BiteArea::Discovery;
    PlayerPort* locked = player(cast_target);
    const bool locked_ok = locked && locked->fighter().alive();
    const uint64_t fighting = n.ai.target();
    PlayerPort* active = fighting ? player(fighting) : nullptr;
    const bool active_ok = active && active->fighter().alive();
    switch (area.discovery) {
        case D::PrefersActive: return active_ok ? fighting : (locked_ok ? cast_target : 0);
        case D::OnlyActive: return active_ok ? fighting : 0;
        case D::OnlyLocked: return locked_ok ? cast_target : 0;
        case D::PrefersLocked: return locked_ok ? cast_target : (active_ok ? fighting : 0);
        case D::None: break;
    }
    return 0;
}

bool CombatWorld::can_animate(const Npc& n) const {
    if (!anim_ || !n.anim) return false;
    for (const CreatureAbility& t : *n.anim) {
        if (t.unknown || t.off || !t.fight) continue;
        const abilities::AbilityDef* d = catalog_.ability(t.ability);
        if (d && is_attack(*d, catalog_, data_.ids().health) && animates(n, *d)) return true;
    }
    return false;
}

bool CombatWorld::montages_readable(const abilities::AbilityDef& def) const {
    if (!anim_) return false;
    bool data = false;
    for (const content::MontageRow& m : def.montages) {
        if (m.soft_path.empty()) continue;
        const MontageData* md = anim_->montage(m.soft_path);
        if (!md) return false;
        if (md->root_unreadable) return false;   // its animation moves the creature in a way the tables could not read: the creature keeps its single swing
        data = true;
    }
    return data;
}

bool CombatWorld::areas_usable(const abilities::AbilityDef& def) const {
    if (!anim_) return false;
    for (const content::AbilityHitRow& r : def.hits) {
        if (r.source != "hit") continue;
        const abilities::HitDef* hit = catalog_.hit(r.hit_guid);
        if (!hit || (hit->row.applicable_targets & 2) == 0 || hit->mods.empty()) continue;   // (only a hit that hurts an enemy lands by an area)
        const BiteArea* area = anim_->area_of(def.row.guid, static_cast<int>(r.key));
        if (!area || !area->usable()) return false;
    }
    return true;
}

bool CombatWorld::animates(const Npc& n, const abilities::AbilityDef& def) const {
    if (!anim_ || !n.anim) return false;
    bool listed = false;
    for (const CreatureAbility& t : *n.anim) listed = listed || t.ability == def.row.guid;
    if (!listed) return false;
    const world::WorldActor* a = registry_.find(n.actor_id);
    if (!a || !npc_knows_ability(*a, def.row.guid)) return false;
    return montages_readable(def) && areas_usable(def);   // (an ability whose hit lands by a volume the world cannot place keeps the creature's single swing)
}

std::string CombatWorld::GateCensus::text() const {
    return "creature attacks by where their hits land: " + std::to_string(any) + " on the target wherever it stands, " + std::to_string(validity) + " inside a cone, " + std::to_string(volumes) +
           " by volumes; " + std::to_string(kept_swing) + " keep the single swing (a volume that cannot be placed); " + std::to_string(unplayed) + " have no hit of their montage to decide by (a thrown rock, a bolt)";
}

CombatWorld::GateCensus CombatWorld::gate_census() const {
    GateCensus out;
    if (!anim_) return out;
    using G = BiteArea::Gate;
    std::set<int64_t> seen;
    for (const auto& [archetype, tasks] : anim_->by_kind()) {
        for (const CreatureAbility& t : tasks) {
            if (t.unknown || t.off || !t.fight || !seen.insert(t.ability).second) continue;
            const abilities::AbilityDef* d = catalog_.ability(t.ability);
            if (!d || !is_attack(*d, catalog_, data_.ids().health) || !montages_readable(*d)) continue;
            if (!areas_usable(*d)) {
                ++out.kept_swing;
                out.kept_names.push_back(d->row.name);
                continue;
            }
            G gate = G::None;   // the ability's first hit that hurts an enemy decides which kind it counts as
            bool found = false;
            for (const content::AbilityHitRow& r : d->hits) {
                const abilities::HitDef* hit = catalog_.hit(r.hit_guid);
                if (r.source != "hit" || !hit || (hit->row.applicable_targets & 2) == 0 || hit->mods.empty()) continue;
                const BiteArea* area = anim_->area_of(d->row.guid, static_cast<int>(r.key));
                if (area) gate = area->gate();
                found = true;
                break;
            }
            if (!found) {
                ++out.unplayed;
                out.unplayed_names.push_back(d->row.name);
                continue;
            }
            ++(gate == G::Validity ? out.validity : gate == G::Volumes ? out.volumes : out.any);
            (gate == G::Validity ? out.validity_names : gate == G::Volumes ? out.volume_names : out.any_names).push_back(d->row.name);
        }
    }
    return out;
}

bool CombatWorld::plan_cast(Npc& n, const abilities::AbilityDef& def, CastPlan& out) {
    out = CastPlan{};
    if (!animates(n, def)) return false;
    // the montage: the first of the ability's whose selection expression holds (a row with none always does; the last one stays when none holds)
    const content::MontageRow* chosen = nullptr;
    std::string err;
    std::unique_ptr<stats::CharacterStats::Evaluation> evaluation;
    SelectHost host(rng_);
    for (const content::MontageRow& m : def.montages) {
        if (m.soft_path.empty()) continue;
        chosen = &m;
        if (m.select_expr == 0) break;
        const content::ExprRow* row = catalog_.expr(m.select_expr);
        if (!row || row->program < 0) continue;
        if (!evaluation) evaluation = n.stats().evaluation(n.worn(), err, true);
        double v = 0;
        std::string e;
        if (evaluation && evaluation->eval(row->program, host, v, e) && v != 0.0) break;
    }
    if (!chosen) return false;
    const MontageData* montage = anim_->montage(chosen->soft_path);
    if (!montage || montage->length <= 0 || montage->rate_scale <= 0) return false;
    // every hit notify fires the hit record of the ability that carries the notify's key
    for (const MontageData::Hit& h : montage->hits) {
        for (const content::AbilityHitRow& r : def.hits) {
            if (r.source != "hit" || r.key != h.key) continue;
            out.hits.push_back({h.time / montage->rate_scale, r.hit_guid, static_cast<int>(r.key)});
            break;
        }
    }
    if (out.hits.empty()) return false;
    out.montage = montage;
    out.seconds = montage->seconds(1.0);
    return true;
}

void CombatWorld::start_attack(Npc& n, uint64_t target, double now, const Attack* picked) {
    PlayerPort* port = player(target);
    // An attack with its own animation.  The tree picks it with the target where it stands; its montage starts on the client with the element the replicator writes; the creature turns
    // to the target, and when the tree says so stands still for the whole montage and keeps that facing; every hit lands at its notify, by the hit's targeting record
    // (on the target wherever it stands unless the record measures, and on everyone inside its volumes); the next attack comes when the montage is over.
    Attack a = n.next;
    if (n.anim && port) {
        a = picked ? *picked : choose_animated(n, &port->fighter());
        if (!a.ability) return;   // nothing the tree allows with the target where it stands: the creature tries again at its next look
    }
    CastPlan plan;
    bool animated = false;
    Vec3 at;
    if (a.ability && n.anim && port && plan_cast(n, *a.ability, plan)) {
        const auto montage = bootstrap::montage_for(plan.montage->path, plan.montage->guid, plan.montage->package_guid);
        if (montage) {
            NpcCast cast;
            cast.ability = a.ability->row.guid;
            cast.target = target;
            at = port->fighter().position();
            cast.tx = at.x;
            cast.ty = at.y;
            cast.tz = at.z;
            cast.montage = plan.montage->guid;
            // The creature stops where it is for the attack (the chase it was in ends with the state of the attack) and keeps the facing it has: a task that stands for its montage and does not turn
            // the creature during it began facing its target within a few degrees (think waits for that), and a task that does turn it does so from here, at the creature's rate, below.
            if (a.task && npcs_world_) npcs_world_->halt(n.actor_id);
            animated = start_npc_cast(registry_, n.actor_id, n.cast, cast, montage->chain);
            if (animated && !plan.montage->root_motion.empty()) {
                // The montage carries the creature (a lunge) and the client moves its copy along it by itself, so the world moves the creature the same way at its own rate and says where it
                // is.  A body in the way (a player's capsule) stops it; the AI brings it back into range afterwards when the lunge left it out of range.
                bool carried = false;
                if (npcs_world_) {
                    npc::NpcWorld::RootMotionPlay play;
                    play.montage = plan.montage->guid;
                    play.length = plan.montage->length;
                    play.rate = plan.montage->rate_scale;
                    for (const MontageData::Step& s : plan.montage->root_motion) play.steps.push_back({s.time, s.x, s.y});
                    const double own_radius = n.radius_cm();
                    play.own_radius = own_radius;
                    if (const RootMotionFreezeRule* rule = anim_ ? anim_->freeze_of(a.ability->row.guid) : nullptr) {
                        play.freeze_stop = rule->kind == RootMotionFreezeRule::Kind::Stop;
                        play.freeze_pause = rule->kind == RootMotionFreezeRule::Kind::Pause;
                        play.freeze_distance = rule->distance;
                        play.freeze_scale = rule->radius_scale;
                        play.ignore_non_target = rule->ignore_non_target;
                    }
                    play.blockers = [this, own_radius, target](std::vector<npc::NpcWorld::RootMotionBlock>& out) {
                        for (PlayerPort* p : players_) {
                            if (!p->fighter().alive()) continue;
                            const Vec3 at = p->fighter().position();
                            out.push_back({at.x, at.y, own_radius + p->fighter().radius_cm(), p->fighter().radius_cm(), p->actor_id() == target});
                        }
                    };
                    carried = npcs_world_->begin_root_motion(n.actor_id, std::move(play));
                }
                if (!carried) {   // nothing can move it along the montage, so the client's copy would slide away from it: it plays no montage
                    end_npc_cast(registry_, n.actor_id, n.cast);
                    animated = false;
                }
            }
        }
    }
    if (animated) {
        if (a.task && !a.task->task.empty() && a.task->cooldown > 0) n.cooldown_until[a.task->task] = now + a.task->cooldown;
        if (a.task) {
            for (const auto& [name, seconds] : a.task->starts) n.cooldown_until[name] = now + seconds;
        }
        if (a.task) n.hold_until = now + plan.seconds;   // (no chase runs in the state of an attack: the creature stands, and turns, until the montage is over)
        n.next_attack_at = now + plan.seconds;
        if (a.task && a.task->rotates && npcs_world_) {   // the tree turns it towards its target while the ability plays (the Rotate To task that runs with it)
            const uint64_t who = target;
            const double until = now + (a.task->rotate_seconds > 0 ? std::min(a.task->rotate_seconds, plan.seconds) : plan.seconds);
            npcs_world_->track_target(
                n.actor_id,
                [this, who](nav::Vec3& out) {
                    PlayerPort* p = player(who);
                    if (!p || !p->fighter().alive()) return false;
                    out = to_nav(p->fighter().position());
                    return true;
                },
                a.task->rotate_speed, until);
        }
    } else {
        const double interval = (n.kit && n.kit->interval_ms > 0 ? n.kit->interval_ms / 1000.0 : cfg_.default_attack_interval);
        n.next_attack_at = now + interval * (1.0 + cfg_.attack_variance * (2.0 * rng_.rand_double() - 1.0));
    }
    n.next = choose_attack(n);
    if (!a.ability) {
        ++stats_.refused_no_attack;
        return;
    }
    Pending p;
    p.npc = n.actor_id;
    p.target = target;
    p.ability = a.ability;
    if (animated) {
        if (npc_casts_logged_ < kLoggedNpcCasts) {
            ++npc_casts_logged_;
            p.logged = true;
            spdlog::info("[combat] {} ({}) plays {}: {} hit(s) over {:.2f} s{}", n.actor_id, n.leaf, a.ability->row.name, plan.hits.size(), plan.seconds,
                         a.task && a.task->stop_movement ? ", standing still" : "");
        }
        p.animated = true;
        for (const PlannedHit& h : plan.hits) {
            p.timed_hits.emplace_back(now + std::max(h.delay, kMinHitDelay), h.hit);
            const BiteArea* area = anim_ ? anim_->area_of(a.ability->row.guid, h.key) : nullptr;
            p.timed_areas.push_back(area ? *area : BiteArea{});   // (each hit lands by its own record, when it falls due)
        }
        for (size_t i = 1; i < p.timed_hits.size(); ++i) {   // the notifies are in time order; the lower bound keeps them so
            p.timed_hits[i].first = std::max(p.timed_hits[i].first, p.timed_hits[i - 1].first);
        }
        p.end_at = now + plan.seconds;
        p.due = p.timed_hits.empty() ? p.end_at : p.timed_hits.front().first;
    } else {
        p.due = now + a.windup_ms / 1000.0;
        std::set<int64_t> seen;
        for (const content::AbilityHitRow& h : a.ability->hits) {
            if ((h.source == "hit" || h.source == "projectile") && seen.insert(h.hit_guid).second) p.hits.push_back(h.hit_guid);
        }
    }
    pending_.push_back(std::move(p));
    ++stats_.npc_attacks;
}

void CombatWorld::deliver_hit(Npc& n, PlayerPort& port, const abilities::AbilityDef& ability, int64_t guid, double now) {
    const abilities::HitDef* hit = catalog_.hit(guid);
    if (!hit || (hit->row.applicable_targets & 2) == 0 || hit->mods.empty()) return;
    HitMeta meta;
    meta.source_type = content::kSourceTypeAbility;
    meta.source_guid = ability.row.guid;
    meta.ability_tags = ability.row.tags;
    Resolved r;
    std::string why;
    if (!resolver_.resolve(n, port.fighter(), *hit, meta, r, why)) {
        spdlog::debug("[combat] {} ({}): {}", n.actor_id, n.leaf, why);
        return;
    }
    double raw_health = 0, health = 0;   // the Health this hit takes: as the data gives it, and after the damage setting
    for (ResolvedMod& m : r.mods) {
        if (m.stat_guid != data_.ids().health || !(m.value < 0) || !std::isfinite(m.value)) continue;
        raw_health += m.value;
        m.value *= cfg_.npc_damage_mult;
        health += m.value;
    }
    if (!r.miss() && npc_hits_logged_ < kLoggedNpcHits) {
        // what the hit was made of: the two levels and the target's mitigation (its gear and level), so a fight can be read from the log
        ++npc_hits_logged_;
        const stats::CharacterStats& target_stats = port.fighter().stats();
        const stats::StatValue* physical = target_stats.find(data_.profile().physical.mitigation);
        const stats::StatValue* magical = target_stats.find(data_.profile().magical.mitigation);
        spdlog::info("[combat] {} (level {}) hit the player (level {}): Health {:.2f}, {:.2f} before the damage setting; the player's PhysicalMitigation {:.4f}, MagicalMitigation {:.4f}{}", n.leaf,
                     n.lvl, port.fighter().level(), health, raw_health, physical ? physical->value : 0.0, magical ? magical->value : 0.0,
                     (r.flags & stats::kHitCrit) ? ", a critical hit" : "");
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
    port.receive_hit(in, now);
    player_exchange(port.actor_id(), now);
}

void CombatWorld::resolve_pending(double now) {
    std::vector<Pending> keep;   // attacks with their own animation that still have a hit or their end to come
    for (size_t i = 0; i < pending_.size();) {
        Pending p = pending_[i];
        if (p.animated) {
            const auto it = npcs_.find(p.npc);
            if (it == npcs_.end() || it->second->dead) {   // (a death or a reset ended the animation already)
                pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            if (p.due > now) {
                ++i;
                continue;
            }
            pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
            Npc& n = *it->second;
            PlayerPort* port = player(p.target);
            while (p.next_hit < p.timed_hits.size() && p.timed_hits[p.next_hit].first <= now) {
                const size_t at_hit = p.next_hit++;
                const int64_t guid = p.timed_hits[at_hit].second;
                const BiteArea& area = p.timed_areas[at_hit];
                // a hit lands by its targeting record when its notify falls due (where the creature is by then, facing as it faces): on the player the record's target part finds, wherever he stands
                // unless the record measures, and on every player inside one of its volumes; a hit that reaches nobody sends nothing
                const uint64_t aimed = area.has_target_part() ? aimed_at(n, area, p.target) : 0;
                const std::vector<PlayerPort*> candidates = players_;
                for (PlayerPort* q : candidates) {
                    if (!q->fighter().alive() || !hit_reaches(n, q->fighter(), area, aimed != 0 && q->actor_id() == aimed)) continue;
                    deliver_hit(n, *q, *p.ability, guid, now);
                }
            }
            if (p.next_hit >= p.timed_hits.size() && now >= p.end_at) {
                if (p.logged && port) {   // where the lunge left the creature, as the server has it (what the client draws is for the client's log)
                    const world::WorldActor* w = registry_.find(n.actor_id);
                    const Vec3 at = port->fighter().position();
                    if (w) {
                        spdlog::info("[combat] {} ({}) {} is over: {:.0f} cm between its centre and the player's, the player {:.0f} deg off its facing", n.actor_id, n.leaf, p.ability->row.name,
                                     std::hypot(at.x - w->x, at.y - w->y), angle_between_degrees(std::atan2(at.y - w->y, at.x - w->x) * 180.0 / 3.14159265358979323846, w->yaw_degrees));
                    }
                }
                stop_cast(n);
                continue;
            }
            p.due = p.next_hit < p.timed_hits.size() ? std::min(p.timed_hits[p.next_hit].first, p.end_at) : p.end_at;
            keep.push_back(std::move(p));
            continue;
        }
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
        for (const int64_t guid : p.hits) deliver_hit(n, *port, *p.ability, guid, now);
    }
    for (Pending& p : keep) pending_.push_back(std::move(p));
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

void CombatWorld::kill(Npc& n, uint64_t killer, double now, bool gm) {
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
    stop_cast(n);   // the animation of an attack under way stops with the creature
    n.hold_until = 0;
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
        e.gm = gm;
        death_hook_(e);
    }
}

// ---- a game master's hands on an NPC ------------------------------------------------------------------------------------------------

bool CombatWorld::gm_kill(uint64_t actor_id, double now, std::string& why) {
    Npc* n = combatant(actor_id);
    if (!n) {
        why = "worldd has no combat stats for this creature";
        return false;
    }
    if (n->dead) {
        why = "it is dead already";
        return false;
    }
    kill(*n, 0, now, true);
    return true;
}

bool CombatWorld::gm_heal(uint64_t actor_id) {
    const auto it = npcs_.find(actor_id);
    if (it == npcs_.end()) return true;   // it has never been in a fight: its health is full
    Npc& n = *it->second;
    if (n.dead) return false;
    if (const stats::StatValue* max = n.cs->find(data_.ids().max_health)) {
        n.cs->set_current(content::kGuidHealth, max->value);
        write_stat(registry_, actor_id, content::kGuidHealth, max->value);
    }
    return true;
}

CombatWorld::GmReset CombatWorld::gm_reset(uint64_t actor_id, double now) {
    const auto it = npcs_.find(actor_id);
    if (it == npcs_.end()) return GmReset::Standing;   // it has never been in a fight: no threat to clear, health full
    Npc& n = *it->second;
    if (n.dead) return GmReset::Dead;
    n.ai.reset_now();
    begin_reset(n, now);   // healed, its flags down, an attack under way stopped, and sent home
    const npc::Mover* m = npcs_world_ ? npcs_world_->mover(actor_id) : nullptr;
    if (m && m->moving()) return GmReset::Walking;
    // no way home on foot (or none to walk): it is home as far as the fight goes; the caller stands it at its spawn point
    n.ai.revive();
    n.returning = false;
    if (npcs_world_) npcs_world_->release(actor_id);
    return GmReset::Standing;
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
    stop_cast(n);   // ... and with a fight that is reset
    n.hold_until = 0;
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const Pending& p) { return p.npc == n.actor_id; }), pending_.end());
    set_flags(n, false, 0);
    if (npcs_world_) npcs_world_->order_return_home(n.actor_id);
}

void CombatWorld::pursue(Npc& n, uint64_t target, double speed_cm_s, double stop_distance_cm) {
    if (!npcs_world_) return;
    npcs_world_->order_pursue(
        n.actor_id,
        [this, target](nav::Vec3& out) {
            PlayerPort* p = player(target);
            if (!p || !p->fighter().alive()) return false;
            out = to_nav(p->fighter().position());
            return true;
        },
        speed_cm_s, stop_distance_cm);
}

bool CombatWorld::faces(const Npc& n, const Vec3& at, double tolerance_degrees) const {
    const world::WorldActor* w = registry_.find(n.actor_id);
    if (!w) return true;
    return angle_between_degrees(std::atan2(at.y - w->y, at.x - w->x) * 180.0 / 3.14159265358979323846, w->yaw_degrees) <= tolerance_degrees;
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
    if (o.began_fight) {
        ++stats_.aggros;
        n.fight_began = now;
    }
    if (o.began_reset) begin_reset(n, now);
    if (o.reset_done) {
        n.returning = false;
        if (npcs_world_) npcs_world_->release(n.actor_id);
    }
    if (o.in_combat) set_flags(n, true, o.target);
    else if (n.ai.state() == npc::ai::State::Idle || n.ai.state() == npc::ai::State::Reset) set_flags(n, false, 0);
    // A montage that carries the creature (a lunge) is played to the end of its carry: the client moves its copy along the whole montage, so a walk order that took the creature's feet back
    // half way would leave the server's creature behind it.  The AI's walking and turning wait until the carry is over; a reset (the way home) and a death end it.
    const bool carried = npcs_world_ && npcs_world_->root_motion_active(n.actor_id);
    if (npcs_world_ && !o.began_reset && now >= n.hold_until && !carried) {   // (a creature that stands for a montage is not sent anywhere until it is over)
        const nav::Vec3 here{a->x, a->y, a->z};
        switch (o.move) {
            case npc::ai::Orders::Move::ChaseTo: pursue(n, o.target, o.speed_cm_s, o.stop_distance_cm); break;
            case npc::ai::Orders::Move::Stand:
                if (before != npc::ai::State::Attack) npcs_world_->order_move(n.actor_id, here, 0.0, 0.0);
                break;
            default: break;
        }
    }
    if (o.attack && now >= n.next_attack_at) {
        // A creature with its own animation starts an attack only where its tree allows one, every time: from where it stands now, facing the way it faces now.  Where the tree allows none it
        // does not cast: it turns towards the target when only its facing is wrong (it turns at its rate while it stands, and is never stopped to turn), and walks up to it when it is too far, and
        // looks again at its next look.
        bool allowed = true;
        Attack picked;
        bool have_pick = false;
        if (n.anim && npcs_world_ && now >= n.hold_until) {
            PlayerPort* port = player(o.target);
            TreeWhy why;
            if (port) {
                picked = choose_animated(n, &port->fighter(), &why);
                have_pick = picked.ability != nullptr;
            }
            if (port && !have_pick) {
                allowed = false;
                const Vec3 at = port->fighter().position();
                if (why.off_angle) {
                    npcs_world_->turn_toward(n.actor_id, at.x, at.y);
                } else if (why.too_far) {
                    const double stop = std::max(0.0, n.next.range_cm * n.ai.params().chase_stop_fraction) + n.radius_cm() + port->fighter().radius_cm();
                    pursue(n, o.target, n.ai.params().chase_speed_cm_s, stop);
                }
            } else if (port && have_pick && picked.task && picked.task->stop_movement && !picked.task->rotates && !faces(n, port->fighter().position(), 2.0)) {
                // a task that stands for its montage and does not turn the creature during it begins with the creature facing its target: it turns first, at its rate, and begins at a later look
                allowed = false;
                const Vec3 at = port->fighter().position();
                npcs_world_->turn_toward(n.actor_id, at.x, at.y);
            }
        }
        if (allowed) start_attack(n, o.target, now, have_pick ? &picked : nullptr);
    }
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
