// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/ability_session_pets_target_tests.inc: a Summoner's pet in a real session: a cast at the wrong side or at what is not there pays nothing and starts no cooldown;
//  the abilities a character has not learned; the auto mode's toggle
//  (registered with AOC_REGISTER_TEST: test_worldd_ability_session.cpp runs it through aoc_tests::run; it is included from ability_session_registered.inc)
// ============================================================================

/// A ServerTriggerAbility that names one actor by its NetGUID (what the client sends for an ability aimed at an actor), or none.
static BunchWriter trigger_naming(const bs::PlayerSetup& setup, int64_t guid, int32_t id, const emit::FIntrepidNetworkGUID* target) {
    const schema::RpcSchema* rpc = class_named("UAoCAbilityComponent").rpc("ServerTriggerAbility");
    repl::RpcParams p;
    bool ok = rpc && repl::set_rpc_param(*rpc, p, "AbilityGuid", repl::Value::i64(guid));
    if (ok && id != 0) ok = repl::set_rpc_param(*rpc, p, "ClientInstanceId", repl::Value::i64(id));
    ok = ok && repl::set_rpc_param(*rpc, p, "InputVector", repl::Value(repl::Vec2d{1, 0}));
    if (ok && target) {
        repl::ArrayValue targets;
        repl::ElementState e;
        e.values[1] = repl::Value(*target);
        targets.elements.push_back(std::move(e));
        ok = repl::set_rpc_param(*rpc, p, "Targets", repl::Value(std::move(targets)));
    }
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Slot", repl::Value::i64(-1));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.ItemInstance", repl::Value(repl::ItemInstance{}));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Location", repl::Value::u64(24));
    if (!ok) {
        std::printf("  [FAIL] the trigger's parameters do not fit\n");
        return BunchWriter();
    }
    return ability_rpc(setup.guids.ability, "ServerTriggerAbility", p);
}

/// The session's pipeline, to change what the character has learned (the session lends it out read-only: a test takes the const off).
static AbilityPipeline& pipeline_of(Run& r) { return *const_cast<AbilityPipeline*>(r.h.s->pipeline()); }

/// `seconds` of play, 100 ms at a time, in which the world ticks as well: the fights and the creatures' walks (the harness ticks the session only).
static void play_with_world(Run& r, double seconds) {
    const int n = static_cast<int>(std::llround(seconds * 10));
    for (int i = 0; i < n; ++i) {
        r.tick(false);
        r.h.fx.world->tick_world(r.now());
    }
}

/// How many cooldown items of the ability the client holds.
static size_t cooldown_items_of(const Run& r, int64_t ability) {
    size_t n = 0;
    for (const auto& item : r.m.cooldowns) n += item.second == ability ? 1 : 0;
    return n;
}

/// The world of these tests: ground, two Wolves that attack nobody (one to be killed) and a questgiver, all beside the player.
struct PetTargetWorld {
    uint64_t wolf = 0, second_wolf = 0, friendly = 0;
    uint64_t next_actor_id = 5000000;
    bool     ground = false;
    void prepare(PlayerWorld& w) {
        std::string err;
        if (!w.load_blueprints(NPC_MOTION_PATH, err)) {
            std::printf("  [FAIL] the Blueprint table: %s\n", err.c_str());
            return;
        }
        auto nav = nav::AreaNav::from_file(navfix::build_file(), &err);
        ground = nav && w.adopt_ground(std::move(nav), err);
        if (!ground) {
            std::printf("  [FAIL] the ground: %s\n", err.c_str());
            return;
        }
        spawns::AreaConfig area;
        area.radius_cm = 1;
        if (!w.load_area(area, err)) std::printf("  [FAIL] the world's actor templates: %s\n", err.c_str());
        auto captured = spawns::CapturedData::load(err);
        if (!captured) {
            std::printf("  [FAIL] the captured data: %s\n", err.c_str());
            return;
        }
        std::string wolf_path, friend_path;
        for (const auto& [p, row] : w.blueprints().rows()) {
            if (p.find("/Beast/Canine/Wolf.Default__Wolf_C") != std::string::npos) wolf_path = p;
            if (p.find("Quest_NPC_Master.Default__Quest_NPC_Master_C") != std::string::npos) friend_path = p;
        }
        wolf = combat_rig::spawn_npc(w.registry(), w.npcs(), *captured, next_actor_id, wolf_path, kX + 120, kY, kZ, 8, 0);
        second_wolf = combat_rig::spawn_npc(w.registry(), w.npcs(), *captured, next_actor_id, wolf_path, kX + 120, kY + 100, kZ, 8, 0);
        if (!friend_path.empty()) friendly = combat_rig::spawn_npc(w.registry(), w.npcs(), *captured, next_actor_id, friend_path, kX + 200, kY, kZ, 8, 0);
    }
    bool ok() const { return ground && wolf != 0 && second_wolf != 0 && friendly != 0; }
};

