// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/mounts_ride_others_ends_tests.inc: the other ways a ride ends and begins in another player's world, and what a ride costs the players who watch it (Social, seeing other
//  players: riders on their mounts)
//
//  The mount dies under its rider, the rider dies, a GM teleports the rider away, a second and a third player watch, the rider gets on the same mount again, the rider's session is dropped
//  without finish(), the rider moves its view up and down, and a flying mount dives faster than a walker can move.  Uses the helpers of mounts_ride_others_tests.inc.
// ============================================================================

/// The fastest speed any chain the watcher's client got on `channel` from `from` on said the actor moves at, and how many of them carried a ReplicatedMovement.
static MotionSeen speed_seen(const Harness& watcher, uint32_t channel, const repl::Actor& actor, size_t from) { return motion_seen(watcher, channel, actor, from); }

/// The values `path` took in the property blocks of the chains the watcher's client got on `channel` from `from` on, in the order they came.
static std::vector<int64_t> values_sent(const Harness& watcher, uint32_t channel, const repl::Actor& actor, size_t from, const char* path) {
    std::vector<int64_t> out;
    for (size_t i = from; i < watcher.got.size(); ++i) {
        const fake::Delivered& d = watcher.got[i];
        if (d.channel != channel || d.close) continue;
        if (const auto v = number_in(chain_states(d, actor), 0, path)) out.push_back(*v);
    }
    return out;
}

// ---- the mount dies under its rider --------------------------------------------------------------------------------------------------------

