// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/alert_effects.inc
//
//  Where an alert's hits land: the hit's mask says Self, Ally, Party or Raid, and its targeting record says how far (a sphere, or a box, around the creature) and whether the line of sight counts.
//  A Self bit reaches the caster (Party names it too, as the player's own hits do); an Ally bit reaches the living creatures on the caster's friendly side, by the same relation the player's hits
//  use for the friendly side of a mask, inside the record, nearest first, up to the hit's cap.  Each of them gets the hit's effects through HitResolver::resolve_effects and
//  CombatWorld::land_effect, the code a player's hits put their effects on a creature with.  (included by combat_world.cpp)
// ============================================================================

namespace {

/// The most creatures one landing line names; the others are counted.
constexpr size_t kAlertLandingNames = 8;

/// How far (cm) a record's volumes reach from the creature's centre, its own body not counted.
double alert_area_reach(const BiteArea& area) {
    double reach = 0;
    for (const HitVolume& v : area.volumes) {
        const double at = std::hypot(v.ox, v.oy);
        reach = std::max(reach, at + (v.shape == HitVolume::Shape::Sphere ? v.radius : std::hypot(v.hx, v.hy)));
    }
    return reach;
}

}  // namespace

void CombatWorld::refresh_creature_stats(Npc& n) {
    if (!n.cs) return;
    std::vector<stats::StatValue> changed;
    stats::CharacterStats::Next next;
    std::string err;
    if (!n.cs->prepare(n.worn(), changed, next, err)) return;
    n.cs->commit(std::move(next));
    write_stats(registry_, n.actor_id, *n.cs);
    // the walk follows the speed stat while a buff lasts and comes back when it ends (make_combatant sets it once at spawn from the same rule)
    if (runs_at_stat_speed(n.archetype)) {
        const double run = std::max({n.bp.max_run_speed, n.bp.max_walk_speed, 300.0});
        double chase = run;
        if (const stats::StatValue* mult = n.cs->find(data_.ids().move_speed_mult); mult && mult->value > 1.0) chase = run * mult->value;
        npc::ai::Params p = n.ai.params();
        if (p.chase_speed_cm_s != chase) {
            p.chase_speed_cm_s = chase;
            n.ai.set_params(p);
        }
    }
}

void CombatWorld::land_alert_hit(Npc& caster, const abilities::AbilityDef& ability, const BiteArea& area, int64_t hit_guid, double now, std::set<std::pair<int64_t, uint64_t>>& logged) {
    const abilities::HitDef* hit = catalog_.hit(hit_guid);
    if (!hit) return;
    const int mask = hit->row.applicable_targets;
    std::vector<Npc*> targets;
    if ((mask & (1 | 8)) != 0) targets.push_back(&caster);   // Self (Party names the caster too, as the player's own hits do)
    // the hit's own cap: 40 when it names none, as a player's hit is capped
    double max_targets = 40;
    if (hit->row.max_targets_expr != 0) {
        const content::ExprRow* row = catalog_.expr(hit->row.max_targets_expr);
        std::string err;
        auto evaluation = caster.stats().evaluation(caster.worn(), err, true);
        SelectHost host(rng_);
        double v = 0;
        if (row && row->program >= 0 && evaluation && evaluation->eval(row->program, host, v, err) && std::isfinite(v) && v >= 1) max_targets = std::min(max_targets, v);
    }
    if ((mask & (4 | 8 | 16)) != 0) {
        const world::WorldActor* me = registry_.find(caster.actor_id);
        std::vector<std::pair<double, Npc*>> allies;
        if (me) {
            std::vector<uint64_t> ids;
            registry_.query(me->x, me->y, alert_area_reach(area) + 1000.0, ids);
            for (const uint64_t id : ids) {
                if (id == caster.actor_id) continue;
                const world::WorldActor* a = registry_.find(id);
                if (!a || a->kind != world::ActorKind::Npc) continue;
                // the friendly side is the relation, as the player's own hits ask it (a GoblinDog, a wolf or an otter beside the goblins is on their side)
                const npc::MotionStats other = motion_.get(a->npc.archetype);
                if (data_.relation(caster.aligns, other.alignments) == Relation::Hostile) continue;
                Npc* n = combatant(id);
                if (!n || n->dead) continue;
                if (!hit_reaches(caster, *n, area, false)) continue;
                allies.emplace_back(distance_xy_cm(caster.position(), n->position()), n);
            }
        }
        std::sort(allies.begin(), allies.end(), [](const std::pair<double, Npc*>& a, const std::pair<double, Npc*>& b) {
            return a.first != b.first ? a.first < b.first : a.second->actor_id < b.second->actor_id;
        });
        for (const auto& ally : allies) {
            if (static_cast<double>(targets.size()) >= max_targets) break;
            targets.push_back(ally.second);
        }
    }
    // what this pulse put on creatures the cast has not named yet, by effect, for one line each: a camp whose casters all rally cannot write a line for every creature of every cast
    struct Landed {
        const abilities::EffectDef* effect = nullptr;
        std::string                 names;
        size_t                      count = 0;
    };
    std::vector<Landed> landed;
    for (Npc* target : targets) {
        if (!target || target->dead) continue;
        HitMeta meta;
        meta.source_type = content::kSourceTypeAbility;
        meta.source_guid = ability.row.guid;
        meta.ability_tags = ability.row.tags;
        meta.targets = static_cast<double>(targets.size());
        if (!resolver_.requirement_holds(caster, *target, *hit, meta)) continue;
        const BookView view(target->book, now);
        meta.target_effects = &view;
        Resolved gate;
        std::string why;
        if (!resolver_.resolve_effects(caster, *target, *hit, meta, gate, why)) {
            spdlog::debug("[combat] {} ({}): the effects of {} on {}: {}", caster.leaf, caster.actor_id, ability.row.name, target->leaf, why);
            continue;
        }
        for (effects::Applied a : gate.effects) {
            if (target->dead) break;
            a.by_creature = true;
            const double shield = resolver_.effect_shield(caster, *target, *a.def);
            if (shield > 0) add_shield(target->actor_id, shield, std::isinf(a.duration) ? 0.0 : a.duration, now, a.def->row.guid, a.by_creature);
            land_effect(*target, a, caster.actor_id, now);
            // what the effect landed on, once for each creature of the cast however many of the montage's pulses put it there
            if (a.def && target->book.stacks_of(a.def->row.guid) > 0 && logged.insert({a.def->row.guid, target->actor_id}).second) {
                const auto line = std::find_if(landed.begin(), landed.end(), [&](const Landed& l) { return l.effect == a.def; });
                Landed& entry = line != landed.end() ? *line : landed.emplace_back(Landed{a.def, {}, 0});
                if (entry.count < kAlertLandingNames) entry.names += fmt::format("{}{} ({})", entry.count == 0 ? "" : ", ", target->actor_id, target->leaf);
                ++entry.count;
            }
        }
    }
    // (the cast's own line names the move; these say where its effects went)
    for (const Landed& l : landed) {
        spdlog::info("[combat] alert {} of caster {} ({}): {} ({}) lands on {}: {}{}", ability.row.name, caster.actor_id, caster.leaf, l.effect->row.name, l.effect->row.guid, l.count, l.names,
                     l.count > kAlertLandingNames ? fmt::format(" and {} more", l.count - kAlertLandingNames) : std::string());
    }
}