void test_summoner_pet_ability_targets() {
    std::printf("Summoner pets: a cast at the wrong side or at what is not there costs nothing\n");
    const svc::Character rec = class25(6, "Summoner", 17753);
    PetTargetWorld pw;
    Run r(0, true, &rec, [&](PlayerWorld& world) { pw.prepare(world); });
    CHECK(pw.ok() && r.h.fx.world->combat() != nullptr, "(a world with fights, two Wolves and a friendly questgiver)");
    if (!pw.ok() || !r.h.fx.world->combat()) return;
    combat::CombatWorld& cw = *r.h.fx.world->combat();
    const auto bytes = capture_blocks::bytes_of(capture_blocks::kClientReadyToPhaseIn.hex);
    BunchWriter phase;
    phase.write_bit_range(bytes.data(), 0, capture_blocks::kClientReadyToPhaseIn.bits);
    r.send_reliable(phase);
    r.play(1.0, false);
    const pets::Definition& guardian = pets::kDefinitions[0];
    r.h.s->teach_ability(guardian.ability);
    r.h.s->set_current_value(kManaStat, r.m.stats[kMaxManaStat]);
    r.trigger(guardian.ability);
    r.play(8.8, false);
    const uint64_t pet = r.h.s->summoned_pet();
    CHECK(pet != 0, "the Guardian comes");
    if (!pet) return;
    std::string why;
    CHECK(cw.gm_kill(pw.second_wolf, 1.0, why), "(the second Wolf is dead)");
    const auto guid_of = [&](uint64_t actor) {
        const world::WorldActor* a = r.h.fx.world->registry().find(actor);
        return a ? a->actor.guid : emit::FIntrepidNetworkGUID{};
    };
    const emit::FIntrepidNetworkGUID wolf_guid = guid_of(pw.wolf), friend_guid = guid_of(pw.friendly), dead_guid = guid_of(pw.second_wolf);
    emit::FIntrepidNetworkGUID missing_guid = wolf_guid;
    missing_guid.ObjectId = 987654321;
    const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
    const int64_t taunt = pets::kGuardianTaunt;
    const int64_t stomp = 6064633577427633586ll;
    r.h.s->set_current_value(pets::kEssence, 100.0f);
    r.play(0.2, false);
    const float full = r.m.stats[pets::kEssence];

    // a refused cast: nothing is paid, no cooldown starts, nothing is made of the creature it named, and the pet starts nothing
    const auto refused_at = [&](const char* what, const emit::FIntrepidNetworkGUID* named) {
        r.send_reliable(trigger_naming(setup, taunt, r.next_id++, named));
        r.play(0.3, false);
        CHECKF(std::abs(r.m.stats[pets::kEssence] - full) < 1.0f, "Taunt at %s costs no Essence (%.1f of %.1f left)", what, r.m.stats[pets::kEssence], full);
        CHECKF(cooldown_items_of(r, taunt) == 0 && !cw.pet_casting(pet), "Taunt at %s starts no cooldown and no cast", what);
    };
    refused_at("a friendly questgiver", &friend_guid);
    CHECK(cw.health_of(pw.friendly) == -1.0, "... and makes no combatant of the questgiver");
    refused_at("a dead Wolf", &dead_guid);
    refused_at("an actor that is not there", &missing_guid);
    refused_at("the caster itself", &setup.guids.collision_cylinder);

    // after those, an accepted cast: the Wolf is taunted, the Essence is paid once, the cooldown starts once (the refused casts left nothing in its way)
    r.send_reliable(trigger_naming(setup, taunt, r.next_id++, &wolf_guid));
    r.play(0.3, false);
    CHECK(std::abs(r.m.stats[pets::kEssence] - (full - 15.0f)) < 1.5f, "Taunt at a hostile Wolf takes its 15 Essence, once");
    CHECK(cooldown_items_of(r, taunt) == 1, "... and starts its cooldown, once");
    for (int i = 0; i < 100 && cw.pet_casting(pet); ++i) play_with_world(r, 0.1);
    std::printf("  [info] after Taunt: the pet casts %d, the Wolf holds %zu effect(s), Demoralized %d, Health %.0f of %.0f\n", cw.pet_casting(pet) ? 1 : 0, cw.effect_count(pw.wolf),
                cw.effect_stacks(pw.wolf, 6064630758760448234ll), cw.health_of(pw.wolf), cw.max_health_of(pw.wolf));
    CHECK(cw.effect_stacks(pw.wolf, 6064630758760448234ll) == 1, "... and the Wolf is demoralized");
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "all owner replication updates read successfully");

    // an ability the character has not learned is not played, whatever the pet could do: the pipeline's own gate
    r.h.s->set_current_value(pets::kEssence, 100.0f);
    r.play(0.2, false);
    pipeline_of(r).set_known({guardian.ability});
    const float before_unknown = r.m.stats[pets::kEssence];
    r.trigger(stomp);
    r.play(0.3, false);
    CHECK(std::abs(r.m.stats[pets::kEssence] - before_unknown) < 1.0f && cooldown_items_of(r, stomp) == 0, "Stomp, not learned, costs nothing and starts no cooldown");
    pipeline_of(r).set_known({guardian.ability, stomp});
    r.trigger(stomp);
    r.play(0.3, false);
    CHECK(std::abs(r.m.stats[pets::kEssence] - (before_unknown - 30.0f)) < 1.5f, "... and learned, it takes its 30 Essence");
}
AOC_REGISTER_TEST(test_summoner_pet_ability_targets);

