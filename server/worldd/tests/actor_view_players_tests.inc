// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/actor_view_players_tests.inc: which actors a player's view is sent (players seeing each other)
//
//  A view is sent the actors of nobody (owner 0: the NPCs, a summoned pet among them), this player's own zones and mount, and another player's Player actor (its pawn); it is never sent
//  this player's own Player actor (the session has its own pawn on channels of its own), nor another player's zones or mount.  A Player actor is never dormant: it is open within the
//  awake ring, closed with Relevancy beyond it, and closed with Destroyed when its player leaves.
// ============================================================================

/// A player's pawn as the registry holds it for the other players' views: an actor of kind Player, never dormant, owned by the player (its PlayerId).  The replicated state is an NPC's, which is
/// enough for a view to open it.
static world::WorldActor make_player_actor(uint64_t id, double x, double y, uint64_t player, const char* name) {
    world::WorldActor w = make_npc(id, x, y, "Wolf");
    w.kind = world::ActorKind::Player;
    w.never_dormant = true;
    w.owner = player;
    w.player.name = name;
    w.label = name;
    return w;
}

static void test_view_another_players_pawn_is_sent_and_its_own_is_not() {
    std::printf("the view: another player's pawn is sent, this player's own is not\n");
    for (const uint64_t viewer : {7ull, 8ull}) {
        world::ActorRegistry reg;
        Pair p;
        ActorView::Config cfg = view_config();
        cfg.player_id = viewer;
        ActorView v(reg, cfg, reserved_3_to_6());
        reg.add(make_player_actor(5000, 3000, 0, 7, "cyb"));
        reg.add(make_player_actor(5100, 5000, 0, 8, "ayla"));
        run(v, p, 0, 0, 3);
        const uint64_t other = viewer == 7 ? 5100 : 5000, own = viewer == 7 ? 5000 : 5100;
        char text[200];
        std::snprintf(text, sizeof(text), "player %llu's view holds the other player's pawn awake and none of its own (one actor tracked)", static_cast<unsigned long long>(viewer));
        CHECK(v.state_of(other) == world::ClientState::Awake && v.state_of(own) == world::ClientState::None && v.tracked() == 1, text);
        const auto s = take(p);
        size_t opens = 0;
        for (const Seen& x : s) opens += x.open ? 1 : 0;
        CHECK(opens == 1, "...and exactly one open went out");
    }
}
AOC_REGISTER_TEST(test_view_another_players_pawn_is_sent_and_its_own_is_not);

static void test_view_player_actors_pets_zones_and_mounts() {
    std::printf("the view: a pet and a player's pawn are both sent; another player's zone and mount are not\n");
    world::ActorRegistry reg;
    Pair p;
    ActorView::Config cfg = view_config();
    cfg.player_id = 7;
    ActorView v(reg, cfg, reserved_3_to_6());
    // a summoned pet is an NPC of nobody's (owner 0), never dormant
    world::WorldActor pet = make_npc(6000, 2000, 0, "Wolf");
    pet.never_dormant = true;
    reg.add(std::move(pet));
    reg.add(make_player_actor(5100, 4000, 0, 8, "ayla"));
    reg.add(make_player_actor(5000, 3000, 0, 7, "cyb"));
    // another player's lingering zone and mount, and this player's own
    const auto owned = [&](uint64_t id, world::ActorKind kind, uint64_t owner) {
        world::WorldActor w = make_npc(id, 2500, 0, "Effect");
        w.kind = kind;
        w.never_dormant = true;
        w.owner = owner;
        reg.add(std::move(w));
    };
    owned(7000, world::ActorKind::Effect, 8);   // ayla's zone
    owned(7100, world::ActorKind::Mount, 8);    // ayla's mount
    owned(7200, world::ActorKind::Effect, 7);   // cyb's own zone
    owned(7300, world::ActorKind::Mount, 7);    // cyb's own mount
    run(v, p, 0, 0, 3);
    CHECK(v.state_of(6000) == world::ClientState::Awake, "the pet (an NPC of nobody's) is sent");
    CHECK(v.state_of(5100) == world::ClientState::Awake, "another player's pawn is sent beside it");
    CHECK(v.state_of(5000) == world::ClientState::None, "this player's own Player actor is not");
    CHECK(v.state_of(7000) == world::ClientState::None && v.state_of(7100) == world::ClientState::None, "another player's zone and mount are not (they stay its owner's)");
    CHECK(v.state_of(7200) == world::ClientState::Awake && v.state_of(7300) == world::ClientState::Awake, "this player's own zone and mount are");
    CHECK(v.tracked() == 4, "four actors in all: the pet, the other pawn, the own zone and the own mount");
}
AOC_REGISTER_TEST(test_view_player_actors_pets_zones_and_mounts);

static void test_view_player_actor_rings() {
    std::printf("the view: a player's pawn opens within 150 m, closes with Relevancy beyond it and is never dormant\n");
    world::ActorRegistry reg;
    Pair p;
    ActorView::Config cfg = view_config();
    cfg.player_id = 7;
    ActorView v(reg, cfg, reserved_3_to_6());
    reg.add(make_player_actor(5100, 10000, 0, 8, "ayla"));   // 100 m
    run(v, p, 0, 0, 2);
    take(p);
    CHECK(v.state_of(5100) == world::ClientState::Awake, "100 m away: awake");
    run(v, p, -10000, 0, 2);   // 200 m away now
    auto s = take(p);
    uint32_t reason = 99;
    for (const Seen& x : s) {
        if (x.close) reason = x.reason;
    }
    CHECK(v.state_of(5100) == world::ClientState::None && reason == kCloseRelevancy, "200 m away: closed with Relevancy, not put to sleep (never dormant)");
    run(v, p, -10000, 0, 5);
    s = take(p);
    CHECK(v.state_of(5100) == world::ClientState::None && s.empty(), "and nothing more is sent while it is out of range (no snapshot of it)");
    run(v, p, 0, 0, 2);
    CHECK(v.state_of(5100) == world::ClientState::Awake, "back within 150 m: it is opened again");
    reg.remove(5100);
    run(v, p, 0, 0, 2);
    s = take(p);
    uint32_t last_reason = 99;
    for (const Seen& x : s) {
        if (x.close) last_reason = x.reason;
    }
    CHECK(v.state_of(5100) == world::ClientState::None && last_reason == kCloseDestroyed, "when its player leaves the world the registry removes it and the view closes it with Destroyed");
}
AOC_REGISTER_TEST(test_view_player_actor_rings);
