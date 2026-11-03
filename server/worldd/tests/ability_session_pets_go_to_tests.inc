// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/ability_session_pets_go_to_tests.inc: a Summoner's pet in a real session: Go To within the ability's range and beyond it; a pet command only when its ability was taken;
//  the leash, which brings a pet farther than 40 m from its owner back in every mode
//  (registered with AOC_REGISTER_TEST: test_worldd_ability_session.cpp runs it through aoc_tests::run; it is included from ability_session_registered.inc)
// ============================================================================

/// A ServerSendPetCommand: the command, and the place it names (or none).
static BunchWriter pet_command_bunch(int64_t command, const repl::Vec3d* place) {
    const auto& pawn = class_named("PlayerPawn_C");
    const auto* rpc = pawn.rpc("ServerSendPetCommand");
    repl::RpcParams params;
    bool ok = rpc && repl::set_rpc_param(*rpc, params, "PetCommandId", repl::Value::i64(command));
    if (ok && place) {
        // the whole struct, as the client sends it: no target (its four words are 0) and the place
        for (const char* word : {"TargetData.TargetGuid.A", "TargetData.TargetGuid.B", "TargetData.TargetGuid.C", "TargetData.TargetGuid.D"}) ok = ok && repl::set_rpc_param(*rpc, params, word, repl::Value::i64(0));
        ok = ok && repl::set_rpc_param(*rpc, params, "TargetData.TargetLocation", repl::Value(*place)) && repl::set_rpc_param(*rpc, params, "TargetData.bHasLocation", repl::Value::boolean(true));
    }
    if (!ok) {
        std::printf("  [FAIL] the pet command's parameters do not fit\n");
        return BunchWriter();
    }
    return actor_bunch(pawn, "ServerSendPetCommand", params);
}

/// A walking map under the player: flat ground round the place the session tests stand at, so that a pet can walk there.
static std::unique_ptr<nav::AreaNav> ground_under_the_player() {
    nav::Geometry g = navfix::test_geometry();
    g.placements.clear();   // open ground only
    g.center_x = kX;
    g.center_y = kY;
    for (nav::TerrainGrid& t : g.terrain) {
        t.x0 += kX;
        t.y0 += kY;
        t.z_base = kZ - 88.0;
        std::fill(t.raw.begin(), t.raw.end(), static_cast<uint16_t>(32768));
    }
    nav::BuildOptions opt;
    opt.threads = 2;
    opt.nav_margin_cm = 3000;
    nav::NavFile file;
    nav::BuildStats stats;
    std::string err;
    if (!nav::build_nav(g, 7, opt, &file, &stats, &err)) {
        std::printf("  [FAIL] the ground under the player: %s\n", err.c_str());
        return nullptr;
    }
    return nav::AreaNav::from_file(file, &err);
}

