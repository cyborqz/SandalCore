// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_hit_checks_tests.inc: the checks of the hit checks on each target (included by combat_registered.inc inside test_worldd_combat's anonymous namespace, after
//  combat_dots_tests.inc, combat_cc_tests.inc and combat_a3_resist_tests.inc, whose test beds and helpers it uses)
//
//    a hit rolls its miss, block and critical on each of its targets, and asks its own requirement of each of them with that target as GetTarget(): the Hemorrhage conversions of the
//    Snipe, the Rupture, the Headshot and the Vine Field ask "does it bleed?" of the Wolf, not of the player
//    the ticks of a damage over time never miss, whatever the mob's evasion
//    a second player's limit-1 debuff goes on a mob beside the first one's, and the same player's refreshes his own
//    two players' damage over time both tick, each as the player who put it on, and a reset in the middle of them leaves them
// ============================================================================

namespace hitchecks {

constexpr double kStart = 100.0;

/// One target of a confirmed hit, as the client is told of it.
struct Told {
    uint64_t actor = 0;
    uint8_t  flags = 0;
    size_t   mods = 0;
    double   value = 0;
    bool     missed() const { return (flags & stats::kHitMiss) != 0; }
};

/// Every target of every confirmation of the hit record `record` in the runtime's events.
std::vector<Told> told(const std::vector<Seen>& seen, int64_t record) {
    std::vector<Told> out;
    for (const Seen& s : seen) {
        for (const bootstrap::HitConfirm& c : s.confirmations) {
            if (c.record_guid != record) continue;
            for (const bootstrap::HitTargetOut& t : c.targets) out.push_back({t.component.ObjectId, t.flags, t.mods.size(), t.mods.empty() ? 0.0 : static_cast<double>(t.mods[0].value)});
        }
    }
    return out;
}

const Told* told_of(const std::vector<Told>& all, uint64_t actor) {
    for (const Told& t : all)
        if (t.actor == actor) return &t;
    return nullptr;
}

/// A bleed on a Wolf, put on by the player of the world (actor 77).
void make_bleed(BardMobWorld& bw, uint64_t wolf, double now, const abilities::EffectDef& bleeding) {
    effects::Applied bleed;
    bleed.def = &bleeding;
    bleed.duration = 600.0;
    bw.world->apply_effects(wolf, 77, {bleed}, now);
}

/// The notes of the hits of the effect hit `hit_guid` on `wolf` that the player of the world has been told of.
std::vector<combat::EffectHitNote> notes_of(BardMobWorld& bw, uint64_t wolf, int64_t hit_guid) {
    std::vector<combat::EffectHitNote> out;
    for (const combat::EffectHitNote& n : bw.player->effect_notes) {
        if (n.holder == wolf && n.hit_guid == hit_guid) out.push_back(n);
    }
    return out;
}

/// A cast of `ability` that names `aimed` as its targets (none: a cast at the ground or the air) and the client's request for the hit `hit` naming `found`, the actors the hit's own
/// shape found (BardCast's one list does both); the world is run to `until`.
BardCast cast_at(BardMobWorld& bw, const char* ability, const std::vector<uint64_t>& aimed, const std::vector<uint64_t>& found, const char* hit, double t, double until) {
    BardCast out;
    const int64_t guid = bw.rig.ability(ability);
    const abilities::AbilityDef* def = bw.rig.catalog->ability(guid);
    if (!def) return out;
    TriggerInfo info;
    info.target_count = static_cast<uint32_t>(aimed.size());
    info.target_actors = aimed;
    out.client_id = bw.rig.next_client_id;
    out.cast = bw.rig.pipe->trigger(guid, bw.rig.next_client_id++, t, info) == Refusal::None;
    if (!out.cast) return out;
    int64_t targeting = content::kTargetingDefaultSelf, hit_guid = 0;
    for (const content::AbilityHitRow& h : def->hits) {
        const abilities::HitDef* hd = bw.rig.catalog->hit(h.hit_guid);
        if (!hd || hd->row.name != hit) continue;
        hit_guid = h.hit_guid;
        if (h.source == "hit") {
            for (const content::TargetingRow& tg : def->targetings) {
                if (tg.key == h.key) targeting = tg.guid;
            }
        }
    }
    if (hit_guid == 0) return out;
    out.requested = bw.rig.pipe->hit_request(guid, targeting, hit_guid, 1, 0, t + 0.02, found) == AbilityPipeline::HitVerdict::Scheduled;
    bard_run(bw, t, until, &out.seen);
    return out;
}

/// A cast of `ability` at `aimed` and the client's requests for each hit of `hits` in turn (each naming `aimed`), 20 ms apart; the world is run to `until`.
BardCast cast_asking(BardMobWorld& bw, const char* ability, const std::vector<uint64_t>& aimed, const std::vector<const char*>& hits, double t, double until) {
    BardCast out;
    const int64_t guid = bw.rig.ability(ability);
    const abilities::AbilityDef* def = bw.rig.catalog->ability(guid);
    if (!def) return out;
    TriggerInfo info;
    info.target_count = static_cast<uint32_t>(aimed.size());
    info.target_actors = aimed;
    out.client_id = bw.rig.next_client_id;
    out.cast = bw.rig.pipe->trigger(guid, bw.rig.next_client_id++, t, info) == Refusal::None;
    if (!out.cast) return out;
    out.requested = true;
    double at = t + 0.02;
    for (const char* name : hits) {
        int64_t targeting = content::kTargetingDefaultSelf, hit_guid = 0;
        for (const content::AbilityHitRow& h : def->hits) {
            const abilities::HitDef* hd = bw.rig.catalog->hit(h.hit_guid);
            if (!hd || hd->row.name != name) continue;
            hit_guid = h.hit_guid;
            if (h.source == "hit") {
                for (const content::TargetingRow& tg : def->targetings) {
                    if (tg.key == h.key) targeting = tg.guid;
                }
            }
        }
        out.requested = out.requested && hit_guid != 0 && bw.rig.pipe->hit_request(guid, targeting, hit_guid, 1, 0, at, aimed) == AbilityPipeline::HitVerdict::Scheduled;
        at += 0.02;
    }
    bard_run(bw, t, until, &out.seen);
    return out;
}

/// The ticks of the hit `hit_name` on `wolf` that the bed's two players were told of, by time.  The notes stay (dots::Bed::ticks empties them for every hit, so a test that reads two kinds
/// of tick of one run reads them with this and empties them with `forget` at the end).
std::vector<dots::Tick> peek(dots::Bed& bed, uint64_t wolf, const char* hit_name) {
    std::vector<dots::Tick> out;
    const abilities::HitDef* hit = bed.f.catalog->hit_named(hit_name);
    for (combat_rig::FakePlayer* p : {&bed.player, &bed.other}) {
        for (const combat::EffectHitNote& n : p->effect_notes) {
            if (!hit || n.holder != wolf || n.hit_guid != hit->row.guid) continue;
            out.push_back({n.at, n.mods.empty() ? 0.0 : n.mods[0].value, n.flags, n.predicted, n.effect_guid, p->actor_id()});
        }
    }
    std::sort(out.begin(), out.end(), [](const dots::Tick& a, const dots::Tick& b) { return a.at < b.at; });
    return out;
}

void forget(dots::Bed& bed) {
    bed.player.effect_notes.clear();
    bed.other.effect_notes.clear();
}

/// The evasion of both aspects raised by `by` (5: a roll of anything against it misses; -5: nothing can miss), as an effect on the Wolf.
void set_evasion(Fixture& f, combat::CombatWorld& world, uint64_t wolf, double by, double now) {
    a3_set_stats(f, world, wolf, {{f.data->ids().physical_evasion, by}, {f.data->ids().magical_evasion, by}}, now);
}

}  // namespace hitchecks