void test_summoner_pet_healing_targets() {
    std::printf("Summoner pets: the Mystic's help reaches its owner, not an enemy\n");
    const svc::Character rec = class25(6, "Summoner", 17753);
    PetTargetWorld pw;
    Run r(0, true, &rec, [&](PlayerWorld& world) { pw.prepare(world); });
    CHECK(pw.ok() && r.h.fx.world->combat() != nullptr, "(a world with fights, two Wolves and a friendly questgiver)");
    if (!pw.ok() || !r.h.fx.world->combat()) return;
    const auto bytes = capture_blocks::bytes_of(capture_blocks::kClientReadyToPhaseIn.hex);
    BunchWriter phase;
    phase.write_bit_range(bytes.data(), 0, capture_blocks::kClientReadyToPhaseIn.bits);
    r.send_reliable(phase);
    r.play(1.0, false);
    const pets::Definition& mystic = pets::kDefinitions[2];
    r.h.s->teach_ability(mystic.ability);
    r.h.s->set_current_value(kManaStat, r.m.stats[kMaxManaStat]);
    r.trigger(mystic.ability);
    r.play(8.8, false);
    CHECK(r.h.s->pet_definition() == &mystic, "the Mystic comes");
    const world::WorldActor* wolf = r.h.fx.world->registry().find(pw.wolf);
    CHECK(wolf != nullptr, "(the Wolf is in the world)");
    if (!wolf) return;
    const emit::FIntrepidNetworkGUID wolf_guid = wolf->actor.guid;
    const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
    const int64_t recreate = 6064633577432614330ll;
    r.h.s->set_current_value(pets::kEssence, 100.0f);
    r.play(0.2, false);
    const float full = r.m.stats[pets::kEssence];
    // Recreate named at an enemy: refused, nothing paid
    r.send_reliable(trigger_naming(setup, recreate, r.next_id++, &wolf_guid));
    r.play(0.3, false);
    CHECK(std::abs(r.m.stats[pets::kEssence] - full) < 1.0f && cooldown_items_of(r, recreate) == 0, "Recreate at an enemy costs no Essence and starts no cooldown");
    // named at the caster, as the client names it for a heal on itself: played, and paid once
    r.send_reliable(trigger_naming(setup, recreate, r.next_id++, &setup.guids.collision_cylinder));
    r.play(0.3, false);
    CHECK(std::abs(r.m.stats[pets::kEssence] - (full - 20.0f)) < 1.5f, "Recreate at its owner takes its 20 Essence, once");
}
AOC_REGISTER_TEST(test_summoner_pet_healing_targets);

void test_summoner_pet_automation_needs_the_ability() {
    std::printf("Summoner pets: the auto mode's toggle is for a character that has learned it\n");
    {
        const svc::Character mage = class25(8, "Mage", 17750);   // no Summoner: it has no such ability, and no pet
        Run r(0, true, &mage);
        r.play(0.5, false);
        CHECK(r.h.s->pipeline() && !r.h.s->pipeline()->known(pets::kAutomationAbility), "(a Mage has not learned the toggle)");
        r.trigger(pets::kAutomationAbility);
        r.play(0.3, false);
        CHECK(!r.h.s->pet_automation() && !r.m.has_effect(pets::kAutomationEffect), "a Mage cannot turn the pet's auto mode on");
    }
    {
        const svc::Character rec = class25(6, "Summoner", 17753);
        Run r(0, true, &rec);
        r.play(0.5, false);
        CHECK(r.h.s->pipeline() && r.h.s->pipeline()->known(pets::kAutomationAbility), "(a Summoner knows the toggle without being taught)");
        r.trigger(pets::kAutomationAbility);
        r.play(0.3, false);
        CHECK(r.h.s->pet_automation() && r.m.has_effect(pets::kAutomationEffect), "a Summoner turns it on");
    }
}
AOC_REGISTER_TEST(test_summoner_pet_automation_needs_the_ability);
