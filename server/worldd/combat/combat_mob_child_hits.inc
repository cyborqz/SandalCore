// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/combat_mob_child_hits.inc  (the lane's milestone 1)
//
//  A creature's hit that carries its harm in the hits it fires when it lands (ab_hit_recursive), the effects those hits put on a player, and the lingering zones they open.
//  The hit lands where the player it reached stands: its child hits then reach the players their own targeting records find around that place (a 24 cm ground trace the player
//  it landed on, a 400 cm explosion the players inside it), put their stat mods on them through IncomingHit and their effects on them through the session's effect state, and
//  open the zones they list there.  The world runs the effects' hits and the zones' hits from the creature (the session's runtime would run them from the player's own stats):
//  each effect ticks its data's Tick hit at its tick rate, a zone its Lingering hits each tick, with its Initial, overlap and End hits where the data puts them.
//  A child, an effect's tick and a zone's tick keep the confirmation shapes the game gives them (a child names the target it was fired on as its Source, an effect's tick its
//  holder, neither is predicted; a zone's tick has no Source actor), which the session reads on IncomingHit.
//  What a creature's OWN hit puts on a player stays what it was (its damage): the debuffs of a hit come only through a child, a zone or the tick of an effect they put on.  A child,
//  a zone's hit and an effect's tick ask their own requirement first, as the hits of the player's side do.
//  Included by combat_world.cpp (its own file: combat_world.cpp stays in its own functions).
// ============================================================================

namespace {

/// The most zones and effect entries of creatures the world runs at once: a bound on the memory of a fight, high enough for a camp of casters against a group.
constexpr size_t kMaxCreatureZones = 128;
constexpr size_t kMaxCreatureEffects = 512;

/// Whether a player's body stands within `radius_cm` of `centre`: the body's own radius counts, as the game's hit volumes meet a character's capsule.
bool body_in_sphere(const Fighter& body, const Vec3& centre, double radius_cm) {
    return distance_cm(body.position(), centre) <= radius_cm + body.radius_cm();
}

}  // namespace

protocol::emit::FIntrepidNetworkGUID CombatWorld::actor_guid(uint64_t actor_id) {
    // another player's pawn is in the players' list, not the registry; a pet stands in both and keeps its actor's guid
    if (PlayerPort* p = player(actor_id); p && !is_pet(actor_id)) return p->pawn_guid();
    const world::WorldActor* a = registry_.find(actor_id);
    return a ? a->actor.guid : protocol::emit::FIntrepidNetworkGUID{};
}

void CombatWorld::deliver_hit(Npc& n, PlayerPort& port, const abilities::AbilityDef& ability, int64_t guid, double now, int64_t projectile) {
    HitLanding landing;
    landing.at = port.fighter().position();
    landing.projectile = projectile;
    deliver_hit_at(n, port, ability, guid, now, landing);
}

void CombatWorld::deliver_hit_at(Npc& n, PlayerPort& port, const abilities::AbilityDef& ability, int64_t guid, double now, const HitLanding& from) {
    const abilities::HitDef* hit = catalog_.hit(guid);
    if (!hit) return;
    // a hit lands on a player only when it reaches players: a self or an ally hit (the mud pool a bog brute spawns with a hit on itself) reaches none, so what it fires and what it opens
    // are not placed at the player the move happened to reach
    const int mask = hit->row.applicable_targets;
    if (mask != 0 && (mask & 2) == 0) return;
    deliver_hit_effects(n, port, ability, *hit, now, from);
    // the record the hit names as its source: the projectile that carried it, else the ability (a child keeps the record of the hit that fired it)
    const int64_t source_type = from.source_type != 0 ? from.source_type : (from.projectile != 0 ? content::kSourceTypeProjectile : content::kSourceTypeAbility);
    const int64_t source_guid = from.source_guid != 0 ? from.source_guid : (from.projectile != 0 ? from.projectile : ability.row.guid);
    // the hits it fires when it lands, where it landed, on the players their own targeting records reach
    if (from.depth < 4) land_children_of(n, ability, *hit, from.at, port.actor_id(), now, from.depth, source_type, source_guid);
    // the zones it opens stand where it landed, whether or not a player was in it
    open_hit_zones(n, ability, *hit, from.at, now);
}