// ---- every target rolls on its own ---------------------------------------------------------------------------------------------------------------------------------------
// One swing of the 1H axe combo reaches two Wolves.  The first Wolf's evasion is raised so that the swing always misses it and the second one's lowered so that it never does: the
// client is told of a miss on the first and of damage on the second, and the first keeps its Health.  Then the other way round, so that the order of the targets decides nothing.
void test_hit_checks_each_target_rolls(Fixture& f) {
    std::printf("a hit rolls its miss on each target of its own: of two Wolves one is missed and one is hit, then the other way round\n");
    const std::string wolf_path = wolf_archetype(f);
    const abilities::AbilityDef* axe = f.catalog->ability_named("WeaponCombo_Melee_1H_Axe");
    CHECK(axe && !axe->hits.empty() && !wolf_path.empty(), "the 1H axe combo and the Wolf are in the data");
    if (!axe || axe->hits.empty() || wolf_path.empty()) return;
    const abilities::HitDef* first = f.catalog->hit(axe->hits[0].hit_guid);
    CHECK(first != nullptr && !first->mods.empty(), "the combo's first hit takes Health");
    if (!first || first->mods.empty()) return;
    for (int round = 0; round < 2; ++round) {
        BardMobWorld bw(f, wolf_path, 4);
        CHECKF(bw.problem.empty(), "the Fighter and the world: %s", bw.problem.c_str());
        if (!bw.problem.empty()) return;
        bw.rig.know({"WeaponCombo_Melee_1H_Axe"});
        const uint64_t a = bw.w.spawn(wolf_path, 400, -100, 8, 0);
        const uint64_t b = bw.w.spawn(wolf_path, 400, 100, 8, 0);
        CHECK(a != 0 && b != 0, "two Wolves 4 m from the Fighter");
        if (a == 0 || b == 0) return;
        const uint64_t missed = round == 0 ? a : b, struck = round == 0 ? b : a;
        hitchecks::set_evasion(f, *bw.world, missed, 5.0, hitchecks::kStart - 1.0);
        hitchecks::set_evasion(f, *bw.world, struck, -5.0, hitchecks::kStart - 1.0);
        const double struck_before = bw.world->health_of(struck), missed_before = bw.world->health_of(missed);
        const BardCast cast = bard_cast(bw, "WeaponCombo_Melee_1H_Axe", {a, b}, first->row.name.c_str(), hitchecks::kStart, hitchecks::kStart + 1.2);
        CHECKF(cast.cast && cast.requested, "round %d: the cast and the client's request for the hit are accepted", round);
        const std::vector<hitchecks::Told> all = hitchecks::told(cast.seen, first->row.guid);
        const hitchecks::Told* tm = hitchecks::told_of(all, missed);
        const hitchecks::Told* ts = hitchecks::told_of(all, struck);
        CHECKF(tm && ts, "round %d: the client is told of both Wolves (%zu targets)", round, all.size());
        if (!tm || !ts) continue;
        CHECKF(tm->missed() && tm->mods == 0, "round %d: the Wolf that cannot be hit is told a miss with no stat mods (flags %d, mods %zu)", round, static_cast<int>(tm->flags), tm->mods);
        CHECKF(!ts->missed() && ts->mods == 1 && ts->value < 0, "round %d: the other Wolf is told its damage (flags %d, mods %zu, value %.1f)", round, static_cast<int>(ts->flags), ts->mods, ts->value);
        CHECKF(bw.world->health_of(missed) == missed_before && bw.world->health_of(struck) < struck_before,
               "round %d: the missed Wolf keeps its Health (%.1f) and the struck one lost it (%.1f of %.1f)", round, bw.world->health_of(missed), bw.world->health_of(struck), struck_before);
    }
}
AOC_REGISTER_TEST(test_hit_checks_each_target_rolls);

// ---- the requirement is asked of each target ---------------------------------------------------------------------------------------------------------------------------
// The Snipe's own Hemorrhage hit requires a bleeding target.  Two Wolves stand in the Snipe's way and only one bleeds: the Hemorrhage goes on that one, the bleed goes off it (the
// conversion), and the client is told of it alone.  Cast again when neither bleeds: nothing happens.  Bleed both and cast once more: both are converted.
void test_hit_checks_requirement_each_target(Fixture& f) {
    std::printf("a hit asks its requirement of each target: the Snipe's Hemorrhage converts the bleeding Wolf of two and no other, twice over\n");
    const std::string wolf_path = wolf_archetype(f);
    const abilities::EffectDef* bleeding = f.catalog->effect_named("Status_Bleeding");
    const abilities::EffectDef* hemorrhage = f.catalog->effect_named("Status_Hemorrhage");
    const abilities::HitDef* conversion = f.catalog->hit_named("Snipe_Hemorrhage");
    CHECK(bleeding && hemorrhage && conversion && !wolf_path.empty(), "the data has the bleed, the Hemorrhage, the Snipe's Hemorrhage hit and the Wolf");
    if (!bleeding || !hemorrhage || !conversion || wolf_path.empty()) return;
    const content::ExprRow* req = f.catalog->expr(conversion->row.requirement_expr);
    CHECKF(req && req->uses_target && req->program >= 0, "the hit's requirement reads its target (a bleeding one)");
    BardMobWorld bw(f, wolf_path, 1);
    CHECKF(bw.problem.empty(), "the Ranger and the world: %s", bw.problem.c_str());
    if (!bw.problem.empty()) return;
    bw.rig.know({"Ranger_Snipe"});
    const uint64_t sick = bw.w.spawn(wolf_path, 600, -100, 8, 0);
    const uint64_t well = bw.w.spawn(wolf_path, 600, 100, 8, 0);
    CHECK(sick != 0 && well != 0, "two Wolves 6 m from the Ranger");
    if (sick == 0 || well == 0) return;
    hitchecks::make_bleed(bw, sick, hitchecks::kStart - 1.0, *bleeding);
    CHECK(bw.world->effect_stacks(sick, bleeding->row.guid) == 1 && bw.world->effect_stacks(well, bleeding->row.guid) == 0, "the first Wolf bleeds and the second does not");

    const BardCast one = bard_cast(bw, "Ranger_Snipe", {sick, well}, "Snipe_Hemorrhage", hitchecks::kStart, hitchecks::kStart + 0.6);
    CHECKF(one.cast && one.requested, "the Snipe is cast and the client's request for its Hemorrhage hit is accepted (cast %d, request %d)", one.cast ? 1 : 0, one.requested ? 1 : 0);
    const std::vector<hitchecks::Told> told1 = hitchecks::told(one.seen, conversion->row.guid);
    CHECKF(told1.size() == 1 && told1[0].actor == sick, "the client is told of the Wolf that bleeds alone (%zu targets)", told1.size());
    CHECKF(bw.world->effect_stacks(sick, hemorrhage->row.guid) == 1, "the bleeding Wolf takes a Hemorrhage (%d stacks)", bw.world->effect_stacks(sick, hemorrhage->row.guid));
    CHECKF(bw.world->effect_stacks(sick, bleeding->row.guid) == 0, "...and the bleed is taken off it: the conversion (%d stacks of bleed left)", bw.world->effect_stacks(sick, bleeding->row.guid));
    CHECKF(bw.world->effect_stacks(well, hemorrhage->row.guid) == 0 && bw.world->effect_count(well) == 0, "the Wolf that does not bleed takes nothing (%d stacks of Hemorrhage, %zu effects)",
           bw.world->effect_stacks(well, hemorrhage->row.guid), bw.world->effect_count(well));

    // the same cast again, thirty seconds on: the Hemorrhage is over, nothing bleeds, nothing is converted
    bard_run(bw, hitchecks::kStart + 0.6, hitchecks::kStart + 29.0);
    CHECK(bw.world->effect_count(sick) == 0 && bw.world->effect_count(well) == 0, "(thirty seconds on the Hemorrhage is over and neither Wolf holds an effect)");
    const BardCast two = bard_cast(bw, "Ranger_Snipe", {sick, well}, "Snipe_Hemorrhage", hitchecks::kStart + 30.0, hitchecks::kStart + 30.6);
    CHECKF(two.cast && two.requested, "the second Snipe is cast and its request accepted (cast %d, request %d)", two.cast ? 1 : 0, two.requested ? 1 : 0);
    CHECKF(hitchecks::told(two.seen, conversion->row.guid).empty(), "...and the client is told of no target: neither Wolf bleeds");
    CHECKF(bw.world->effect_count(sick) == 0 && bw.world->effect_count(well) == 0, "...and nothing is put on either Wolf (%zu and %zu effects)", bw.world->effect_count(sick), bw.world->effect_count(well));

    // both bleed: the third cast converts both, each as its own check
    bard_run(bw, hitchecks::kStart + 30.6, hitchecks::kStart + 59.0);
    hitchecks::make_bleed(bw, sick, hitchecks::kStart + 59.0, *bleeding);
    hitchecks::make_bleed(bw, well, hitchecks::kStart + 59.0, *bleeding);
    const BardCast three = bard_cast(bw, "Ranger_Snipe", {sick, well}, "Snipe_Hemorrhage", hitchecks::kStart + 60.0, hitchecks::kStart + 60.6);
    const std::vector<hitchecks::Told> told3 = hitchecks::told(three.seen, conversion->row.guid);
    CHECKF(three.cast && three.requested && told3.size() == 2 && hitchecks::told_of(told3, sick) && hitchecks::told_of(told3, well), "the third Snipe: both Wolves bleed and the client is told of both (%zu targets)", told3.size());
    CHECKF(bw.world->effect_stacks(sick, hemorrhage->row.guid) == 1 && bw.world->effect_stacks(well, hemorrhage->row.guid) == 1 && bw.world->effect_stacks(sick, bleeding->row.guid) == 0 &&
               bw.world->effect_stacks(well, bleeding->row.guid) == 0,
           "...each takes one Hemorrhage and gives up its bleed (%d and %d stacks of Hemorrhage)", bw.world->effect_stacks(sick, hemorrhage->row.guid), bw.world->effect_stacks(well, hemorrhage->row.guid));
}
AOC_REGISTER_TEST(test_hit_checks_requirement_each_target);