static void test_others_see_a_mount_die_under_its_rider() {
    std::printf("other players see a mount die under its rider: the rider comes off, the mount lies dead and closes\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    CHECK(ride_on(a), "(the rider rides)");
    if (!a.s->mounted()) return;
    Rider r = start_riding(a);
    for (int i = 0; i < 10; ++i) ride_step(a, r, 99, 0);
    const world::WorldActor* m = a.mount();
    const world::WorldActor* pawn = shown_pawn(a);
    const repl::Actor mount_copy = m->actor, pawn_copy = pawn->actor;
    const uint32_t mch = sc.channel(m->id), pch = sc.channel(pawn->id);
    CHECK(mch != 0 && pch != 0, "(the watcher holds the rider's pawn and its mount)");
    if (!mch || !pch) return;
    const uint64_t info_id = id_of_component(*pawn, "UNPCSecondaryInfo");
    const uint64_t rider_ptr_in = id_of_component(*m, "UNPCSecondaryInfo");
    const uint64_t combat_id = id_of_component(*m, "UNPCCombatInfo");
    const size_t from = b.got.size();
    a.s->take_damage(a.conn, 10000.0, a.t);
    a.run(milliseconds(300));
    CHECK(a.s->mount_deaths() == 1 && !a.s->mounted(), "(the mount died under the rider, who is off)");
    const auto pawn_state = channel_state(b, pch, pawn_copy, from);
    CHECK(!names(object_in(pawn_state.merged, 0, "AttachmentReplication.AttachParent")) && number_in(pawn_state.merged, 0, "AttachmentState.bAttachParentValid") == 0 &&
              !names(object_in(pawn_state.merged, info_id, "MountPtr")),
          "the watcher's client was sent the pawn taken off the mount");
    const auto mount_state = channel_state(b, mch, mount_copy, from);
    CHECK(number_in(mount_state.merged, combat_id, "bIsDead") == 1 && !names(object_in(mount_state.merged, rider_ptr_in, "RiderPtr")) && !names(object_in(mount_state.merged, 0, "Owner")),
          "...and the mount dead, with no rider and no owner");
    CHECK(sc.channel(m->id) == mch && sc.channel(pawn->id) == pch, "(the watcher still holds both for a moment)");
    a.run(milliseconds(1700));
    const auto closes = closes_on(b, mch, from);
    CHECK(mount_count(a) == 0 && closes.size() == 1 && closes[0] == 0, "1.5 s later the dead mount leaves the world and the watcher's channel is closed with Destroyed");
    CHECK(sc.channel(pawn->id) == pch, "the rider's pawn stays in the watcher's world");
}
AOC_REGISTER_TEST(test_others_see_a_mount_die_under_its_rider);

// ---- the rider dies ------------------------------------------------------------------------------------------------------------------------

static void test_others_see_a_rider_die_on_its_mount() {
    std::printf("other players see a rider die on its mount: it comes off and the mount dissolves and leaves\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    CHECK(ride_on(a), "(the rider rides)");
    if (!a.s->mounted()) return;
    const world::WorldActor* m = a.mount();
    const world::WorldActor* pawn = shown_pawn(a);
    const repl::Actor mount_copy = m->actor, pawn_copy = pawn->actor;
    const uint32_t mch = sc.channel(m->id), pch = sc.channel(pawn->id);
    const uint64_t flags_id = id_of_component(*m, "UMountComponent");
    CHECK(mch != 0 && pch != 0, "(the watcher holds the rider's pawn and its mount)");
    if (!mch || !pch) return;
    const size_t from = b.got.size();
    a.s->set_current_value(content::kGuidHealth, 0.0f);
    a.run(milliseconds(300));
    CHECK(a.s->dead() && !a.s->mounted(), "(the rider died and is off)");
    const auto pawn_state = channel_state(b, pch, pawn_copy, from);
    CHECK(!names(object_in(pawn_state.merged, 0, "AttachmentReplication.AttachParent")) && number_in(pawn_state.merged, 0, "AttachmentState.bAttachParentValid") == 0,
          "the watcher's client was sent the pawn taken off the mount");
    const auto mount_state = channel_state(b, mch, mount_copy, from);
    CHECK(number_in(mount_state.merged, flags_id, "bIsDesummoning") == 1, "...and the mount dissolving (bIsDesummoning)");
    a.run(milliseconds(3400));
    const auto closes = closes_on(b, mch, from);
    CHECK(mount_count(a) == 0 && closes.size() == 1 && closes[0] == 0, "3.2 s later the mount is gone and the watcher's channel is closed with Destroyed");
}
AOC_REGISTER_TEST(test_others_see_a_rider_die_on_its_mount);

// ---- a GM teleports the rider -------------------------------------------------------------------------------------------------------------

static void test_others_see_a_rider_teleported_away() {
    std::printf("other players see a rider teleported away: the one it leaves sees the pawn go and the mount stand, the one it comes to sees it standing\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    const GoodPosition start = a.s->movement().good();
    auto c = join_world(a, sc.all, other_record(other_id(3), "kira", 5, std::make_pair(start.x + 20000.0, start.y)));   // 200 m east: out of the rider's range, and of its mount's
    CHECK(ride_on(a), "(the rider rides)");
    if (!a.s->mounted()) return;
    const world::WorldActor* m = a.mount();
    const world::WorldActor* pawn = shown_pawn(a);
    const repl::Actor mount_copy = m->actor;
    const uint64_t pawn_id = pawn->id, mount_id = m->id;
    const uint32_t mch = sc.channel(mount_id), pch = sc.channel(pawn_id);
    CHECK(mch != 0 && pch != 0 && c->s->actor_view()->channel_of(pawn_id) == 0, "(the first watcher holds the rider's pawn and its mount, the second holds neither)");
    if (!mch || !pch) return;
    const GoodPosition on_mount = a.s->movement().good();
    const size_t from_b = b.got.size(), from_c = c->got.size();
    const PlayerSession::GmMoveResult t = a.s->gm_teleport(a.conn, on_mount.x + 20000.0, on_mount.y, on_mount.z, a.t, "the test");
    a.run(milliseconds(500));
    CHECK(t.status == PlayerSession::GmMoveResult::Status::Moved && t.dismounted && !a.s->mounted(), "(the rider was dismounted and teleported 200 m east)");
    const auto closes = closes_on(b, pch, from_b);
    CHECK(closes.size() == 1 && closes[0] == kCloseRelevancy && sc.channel(pawn_id) == 0, "the watcher at the old place is sent the pawn's channel closed (out of range)");
    const uint32_t mch_now = sc.channel(mount_id);
    const auto left = channel_state(b, mch_now, mount_copy, from_b);
    CHECK(mch_now == mch && !names(object_in(left.merged, id_of_component(*a.mount(), "UNPCSecondaryInfo"), "RiderPtr")) && closes_on(b, mch, from_b).empty(),
          "...and keeps the mount, which stands there with nobody on it");
    const uint32_t pch_c = c->s->actor_view()->channel_of(pawn_id);
    CHECK(pch_c != 0 && c->s->actor_view()->channel_of(mount_id) == 0, "the watcher at the new place is sent the pawn, and not the mount (200 m from it)");
    if (!pch_c) return;
    const fake::Delivered* open = open_on(*c, pch_c, from_c);
    CHECK(open != nullptr, "...with an open");
    if (!open) return;
    const States st = chain_states(*open, shown_pawn(a)->actor);
    CHECK(!names(object_in(st, 0, "AttachmentReplication.AttachParent")) && number_in(st, 0, "AttachmentState.bAttachParentValid") != 1 &&
              !names(object_in(st, id_of_component(*shown_pawn(a), "UNPCSecondaryInfo"), "MountPtr")),
          "...standing, attached to nothing");
}
AOC_REGISTER_TEST(test_others_see_a_rider_teleported_away);

// ---- a second and a third player watch ----------------------------------------------------------------------------------------------------

static void test_a_second_and_a_third_watcher_see_the_same_ride() {
    std::printf("a second and a third player see the same ride: each is sent the pawn attached with the mesh it sits at, and the mount that carries it\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    CHECK(ride_on(a), "(the rider rides)");
    if (!a.s->mounted()) return;
    Rider r = start_riding(a);
    for (int i = 0; i < 30; ++i) ride_step(a, r, 99, 0);   // long enough for the first watcher to have acknowledged the mesh it was sent
    const world::WorldActor* m = a.mount();
    const world::WorldActor* pawn = shown_pawn(a);
    // the second and the third join while the ride goes on
    auto c = join_world(a, sc.all, other_record(other_id(3), "kira", 5));
    auto d = join_world(a, sc.all, other_record(other_id(4), "nuri", 6));
    a.run(milliseconds(200));
    const std::array<Harness*, 2> late{c.get(), d.get()};
    for (Harness* w : late) {
        const uint32_t mch = w->s->actor_view()->channel_of(m->id), pch = w->s->actor_view()->channel_of(pawn->id);
        CHECK(mch != 0 && pch != 0, "a player who joined while the rider rides holds the rider's pawn and its mount");
        if (!mch || !pch) continue;
        const fake::Delivered* pawn_open = open_on(*w, pch);
        const fake::Delivered* mount_open = open_on(*w, mch);
        CHECK(pawn_open && mount_open, "...from an open of each");
        if (!pawn_open || !mount_open) continue;
        const States ps = chain_states(*pawn_open, pawn->actor);
        CHECK(same_object(object_in(ps, 0, "AttachmentReplication.AttachParent"), m->actor.guid) && exports_name(*pawn_open, "CharacterMesh0"),
              "...the pawn's open attaches it to the mount and exports the mesh it sits at, though the first watcher has acknowledged that mesh long ago");
        const States ms = chain_states(*mount_open, m->actor);
        CHECK(number_in(ms, 0, "RemoteRole") == 1 && !names(object_in(ms, 0, "Owner")) && same_object(object_in(ms, id_of_component(*m, "UNPCSecondaryInfo"), "RiderPtr"), a.s->bootstrap().setup().guids.pawn),
              "...the mount's open is the simulated proxy's, with its rider named");
    }
    // all three are sent the mount's motion from here on, and the pawn taken off at the end
    const std::array<Harness*, 3> watchers{&b, c.get(), d.get()};
    std::array<size_t, 3> from{};
    for (size_t i = 0; i < watchers.size(); ++i) from[i] = watchers[i]->got.size();
    for (int i = 0; i < 10; ++i) ride_step(a, r, 99, 0);
    for (size_t i = 0; i < watchers.size(); ++i) {
        const uint32_t mch = watchers[i]->s->actor_view()->channel_of(m->id);
        const MotionSeen motion = speed_seen(*watchers[i], mch, m->actor, from[i]);
        CHECK(mch != 0 && motion.updates >= 5 && motion.last && std::fabs(motion.last->x - a.mount()->x) < 150.0, "a watcher goes on being sent the mount's motion, where the mount is");
    }
    for (size_t i = 0; i < watchers.size(); ++i) from[i] = watchers[i]->got.size();
    get_off(a);
    a.run(milliseconds(300));
    CHECK(!a.s->mounted(), "(the rider is off)");
    for (size_t i = 0; i < watchers.size(); ++i) {
        const uint32_t pch = watchers[i]->s->actor_view()->channel_of(pawn->id);
        const auto off = channel_state(*watchers[i], pch, pawn->actor, from[i]);
        CHECK(pch != 0 && !names(object_in(off.merged, 0, "AttachmentReplication.AttachParent")) && number_in(off.merged, 0, "AttachmentState.bAttachParentValid") == 0,
              "a watcher is sent the pawn taken off the mount");
    }
}
AOC_REGISTER_TEST(test_a_second_and_a_third_watcher_see_the_same_ride);

// ---- the rider gets on the same mount again ------------------------------------------------------------------------------------------------

static void test_others_see_a_rider_get_on_the_same_mount_again() {
    std::printf("other players see a rider get on, get off and get on the same mount again: one attaching update each time, the mount's motion again\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    CHECK(ride_on(a), "(the rider rides)");
    if (!a.s->mounted()) return;
    const world::WorldActor* m = a.mount();
    const world::WorldActor* pawn = shown_pawn(a);
    const emit::FIntrepidNetworkGUID mount_guid = m->actor.guid;
    const uint32_t mch = sc.channel(m->id), pch = sc.channel(pawn->id);
    CHECK(mch != 0 && pch != 0, "(the watcher holds the rider's pawn and its mount)");
    if (!mch || !pch) return;
    CHECK(a.s->shown_mount().ObjectId == mount_guid.ObjectId && a.s->shown_mountable_pet().ObjectId == mount_guid.ObjectId, "(the session says the pawn it shows rides the mount and names it)");
    Rider r = start_riding(a);
    for (int i = 0; i < 30; ++i) ride_step(a, r, 99, 0);
    get_off(a);
    a.run(milliseconds(300));
    CHECK(!a.s->mounted() && a.s->shown_mount().ObjectId == 0 && a.s->shown_mountable_pet().ObjectId == mount_guid.ObjectId, "(the rider is off: the pawn rides nothing and still names the mount)");
    // on again
    const world::WorldActor* standing = a.mount();
    const double left_x = standing->x;
    const size_t from = b.got.size();
    get_on(a);
    a.run(milliseconds(300));
    CHECK(a.s->mounted() && a.s->shown_mount().ObjectId == mount_guid.ObjectId, "(the rider is on again)");
    size_t attaching = 0;
    const fake::Delivered* again = nullptr;
    for (size_t i = from; i < b.got.size(); ++i) {
        const fake::Delivered& d = b.got[i];
        if (d.channel != pch || d.close) continue;
        if (names(object_in(chain_states(d, pawn->actor), 0, "AttachmentReplication.AttachParent"))) {
            again = &d;
            ++attaching;
        }
    }
    CHECK(attaching == 1, "the watcher's client was sent exactly one update that attaches the pawn again");
    if (again) {
        const States st = chain_states(*again, pawn->actor);
        CHECK(same_object(object_in(st, 0, "AttachmentReplication.AttachParent"), mount_guid) && same_object(object_in(st, id_of_component(*pawn, "UNPCSecondaryInfo"), "MountPtr"), mount_guid),
              "...to the same mount, with MountPtr");
        CHECK(!exports_name(*again, "CharacterMesh0"), "...without the mesh again: the watcher's client has acknowledged it");
    }
    const size_t ride_from = b.got.size();
    r = start_riding(a);
    for (int i = 0; i < 20; ++i) ride_step(a, r, 99, 0);
    const MotionSeen again_riding = motion_seen(b, mch, m->actor, ride_from);
    CHECK(again_riding.updates >= 10 && again_riding.peak_speed > 900.0 && again_riding.peak_speed < 1100.0 && again_riding.last && std::fabs(again_riding.last->x - a.mount()->x) < 150.0 &&
              a.mount()->x - left_x > 1500.0,
          "the mount's motion goes to the watcher again, at the speed the rider rides at");
    CHECK(names(object_in(channel_state(b, mch, m->actor, from).merged, id_of_component(*m, "UNPCSecondaryInfo"), "RiderPtr")), "...and the mount names its rider again");
}
AOC_REGISTER_TEST(test_others_see_a_rider_get_on_the_same_mount_again);

// ---- the rider's session is dropped without finish() -----------------------------------------------------------------------------------------

static void test_a_dropped_session_takes_its_mount_and_pawn_with_it() {
    std::printf("a session dropped without finish() leaves nothing of its player in the world: not the pawn, not the mount the others see\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    CHECK(ride_on(a), "(the rider rides)");
    if (!a.s->mounted()) return;
    const uint64_t pawn_id = sc.pawn_id(), mount_id = a.mount()->id;
    const uint32_t mch = sc.channel(mount_id), pch = sc.channel(pawn_id);
    CHECK(mch != 0 && pch != 0, "(the watcher holds the rider's pawn and its mount)");
    if (!mch || !pch) return;
    const size_t from = b.got.size();
    sc.all.erase(std::remove(sc.all.begin(), sc.all.end(), &a), sc.all.end());   // (the rider's harness does not tick any more)
    a.s.reset();
    b.run(milliseconds(300));
    CHECK(mount_count(b) == 0 && b.fx.world->registry().count(world::ActorKind::Player) == 1, "the registry holds neither the rider's mount nor its pawn (the watcher's own is the one pawn left)");
    const auto pawn_closes = closes_on(b, pch, from), mount_closes = closes_on(b, mch, from);
    CHECK(pawn_closes.size() == 1 && pawn_closes[0] == 0 && mount_closes.size() == 1 && mount_closes[0] == 0, "the watcher's channels of the pawn and of the mount were closed with Destroyed");
}
AOC_REGISTER_TEST(test_a_dropped_session_takes_its_mount_and_pawn_with_it);

// ---- the rider looks up and down ---------------------------------------------------------------------------------------------------------------

static void test_the_riders_view_pitch_is_the_mounts_not_the_pawns() {
    std::printf("a rider looks up and down: the mount carries the view pitch to the other players, the seated pawn is sent nothing\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    CHECK(ride_on(a), "(the rider rides)");
    if (!a.s->mounted()) return;
    const world::WorldActor* m = a.mount();
    const world::WorldActor* pawn = shown_pawn(a);
    const uint32_t mch = sc.channel(m->id), pch = sc.channel(pawn->id);
    CHECK(mch != 0 && pch != 0, "(the watcher holds the rider's pawn and its mount)");
    if (!mch || !pch) return;
    Rider r = start_riding(a);
    const size_t from = b.got.size();
    for (int i = 0; i < 24; ++i) ride_step(a, r, 99, 0, 0, 0.1, repl::kMoveWalking, pitch16_of(-5.0 * (1 + i % 4)));   // 5, 10, 15, 20 degrees down, over and over
    a.run(milliseconds(200));
    const std::vector<int64_t> pitches = values_sent(b, mch, m->actor, from, "RemoteViewPitch16");
    CHECK(pitches.size() >= 4, "the watcher's client was sent the mount's RemoteViewPitch16 as the rider's view changed (it is what retail sends on the mount)");
    CHECK(!pitches.empty() && pitches.back() == static_cast<int64_t>(pitch16_of(-5.0 * (1 + 23 % 4))), "...the last of them is the rider's last pitch");
    CHECK(chains_count(b, pch, from) == 0, "the seated pawn was sent no update of its own while the rider looked about: retail sends it none after the attaching one");
}
AOC_REGISTER_TEST(test_the_riders_view_pitch_is_the_mounts_not_the_pawns);

// ---- a flying mount dives ----------------------------------------------------------------------------------------------------------------------

static void test_others_see_a_flying_mount_dive_faster_than_a_walker_moves() {
    std::printf("other players see a flying mount dive at 3,671 cm/s, a speed the game's own dive reaches: it keeps its velocity\n");
    Harness a(cyb_on_the_ground(), adopt_the_ground);
    a.play();
    Players all;
    auto b = join_world(a, all, other_record(other_id(2), "ayla", 4, std::make_pair(0.0, 0.0)));
    CHECK(ride_on(a, kFlyingSlot), "(the player rides a flyer)");
    if (!a.s->mounted()) return;
    const world::WorldActor* m = a.mount();
    const uint32_t mch = b->s->actor_view()->channel_of(m->id);
    CHECK(mch != 0, "(the watcher holds the flying mount)");
    if (!mch) return;
    Rider r = start_riding(a);
    take_off(a, r, 4000.0);
    CHECK(a.s->moves_refused() == 0 && a.s->movement().good().movement_mode == repl::kMoveFlying, "(the rider flies)");
    const size_t from = b->got.size();
    double v = 1290.0;
    for (int i = 0; i < 28; ++i) {   // the dive of the game's own flight: 34.5 degrees down, the speed climbing at 1,600 cm/s^2 to 3,671
        v = std::min(3671.0, v + 160.0);
        ride_step(a, r, v * 0.1 * std::cos(0.45), 0, -v * 0.1 * std::sin(0.45), 0.1, repl::kMoveFlying, pitch16_of(-34.5));
    }
    CHECK(a.s->moves_refused() == 0, "(the dive is accepted by the move checker)");
    const MotionSeen dive = speed_seen(*b, mch, m->actor, from);
    CHECK(dive.updates >= 20 && dive.peak_speed > 3600.0 && dive.peak_speed < 3750.0, "the watcher is sent the dive's speed, 3,671 cm/s, in the mount's velocity (not a standing mount whose place jumps)");
    CHECK(dive.last && std::hypot(dive.last->vx, dive.last->vy, dive.last->vz) > 3500.0 && dive.last->vz < -1500.0, "...the last of them still falling: its vertical speed is a dive's");
}
AOC_REGISTER_TEST(test_others_see_a_flying_mount_dive_faster_than_a_walker_moves);