void CombatWorld::deliver_hit_effects(Npc& n, PlayerPort& port, const abilities::AbilityDef& ability, const abilities::HitDef& hit, double now, const HitLanding& landing) {
    const int mask = hit.row.applicable_targets;
    if (mask != 0 && (mask & 2) == 0) return;   // (a player is reached by the enemies' hits and by the ones with no side)
    // a creature's own hit lands its damage, and the hits it fires and the zones it opens, as it did; what a hit puts on a player (its debuffs) comes through a child, a zone or an effect's tick
    const bool own = landing.form == kHitOwn;
    // a hit with nothing to pass on is an empty marker; one that only throws its target (the Mycelions' impact knock) still reaches it, so its push reaches the player's side.  An impulse that
    // does not apply is a throw only when the hit carries an effect as well (the knock's): a hit with no applying impulse and no effect (the Mycelions' launch hit scan, a marker the game only
    // uses to feel for a body) lands nothing
    const bool throws = hit.impulse && (hit.impulse->applies || !hit.effects.empty());
    if (hit.mods.empty() && (own || hit.effects.empty()) && hit.recursive.empty() && hit.zones.empty() && !throws && !hit.teleport) return;
    HitMeta meta;
    // a hit a projectile carries names the projectile's record as its source, as the game confirms it; a child keeps the record of the hit that fired it
    meta.source_type = landing.source_type != 0 ? landing.source_type : (landing.projectile != 0 ? content::kSourceTypeProjectile : content::kSourceTypeAbility);
    meta.source_guid = landing.source_guid != 0 ? landing.source_guid : (landing.projectile != 0 ? landing.projectile : ability.row.guid);
    meta.ability_tags = ability.row.tags;
    meta.instigator_actor = n.actor_id;
    meta.target_actor = port.actor_id();
    Resolved r;
    std::string why;
    if (!resolver_.resolve(n, port.fighter(), hit, meta, r, why)) {
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
        spdlog::info("[combat] {} (level {}) hit the player (level {}): Health {:.2f}, {:.2f} before the damage setting; the player's PhysicalMitigation {:.4f}, MagicalMitigation {:.4f}{}", log_name(n.actor_id, n.archetype, n.leaf),
                     n.lvl, port.fighter().level(), health, raw_health, physical ? physical->value : 0.0, magical ? magical->value : 0.0,
                     (r.flags & stats::kHitCrit) ? ", a critical hit" : "");
    }
    IncomingHit in;
    in.instigator = n.actor_id;
    in.hit_guid = hit.row.guid;
    in.source_type = meta.source_type;
    in.source_guid = meta.source_guid;
    in.flags = r.flags;
    in.mods = r.mods;
    in.at = now;
    if (landing.form != kHitOwn) {
        // the shapes the game gives the hits that are not the creature's own: a child and a zone's tick are predicted on neither side, an effect's tick on both; a child and an
        // effect's tick name an actor as their Source, a zone's tick has none
        in.shape_known = true;
        in.source_is_actor = false;
        in.predicted_autonomous = in.predicted_simulated = landing.form == kHitEffect;
        in.source_actor = landing.form == kHitZone ? 0 : landing.source_actor;
    }
    if (!own && !r.miss() && !hit.effects.empty()) {
        Resolved gate;
        std::string ewhy;
        // the creature works the effects out as their caster: the values and durations are its own (the target as it stands now)
        if (resolver_.resolve_effects(n, port.fighter(), hit, meta, gate, ewhy)) {
            in.effects = gate.effects;
        } else {
            spdlog::debug("[combat] {} ({}) puts no effect of {} on the player: {}", n.actor_id, n.leaf, hit.row.name, ewhy);
        }
    }
    r.miss() ? ++stats_.npc_misses : ++stats_.npc_hits;
    port.receive_hit(in, now);
    player_exchange(port.actor_id(), now);
    if (!r.miss()) {
        for (const effects::Applied& applied : in.effects) schedule_creature_effect(n, applied, port.actor_id(), now);
    }
    // the creature's own hit raises what its effects react to (the Blood Fury drains health from a bleeding target): the hit's numbers and its target are what the triggers read
    if (!n.book.empty()) {
        HitOrigin origin;
        origin.landed_here = true;
        origin.hit = &hit;
        origin.source_type = meta.source_type;
        origin.source_guid = meta.source_guid;
        origin.ability_tags = meta.ability_tags;
        HolderEvent ev;
        ev.attacker = port.actor_id();
        ev.target = port.actor_id();
        ev.origin = &origin;
        ev.flags = r.flags;
        for (size_t i = 0; i < r.mods.size(); ++i) {
            ev.mods[r.mods[i].stat_guid] += r.mods[i].value;
            ev.effective[r.mods[i].stat_guid] += r.mods[i].value;
        }
        for (const effects::Instance& e : n.book.instances()) ev.held.emplace_back(e.def, e.instigator);
        holder_reacts(n.actor_id, ev, now, 4);
    }
}