// ---- the four Hemorrhage conversions, from their real abilities ----------------------------------------------------------------------------------------------------------------
// Each of the four conversion hits requires a bleeding target (the data's GetEffectCount(GetTarget(), Bleeding) > 0).  Cast at a bleeding Wolf and a clean one, the ability's own hit
// converts the bleeding one and leaves the other alone.
void test_hit_checks_the_four_conversions(Fixture& f) {
    std::printf("the four Hemorrhage conversions (Snipe, Rupture, Headshot, Vine Field) fire on the bleeding Wolf of two and on no other\n");
    const std::string wolf_path = wolf_archetype(f);
    const abilities::EffectDef* bleeding = f.catalog->effect_named("Status_Bleeding");
    const abilities::EffectDef* hemorrhage = f.catalog->effect_named("Status_Hemorrhage");
    CHECK(bleeding && hemorrhage && !wolf_path.empty(), "the data has the bleed, the Hemorrhage and the Wolf");
    if (!bleeding || !hemorrhage || wolf_path.empty()) return;
    struct Row {
        const char* ability;
        int         class_enum;
        const char* hit;
        bool        at_actors;   // the cast names the Wolves (false: the Vine Field is cast at the ground; its hit finds the Wolves)
    };
    const Row rows[] = {
        {"Ranger_Snipe", 1, "Snipe_Hemorrhage", true},
        {"Fighter_Rupture", 4, "Fighter_Rupture_Hemorrhage", true},
        {"Ranger_Headshot", 1, "Headshot_HemorrageConversion_Hit", true},
        {"Ranger_VineField", 1, "Ranger_VineField_Hemorrhage", false},
    };
    for (const Row& r : rows) {
        const abilities::HitDef* hit = f.catalog->hit_named(r.hit);
        CHECKF(hit != nullptr && f.catalog->ability_named(r.ability) != nullptr, "%s: the ability and its hit %s are in the data", r.ability, r.hit);
        if (!hit) continue;
        const content::ExprRow* req = f.catalog->expr(hit->row.requirement_expr);
        CHECKF(req && req->uses_target && req->program >= 0, "%s: the hit's requirement reads its target", r.hit);
        BardMobWorld bw(f, wolf_path, r.class_enum);
        CHECKF(bw.problem.empty(), "%s: the character and the world: %s", r.ability, bw.problem.c_str());
        if (!bw.problem.empty()) continue;
        bw.rig.know({r.ability});
        const uint64_t sick = bw.w.spawn(wolf_path, 600, -100, 8, 0);
        const uint64_t well = bw.w.spawn(wolf_path, 600, 100, 8, 0);
        if (sick == 0 || well == 0) continue;
        hitchecks::make_bleed(bw, sick, hitchecks::kStart - 1.0, *bleeding);
        const BardCast cast = hitchecks::cast_at(bw, r.ability, r.at_actors ? std::vector<uint64_t>{sick, well} : std::vector<uint64_t>{}, {sick, well}, r.hit, hitchecks::kStart, hitchecks::kStart + 0.8);
        CHECKF(cast.cast && cast.requested, "%s: the cast and the client's request for %s are accepted (cast %d, request %d)", r.ability, r.hit, cast.cast ? 1 : 0, cast.requested ? 1 : 0);
        if (!cast.cast) {
            for (const Seen& s : bw.rig.take())
                if (s.refusal) std::printf("    %s\n", s.note.c_str());
        }
        const std::vector<hitchecks::Told> told = hitchecks::told(cast.seen, hit->row.guid);
        CHECKF(told.size() == 1 && told[0].actor == sick, "%s: the client is told of the bleeding Wolf alone (%zu targets)", r.hit, told.size());
        CHECKF(bw.world->effect_stacks(sick, hemorrhage->row.guid) == 1 && bw.world->effect_stacks(sick, bleeding->row.guid) == 0,
               "%s: the bleeding Wolf takes a Hemorrhage and gives up its bleed (%d stacks, %d bleed)", r.hit, bw.world->effect_stacks(sick, hemorrhage->row.guid),
               bw.world->effect_stacks(sick, bleeding->row.guid));
        CHECKF(bw.world->effect_stacks(well, hemorrhage->row.guid) == 0, "%s: the clean Wolf takes none (%d stacks)", r.hit, bw.world->effect_stacks(well, hemorrhage->row.guid));
    }
}
AOC_REGISTER_TEST(test_hit_checks_the_four_conversions);

