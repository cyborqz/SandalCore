// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_three_fixes_tests.inc: the three small ability fixes of the combat lane
//
//    Scatter Shot's ground marks: a normal cast sends the thirty FX-only impact hits of its three arcing ground arrows, ten for each arrow's record, at one
//    point under the first enemy found (under the caster when there is none), with no stat mods and out of the actor as their source; the charged shot sends
//    none.  A held Scatter Shot (the Concentrated talent) that the client ends, or that another ability interrupts, is dropped at once: no shot, no cost and
//    no cooldown follow, and the client is answered as every ended instance is.  Air Strike's areas ask Ranger_AirStrike_ICD of each target (0.5 s): a target
//    one area hurt is safe from the next areas while its own cooldown lasts and not for the whole cast.
//  (registered with AOC_REGISTER_TEST: test_worldd_combat.cpp runs it through aoc_tests::run; it is included from combat_registered.inc)
// ============================================================================

namespace three_fixes {

constexpr double kPi = 3.14159265358979323846;

/// One ground impact confirmation as this test reads it.
struct Mark {
    double   at = 0;
    int64_t  source = 0;
    bool     source_is_actor = true;
    bool     predicted_autonomous = false;
    bool     predicted_simulated = false;
    size_t   targets = 0;
    uint64_t target = 0;
    size_t   mods = 0;
    double   x = 0, y = 0, z = 0;
};

/// Every confirmation of the hit record `hit_guid` in the runtime's events.
std::vector<Mark> marks_of(const Rig& rig, int64_t hit_guid) {
    std::vector<Mark> out;
    for (const Seen& s : rig.log) {
        for (const bootstrap::HitConfirm& c : s.confirmations) {
            if (c.record_guid != hit_guid) continue;
            Mark m;
            m.at = s.at;
            m.source = c.source_guid;
            m.source_is_actor = c.source_is_actor;
            m.predicted_autonomous = c.predicted_autonomous;
            m.predicted_simulated = c.predicted_simulated;
            m.targets = c.targets.size();
            m.mods = c.mods.size();
            if (!c.targets.empty()) {
                m.target = c.targets.front().component.ObjectId;
                m.x = c.targets.front().impact_location.x;
                m.y = c.targets.front().impact_location.y;
                m.z = c.targets.front().impact_location.z;
                m.mods += c.targets.front().mods.size();
            }
            out.push_back(m);
        }
    }
    return out;
}

/// The ground arrows the ability's data lists for the ground impact hit: the source records of the thirty confirmations.
std::vector<int64_t> ground_sources(const abilities::AbilityDef& def, int64_t hit_guid) {
    std::vector<int64_t> out;
    for (const content::AbilityHitRow& h : def.hits) {
        if (h.source == "projectile" && h.hit_guid == hit_guid) out.push_back(h.owner_guid);
    }
    return out;
}

/// How many confirmations of `hit_guid` name the actor `target`.
size_t hits_on(const Rig& rig, int64_t hit_guid, uint64_t target) {
    size_t n = 0;
    for (const Seen& s : rig.log) {
        for (const bootstrap::HitConfirm& c : s.confirmations) {
            if (c.record_guid != hit_guid) continue;
            for (const bootstrap::HitTargetOut& t : c.targets) n += t.component.ObjectId == target ? 1 : 0;
        }
    }
    return n;
}

/// The server instance id of the pawn's ability item for `ability` (-1: none).
int32_t ability_instance(const Rig& rig, int64_t ability) {
    for (const auto& [id, item] : rig.pawn.abilities) {
        if (item.ability_guid == ability) return static_cast<int32_t>(id);
    }
    return -1;
}

/// The record of the hit named `name` (null when the data has none).
const abilities::HitDef* hit_named(Fixture& f, const char* name) { return f.catalog->hit_named(name); }

/// When Air Strike's first arrow's area opens: its 1.0296 s launch, then the arrow's flight, the caster's 1,098 cm over sin 130 degrees at 4,000 cm/s.
double first_area_at(double t0, double ground_z) {
    return t0 + 1.0296 + ((1098.0 - ground_z) / std::sin(130.0 * kPi / 180.0)) / 4000.0;
}

/// Two Rangers of the same world, each with his own ability pipeline and hit port (the way a session runs one pipeline for each player), the first with the
/// Concentrated talent.
struct TwoRangers {
    combat_rig::World      w;
    combat::NpcStatBook    book;
    combat::CombatWorld    world;
    Rig                    a, b;
    combat_rig::FakePlayer pa, pb;
    combat::WorldHitPort   porta, portb;

    TwoRangers(Fixture& f)
        : book(*f.ctx->bundle, *f.ctx->gear),
          world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion),
          a(1, 25, {"Ranger_Passive_ScatterShot_Concentrated"}, true, 11),
          b(1, 25, {}, true, 12),
          pa(a, 77, {0, 0, 1098}),
          pb(b, 78, {600, 0, 1098}),
          porta(world, w.registry, pa.fighter()),
          portb(world, w.registry, pb.fighter()) {
        if (!a.ok() || !b.ok()) return;
        world.add_player(&pa);
        world.add_player(&pb);
        a.pipe->set_combat(&porta);
        b.pipe->set_combat(&portb);
    }
    bool ok() const { return a.ok() && b.ok() && w.problem.empty(); }
};