bool CombatWorld::hit_requirement_holds(Npc& n, PlayerPort& port, const abilities::AbilityDef* ability, const abilities::HitDef& hit, int64_t source_type, int64_t source_guid, bool target_is_active) {
    if (hit.row.requirement_expr == 0) return true;
    HitMeta meta;
    meta.source_type = source_type;
    meta.source_guid = source_guid;
    meta.instigator_actor = n.actor_id;
    meta.target_actor = port.actor_id();
    if (ability) meta.ability_tags = ability->row.tags;
    meta.target_is_active = target_is_active;   // (GetTarget() against GetActiveTarget(GetInstigator()): the creature's own target is the one the hit that fired the child landed on)
    meta.active_in_hit = true;
    return resolver_.requirement_holds(n, port.fighter(), hit, meta);
}

void CombatWorld::land_children_of(Npc& n, const abilities::AbilityDef& ability, const abilities::HitDef& hit, const Vec3& at, uint64_t parent_target, double now, int depth, int64_t source_type,
                                   int64_t source_guid) {
    if (depth > 4) return;
    for (const content::RecursiveHitRow& row : hit.recursive) {
        const abilities::HitDef* child = catalog_.hit(row.child_hit);
        if (!child) continue;
        const int mask = child->row.applicable_targets;
        if (mask != 0 && (mask & 2) == 0) continue;   // (a self or ally child is not a player's: the world has no creature-side landing for those yet)
        if (child->row.delay_expr != 0) continue;     // (a child that waits for its own time would land at the wrong moment: the world has no delayed landing for a creature yet)
        HitLanding landing;
        landing.at = at;
        landing.source_type = source_type;
        landing.source_guid = source_guid;
        landing.source_actor = parent_target;   // (a child names the target its parent landed on as its Source, as the recorded child does)
        landing.form = kHitChild;
        landing.depth = depth;
        std::vector<PlayerPort*> reached;
        if (row.radius_cm > 0) {
            for (PlayerPort* p : players_) {
                if (p->fighter().alive() && body_in_sphere(p->fighter(), at, row.radius_cm)) reached.push_back(p);
            }
        } else if (PlayerPort* p = player(parent_target); p && p->fighter().alive()) {
            reached.push_back(p);
        }
        // the child's own requirement, asked for each player it reached (a quarter chance, a target that is not frozen, a target that is not the creature's own): it lands on the ones it
        // holds for, and a child that holds for nobody lands nothing and fires nothing
        if (child->row.requirement_expr != 0) {
            std::vector<PlayerPort*> holding;
            for (PlayerPort* p : reached) {
                if (hit_requirement_holds(n, *p, &ability, *child, source_type, source_guid, p->actor_id() == parent_target)) holding.push_back(p);
            }
            if (holding.empty()) continue;
            reached = std::move(holding);
        }
        for (PlayerPort* p : reached) deliver_hit_effects(n, *p, ability, *child, now, landing);
        // the child's own children and zones fire once for the hit it was (not once for each player its volume reached)
        land_children_of(n, ability, *child, at, reached.empty() ? parent_target : reached.front()->actor_id(), now, depth + 1, source_type, source_guid);
        open_hit_zones(n, ability, *child, at, now);
    }
}