// ---- ticks never miss ------------------------------------------------------------------------------------------------------------------------------------------------------
// A mob whose evasion is raised so far that every ordinary hit of the Ranger misses it: the Hemorrhage a Snipe's conversion puts on it still ticks every half second for its
// five seconds, every tick landing and none missed (retail's recorded ticks never missed).
void test_hit_checks_a_conversion_tick_never_misses(Fixture& f) {
    std::printf("a Hemorrhage a conversion puts on a Wolf that nothing can hit ticks ten times and no tick misses\n");
    const std::string wolf_path = wolf_archetype(f);
    const abilities::EffectDef* bleeding = f.catalog->effect_named("Status_Bleeding");
    const abilities::EffectDef* hemorrhage = f.catalog->effect_named("Status_Hemorrhage");
    const abilities::HitDef* tick = f.catalog->hit_named("Status_Hemorrhage_Tick");
    const abilities::HitDef* snipe = f.catalog->hit_named("Ranger_Snipe_V2");
    CHECK(bleeding && hemorrhage && tick && snipe && !wolf_path.empty(), "the data has the bleed, the Hemorrhage, its tick, the Snipe's damage hit and the Wolf");
    if (!bleeding || !hemorrhage || !tick || !snipe || wolf_path.empty()) return;
    BardMobWorld bw(f, wolf_path, 1);
    CHECKF(bw.problem.empty(), "the Ranger and the world: %s", bw.problem.c_str());
    if (!bw.problem.empty()) return;
    bw.rig.know({"Ranger_Snipe"});
    const uint64_t wolf = bw.w.spawn(wolf_path, 600, 0, 8, 0);
    CHECK(wolf != 0, "a Wolf 6 m from the Ranger");
    if (!wolf) return;
    hitchecks::set_evasion(f, *bw.world, wolf, 5.0, hitchecks::kStart - 1.0);
    // the control: an ordinary hit of the same Ranger misses this Wolf every time
    combat::Fighter* target = bw.world->fighter_of(wolf);
    size_t misses = 0;
    for (int i = 0; i < 40 && target; ++i) {
        combat::HitMeta meta;
        meta.source_type = content::kSourceTypeAbility;
        combat::Resolved r;
        std::string why;
        if (bw.world->resolver().resolve(bw.player->fighter(), *target, *snipe, meta, r, why) && r.miss()) ++misses;
    }
    CHECKF(misses == 40, "(the control: 40 of 40 ordinary hits of the Snipe miss this Wolf: %zu)", misses);
    hitchecks::make_bleed(bw, wolf, hitchecks::kStart - 0.5, *bleeding);
    const BardCast cast = bard_cast(bw, "Ranger_Snipe", {wolf}, "Snipe_Hemorrhage", hitchecks::kStart, hitchecks::kStart + 8.0);
    CHECKF(cast.cast && cast.requested, "the Snipe is cast and its Hemorrhage hit requested (cast %d, request %d)", cast.cast ? 1 : 0, cast.requested ? 1 : 0);
    const std::vector<combat::EffectHitNote> notes = hitchecks::notes_of(bw, wolf, tick->row.guid);
    CHECKF(notes.size() == 10, "the Hemorrhage ticks ten times in its five seconds (%zu ticks)", notes.size());
    size_t missed = 0, took = 0;
    for (const combat::EffectHitNote& n : notes) {
        missed += (n.flags & stats::kHitMiss) != 0 ? 1 : 0;
        took += !n.mods.empty() && n.mods[0].value < 0 ? 1 : 0;
    }
    CHECKF(missed == 0 && took == notes.size(), "...and not one of them misses: each takes Health (%zu missed, %zu took Health of %zu)", missed, took, notes.size());
    CHECK(bw.world->effect_stacks(wolf, hemorrhage->row.guid) == 0, "(the Hemorrhage is gone after its five seconds)");
}
AOC_REGISTER_TEST(test_hit_checks_a_conversion_tick_never_misses);

// The same on the test bed with the three damage over times at once: a Hemorrhage, a bleed and a poison on a Wolf of impossible evasion, put on by each of two players.  Every tick of
// every one of them lands, and the counts are the data's: 10 of a Hemorrhage, 5 of a bleed, 9 of a poison.
void test_hit_checks_ticks_never_miss(Fixture& f) {
    std::printf("the ticks of a Hemorrhage, a bleed and a poison never miss a Wolf of impossible evasion, whoever put them on\n");
    dots::Bed bed(f);
    CHECKF(bed.ok(), "the bed: %s", bed.problem().c_str());
    if (!bed.ok()) return;
    const abilities::HitDef* ordinary = f.catalog->hit_named("Ranger_Snipe_V2");
    CHECK(ordinary && f.catalog->hit_named("Status_Hemorrhage_Tick") && f.catalog->hit_named("Status_Bleeding_Tick") && f.catalog->hit_named("Status_Poisoned_Tick"),
          "the three ticks and an ordinary damage hit are in the data");
    if (!ordinary) return;
    const double t0 = hitchecks::kStart;
    const uint64_t wolf = bed.wolf(0);
    CHECK(wolf != 0, "a Wolf 30 m away");
    if (!wolf) return;
    hitchecks::set_evasion(f, bed.world, wolf, 5.0, t0 - 1.0);
    combat::Fighter* target = bed.world.fighter_of(wolf);
    size_t misses = 0;
    for (int i = 0; i < 40 && target; ++i) {
        combat::HitMeta meta;
        meta.source_type = content::kSourceTypeAbility;
        combat::Resolved r;
        std::string why;
        if (bed.world.resolver().resolve(bed.player.fighter(), *target, *ordinary, meta, r, why) && r.miss()) ++misses;
    }
    CHECKF(misses == 40, "(the control: 40 of 40 ordinary hits miss this Wolf: %zu)", misses);
    for (combat_rig::FakePlayer* p : {&bed.player, &bed.other}) {
        bed.land(wolf, "Snipe_Hemorrhage", t0, *p);
        bed.land(wolf, "Gryphon_Rake_Hit", t0, *p);
        bed.land(wolf, "Scorpion_TailSting_Hit", t0, *p);
    }
    bed.run(t0 + 0.05, t0 + 22.0);
    const struct {
        const char* hit;
        size_t      each;
    } expected[] = {{"Status_Hemorrhage_Tick", 10}, {"Status_Bleeding_Tick", 5}, {"Status_Poisoned_Tick", 9}};
    for (const auto& e : expected) {
        const std::vector<dots::Tick> ticks = hitchecks::peek(bed, wolf, e.hit);
        size_t by_first = 0, by_second = 0, missed = 0, took = 0;
        for (const dots::Tick& t : ticks) {
            by_first += t.by == bed.player.actor_id() ? 1 : 0;
            by_second += t.by == bed.other.actor_id() ? 1 : 0;
            missed += t.missed() ? 1 : 0;
            took += t.value < 0 ? 1 : 0;
        }
        CHECKF(by_first == e.each && by_second == e.each, "%s: %zu ticks by each player, as many as the data gives (%zu and %zu)", e.hit, e.each, by_first, by_second);
        CHECKF(missed == 0 && took == ticks.size(), "%s: none missed, every one took Health (%zu missed, %zu took of %zu)", e.hit, missed, took, ticks.size());
    }
}
AOC_REGISTER_TEST(test_hit_checks_ticks_never_miss);