void test_summoner_pet_go_to_and_leash() {
    std::printf("Summoner pets: Go To within the ability's range, the commands of a refused ability, and the leash in every mode\n");
    const svc::Character rec = class25(6, "Summoner", 17753);
    bool ground = false;
    Run r(0, true, &rec, [&](PlayerWorld& world) {
        std::string error;
        CHECK(world.load_blueprints(NPC_MOTION_PATH, error), "the pet Blueprints load");
        auto nav = ground_under_the_player();
        ground = nav && world.adopt_ground(std::move(nav), error);
        CHECK(ground, "the ground under the player is adopted");
        spawns::AreaConfig area;
        area.radius_cm = 1;
        CHECK(world.load_area(area, error), "the world's actor templates load");
    });
    CHECK(ground && r.h.fx.world->npcs() != nullptr, "(a world with ground under the player)");
    if (!ground || !r.h.fx.world->npcs()) return;
    npc::NpcWorld& npcs = *r.h.fx.world->npcs();
    // (the harness ticks the session only: the creatures' walks need the world's tick too)
    const auto play = [&](double seconds) {
        for (int i = 0, n = static_cast<int>(std::llround(seconds * 10)); i < n; ++i) {
            r.tick(false);
            r.h.fx.world->tick_world(r.now());
        }
    };
    const auto walk = [&](double cm_per_s, int steps) {
        for (int i = 0; i < steps; ++i) {
            r.tick_at(cm_per_s);
            r.h.fx.world->tick_world(r.now());
        }
    };
    const auto bytes = capture_blocks::bytes_of(capture_blocks::kClientReadyToPhaseIn.hex);
    BunchWriter phase;
    phase.write_bit_range(bytes.data(), 0, capture_blocks::kClientReadyToPhaseIn.bits);
    r.send_reliable(phase);
    play(1.0);
    const pets::Definition& guardian = pets::kDefinitions[0];
    r.h.s->teach_ability(guardian.ability);
    r.h.s->set_current_value(kManaStat, r.m.stats[kMaxManaStat]);
    r.trigger(guardian.ability);
    play(8.8);
    const uint64_t pet = r.h.s->summoned_pet();
    CHECK(pet != 0, "the Guardian comes");
    if (!pet) return;
    const auto where = [&]() -> std::pair<double, double> {
        const world::WorldActor* a = r.h.fx.world->registry().find(pet);
        return a ? std::make_pair(a->x, a->y) : std::make_pair(0.0, 0.0);
    };
    const auto metres_between = [](std::pair<double, double> a, std::pair<double, double> b) { return std::hypot(a.first - b.first, a.second - b.second) / 100.0; };
    const auto owner_at = [&]() { return std::make_pair(r.x, kY); };
    play(3.0);   // (it walks up to its owner)
    CHECK(r.h.s->pet_following() && metres_between(where(), owner_at()) < 6.0, "the pet follows its owner and stands near");

    // Go To names a place 400 m away: the ability reaches 40 m.  The pet goes on as it was, and no path is searched for.
    const repl::Vec3d far_place{kX, kY + 40000.0, kZ - 88.0};
    const uint64_t moves = npcs.stats().moves_started;
    r.send_reliable(pet_command_bunch(pets::kGoTo, &far_place));
    play(0.3);
    CHECK(r.h.s->pet_following() && npcs.stats().moves_started == moves, "Go To 400 m away: refused, the pet still follows and no walk is started");
    const repl::Vec3d nowhere{std::nan(""), kY, kZ - 88.0};
    r.send_reliable(pet_command_bunch(pets::kGoTo, &nowhere));
    play(0.3);
    CHECK(r.h.s->pet_following() && npcs.stats().moves_started == moves, "Go To a place that is no number: refused too");

    // Go To names a place 30 m away: the pet walks there and no longer follows
    const repl::Vec3d near_place{kX, kY + 3000.0, kZ - 88.0};
    r.send_reliable(pet_command_bunch(pets::kGoTo, &near_place));
    play(0.3);
    CHECK(!r.h.s->pet_following() && npcs.stats().moves_started == moves + 1, "Go To 30 m away: the pet sets off, and no longer follows");
    play(6.0);
    CHECK(metres_between(where(), {kX, kY + 3000.0}) < 3.0, "... and it stands at the place");
    CHECK(!r.h.s->pet_following(), "... where it stays");

    // the owner walks 45 m the other way: the pet is then farther than 40 m from it, and comes back to follow
    const auto stood = where();
    walk(500.0, 90);
    CHECK(metres_between(stood, owner_at()) > 40.0, "(the owner is more than 40 m from where the pet was sent)");
    play(1.0);
    CHECK(r.h.s->pet_following() && metres_between(where(), owner_at()) < 10.0, "the leash brings the sent pet back to its owner, and it follows again");

    // Stay: within 40 m the pet stays where it is
    r.send_reliable(pet_command_bunch(pets::kStay, nullptr));
    play(0.5);
    CHECK(!r.h.s->pet_following(), "Stay: the pet no longer follows");
    const auto stays = where();
    walk(500.0, 60);
    CHECK(metres_between(stays, owner_at()) > 25.0 && metres_between(stays, owner_at()) < 40.0, "(the owner walked 30 m from the stayed pet)");
    CHECK(!r.h.s->pet_following() && metres_between(where(), stays) < 1.0, "within 40 m a stayed pet stays where it is");
    // ... and beyond 40 m it comes back
    walk(500.0, 30);
    play(1.0);
    CHECK(metres_between(stays, owner_at()) > 40.0, "(the owner is more than 40 m from the stayed pet)");
    CHECK(r.h.s->pet_following() && metres_between(where(), owner_at()) < 10.0, "the leash brings the stayed pet back to its owner, and it follows again");
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "all owner replication updates read successfully");
}
AOC_REGISTER_TEST(test_summoner_pet_go_to_and_leash);

void test_summoner_pet_command_needs_its_ability() {
    std::printf("Summoner pets: the pet obeys a command only when the pipeline took the ability\n");
    const svc::Character rec = class25(6, "Summoner", 17753);
    Run r(0, true, &rec, [&](PlayerWorld& world) {
        std::string error;
        CHECK(world.load_blueprints(NPC_MOTION_PATH, error), "the pet Blueprints load");
        spawns::AreaConfig area;
        area.radius_cm = 1;
        CHECK(world.load_area(area, error), "the world's actor templates load");
    });
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
    CHECK(r.h.s->summoned_pet() != 0 && r.h.s->pet_following(), "the Guardian comes, and follows");
    constexpr int64_t kStayAbility = 6064633576956297816ll, kFollowAbility = 6064633576846066210ll;
    // (the commands share a cooldown that runs from 0.25 s to 0.75 s after a cast, the cost step and the half second the record gives: each is cast after the one before it has run its course)
    const int32_t first_stay = r.trigger(kStayAbility);
    r.play(1.5, false);
    CHECK(!r.h.s->pet_following(), "Stay, taken: the pet no longer follows");
    r.trigger(kFollowAbility);
    r.play(1.5, false);
    CHECK(r.h.s->pet_following(), "Follow, taken: it follows again");
    r.send_trigger(kStayAbility, first_stay);   // (the same ClientInstanceId again: the pipeline drops it as a replay)
    r.play(1.5, false);
    CHECK(r.h.s->pet_following(), "Stay, refused as a replay: the pet is not told to stay");
    r.trigger(kStayAbility);
    r.play(1.5, false);
    CHECK(!r.h.s->pet_following(), "Stay, taken again: the pet stays");
    // a command cast while that cooldown runs (it starts with the Stay's cost step, a moment after the trigger) is refused by the pipeline, and the pet is not told
    r.trigger(kFollowAbility);
    r.play(1.5, false);
    CHECK(r.h.s->pet_following(), "(it follows)");
    r.trigger(kStayAbility);
    r.play(0.5, false);
    CHECK(!r.h.s->pet_following(), "Stay, taken: the pet stays");
    r.trigger(kFollowAbility);   // (the Stay's cooldown is running now)
    r.play(0.2, false);
    CHECK(!r.h.s->pet_following(), "Follow, refused on the commands' cooldown: the pet is not told to follow");
}
AOC_REGISTER_TEST(test_summoner_pet_command_needs_its_ability);