void CombatWorld::open_hit_zones(Npc& n, const abilities::AbilityDef& ability, const abilities::HitDef& hit, const Vec3& at, double now) {
    for (const int64_t zone_guid : hit.zones) {
        const content::ZoneRow* row = catalog_.zone(zone_guid);
        if (!row) continue;
        if (!row->placeable()) {
            // the zone's own targeting record is not a sphere the tables hold (the extraction of the zone targetings is a reader change, and a box is not placed as a sphere): the zone
            // cannot be placed, so it opens nothing
            spdlog::debug("[combat] {} ({}): the zone {} has no sphere targeting record in the tables: it is not placed", n.actor_id, n.leaf, row->name);
            continue;
        }
        if (creature_zones_.size() >= kMaxCreatureZones) {
            spdlog::warn("[combat] {} ({}): {} zones of creatures are standing: the zone {} is left out", n.actor_id, n.leaf, kMaxCreatureZones, row->name);
            continue;
        }
        CreatureZone zone;
        zone.npc = n.actor_id;
        zone.ability = ability.row.guid;
        zone.zone = zone_guid;
        zone.at = at;
        zone.radius = row->volume.radius;
        zone.open_at = now;
        zone.end_at = now + (row->life_time > 0 ? row->life_time : 30.0);
        zone.tick = std::max(0.25, row->tick_rate);
        zone.next_tick = now + zone.tick;
        for (PlayerPort* p : players_) {
            if (p->fighter().alive() && body_in_sphere(p->fighter(), zone.at, zone.radius)) zone.inside.insert(p->actor_id());
        }
        creature_zones_.push_back(zone);
        const size_t index = creature_zones_.size() - 1;
        spdlog::info("[combat] {} ({}) leaves {} at {:.0f}, {:.0f} for {:.1f} s ({} cm): {} player(s) inside", log_name(n.actor_id, n.archetype, n.leaf), n.actor_id, row->name, zone.at.x, zone.at.y,
                     zone.end_at - zone.open_at, zone.radius, creature_zones_[index].inside.size());
        land_zone_role(n, index, "InitialHitsIds", now);
        land_zone_role(n, index, "OnBeginOverlapHitsIds", now);
    }
}

void CombatWorld::land_zone_role(Npc& n, size_t index, const char* role, double now, uint64_t only) {
    if (index >= creature_zones_.size()) return;
    const uint64_t npc = creature_zones_[index].npc;
    const int64_t ability = creature_zones_[index].ability;
    const int64_t zone_guid = creature_zones_[index].zone;
    // a begin-overlap or end-overlap hit lands on the player who came in or went out; the others (initial, lingering, end) on everyone inside
    const std::vector<uint64_t> inside = only != 0 ? std::vector<uint64_t>{only} : std::vector<uint64_t>(creature_zones_[index].inside.begin(), creature_zones_[index].inside.end());
    const content::ZoneRow* row = catalog_.zone(zone_guid);
    if (!row) return;
    const auto list = row->hits.find(role);
    if (list == row->hits.end()) return;
    for (const int64_t guid : list->second) {
        const abilities::HitDef* hit = catalog_.hit(guid);
        if (!hit) continue;
        if (hit->row.applicable_targets != 0 && (hit->row.applicable_targets & 2) == 0) continue;
        for (const uint64_t player_id : inside) {
            PlayerPort* p = player(player_id);
            if (!p || !p->fighter().alive()) continue;
            Npc* caster = combatant(npc);
            if (!caster) return;
            land_zone_hit(*caster, *p, ability, guid, zone_guid, now);
        }
    }
}

void CombatWorld::land_zone_hit(Npc& n, PlayerPort& port, int64_t ability, int64_t hit_guid, int64_t zone_guid, double now) {
    const abilities::AbilityDef* def = catalog_.ability(ability);
    const abilities::HitDef* hit = catalog_.hit(hit_guid);
    if (!def || !hit) return;
    if (!hit_requirement_holds(n, port, def, *hit, content::kSourceTypeZone, zone_guid, false)) return;
    HitLanding landing;
    landing.at = port.fighter().position();
    landing.source_type = content::kSourceTypeZone;
    landing.source_guid = zone_guid;
    landing.form = kHitZone;
    deliver_hit_at(n, port, *def, hit_guid, now, landing);
}