// ---- a second player's debuff stacks ------------------------------------------------------------------------------------------------------------------------------------
// Blinded, Demoralized and Weakened compare sources and have a global limit of 1 (the data): that is one stack for each caster, not one on the Wolf, so a second player's application goes on
// beside the first one's, the same player's refreshes his own, each ends on its own time, and what one player put on goes when he leaves.
void test_hit_checks_a_second_players_debuffs_stack(Fixture& f) {
    std::printf("a second player's Blinded, Demoralized and Weakened go on a Wolf beside the first one's; the same player's refreshes his own\n");
    dots::Bed bed(f);
    CHECKF(bed.ok(), "the bed: %s", bed.problem().c_str());
    if (!bed.ok()) return;
    const abilities::EffectDef* blinded = f.catalog->effect_named("Status_Blinded");
    const abilities::EffectDef* demoralized = f.catalog->effect_named("Status_Demoralized");
    const abilities::EffectDef* weakened = f.catalog->effect_named("Status_Weakened");
    CHECK(blinded && demoralized && weakened && !blinded->mods.empty() && !demoralized->mods.empty(), "the three debuffs and their stat mods are in the data");
    if (!blinded || !demoralized || !weakened) return;
    CHECK(blinded->row.compare_source && demoralized->row.compare_source && weakened->row.compare_source && blinded->row.global_stack_limit == 1 && demoralized->row.global_stack_limit == 1 &&
              weakened->row.global_stack_limit == 1,
          "(the data: each compares sources and has a global limit of 1)");
    const double t0 = hitchecks::kStart;
    const int64_t accuracy = blinded->mods[0].stat_guid, damage_mult = demoralized->mods[0].stat_guid;

    // Blinded: one player's, again the same player's, then the second player's
    const uint64_t wolf = bed.wolf(0);
    CHECK(wolf != 0, "a Wolf 30 m away");
    if (!wolf) return;
    const double accuracy_none = bed.stat_of(wolf, accuracy);
    bed.land(wolf, "Beetle_Subsonics_Hit", t0, bed.player);
    const double accuracy_one = bed.stat_of(wolf, accuracy);
    CHECKF(bed.items_named(wolf, "Status_Blinded") == 1 && accuracy_one < accuracy_none - 1e-9, "the first player's Blinded lowers the accuracy (%.3f, it was %.3f)", accuracy_one, accuracy_none);
    bed.land(wolf, "Beetle_Subsonics_Hit", t0 + 1.0, bed.player);
    CHECKF(bed.items_named(wolf, "Status_Blinded") == 1 && bed.world.effect_stacks(wolf, blinded->row.guid) == 1 && near(bed.stat_of(wolf, accuracy), accuracy_one, 1e-9),
           "the same player's second Blinded refreshes his own: one instance, the accuracy as it was (%.3f)", bed.stat_of(wolf, accuracy));
    bed.land(wolf, "Beetle_Subsonics_Hit", t0 + 2.0, bed.other);
    const double accuracy_two = bed.stat_of(wolf, accuracy);
    CHECKF(bed.items_named(wolf, "Status_Blinded") == 2 && bed.world.effect_stacks(wolf, blinded->row.guid) == 2,
           "a second player's Blinded is an instance of its own beside the first one's (%zu items, %d stacks)", bed.items_named(wolf, "Status_Blinded"), bed.world.effect_stacks(wolf, blinded->row.guid));
    CHECKF(accuracy_two < accuracy_one - 1e-9 && near(accuracy_none - accuracy_two, 2.0 * (accuracy_none - accuracy_one), 1e-6), "...and the accuracy falls by both (%.3f, then %.3f from %.3f)", accuracy_one, accuracy_two, accuracy_none);
    double lasts = 0;
    for (const auto& [name, item] : bed.items(wolf))
        if (name == "Status_Blinded") lasts = item.item.duration;
    CHECKF(lasts > 1.0, "(a Blinded lasts %.1f s)", lasts);
    // the first player's, refreshed at t0 + 1 s, ends first; the second player's is left alone
    bed.run(t0 + 2.05, t0 + 1.0 + lasts + 0.5);
    CHECKF(bed.items_named(wolf, "Status_Blinded") == 1 && near(bed.stat_of(wolf, accuracy), accuracy_one, 1e-9),
           "the first player's Blinded ends on its time and the second player's is left (%zu items, accuracy %.3f)", bed.items_named(wolf, "Status_Blinded"), bed.stat_of(wolf, accuracy));
    bed.run(t0 + 1.0 + lasts + 0.55, t0 + 2.0 + lasts + 0.5);
    CHECK(bed.items_named(wolf, "Status_Blinded") == 0 && near(bed.stat_of(wolf, accuracy), accuracy_none, 1e-9), "...and the second player's ends on its own time: the accuracy is what it was");

    // Demoralized and Weakened, both from one Battle Cry each
    const uint64_t wolf2 = bed.wolf(300);
    const double mult_none = bed.stat_of(wolf2, damage_mult);
    bed.land(wolf2, "Fighter_BattleCry_Enemy", t0, bed.player);
    const double mult_one = bed.stat_of(wolf2, damage_mult);
    bed.land(wolf2, "Fighter_BattleCry_Enemy", t0 + 0.5, bed.other);
    const double mult_two = bed.stat_of(wolf2, damage_mult);
    CHECKF(bed.items_named(wolf2, "Status_Demoralized") == 2 && bed.items_named(wolf2, "Status_Weakened") == 2, "two players' Battle Cries put two Demoralized and two Weakened on the Wolf (%zu, %zu)",
           bed.items_named(wolf2, "Status_Demoralized"), bed.items_named(wolf2, "Status_Weakened"));
    CHECKF(mult_one < mult_none - 1e-9 && mult_two < mult_one - 1e-9, "...and what the Wolf deals falls by both: its damage multiplier %.3f, %.3f, %.3f", mult_none, mult_one, mult_two);

    // what a player put on goes when he leaves; the other's stays
    bed.world.remove_player(&bed.other);
    CHECKF(bed.items_named(wolf2, "Status_Demoralized") == 1 && bed.items_named(wolf2, "Status_Weakened") == 1 && near(bed.stat_of(wolf2, damage_mult), mult_one, 1e-9),
           "a player who leaves takes his Demoralized and Weakened off the Wolf and the other's stay (%zu, %zu, multiplier %.3f)", bed.items_named(wolf2, "Status_Demoralized"),
           bed.items_named(wolf2, "Status_Weakened"), bed.stat_of(wolf2, damage_mult));
    bed.world.add_player(&bed.other);
}
AOC_REGISTER_TEST(test_hit_checks_a_second_players_debuffs_stack);

// ---- two players' damage over time both tick ---------------------------------------------------------------------------------------------------------------------------
// The first player lands two Hemorrhages and three stings of poison, the second one Hemorrhage and two stings: each Hemorrhage is an instance of its own (three), each player's poison
// is one instance that grows in place (three stacks, two stacks) and ticks as its own; every tick is told to the player who put it on, at the counts the data gives, and a tick of
// three stacks is three halves of a tick of two.
void test_hit_checks_two_players_damage_over_time_tick(Fixture& f) {
    std::printf("two players' Hemorrhages and poisons on one Wolf tick together, each as its own player's, at the counts and the stack sizes the data gives\n");
    dots::Bed bed(f);
    CHECKF(bed.ok(), "the bed: %s", bed.problem().c_str());
    if (!bed.ok()) return;
    const abilities::EffectDef* hemorrhage = f.catalog->effect_named("Status_Hemorrhage");
    const abilities::EffectDef* poison = f.catalog->effect_named("Status_Poisoned");
    CHECK(hemorrhage && poison, "the Hemorrhage and the poison are in the data");
    if (!hemorrhage || !poison) return;
    const double t0 = hitchecks::kStart;
    const uint64_t wolf = bed.wolf(0);
    CHECK(wolf != 0, "a Wolf 30 m away");
    if (!wolf) return;
    bed.land(wolf, "Snipe_Hemorrhage", t0, bed.player);
    bed.land(wolf, "Snipe_Hemorrhage", t0 + 1.0, bed.player);
    bed.land(wolf, "Snipe_Hemorrhage", t0 + 0.5, bed.other);
    for (int i = 0; i < 3; ++i) bed.land(wolf, "Scorpion_TailSting_Hit", t0 + 0.1 * i, bed.player);
    for (int i = 0; i < 2; ++i) bed.land(wolf, "Scorpion_TailSting_Hit", t0 + 0.05 + 0.1 * i, bed.other);
    CHECKF(bed.items_named(wolf, "Status_Hemorrhage") == 3 && bed.world.effect_stacks(wolf, hemorrhage->row.guid) == 3, "three Hemorrhages are three instances (%zu items, %d stacks)", bed.items_named(wolf, "Status_Hemorrhage"),
           bed.world.effect_stacks(wolf, hemorrhage->row.guid));
    CHECKF(bed.items_named(wolf, "Status_Poisoned") == 2 && bed.world.effect_stacks(wolf, poison->row.guid) == 5, "two players' poisons are two instances, of three and two stacks (%zu items, %d stacks in all)",
           bed.items_named(wolf, "Status_Poisoned"), bed.world.effect_stacks(wolf, poison->row.guid));
    bed.run(t0 + 0.05, t0 + 22.0);
    size_t h_first = 0, h_second = 0, p_first = 0, p_second = 0;
    double v_first = 0, v_second = 0;
    for (const dots::Tick& t : hitchecks::peek(bed, wolf, "Status_Hemorrhage_Tick")) {
        h_first += t.by == bed.player.actor_id() ? 1 : 0;
        h_second += t.by == bed.other.actor_id() ? 1 : 0;
    }
    for (const dots::Tick& t : hitchecks::peek(bed, wolf, "Status_Poisoned_Tick")) {
        if (t.by == bed.player.actor_id()) {
            ++p_first;
            if (t.plain() && v_first == 0) v_first = t.value;
        } else if (t.by == bed.other.actor_id()) {
            ++p_second;
            if (t.plain() && v_second == 0) v_second = t.value;
        }
    }
    CHECKF(h_first == 20 && h_second == 10, "the Hemorrhages tick 10 times each: 20 told to the first player, 10 to the second (%zu, %zu)", h_first, h_second);
    CHECKF(p_first == 10 && p_second == 10, "each player's poison ticks 10 times: the last sting refreshes it, so its 20 s end after the tick at 20 s (%zu, %zu)", p_first, p_second);
    CHECKF(v_first < 0 && v_second < 0 && near(v_first / v_second, 1.5, 0.05), "...a tick of three stacks is 1.5 times a tick of two (%.4f against %.4f)", v_first, v_second);
    CHECK(bed.world.effect_count(wolf) == 0, "(everything is gone when its time is up)");
}
AOC_REGISTER_TEST(test_hit_checks_two_players_damage_over_time_tick);

