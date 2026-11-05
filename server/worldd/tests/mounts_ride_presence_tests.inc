// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/mounts_ride_presence_tests.inc: a rider and the pawn other players are shown (players seeing each other)
//
//  While a player rides, the pawn the other players are shown is not moved: it stays where the player got on and shows no velocity (the mount, which another player's client does not get
//  yet, is where the player is).  When the player gets off, the pawn follows the player again.  The session ticks the same way with the view passes on threads or not (the sync is one call
//  of the first part of the tick), so one session without a pool shows the rule for all of them.
// ============================================================================

/// The registry's Player actor of the player `id` (its pawn); null when there is none.
static const world::WorldActor* rider_pawn(const Harness& h, uint64_t id) {
    const world::WorldActor* found = nullptr;
    h.fx.world->registry().for_each([&](const world::WorldActor& a) {
        if (a.kind == world::ActorKind::Player && a.owner == id) found = &a;
    });
    return found;
}

/// Whether the pawn shows the walking input.
static bool rider_pawn_walks(const world::WorldActor& a) {
    const repl::ObjectState& o = a.actor.obj;
    const repl::Value* v = o.schema ? o.state.get(*o.schema, "CurrentMovementInput") : nullptr;
    const auto* input = v ? v->get_if<repl::PlayerMovementInput>() : nullptr;
    return input && input->bits != 0;
}

static void test_presence_a_riders_pawn_stays_where_the_player_got_on() {
    std::printf("players seeing each other: the pawn other players are shown stays where its player got on, and follows again after the dismount\n");
    Harness h(cyb_record());
    h.play();
    const uint64_t id = static_cast<uint64_t>(h.s->bootstrap().setup().player_id);
    CHECK(rider_pawn(h, id) != nullptr, "(the player's pawn is in the registry after the phase-in)");
    CHECK(ride_on(h), "(the player rides)");
    if (!h.s->mounted() || !rider_pawn(h, id)) return;
    h.run(milliseconds(400));
    const double got_on_x = rider_pawn(h, id)->x, got_on_y = rider_pawn(h, id)->y;
    Rider r = start_riding(h);
    for (int i = 0; i < 20; ++i) ride_step(h, r, 99, 0);   // 19.8 m at 990 cm/s
    h.run(milliseconds(400));
    const world::WorldActor* pawn = rider_pawn(h, id);
    const world::WorldActor* mount = h.mount();
    CHECK(pawn && mount && std::fabs(mount->x - got_on_x) > 1500.0, "(the mount carried the rider more than 15 m)");
    CHECK(pawn && std::fabs(pawn->x - got_on_x) < 1.0 && std::fabs(pawn->y - got_on_y) < 1.0,
          "20 moves on the mount later the registry's pawn is where the player got on (within a centimetre), not where the mount is");
    CHECK(pawn && !rider_pawn_walks(*pawn), "...and shows no walking input (it stands, it does not run in place)");
    // getting off: the pawn is the controlled actor again, and the other players' copy follows it
    get_off(h);
    h.run(milliseconds(100));
    const GoodPosition landed = h.s->movement().good();
    g_pawn_ts = 0.05f;
    move_pawn(h, move_at(g_pawn_ts, landed.x, landed.y, landed.z - 20, yaw16_of(landed.yaw_degrees), repl::kMoveFalling));
    h.run(milliseconds(300));
    pawn = rider_pawn(h, id);
    CHECK(pawn && std::fabs(pawn->x - landed.x) < 600.0 && std::fabs(pawn->y - landed.y) < 600.0 && std::fabs(pawn->x - got_on_x) > 1500.0,
          "after the dismount the pawn is where the player landed (beside the mount, more than 15 m from where it got on)");
}
AOC_REGISTER_TEST(test_presence_a_riders_pawn_stays_where_the_player_got_on);