void CombatWorld::schedule_creature_effect(Npc& n, const effects::Applied& applied, uint64_t player_id, double now) {
    const abilities::EffectDef* def = applied.def;
    if (!def) return;
    const content::EffectRow& row = def->row;
    const bool forever = !std::isfinite(applied.duration);
    const double end = forever ? std::numeric_limits<double>::infinity() : now + std::max(0.0, applied.duration);
    const bool in_place = row.stack_type.find("StackInPlace") != std::string::npos;
    const bool never_refresh = row.stack_duration_refresh.find("NeverRefresh") != std::string::npos;
    const auto schedule = [&](CreatureEffect& e) {
        e.start = now;
        e.end = end;
        e.tick = std::max(0.0, row.tick_timer);
        e.next_tick = e.tick > 0 ? (row.ignore_initial_tick ? now + e.tick : now) : -1.0;
    };
    // the player holds one instance of an effect whoever put it on (its in-place limit, or its override), so the world runs one: the latest application's caster keeps it
    CreatureEffect* found = nullptr;
    for (CreatureEffect& e : creature_effects_) {
        if (e.player == player_id && e.def == def) {
            found = &e;
            break;
        }
    }
    if (!found) {
        if (creature_effects_.size() >= kMaxCreatureEffects) return;
        CreatureEffect e;
        e.npc = n.actor_id;
        e.player = player_id;
        e.def = def;
        schedule(e);
        creature_effects_.push_back(e);
        found = &creature_effects_.back();
    } else if (in_place) {
        // the player's effect state adds the stacks and refreshes the instance; the world's tick keeps its rhythm and its end follows the new application
        found->npc = n.actor_id;
        if (!never_refresh) found->end = end;
    } else {
        found->npc = n.actor_id;
        schedule(*found);
    }
    // the effect's Apply hits run with every application (a creature's effects are run by the world, not by the player's own runtime)
    apply_creature_effect_hits(n, player_id, *def, "Apply", now);
    // (a Tick hit with no rate: its one tick at the application)
    if (def->row.tick_timer <= 0 && !def->row.ignore_initial_tick) apply_creature_effect_hits(n, player_id, *def, "Tick", now);
}

void CombatWorld::apply_creature_effect_hits(Npc& n, uint64_t player_id, const abilities::EffectDef& effect, const char* phase, double now) {
    PlayerPort* port = player(player_id);
    if (!port || !port->fighter().alive()) return;
    for (const auto& [when, hit_guid] : effect.hits) {
        if (when != phase) continue;
        land_creature_effect_hit(n, *port, effect, hit_guid, now);
    }
}

void CombatWorld::land_creature_effect_hit(Npc& n, PlayerPort& port, const abilities::EffectDef& effect, int64_t hit_guid, double now) {
    // the effect's hit names the effect's record as its source and is landed on its holder: the shape the recorded burn tick has
    const abilities::HitDef* hit = catalog_.hit(hit_guid);
    if (!hit) return;
    const int mask = hit->row.applicable_targets;
    if (mask != 0 && (mask & 2) == 0) return;
    if (hit->mods.empty() && hit->effects.empty() && hit->recursive.empty() && hit->zones.empty()) return;
    HitMeta meta;
    meta.source_type = content::kSourceTypeEffect;
    meta.source_guid = effect.row.guid;
    meta.instigator_actor = n.actor_id;
    meta.target_actor = port.actor_id();
    if (!resolver_.requirement_holds(n, port.fighter(), *hit, meta)) return;
    Resolved r;
    std::string why;
    if (!resolver_.resolve(n, port.fighter(), *hit, meta, r, why)) {
        spdlog::debug("[combat] {} ({}): the tick of {}: {}", n.actor_id, n.leaf, effect.row.name, why);
        return;
    }
    IncomingHit in;
    in.instigator = n.actor_id;
    in.hit_guid = hit_guid;
    in.source_type = content::kSourceTypeEffect;
    in.source_guid = effect.row.guid;
    in.flags = r.flags;
    in.mods = r.mods;
    in.at = now;
    in.shape_known = true;
    in.source_is_actor = false;
    in.predicted_autonomous = in.predicted_simulated = true;
    in.source_actor = port.actor_id();
    if (!r.miss() && !hit->effects.empty()) {
        Resolved gate;
        std::string ewhy;
        if (resolver_.resolve_effects(n, port.fighter(), *hit, meta, gate, ewhy)) in.effects = gate.effects;
    }
    r.miss() ? ++stats_.npc_misses : ++stats_.npc_hits;
    port.receive_hit(in, now);
    player_exchange(port.actor_id(), now);
    if (!r.miss()) {
        for (const effects::Applied& applied : in.effects) schedule_creature_effect(n, applied, port.actor_id(), now);
    }
}