// ---- a mob that resets in the middle of the debuffs ---------------------------------------------------------------------------------------------------------------------
// Two players put a Hemorrhage, a bleed and a Blinded on one Wolf; 2.2 s on it resets (the reset a broken leash makes).  The reset takes nothing off it: every tick of both players
// lands (none missed, the Wolf's evasion is raised) before, during and after the reset, and each Blinded ends on its own time.
void test_hit_checks_a_reset_leaves_the_debuffs(Fixture& f) {
    std::printf("a Wolf that resets in the middle of two players' debuffs keeps them all: every tick still lands, each debuff ends on its own time\n");
    dots::Bed bed(f);
    CHECKF(bed.ok(), "the bed: %s", bed.problem().c_str());
    if (!bed.ok()) return;
    const abilities::EffectDef* blinded = f.catalog->effect_named("Status_Blinded");
    CHECK(blinded != nullptr, "Blinded is in the data");
    if (!blinded) return;
    const double t0 = hitchecks::kStart;
    const uint64_t wolf = bed.wolf(0);
    CHECK(wolf != 0, "a Wolf 30 m away");
    if (!wolf) return;
    hitchecks::set_evasion(f, bed.world, wolf, 5.0, t0 - 1.0);
    const size_t carrier = bed.world.effect_count(wolf);   // (the effect that holds the raised evasion)
    bed.land(wolf, "Snipe_Hemorrhage", t0, bed.player);
    bed.land(wolf, "Snipe_Hemorrhage", t0, bed.other);
    bed.land(wolf, "Gryphon_Rake_Hit", t0, bed.player);
    bed.land(wolf, "Gryphon_Rake_Hit", t0, bed.other);
    bed.land(wolf, "Beetle_Subsonics_Hit", t0, bed.player);
    bed.land(wolf, "Beetle_Subsonics_Hit", t0 + 1.0, bed.other);
    CHECKF(bed.items_named(wolf, "Status_Hemorrhage") == 2 && bed.items_named(wolf, "Status_Bleeding") == 2 && bed.items_named(wolf, "Status_Blinded") == 2,
           "two Hemorrhages, two bleeds and two Blinded are on the Wolf (%zu, %zu, %zu)", bed.items_named(wolf, "Status_Hemorrhage"), bed.items_named(wolf, "Status_Bleeding"),
           bed.items_named(wolf, "Status_Blinded"));
    bed.run(t0 + 0.05, t0 + 2.2);
    const size_t effects_before = bed.world.effect_count(wolf);
    bed.world.gm_reset(wolf, t0 + 2.2);
    CHECKF(bed.world.effect_count(wolf) == effects_before && effects_before >= carrier + 6, "the reset takes no effect off the Wolf (%zu before, %zu after)", effects_before, bed.world.effect_count(wolf));
    double lasts = 0;
    for (const auto& [name, item] : bed.items(wolf))
        if (name == "Status_Blinded") lasts = item.item.duration;
    bed.run(t0 + 2.25, t0 + lasts + 0.5);
    CHECKF(bed.items_named(wolf, "Status_Blinded") == 1, "the first Blinded ends on its time, through the reset; the second one is left (%zu)", bed.items_named(wolf, "Status_Blinded"));
    bed.run(t0 + lasts + 0.55, t0 + 1.0 + lasts + 0.5);
    CHECKF(bed.items_named(wolf, "Status_Blinded") == 0 && bed.world.effect_count(wolf) == carrier, "...and then the second one's too: the Wolf holds nothing but its raised evasion (%zu effects)", bed.world.effect_count(wolf));
    size_t missed = 0, h_first = 0, h_second = 0, b_first = 0, b_second = 0;
    for (const dots::Tick& t : hitchecks::peek(bed, wolf, "Status_Hemorrhage_Tick")) {
        missed += t.missed() ? 1 : 0;
        h_first += t.by == bed.player.actor_id() ? 1 : 0;
        h_second += t.by == bed.other.actor_id() ? 1 : 0;
    }
    for (const dots::Tick& t : hitchecks::peek(bed, wolf, "Status_Bleeding_Tick")) {
        missed += t.missed() ? 1 : 0;
        b_first += t.by == bed.player.actor_id() ? 1 : 0;
        b_second += t.by == bed.other.actor_id() ? 1 : 0;
    }
    CHECKF(h_first == 10 && h_second == 10 && b_first == 5 && b_second == 5, "every tick of both players came, before, during and after the reset: 10 and 10 of the Hemorrhages, 5 and 5 of the bleeds (%zu, %zu, %zu, %zu)",
           h_first, h_second, b_first, b_second);
    CHECKF(missed == 0, "...and none of them missed (%zu missed)", missed);
}
AOC_REGISTER_TEST(test_hit_checks_a_reset_leaves_the_debuffs);