// ---- Scatter Shot's ground marks ------------------------------------------------------------------------------------------------------------------

// A normal Scatter Shot: the three arcing ground arrows fire ten times each where they land, all at one point under the first enemy in the cone, with the
// ground arrow's record as the source (not the actor) and no stat mods, when the homing arrow's hit lands.  The charged shot has no ground arrows and sends none.
void test_scatter_shot_ground_marks(Fixture& f) {
    std::printf("a normal Scatter Shot sends thirty ground impact confirmations, ten for each of its three ground arrows, on the first enemy and at one point\n");
    const abilities::HitDef* ground = hit_named(f, "Ranger_Scattershot_GroundFX_ImpactHit");
    const abilities::HitDef* damage = hit_named(f, "Ranger_ScatterShot");
    CHECK(ground != nullptr && damage != nullptr, "the ground impact hit and the scatter's damage hit are in the data");
    if (!ground || !damage) return;
    Forge g(f, 1, {});
    CHECKF(g.ok(), "the Ranger and the world: %s%s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    g.rig.asks = false;
    const uint64_t wolf = g.wolf(900, 0);
    CHECK(wolf != 0, "a Wolf 9 m ahead of the Ranger");
    if (wolf == 0) return;
    const double t0 = 1.0;
    hitchecks::set_evasion(f, g.world, wolf, -5.0, t0 - 1.0);   // (the damage hit is the one this test times the marks by: it must land)
    g.rig.know({"Ranger_ScatterShot"});
    CHECK(g.rig.trigger("Ranger_ScatterShot", t0, false) == Refusal::None, "the Scatter Shot is cast");
    g.rig.run_to(t0 + 1.5);
    const std::vector<Mark> found = marks_of(g.rig, ground->row.guid);
    CHECKF(found.size() == 30, "thirty ground impact confirmations are sent (%zu)", found.size());
    const abilities::AbilityDef* def = g.rig.catalog->ability_named("Ranger_ScatterShot");
    CHECK(def != nullptr, "the ability is in the data");
    if (!def) return;
    const std::vector<int64_t> sources = ground_sources(*def, ground->row.guid);
    CHECKF(sources.size() == 3, "the data lists three ground arrows that fire the hit (%zu)", sources.size());
    std::map<int64_t, size_t> by_source;
    for (const Mark& m : found) ++by_source[m.source];
    size_t tens = 0;
    for (const int64_t s : sources) tens += by_source[s] == 10 ? 1 : 0;
    CHECKF(tens == 3, "each of the three ground arrows' records is the source of ten of them (%zu of 3)", tens);
    size_t actor_sources = 0, with_mods = 0, not_predicted = 0, wrong_target = 0;
    for (const Mark& m : found) {
        actor_sources += m.source_is_actor ? 1 : 0;
        with_mods += m.mods != 0 ? 1 : 0;
        not_predicted += (m.predicted_autonomous && m.predicted_simulated) ? 0 : 1;
        wrong_target += (m.targets == 1 && m.target == wolf) ? 0 : 1;
    }
    CHECKF(actor_sources == 0, "the actor is the source of none of them (%zu)", actor_sources);
    CHECKF(with_mods == 0, "none of them carries a stat mod (%zu)", with_mods);
    CHECKF(not_predicted == 0, "all of them are predicted on both sides (%zu are not)", not_predicted);
    CHECKF(wrong_target == 0, "all of them name the Wolf ahead of the Ranger (%zu do not)", wrong_target);
    double ground_z = 1000.0;
    const bool known = g.world.ground_height(900, 0, 1088, &ground_z);
    CHECKF(known, "the world knows the ground under the Wolf");
    size_t off_point = 0, off_moment = 0;
    for (const Mark& m : found) {
        off_point += (std::fabs(m.x - 900.0) <= 1.0 && std::fabs(m.y) <= 1.0 && std::fabs(m.z - ground_z) <= 1.0) ? 0 : 1;
        off_moment += std::fabs(m.at - found.front().at) <= 1e-9 ? 0 : 1;
    }
    CHECKF(off_point == 0, "all of them are at one point on the ground under the Wolf (%zu are off it)", off_point);
    CHECKF(off_moment == 0, "all of them come in one moment (%zu are elsewhere)", off_moment);
    double damage_at = -1;
    for (const Seen& s : g.rig.log) {
        for (const bootstrap::HitConfirm& c : s.confirmations) {
            if (c.record_guid != damage->row.guid) continue;
            for (const bootstrap::HitTargetOut& t : c.targets) {
                if (t.component.ObjectId == wolf && damage_at < 0) damage_at = s.at;
            }
        }
    }
    CHECKF(!found.empty() && damage_at >= 0 && std::fabs(damage_at - found.front().at) < 1e-9, "the marks come with the homing arrow's damage hit (%.3f against %.3f)",
           found.empty() ? -1.0 : found.front().at, damage_at);
    CHECKF(g.lost({wolf}) > 0.0, "the homing arrow hurts the Wolf (%.1f)", g.lost({wolf}));
}

// The charged shot (the full charge) has no ground arrows: it sends no ground impact confirmation, and its own charged hit lands.
void test_scatter_shot_ground_marks_charged(Fixture& f) {
    std::printf("a charged Scatter Shot sends no ground impact confirmation\n");
    const abilities::HitDef* ground = hit_named(f, "Ranger_Scattershot_GroundFX_ImpactHit");
    const abilities::HitDef* charged = hit_named(f, "Ranger_Scattershot_Charged");
    CHECK(ground != nullptr && charged != nullptr, "the ground impact hit and the charged hit are in the data");
    if (!ground || !charged) return;
    const int64_t branch = ability_rig::record_guid(real_db_path(), "AnimBranchRecordBase", "Ranger_Passive_ScatterShot_Concentrated");
    CHECK(branch != 0, "the full charge's branch is in the data");
    if (branch == 0) return;
    Forge g(f, 1, {"Ranger_Passive_ScatterShot_Concentrated"});
    CHECKF(g.ok(), "the Ranger and the world: %s%s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    g.rig.asks = false;
    const uint64_t wolf = g.wolf(900, 0);
    CHECK(wolf != 0, "a Wolf 9 m ahead of the Ranger");
    if (wolf == 0) return;
    const double t0 = 1.0;
    hitchecks::set_evasion(f, g.world, wolf, -5.0, t0 - 1.0);
    g.rig.know({"Ranger_ScatterShot"});
    const int32_t client_id = g.rig.next_client_id;
    CHECK(g.rig.trigger("Ranger_ScatterShot", t0, false) == Refusal::None, "the Scatter Shot is cast");
    g.rig.run_to(t0 + 1.1);
    CHECK(g.rig.pipe->branch_request(client_id, branch, t0 + 1.11), "the client asks for the full charge's branch");
    g.rig.run_to(t0 + 2.0);
    const std::vector<Mark> found = marks_of(g.rig, ground->row.guid);
    CHECKF(found.empty(), "the charged shot sends no ground impact confirmation (%zu)", found.size());
    CHECKF(hits_on(g.rig, charged->row.guid, wolf) >= 1, "the charged shot's own hit lands on the Wolf (%zu)", hits_on(g.rig, charged->row.guid, wolf));
    CHECKF(g.lost({wolf}) > 0.0, "the charged shot hurts the Wolf (%.1f)", g.lost({wolf}));
}

// No enemy in the cone: the ground arrows still land, and the marks are confirmed on the caster at the ground under him.
void test_scatter_shot_ground_marks_without_an_enemy(Fixture& f) {
    std::printf("a Scatter Shot with nobody in its cone sends the thirty ground impact confirmations on the caster\n");
    const abilities::HitDef* ground = hit_named(f, "Ranger_Scattershot_GroundFX_ImpactHit");
    CHECK(ground != nullptr, "the ground impact hit is in the data");
    if (!ground) return;
    Forge g(f, 1, {});
    CHECKF(g.ok(), "the Ranger and the world: %s%s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    g.rig.asks = false;
    g.rig.know({"Ranger_ScatterShot"});
    const double t0 = 1.0;
    CHECK(g.rig.trigger("Ranger_ScatterShot", t0, false) == Refusal::None, "the Scatter Shot is cast with nobody ahead");
    g.rig.run_to(t0 + 1.5);
    const std::vector<Mark> found = marks_of(g.rig, ground->row.guid);
    CHECKF(found.size() == 30, "thirty ground impact confirmations are sent (%zu)", found.size());
    size_t on_caster = 0;
    for (const Mark& m : found) on_caster += m.target == g.player.pawn_guid().ObjectId ? 1 : 0;
    CHECKF(on_caster == 30, "all of them name the caster (%zu)", on_caster);
    double ground_z = 1000.0;
    CHECK(g.world.ground_height(0, 0, 1098, &ground_z), "the world knows the ground under the caster");
    size_t off_point = 0;
    for (const Mark& m : found) off_point += (std::fabs(m.x) <= 1.0 && std::fabs(m.y) <= 1.0 && std::fabs(m.z - ground_z) <= 1.0) ? 0 : 1;
    CHECKF(off_point == 0, "all of them are at the ground under the caster (%zu are off it)", off_point);
}

// The same cast twice: a duplicate trigger is refused and sends nothing more; a real second cast (its cooldown cleared) sends its own thirty and no more.
void test_scatter_shot_ground_marks_twice(Fixture& f) {
    std::printf("a duplicate Scatter Shot trigger sends no second set of ground marks, and a second cast sends its own thirty\n");
    const abilities::HitDef* ground = hit_named(f, "Ranger_Scattershot_GroundFX_ImpactHit");
    CHECK(ground != nullptr, "the ground impact hit is in the data");
    if (!ground) return;
    Forge g(f, 1, {});
    CHECKF(g.ok(), "the Ranger and the world: %s%s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    g.rig.asks = false;
    const uint64_t wolf = g.wolf(900, 0);
    CHECK(wolf != 0, "a Wolf 9 m ahead of the Ranger");
    g.rig.know({"Ranger_ScatterShot"});
    const double t0 = 1.0;
    const int64_t ability = g.rig.ability("Ranger_ScatterShot");
    const int32_t id = g.rig.next_client_id++;
    TriggerInfo info;
    CHECK(g.rig.pipe->trigger(ability, id, t0, info) == Refusal::None, "the Scatter Shot is cast");
    g.rig.run_to(t0 + 1.5);
    CHECKF(marks_of(g.rig, ground->row.guid).size() == 30, "the first cast sends thirty (%zu)", marks_of(g.rig, ground->row.guid).size());
    CHECK(g.rig.pipe->trigger(ability, id, t0 + 1.5, info) == Refusal::Duplicate, "the same call again is refused as a duplicate");
    g.rig.run_to(t0 + 2.0);
    CHECKF(marks_of(g.rig, ground->row.guid).size() == 30, "the duplicate sends no second set (%zu)", marks_of(g.rig, ground->row.guid).size());
    g.rig.pipe->clear_cooldowns(t0 + 2.0);
    const int32_t id2 = g.rig.next_client_id;
    const Refusal again = g.rig.pipe->trigger(ability, id2, t0 + 2.0, info);
    CHECKF(again == Refusal::None, "a second Scatter Shot is cast (%s)", refusal_name(again));
    g.rig.run_to(t0 + 3.5);
    CHECKF(marks_of(g.rig, ground->row.guid).size() == 60, "the second cast sends its own thirty (%zu in all)", marks_of(g.rig, ground->row.guid).size());
}

// ---- a held Scatter Shot -------------------------------------------------------------------------------------------------------------------------

/// A Ranger with the Concentrated talent, a Wolf 3 m ahead, the held cast started at 1.0 s and the client's id of the cast.
struct HeldShot {
    Forge    g;
    uint64_t wolf = 0;
    int32_t  client_id = 0;
    bool     accepted = false;
    HeldShot(Fixture& f, double t0) : g(f, 1, {"Ranger_Passive_ScatterShot_Concentrated"}) {
        if (!g.ok()) return;
        g.rig.asks = false;
        wolf = g.wolf(300, 0);
        if (wolf == 0) return;
        hitchecks::set_evasion(f, g.world, wolf, -5.0, t0 - 1.0);
        g.rig.know({"Ranger_ScatterShot"});
        client_id = g.rig.next_client_id;
        accepted = g.rig.trigger("Ranger_ScatterShot", t0, false) == Refusal::None;
    }
    bool ok() const { return g.ok() && wolf != 0; }
};

// Escape (the client's end request) ends the held cast at once: no shot, no Mana, no cooldown, and the client is answered with the instance's end.
void test_held_scatter_shot_ends_with_escape(Fixture& f) {
    std::printf("a held Scatter Shot that Escape ends is over at once: no shot, no cost and no cooldown follow\n");
    constexpr double t0 = 1.0;
    HeldShot h(f, t0);
    CHECKF(h.ok() && h.accepted, "the held Scatter Shot is cast (%s)", h.g.rig.problem.c_str());
    if (!h.ok() || !h.accepted) return;
    h.g.rig.run_to(t0 + 0.3);
    const int32_t instance = ability_instance(h.g.rig, h.g.rig.ability("Ranger_ScatterShot"));
    CHECKF(instance >= 0, "the held cast's ability item is on the pawn (%d)", instance);
    if (instance < 0) return;
    const double mana = h.g.rig.mana();
    const size_t ends = h.g.rig.pawn.ends.size();
    CHECK(h.g.rig.pipe->end_request(instance, t0 + 0.3), "the client ends the held cast");
    h.g.rig.run_to(t0 + 3.5);
    CHECKF(h.g.lost({h.wolf}) == 0.0, "no shot follows the end: the Wolf takes no damage (%.1f)", h.g.lost({h.wolf}));
    CHECKF(h.g.rig.mana() >= mana - 0.5, "...and no Mana is spent (%.1f of %.1f)", h.g.rig.mana(), mana);
    CHECKF(h.g.rig.pipe->charges_left(h.g.rig.ability("Ranger_ScatterShot")) == -1, "...and no cooldown is running (%d)", h.g.rig.pipe->charges_left(h.g.rig.ability("Ranger_ScatterShot")));
    CHECKF(h.g.rig.pawn.ends.size() == ends + 1 && h.g.rig.pawn.ends.back() == h.client_id, "the client is told the instance ended (%zu end(s))", h.g.rig.pawn.ends.size());
    CHECKF(h.g.rig.pawn.broken.empty(), "the pawn took every change (%s)", h.g.rig.pawn.broken.c_str());
}

// Another ability ends the held cast at once, without an end message of its own (the client pressed another key): nothing of the Scatter Shot happens.
void test_held_scatter_shot_ends_with_another_ability(Fixture& f) {
    std::printf("another ability ends a held Scatter Shot at once: no shot, no cost and no cooldown follow\n");
    constexpr double t0 = 1.0;
    HeldShot h(f, t0);
    CHECKF(h.ok() && h.accepted, "the held Scatter Shot is cast (%s)", h.g.rig.problem.c_str());
    if (!h.ok() || !h.accepted) return;
    h.g.rig.know({"Ranger_ScatterShot", "Ranger_Disengage"});
    h.g.rig.run_to(t0 + 0.3);
    const double mana = h.g.rig.mana();
    const size_t ends = h.g.rig.pawn.ends.size();
    CHECK(h.g.rig.trigger("Ranger_Disengage", t0 + 0.3, false) == Refusal::None, "another ability is triggered while the key is held");
    h.g.rig.run_to(t0 + 3.5);
    CHECKF(h.g.lost({h.wolf}) == 0.0, "no shot follows: the Wolf takes no damage (%.1f)", h.g.lost({h.wolf}));
    CHECKF(h.g.rig.pipe->charges_left(h.g.rig.ability("Ranger_ScatterShot")) == -1, "...and no Scatter Shot cooldown is running (%d)", h.g.rig.pipe->charges_left(h.g.rig.ability("Ranger_ScatterShot")));
    // the other ability's own cost is its own: a control cast of it alone measures what the Mana should have lost, and no more
    double other_cost = 0.0;
    {
        Forge c(f, 1, {});
        if (c.ok()) {
            c.rig.asks = false;
            c.rig.know({"Ranger_Disengage"});
            const double before = c.rig.mana();
            if (c.rig.trigger("Ranger_Disengage", t0, false) == Refusal::None) {
                c.rig.run_to(t0 + 3.5);
                other_cost = before - c.rig.mana();
            }
        }
    }
    CHECKF(h.g.rig.mana() >= mana - other_cost - 0.5, "...and only the other ability's own cost is paid (%.1f of %.1f, its cost %.1f)", h.g.rig.mana(), mana, other_cost);
    CHECKF(h.g.rig.pawn.ends.size() == ends, "the client is told no end for the held instance (the new ability is its own) (%zu end(s))", h.g.rig.pawn.ends.size());
}

// A Dodge ends the held cast (as it ends any cast bar): no shot, no cost, and the client is told.
void test_held_scatter_shot_ends_with_a_dodge(Fixture& f) {
    std::printf("a Dodge ends a held Scatter Shot: no shot, no cost and the client is told it is over\n");
    constexpr double t0 = 1.0;
    HeldShot h(f, t0);
    CHECKF(h.ok() && h.accepted, "the held Scatter Shot is cast (%s)", h.g.rig.problem.c_str());
    if (!h.ok() || !h.accepted) return;
    h.g.rig.run_to(t0 + 0.3);
    const double mana = h.g.rig.mana();
    const size_t ends = h.g.rig.pawn.ends.size();
    const size_t ended = h.g.rig.pipe->end_by_dodge(t0 + 0.3);
    CHECKF(ended >= 1, "a Dodge ends the held cast (%zu)", ended);
    h.g.rig.run_to(t0 + 3.5);
    CHECKF(h.g.lost({h.wolf}) == 0.0, "no shot follows: the Wolf takes no damage (%.1f)", h.g.lost({h.wolf}));
    CHECKF(h.g.rig.mana() >= mana - 0.5, "...and no Mana is spent (%.1f of %.1f)", h.g.rig.mana(), mana);
    CHECKF(h.g.rig.pipe->charges_left(h.g.rig.ability("Ranger_ScatterShot")) == -1, "...and no cooldown is running (%d)", h.g.rig.pipe->charges_left(h.g.rig.ability("Ranger_ScatterShot")));
    CHECKF(h.g.rig.pawn.ends.size() == ends + 1 && h.g.rig.pawn.ends.back() == h.client_id, "the client is told the instance ended, once (%zu end(s))", h.g.rig.pawn.ends.size());
}

// The key let go at once still sends the normal scatter, and the full charge still sends the charged shot (the fix changes neither).
void test_held_scatter_shot_release_and_full_charge(Fixture& f) {
    std::printf("a held Scatter Shot released at once sends the scatter, and a full charge sends the charged shot\n");
    constexpr double t0 = 1.0;
    const abilities::HitDef* scatter = hit_named(f, "Ranger_ScatterShot");
    const abilities::HitDef* charged = hit_named(f, "Ranger_Scattershot_Charged");
    const int64_t branch = ability_rig::record_guid(real_db_path(), "AnimBranchRecordBase", "Ranger_Passive_ScatterShot_Concentrated");
    CHECK(scatter != nullptr && charged != nullptr && branch != 0, "the scatter's hits and the full charge's branch are in the data");
    if (!scatter || !charged || branch == 0) return;
    {
        HeldShot h(f, t0);
        CHECKF(h.ok() && h.accepted, "the released Scatter Shot is cast (%s)", h.g.rig.problem.c_str());
        if (!h.ok() || !h.accepted) return;
        const double mana = h.g.rig.mana();
        h.g.rig.run_to(t0 + 0.5);
        CHECK(h.g.rig.pipe->cancel(h.client_id, t0 + 0.5, 0.2), "the key goes up half a second after the trigger");
        h.g.rig.run_to(t0 + 1.5);
        CHECKF(h.g.lost({h.wolf}) > 0.0, "the scatter is sent: the Wolf takes damage (%.1f)", h.g.lost({h.wolf}));
        CHECKF(h.g.rig.mana() < mana - 0.5, "...and its cost is paid (%.1f of %.1f)", h.g.rig.mana(), mana);
        CHECKF(hits_on(h.g.rig, scatter->row.guid, h.wolf) >= 1, "the scatter's own hit lands on the Wolf (%zu)", hits_on(h.g.rig, scatter->row.guid, h.wolf));
    }
    {
        HeldShot h(f, t0);
        CHECKF(h.ok() && h.accepted, "the full-charge Scatter Shot is cast (%s)", h.g.rig.problem.c_str());
        if (!h.ok() || !h.accepted) return;
        const double mana = h.g.rig.mana();
        h.g.rig.run_to(t0 + 1.1);
        CHECK(h.g.rig.pipe->branch_request(h.client_id, branch, t0 + 1.11), "the client asks for the full charge's branch");
        h.g.rig.run_to(t0 + 2.0);
        CHECKF(h.g.rig.mana() < mana - 0.5, "the charged shot pays its cost (%.1f of %.1f)", h.g.rig.mana(), mana);
        CHECKF(hits_on(h.g.rig, charged->row.guid, h.wolf) >= 1, "the charged shot's hit lands on the Wolf (%zu)", hits_on(h.g.rig, charged->row.guid, h.wolf));
    }
}

// The caster dies before the held cast's fallback: nothing follows the death.
void test_held_scatter_shot_caster_dies(Fixture& f) {
    std::printf("a held Scatter Shot whose caster dies fires nothing and takes nothing\n");
    constexpr double t0 = 1.0;
    HeldShot h(f, t0);
    CHECKF(h.ok() && h.accepted, "the held Scatter Shot is cast (%s)", h.g.rig.problem.c_str());
    if (!h.ok() || !h.accepted) return;
    h.g.rig.run_to(t0 + 0.3);
    const double mana = h.g.rig.mana();
    h.g.rig.pipe->set_dead(true);
    h.g.rig.run_to(t0 + 3.5);
    CHECKF(h.g.lost({h.wolf}) == 0.0, "no shot follows the death: the Wolf takes no damage (%.1f)", h.g.lost({h.wolf}));
    CHECKF(h.g.rig.mana() >= mana - 0.5, "...and no Mana is spent (%.1f of %.1f)", h.g.rig.mana(), mana);
    CHECKF(h.g.rig.pipe->charges_left(h.g.rig.ability("Ranger_ScatterShot")) == -1, "...and no cooldown is running (%d)", h.g.rig.pipe->charges_left(h.g.rig.ability("Ranger_ScatterShot")));
}

// Two Rangers side by side: the held one Escape ends sends nothing and hurts nobody, and the other's cast is untouched and sends its thirty on its own Wolf.
void test_two_rangers_side_by_side(Fixture& f) {
    std::printf("two Rangers side by side: ending one's held cast changes nothing of the other's\n");
    const abilities::HitDef* ground = hit_named(f, "Ranger_Scattershot_GroundFX_ImpactHit");
    const std::string wolf_path = wolf_archetype(f);
    CHECK(ground != nullptr && !wolf_path.empty(), "the ground impact hit and the Wolf are in the data");
    if (!ground || wolf_path.empty()) return;
    TwoRangers s(f);
    CHECKF(s.ok(), "the two Rangers and their world: %s%s%s", s.a.problem.c_str(), s.b.problem.c_str(), s.w.problem.c_str());
    if (!s.ok()) return;
    s.a.asks = false;
    s.b.asks = false;
    const uint64_t wolf_a = s.w.spawn(wolf_path, 300, 0, 25, 100);
    const uint64_t wolf_b = s.w.spawn(wolf_path, 900, 0, 25, 100);
    CHECK(wolf_a != 0 && wolf_b != 0, "each Ranger has a Wolf ahead of him");
    if (wolf_a == 0 || wolf_b == 0) return;
    constexpr double t0 = 1.0;
    hitchecks::set_evasion(f, s.world, wolf_a, -5.0, t0 - 1.0);
    hitchecks::set_evasion(f, s.world, wolf_b, -5.0, t0 - 1.0);
    s.a.know({"Ranger_ScatterShot"});
    s.b.know({"Ranger_ScatterShot"});
    CHECK(s.a.trigger("Ranger_ScatterShot", t0, false) == Refusal::None, "the first Ranger's held Scatter Shot is cast");
    CHECK(s.b.trigger("Ranger_ScatterShot", t0, false) == Refusal::None, "the second Ranger's Scatter Shot is cast");
    s.a.run_to(t0 + 0.3);
    s.b.run_to(t0 + 0.3);
    const int32_t instance = ability_instance(s.a, s.a.ability("Ranger_ScatterShot"));
    CHECKF(instance >= 0, "the first Ranger's held cast has its ability item (%d)", instance);
    if (instance < 0) return;
    CHECK(s.a.pipe->end_request(instance, t0 + 0.3), "the first Ranger's client ends the held cast");
    s.a.run_to(t0 + 3.5);
    s.b.run_to(t0 + 3.5);
    const std::vector<Mark> marks_a = marks_of(s.a, ground->row.guid);
    const std::vector<Mark> marks_b = marks_of(s.b, ground->row.guid);
    CHECKF(marks_a.empty(), "the ended held cast sends no ground marks (%zu)", marks_a.size());
    CHECKF(marks_b.size() == 30, "the other Ranger's cast sends its thirty ground marks (%zu)", marks_b.size());
    size_t on_b = 0;
    for (const Mark& m : marks_b) on_b += m.target == wolf_b ? 1 : 0;
    CHECKF(on_b == 30, "all thirty are on the Wolf ahead of the second Ranger (%zu)", on_b);
    CHECKF(s.world.max_health_of(wolf_a) - s.world.health_of(wolf_a) == 0.0, "the first Ranger's Wolf takes no damage (%.1f)",
           s.world.max_health_of(wolf_a) - s.world.health_of(wolf_a));
    CHECKF(s.world.health_of(wolf_b) < s.world.max_health_of(wolf_b), "the second Ranger's shot hurts his Wolf (%.1f of %.1f)", s.world.health_of(wolf_b),
           s.world.max_health_of(wolf_b));
}

// ---- Air Strike's per-target internal cooldown -----------------------------------------------------------------------------------------------------

// A Wolf inside all three of Air Strike's areas is hurt once: the first area's hit puts Ranger_AirStrike_ICD on it and the next areas ask and skip it, not a set
// of the whole cast.
void test_air_strike_hits_once_per_its_internal_cooldown(Fixture& f) {
    std::printf("a Wolf inside all three Air Strike areas is hurt once: the hit's own 0.5 s cooldown holds the next areas\n");
    const abilities::HitDef* aoe = hit_named(f, "Ranger_AirStrike_AoE");
    const abilities::EffectDef* icd = f.catalog->effect_named("Ranger_AirStrike_ICD");
    CHECK(aoe != nullptr && icd != nullptr, "the Air Strike's area hit and its internal cooldown are in the data");
    if (!aoe || !icd) return;
    Forge g(f, 1, {});
    CHECKF(g.ok(), "the Ranger and the world: %s%s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    g.rig.asks = false;
    const uint64_t wolf = g.wolf(0, 300);
    CHECK(wolf != 0, "a Wolf beside the Ranger, in all three landing areas");
    if (wolf == 0) return;
    constexpr double t0 = 1.0;
    hitchecks::set_evasion(f, g.world, wolf, -5.0, t0 - 1.0);
    g.rig.know({"Ranger_AirStrike"});
    double ground_z = 1000.0;
    CHECK(g.world.ground_height(0, 0, 1098, &ground_z), "the world knows the ground under the caster");
    CHECK(g.rig.trigger("Ranger_AirStrike", t0, false) == Refusal::None, "the Air Strike is cast");
    g.rig.run_to(first_area_at(t0, ground_z) + 0.02);
    CHECKF(hits_on(g.rig, aoe->row.guid, wolf) == 1, "the Wolf is hurt by the first area (%zu)", hits_on(g.rig, aoe->row.guid, wolf));
    CHECKF(g.world.effect_stacks(wolf, icd->row.guid) > 0, "the area's hit left its 0.5 s cooldown on the Wolf (%d)", g.world.effect_stacks(wolf, icd->row.guid));
    g.rig.run_to(t0 + 2.5);
    CHECKF(hits_on(g.rig, aoe->row.guid, wolf) == 1, "the next areas ask the cooldown and skip the Wolf (%zu)", hits_on(g.rig, aoe->row.guid, wolf));
}

// A Wolf that carries another caster's Air Strike cooldown when the first area lands is not hurt by it; the set of the whole cast would mark it and skip it in
// the later areas too.  The cooldown is 0.46 s old at the first area (so it still holds there) and its 0.5 s run out before the second (an 81 ms gap); the world's
// tick expires it between the two, so the second area hurts the Wolf, and the third asks the cooldown the second put on it and skips it.
void test_air_strike_internal_cooldown_expires_between_volleys(Fixture& f) {
    std::printf("an Air Strike cooldown that runs out between two volleys no longer holds the target: the second area hurts the Wolf\n");
    const abilities::HitDef* aoe = hit_named(f, "Ranger_AirStrike_AoE");
    const abilities::EffectDef* icd = f.catalog->effect_named("Ranger_AirStrike_ICD");
    CHECK(aoe != nullptr && icd != nullptr, "the Air Strike's area hit and its internal cooldown are in the data");
    if (!aoe || !icd) return;
    Forge g(f, 1, {});
    CHECKF(g.ok(), "the Ranger and the world: %s%s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    g.rig.asks = false;
    const uint64_t wolf = g.wolf(0, 300);
    CHECK(wolf != 0, "a Wolf beside the Ranger, in all three landing areas");
    if (wolf == 0) return;
    constexpr double t0 = 1.0;
    hitchecks::set_evasion(f, g.world, wolf, -5.0, t0 - 1.0);
    g.rig.know({"Ranger_AirStrike"});
    // the first arrow's area opens 1.0296 s after the trigger, the arrow's flight being the caster's 98 cm over sin 130 degrees at 4,000 cm/s; the second area
    // 1.1178 s after it, straight down.  The cooldown starts 0.46 s before the first, so it still holds there and has run out before the second.
    double ground_z = 1000.0;
    CHECK(g.world.ground_height(0, 0, 1098, &ground_z), "the world knows the ground under the caster");
    const double first = first_area_at(t0, ground_z);
    const double second = t0 + 1.1178 + (1098.0 - ground_z) / 4000.0;
    CHECKF(second - first > 0.0, "(the second area lands after the first: %.3f, %.3f)", first, second);
    effects::Applied put;
    put.def = icd;
    put.duration = 0.5;
    g.world.apply_effects(wolf, 77, {put}, first - 0.46);   // (an Air Strike cooldown from before the cast: 0.46 s old at the first area)
    CHECKF(g.world.effect_stacks(wolf, icd->row.guid) == 1, "the Wolf starts with another caster's 0.5 s Air Strike cooldown (%d)", g.world.effect_stacks(wolf, icd->row.guid));
    CHECK(g.rig.trigger("Ranger_AirStrike", t0, false) == Refusal::None, "the Air Strike is cast");
    g.rig.run_to(first + 0.02);
    CHECKF(g.world.effect_stacks(wolf, icd->row.guid) == 1, "the first area is held by the cooldown (no hit: %zu)", hits_on(g.rig, aoe->row.guid, wolf));
    g.world.tick(first + 0.06);   // (the world's clock expires the cooldown, as it does every tick in a session, and the Wolf's stats read its effects again)
    CHECKF(g.world.effect_stacks(wolf, icd->row.guid) == 0, "the cooldown has run out before the second area (%d)", g.world.effect_stacks(wolf, icd->row.guid));
    g.rig.run_to(t0 + 2.5);
    CHECKF(hits_on(g.rig, aoe->row.guid, wolf) == 1, "the first area is held by the cooldown and the second hurts the Wolf (%zu)", hits_on(g.rig, aoe->row.guid, wolf));
}

// A Wolf that walks out between the areas is not hurt by the areas it left, and keeps the one hit of the area it stood in.
void test_air_strike_target_walks_out(Fixture& f) {
    std::printf("a Wolf that walks out of the path between two Air Strike areas keeps its one hit and takes no other\n");
    const abilities::HitDef* aoe = hit_named(f, "Ranger_AirStrike_AoE");
    CHECK(aoe != nullptr, "the Air Strike's area hit is in the data");
    if (!aoe) return;
    Forge g(f, 1, {});
    CHECKF(g.ok(), "the Ranger and the world: %s%s", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    g.rig.asks = false;
    const uint64_t wolf = g.wolf(0, 300);
    CHECK(wolf != 0, "a Wolf beside the Ranger, in all three landing areas");
    if (wolf == 0) return;
    constexpr double t0 = 1.0;
    hitchecks::set_evasion(f, g.world, wolf, -5.0, t0 - 1.0);
    g.rig.know({"Ranger_AirStrike"});
    CHECK(g.rig.trigger("Ranger_AirStrike", t0, false) == Refusal::None, "the Air Strike is cast");
    g.rig.run_to(t0 + 1.08);   // (the first area has landed: its flight is 32 ms after the 1.0296 s launch)
    CHECKF(hits_on(g.rig, aoe->row.guid, wolf) == 1, "the first area hurts the Wolf before it leaves (%zu)", hits_on(g.rig, aoe->row.guid, wolf));
    CHECK(g.w.registry.move(wolf, 6000, 0, 1088, 0), "the Wolf walks 60 m out of the path");
    g.rig.run_to(t0 + 2.5);
    CHECKF(hits_on(g.rig, aoe->row.guid, wolf) == 1, "the areas it left do not hurt it again (%zu)", hits_on(g.rig, aoe->row.guid, wolf));
}

}  // namespace three_fixes

using three_fixes::test_air_strike_hits_once_per_its_internal_cooldown;
using three_fixes::test_air_strike_internal_cooldown_expires_between_volleys;
using three_fixes::test_air_strike_target_walks_out;
using three_fixes::test_held_scatter_shot_caster_dies;
using three_fixes::test_held_scatter_shot_ends_with_a_dodge;
using three_fixes::test_held_scatter_shot_ends_with_another_ability;
using three_fixes::test_held_scatter_shot_ends_with_escape;
using three_fixes::test_held_scatter_shot_release_and_full_charge;
using three_fixes::test_scatter_shot_ground_marks;
using three_fixes::test_scatter_shot_ground_marks_charged;
using three_fixes::test_scatter_shot_ground_marks_twice;
using three_fixes::test_scatter_shot_ground_marks_without_an_enemy;
using three_fixes::test_two_rangers_side_by_side;

AOC_REGISTER_TEST(test_scatter_shot_ground_marks);
AOC_REGISTER_TEST(test_scatter_shot_ground_marks_charged);
AOC_REGISTER_TEST(test_scatter_shot_ground_marks_without_an_enemy);
AOC_REGISTER_TEST(test_scatter_shot_ground_marks_twice);
AOC_REGISTER_TEST(test_held_scatter_shot_ends_with_escape);
AOC_REGISTER_TEST(test_held_scatter_shot_ends_with_another_ability);
AOC_REGISTER_TEST(test_held_scatter_shot_ends_with_a_dodge);
AOC_REGISTER_TEST(test_held_scatter_shot_release_and_full_charge);
AOC_REGISTER_TEST(test_held_scatter_shot_caster_dies);
AOC_REGISTER_TEST(test_two_rangers_side_by_side);
AOC_REGISTER_TEST(test_air_strike_hits_once_per_its_internal_cooldown);
AOC_REGISTER_TEST(test_air_strike_internal_cooldown_expires_between_volleys);
AOC_REGISTER_TEST(test_air_strike_target_walks_out);