void CombatWorld::advance_creature_zones(double now) {
    for (size_t i = 0; i < creature_zones_.size();) {
        const uint64_t npc = creature_zones_[i].npc;
        const Vec3 at = creature_zones_[i].at;
        const double radius = creature_zones_[i].radius;
        Npc* caster = combatant(npc);
        if (!caster) {
            creature_zones_.erase(creature_zones_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        // who stands in it now: a player or a pet whose body touches the zone's sphere
        std::set<uint64_t> inside;
        for (PlayerPort* p : players_) {
            if (p->fighter().alive() && body_in_sphere(p->fighter(), at, radius)) inside.insert(p->actor_id());
        }
        std::vector<uint64_t> entered, left;
        for (const uint64_t who : inside) {
            if (creature_zones_[i].inside.count(who) == 0) entered.push_back(who);
        }
        for (const uint64_t who : creature_zones_[i].inside) {
            if (inside.count(who) == 0) left.push_back(who);
        }
        creature_zones_[i].inside = inside;
        // a begin-overlap hit lands on the player who came in and an end-overlap hit on the player who went out, not on the ones who stayed
        for (const bool leaving : {false, true}) {
            for (const uint64_t who : leaving ? left : entered) {
                if (i >= creature_zones_.size()) break;
                land_zone_role(*caster, i, leaving ? "OnEndOverlapHitsIds" : "OnBeginOverlapHitsIds", now, who);
            }
        }
        if (i >= creature_zones_.size()) continue;
        // its ticks: every tick up to and including the end (a fire of 6 s ticks at 1, 2, 3, 4, 5 and 6 s)
        for (;;) {
            if (i >= creature_zones_.size()) break;
            const double due = creature_zones_[i].next_tick;
            const double end = creature_zones_[i].end_at;
            if (!(due <= now && due <= end + 1e-9)) break;
            const double tick = creature_zones_[i].tick;
            land_zone_role(*caster, i, "LingeringHitsIds", now);
            if (i >= creature_zones_.size()) break;
            creature_zones_[i].next_tick = due + tick;
        }
        if (i >= creature_zones_.size()) continue;
        if (now >= creature_zones_[i].end_at) {
            const int64_t zone_guid = creature_zones_[i].zone;
            land_zone_role(*caster, i, "EndHitsIds", now);
            if (i < creature_zones_.size()) {
                const content::ZoneRow* row = catalog_.zone(zone_guid);
                spdlog::info("[combat] {} ({})'s {} goes out at {:.2f} s", log_name(caster->actor_id, caster->archetype, caster->leaf), caster->actor_id, row ? row->name : std::string("zone"), now);
                creature_zones_.erase(creature_zones_.begin() + static_cast<std::ptrdiff_t>(i));
            }
            continue;
        }
        ++i;
    }
}

void CombatWorld::advance_creature_effects(double now) {
    for (size_t i = 0; i < creature_effects_.size();) {
        {
            const CreatureEffect& e = creature_effects_[i];
            Npc* caster = combatant(e.npc);
            PlayerPort* port = player(e.player);
            const bool drop = !caster || !e.def || !port || !port->fighter().alive() || (port->fighter().is_player() && now > e.start + 0.5 && !port->holds_effect(e.def->row.guid));
            if (drop) {
                creature_effects_.erase(creature_effects_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            if (now >= e.end) {
                const uint64_t who = e.player;
                const abilities::EffectDef* def = e.def;
                apply_creature_effect_hits(*caster, who, *def, "Expire", now);
                apply_creature_effect_hits(*caster, who, *def, "End", now);
                creature_effects_.erase(creature_effects_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            if (e.tick <= 0 || e.next_tick < 0) {
                ++i;
                continue;
            }
        }
        // the ticks due by now: each lands its Tick hit and the next one moves on (a tick's hit may put another effect on, so the entry is read again by index)
        for (;;) {
            const double due = creature_effects_[i].next_tick;
            const double end = creature_effects_[i].end;
            if (!(due <= now && due < end)) break;
            Npc* caster = combatant(creature_effects_[i].npc);
            const uint64_t who = creature_effects_[i].player;
            const abilities::EffectDef* def = creature_effects_[i].def;
            if (!caster || !def) break;
            apply_creature_effect_hits(*caster, who, *def, "Tick", now);
            if (i >= creature_effects_.size()) break;
            creature_effects_[i].next_tick = due + creature_effects_[i].tick;
        }
        if (i < creature_effects_.size()) ++i;
    }
}