// ---- the book's rule for a limit of 1 --------------------------------------------------------------------------------------------------------------------------------------
// The effect book on its own: a record that compares sources and has a limit of 1 holds one instance for each instigator, up to the most the book allows (40); the same instigator
// refreshes his instance; and a record that compares sources with a limit above 1 still stops at its limit.
void test_hit_checks_the_book_holds_one_stack_for_each_instigator(Fixture& f) {
    std::printf("the effect book holds a limit-1 record once for each instigator, at most 40 of them, and a record with a limit of 3 or more stops at its limit\n");
    const abilities::EffectDef* blinded = f.catalog->effect_named("Status_Blinded");
    CHECK(blinded && blinded->row.compare_source && blinded->row.global_stack_limit == 1, "Blinded compares sources and has a limit of 1");
    if (!blinded) return;
    effects::Applied a;
    a.def = blinded;
    a.stacks = 1;
    a.duration = 10.0;
    effects::Book book;
    effects::Book::Changes changes;
    size_t made = 0;
    for (uint64_t instigator = 1001; instigator <= 1045; ++instigator) made += book.apply(a, instigator, 100.0, changes) ? 1 : 0;
    CHECKF(made == effects::Book::kMaxSources && book.size() == static_cast<size_t>(effects::Book::kMaxSources), "45 instigators put Blinded on one holder: %d instances are made and kept (%zu made, %zu held)",
           effects::Book::kMaxSources, made, book.size());
    effects::Book::Changes again;
    book.apply(a, 1003, 105.0, again);
    size_t mine = 0;
    double latest = 0, earliest = 1e18;
    for (const effects::Instance& e : book.instances()) {
        mine += e.instigator == 1003 ? 1 : 0;
        if (e.instigator == 1003) latest = e.end;
        if (e.instigator == 1004) earliest = e.end;
    }
    CHECKF(mine == 1 && book.size() == static_cast<size_t>(effects::Book::kMaxSources), "an instigator that is on the holder renews his own and makes no other (%zu of his, %zu held)", mine, book.size());
    CHECKF(near(latest, 115.0, 1e-9) && near(earliest, 110.0, 1e-9), "...to ten seconds from then (%.1f), the others' times as they were (%.1f)", latest, earliest);

    // a record that compares sources and has a limit above 1: Cleric_DefiantLight has 3
    const abilities::EffectDef* limited = f.catalog->effect_named("Cleric_DefiantLight");
    CHECK(limited && limited->row.compare_source && limited->row.global_stack_limit == 3, "Cleric_DefiantLight compares sources and has a limit of 3");
    if (!limited) return;
    effects::Applied b;
    b.def = limited;
    b.stacks = 1;
    b.duration = 10.0;
    effects::Book small;
    effects::Book::Changes c2;
    size_t held = 0;
    for (uint64_t instigator = 2001; instigator <= 2005; ++instigator) held += small.apply(b, instigator, 100.0, c2) ? 1 : 0;
    CHECKF(held == 3 && small.size() == 3, "five instigators put it on a holder: three instances are made and kept, as its limit says (%zu made, %zu held)", held, small.size());
}
AOC_REGISTER_TEST(test_hit_checks_the_book_holds_one_stack_for_each_instigator);

// ---- a hit that cannot miss -------------------------------------------------------------------------------------------------------------------------------------------------
// HitMeta::cannot_miss takes the miss out of the three rolls and leaves the other two: on a Wolf that nothing can hit, 200 rolls of a damaging hit all miss, and with the flag
// none misses and the critical roll is still made.
void test_hit_checks_cannot_miss(Fixture& f) {
    std::printf("a hit that cannot miss rolls no miss on a Wolf that nothing can hit and still rolls its critical hits\n");
    dots::Bed bed(f);
    CHECKF(bed.ok(), "the bed: %s", bed.problem().c_str());
    if (!bed.ok()) return;
    const abilities::HitDef* hit = f.catalog->hit_named("Ranger_Snipe_V2");
    CHECK(hit != nullptr, "an ordinary damage hit is in the data");
    const uint64_t wolf = bed.wolf(0);
    if (!hit || !wolf) return;
    // (the Wolf can be missed every time, and its critical avoidance is lowered so far that every roll that is not a miss is a critical hit)
    const combat::CombatStatIds& ids = f.data->ids();
    a3_set_stats(f, bed.world, wolf, {{ids.physical_evasion, 5.0}, {ids.magical_evasion, 5.0}, {ids.physical_crit_avoidance, -5.0}, {ids.magical_crit_avoidance, -5.0}}, hitchecks::kStart - 1.0);
    combat::Fighter* target = bed.world.fighter_of(wolf);
    size_t plain_misses = 0, flagged_misses = 0, flagged_crits = 0, flagged_mods = 0;
    for (int i = 0; i < 200; ++i) {
        for (const bool cannot : {false, true}) {
            combat::HitMeta meta;
            meta.source_type = content::kSourceTypeAbility;
            meta.cannot_miss = cannot;
            combat::Resolved r;
            std::string why;
            if (!bed.world.resolver().resolve(bed.player.fighter(), *target, *hit, meta, r, why)) continue;
            if (!cannot) {
                plain_misses += r.miss() ? 1 : 0;
            } else {
                flagged_misses += r.miss() ? 1 : 0;
                flagged_crits += (r.flags & stats::kHitCrit) != 0 ? 1 : 0;
                flagged_mods += r.mods.empty() ? 0 : 1;
            }
        }
    }
    CHECKF(plain_misses == 200, "without the flag every one of 200 rolls misses this Wolf (%zu)", plain_misses);
    CHECKF(flagged_misses == 0 && flagged_mods == 200, "with the flag none misses and every roll works out its damage (%zu missed, %zu with damage)", flagged_misses, flagged_mods);
    CHECKF(flagged_crits == 200, "...and the critical roll is still made: every roll is a critical hit against this Wolf (%zu of 200)", flagged_crits);
}
AOC_REGISTER_TEST(test_hit_checks_cannot_miss);

// ---- one conversion, not two -----------------------------------------------------------------------------------------------------------------------------------------------
// The Snipe's damage hit carries a Hemorrhage of its own that goes on a bleeding target, and its Hemorrhage hit converts a bleeding target too: the data lists both.  When the client
// asks for both in one cast, in either order, a bleeding Wolf takes ONE Hemorrhage: the first hit that lands turns the bleed into it and the second finds nothing left to convert.
void test_hit_checks_the_snipe_converts_once(Fixture& f) {
    std::printf("a Snipe that asks for its damage hit and its Hemorrhage hit turns a bleed into one Hemorrhage, in either order\n");
    const std::string wolf_path = wolf_archetype(f);
    const abilities::EffectDef* bleeding = f.catalog->effect_named("Status_Bleeding");
    const abilities::EffectDef* hemorrhage = f.catalog->effect_named("Status_Hemorrhage");
    CHECK(bleeding && hemorrhage && f.catalog->hit_named("Ranger_Snipe_V2") && !wolf_path.empty(), "the data has the bleed, the Hemorrhage and the Snipe's damage hit");
    if (!bleeding || !hemorrhage || wolf_path.empty()) return;
    for (int order = 0; order < 2; ++order) {
        BardMobWorld bw(f, wolf_path, 1);
        CHECKF(bw.problem.empty(), "the Ranger and the world: %s", bw.problem.c_str());
        if (!bw.problem.empty()) return;
        bw.rig.know({"Ranger_Snipe"});
        const uint64_t wolf = bw.w.spawn(wolf_path, 600, 0, 8, 0);
        if (!wolf) return;
        hitchecks::make_bleed(bw, wolf, hitchecks::kStart - 1.0, *bleeding);
        const double before = bw.world->health_of(wolf);
        const std::vector<const char*> asks = order == 0 ? std::vector<const char*>{"Ranger_Snipe_V2", "Snipe_Hemorrhage"} : std::vector<const char*>{"Snipe_Hemorrhage", "Ranger_Snipe_V2"};
        const BardCast cast = hitchecks::cast_asking(bw, "Ranger_Snipe", {wolf}, asks, hitchecks::kStart, hitchecks::kStart + 9.0);
        CHECKF(cast.cast && cast.requested, "order %d: the Snipe is cast and both requests are accepted (cast %d, requests %d)", order, cast.cast ? 1 : 0, cast.requested ? 1 : 0);
        CHECKF(bw.world->health_of(wolf) < before, "order %d: the Snipe's damage lands (%.1f of %.1f)", order, bw.world->health_of(wolf), before);
        CHECKF(bw.world->effect_stacks(wolf, bleeding->row.guid) == 0, "order %d: the bleed is gone (%d stacks)", order, bw.world->effect_stacks(wolf, bleeding->row.guid));
        const std::vector<combat::EffectHitNote> ticks = hitchecks::notes_of(bw, wolf, f.catalog->hit_named("Status_Hemorrhage_Tick")->row.guid);
        CHECKF(ticks.size() == 10, "order %d: one Hemorrhage was made: ten ticks in its five seconds, not twenty (%zu)", order, ticks.size());
    }
}
AOC_REGISTER_TEST(test_hit_checks_the_snipe_converts_once);


// ---- a second player's root --------------------------------------------------------------------------------------------------------------------------------------------------
// Rooted is the fifth record the limit-1 rule reaches that players put on mobs, and it holds a mob's feet: two players' roots are two instances, the Wolf stays rooted while either
// lasts, and when the first player leaves it is still rooted by the second one's.
void test_hit_checks_a_second_players_root_holds(Fixture& f) {
    std::printf("two players' roots on one Wolf are two instances: the Wolf stays rooted when the first player leaves and is free when the second one's ends\n");
    dots::Bed bed(f);
    CHECKF(bed.ok(), "the bed: %s", bed.problem().c_str());
    if (!bed.ok()) return;
    const abilities::EffectDef* rooted = f.catalog->effect_named("Status_Rooted");
    CHECK(rooted && rooted->row.compare_source && rooted->row.global_stack_limit == 1 && static_cast<bool>(rooted->cc), "Rooted compares sources, has a limit of 1 and a crowd control record");
    if (!rooted || !static_cast<bool>(rooted->cc)) return;
    const double t0 = hitchecks::kStart;
    const uint64_t wolf = bed.wolf(0);
    CHECK(wolf != 0, "a Wolf 30 m away");
    if (!wolf) return;
    const auto flags = [&]() -> int64_t {
        const world::WorldActor* a = bed.w.registry.find(wolf);
        return a ? cc_now(*a).flags : -1;
    };
    CHECKF((flags() & content::kCcRoot) == 0, "(the Wolf is free to start with: flags %lld)", static_cast<long long>(flags()));
    bed.land(wolf, "Ranger_BearTrap_Root", t0, bed.player);
    CHECKF(bed.items_named(wolf, "Status_Rooted") == 1 && (flags() & content::kCcRoot) != 0, "the first player's root holds the Wolf (%zu items, flags %lld)", bed.items_named(wolf, "Status_Rooted"),
           static_cast<long long>(flags()));
    double lasts = 0;
    for (const auto& [name, item] : bed.items(wolf))
        if (name == "Status_Rooted") lasts = item.item.duration;
    CHECKF(lasts > 2.0, "(a root lasts %.1f s)", lasts);
    bed.run(t0 + 0.05, t0 + 1.0);
    bed.land(wolf, "Ranger_BearTrap_Root", t0 + 1.0, bed.other);
    CHECKF(bed.items_named(wolf, "Status_Rooted") == 2, "a second player's root is an instance of its own beside the first one's (%zu items)", bed.items_named(wolf, "Status_Rooted"));
    bed.world.remove_player(&bed.player);
    CHECKF(bed.items_named(wolf, "Status_Rooted") == 1 && (flags() & content::kCcRoot) != 0, "the first player leaves: his root goes and the Wolf is still held by the other's (%zu items, flags %lld)",
           bed.items_named(wolf, "Status_Rooted"), static_cast<long long>(flags()));
    bed.run(t0 + 1.05, t0 + 1.0 + lasts - 0.1);
    CHECKF((flags() & content::kCcRoot) != 0, "...until the second root's own time is nearly up (flags %lld)", static_cast<long long>(flags()));
    bed.run(t0 + 1.0 + lasts - 0.05, t0 + 1.0 + lasts + 0.5);
    CHECKF(bed.items_named(wolf, "Status_Rooted") == 0 && (flags() & content::kCcRoot) == 0, "...and then the Wolf is free (%zu items, flags %lld)", bed.items_named(wolf, "Status_Rooted"),
           static_cast<long long>(flags()));
    bed.world.add_player(&bed.player);
}
AOC_REGISTER_TEST(test_hit_checks_a_second_players_root_holds);




// ---- the classes' own gated hits ------------------------------------------------------------------------------------------------------------------------------------------------
// Hits of the classes whose requirement asks what the target holds, from their real abilities: two Wolves stand in the way, one holds the effect the requirement counts and one does not,
// and the client's request names both.  Each hit lands on the Wolf its requirement holds for and on no other, as its data says: the Ground Pound's Trip on the Wolf that is not tripped
// and its Launch on the tripped one, the Blizzard's freeze on the chilled one and the Air Strike's volley on the Wolf that is not in its internal cooldown (its volleys repeat: every one of them
// lands on that Wolf).
void test_hit_checks_the_classes_gated_hits(Fixture& f) {
    std::printf("hits of the Tank, the Mage and the Ranger that ask what the target holds land on the Wolf of two it holds for and on no other\n");
    const std::string wolf_path = wolf_archetype(f);
    struct Row {
        const char* ability;
        int         class_enum;
        const char* hit;
        const char* effect;     // what one of the two Wolves holds
        bool        holder;     // the hit lands on the Wolf that holds it (false: on the one that does not)
    };
    const Row rows[] = {
        {"Tank_GroundPound", 7, "Tank_GroundPound_Trip_Hit", "Status_Tripped", false},
        {"Tank_GroundPound", 7, "Tank_GroundPound_Launch Hit", "Status_Tripped", true},
        {"Mage_Blizzard", 8, "Mage_Status_Chilled->Frozen", "Status_Chilled", true},
        {"Ranger_AirStrike", 1, "Ranger_AirStrike_AoE", "Ranger_AirStrike_ICD", false},
    };
    for (const Row& r : rows) {
        const abilities::HitDef* hit = f.catalog->hit_named(r.hit);
        const abilities::EffectDef* held = f.catalog->effect_named(r.effect);
        CHECKF(hit != nullptr && held != nullptr && f.catalog->ability_named(r.ability) != nullptr, "%s: the ability, its hit %s and the effect %s are in the data", r.ability, r.hit, r.effect);
        if (!hit || !held) continue;
        const content::ExprRow* req = f.catalog->expr(hit->row.requirement_expr);
        CHECKF(req && req->uses_target && req->program >= 0, "%s: the hit's requirement reads its target", r.hit);
        BardMobWorld bw(f, wolf_path, r.class_enum);
        CHECKF(bw.problem.empty(), "%s: the character and the world: %s", r.ability, bw.problem.c_str());
        if (!bw.problem.empty()) continue;
        bw.rig.know({r.ability});
        if (r.class_enum == 7) bw.rig.set_current(6064629429438907308ll, 100.0);   // (the Tank's Courage: the Ground Pound asks for 20)
        const uint64_t with = bw.w.spawn(wolf_path, 300, -100, 8, 0);
        const uint64_t without = bw.w.spawn(wolf_path, 300, 100, 8, 0);
        if (with == 0 || without == 0) continue;
        effects::Applied put;
        put.def = held;
        put.duration = 600.0;
        bw.world->apply_effects(with, 77, {put}, hitchecks::kStart - 1.0);
        CHECKF(bw.world->effect_stacks(with, held->row.guid) == 1, "%s: the first Wolf holds %s (%d stacks)", r.hit, r.effect, bw.world->effect_stacks(with, held->row.guid));
        const BardCast cast = hitchecks::cast_at(bw, r.ability, {}, {with, without}, r.hit, hitchecks::kStart, hitchecks::kStart + 5.0);
        CHECKF(cast.cast && cast.requested, "%s: the cast and the client's request for %s are accepted (cast %d, request %d)", r.ability, r.hit, cast.cast ? 1 : 0, cast.requested ? 1 : 0);
        if (!cast.cast) {
            for (const Seen& s : bw.rig.take())
                if (s.refusal) std::printf("    %s\n", s.note.c_str());
        }
        const std::vector<hitchecks::Told> told = hitchecks::told(cast.seen, hit->row.guid);
        const uint64_t expected = r.holder ? with : without;
        size_t others = 0;
        for (const hitchecks::Told& t : told) others += t.actor == expected ? 0 : 1;
        CHECKF(!told.empty() && others == 0, "%s: the client is told of the Wolf that %s %s and of no other (%zu targets, %zu of them the other Wolf)", r.hit, r.holder ? "holds" : "does not hold", r.effect, told.size(), others);
    }
}
AOC_REGISTER_TEST(test_hit_checks_the_classes_gated_hits);
