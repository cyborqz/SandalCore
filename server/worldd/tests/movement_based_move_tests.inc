// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/movement_based_move_tests.inc: moves of a character that stands on a movable thing (session/player_based_move.cpp)
//
//  A pawn that stands on the mount it just got off, or on a corpse, sends every move with that thing as its movement base, and the move's Location is then relative to the base:
//  the offset from the base, turned back by the base's rotation.  A test run lost 13 seconds of moves that way: the reader refused a base named in place, the position of the
//  player froze 564 cm from the horse, the mount was refused ("564 cm away, at most 500") and the game's saved moves hit their limit of 96 twice.  Now:
//    a base worldd can place (the root cylinder of an actor it holds) puts the move at the base's place plus the offset turned by the base's yaw, and the move is judged like any move;
//    a base it cannot place is followed from the move before on the same base (the player moves by what the offset changed by) and judged the same way; the first move on a base
//      stays where the player was;
//    a client that names a new base with every move is put back after kBasedHeldCap of them;
//    the moves that do not read are counted.
//  The expected places are worked out by hand or through the game's own conversion written out on its own (the inverse of the base's rotation as a quaternion), never through the
//  turn the server uses.
// ============================================================================

namespace based_move_checks {

/// The root cylinder of a mount that stands at (x, y, z) facing `yaw` degrees, as the registry holds it.
inline bool add_mount(Harness& h, uint64_t id, double x, double y, double z, double yaw) {
    world::WorldActor mount;
    mount.id = id;
    mount.kind = world::ActorKind::Mount;
    mount.actor.guid.ObjectId = id;
    mount.x = x;
    mount.y = y;
    mount.z = z;
    mount.yaw_degrees = yaw;
    mount.npc.collision_cylinder = id + 4;
    return h.fx.world->registry().add(mount);
}

/// A component of the actor `actor_id`, named in place the way the game names an object the server never gave it an id for.
inline repl::InlineObject component_of(uint64_t actor_id, const char* path) {
    repl::InlineObject o;
    o.flags = 5;
    o.outer.ObjectId = actor_id;
    o.outer.ServerId = kServerId;
    o.outer.Randomizer = kRandomizer;
    o.path = path;
    o.checksum = 0xb876932f;
    return o;
}

/// A move that stands on `base`: its location is `offset` from it.
inline repl::NetworkMove move_on(float ts, const repl::InlineObject& base, const repl::Vec3d& offset) {
    repl::NetworkMove m = move_at(ts, offset.x, offset.y, offset.z);
    m.base_inline = base;
    return m;
}

/// What the game does to send a place on a base: the world place taken from the base's place, then turned by the INVERSE of the base's rotation.  That is the engine's inverse
/// transform in its quaternion form (V + w T + Q x T with T = 2 (Q x V)), written out here for a rotation about the vertical axis.
inline repl::Vec3d client_relative(const repl::Vec3d& world, const repl::Vec3d& base, double base_yaw_degrees) {
    const double half = base_yaw_degrees * 3.14159265358979323846 / 360.0;
    const double qx = 0, qy = 0, qz = -std::sin(half), qw = std::cos(half);   // the inverse: the vector part of the base's quaternion negated
    const double vx = world.x - base.x, vy = world.y - base.y, vz = world.z - base.z;
    const double tx = 2 * (qy * vz - qz * vy), ty = 2 * (qz * vx - qx * vz), tz = 2 * (qx * vy - qy * vx);
    return repl::Vec3d{vx + qw * tx + (qy * tz - qz * ty), vy + qw * ty + (qz * tx - qx * tz), vz + qw * tz + (qx * ty - qy * tx)};
}

inline bool close(const repl::Vec3d& a, const repl::Vec3d& b, double tolerance = 0.05) {
    return std::fabs(a.x - b.x) <= tolerance && std::fabs(a.y - b.y) <= tolerance && std::fabs(a.z - b.z) <= tolerance;
}

inline repl::Vec3d place_of(const GoodPosition& g) { return repl::Vec3d{g.x, g.y, g.z}; }

inline bool answered_with_ack(const std::vector<const fake::Delivered*>& r, float ts) {
    return r.size() == 1 && is_ack(*r[0], ts);
}

}  // namespace based_move_checks

static void test_a_move_on_a_mount_is_put_where_its_yaw_says() {
    using namespace based_move_checks;
    std::printf("moves on a base worldd can place: the offset is turned by the base's yaw\n");
    Harness h;
    h.send_move(move_at(1.0f, kX, kY, kZ));
    constexpr uint64_t kHorse = 0x4000;
    const repl::Vec3d base{kX + 20.0, kY, kZ};
    CHECK(add_mount(h, kHorse, base.x, base.y, base.z, 0.0), "(the horse is in the registry)");

    // worked by hand: the player 10 cm to the east (+X) of the horse.  Facing east the east is straight ahead; facing north (yaw 90, +Y) it is to the horse's right, which is -Y
    // in its own frame; facing west it is behind it; facing south it is on its left
    const repl::Vec3d east{10.0, 0.0, 0.0};
    CHECK(close(client_relative(east, repl::Vec3d{0, 0, 0}, 0), repl::Vec3d{10, 0, 0}) && close(client_relative(east, repl::Vec3d{0, 0, 0}, 90), repl::Vec3d{0, -10, 0}) &&
              close(client_relative(east, repl::Vec3d{0, 0, 0}, 180), repl::Vec3d{-10, 0, 0}) && close(client_relative(east, repl::Vec3d{0, 0, 0}, -90), repl::Vec3d{0, 10, 0}),
          "(the conversion the checks use agrees with the four hand-worked cases)");

    struct Case { double yaw; repl::Vec3d beside; };
    const Case cases[] = {{0, {10, 0, 0}}, {90, {10, 0, 0}}, {180, {10, 0, 0}}, {-90, {10, 0, 0}}, {30, {25, 15, 4}}, {-120, {-8, 20, -2}}, {200, {30, -10, 0}}};
    float ts = 1.0f;
    bool placed = true, acknowledged = true, turn_agrees = true;
    for (const Case& c : cases) {
        ts += 0.1f;
        h.fx.world->registry().move(kHorse, base.x, base.y, base.z, c.yaw);
        const repl::Vec3d world{base.x + c.beside.x, base.y + c.beside.y, base.z + c.beside.z};
        const repl::Vec3d offset = client_relative(world, base, c.yaw);
        const auto r = h.send_move(move_on(ts, component_of(kHorse, "CollisionCylinder"), offset));
        const repl::Vec3d at = place_of(h.s->movement().good());
        if (!close(at, world)) {
            placed = false;
            std::printf("    (yaw %.0f: the player is at (%.2f, %.2f, %.2f), expected (%.2f, %.2f, %.2f); the offset sent was (%.2f, %.2f, %.2f))\n", c.yaw, at.x, at.y, at.z, world.x, world.y,
                        world.z, offset.x, offset.y, offset.z);
        }
        acknowledged = acknowledged && answered_with_ack(r, ts);
        // the acceleration is turned by the same yaw: what the game sends as a direction in the base's frame comes back as the world's
        turn_agrees = turn_agrees && close(PlayerSession::turn_by_yaw(c.yaw, offset), c.beside);
    }
    CHECK(placed, "a move on the horse's cylinder is applied at the horse's place plus the offset turned by its yaw, for seven yaws (the horse stood 20 cm away; the player is put where he is)");
    CHECK(acknowledged && h.s->moves_refused() == 0 && h.s->corrections_sent() == 0 && h.s->moves_based() == 7 && h.s->moves_based_unplaced() == 0,
          "and each is acknowledged, none refused or corrected, counted as a based move that was placed");
    CHECK(turn_agrees, "(the acceleration and the offset are turned by the same function: it brings the base's frame back to the world's for each yaw)");

    // the plain reference to the cylinder (the game has an id for it when the server gave it one) is placed the same; one to the actor itself is not its cylinder
    repl::NetworkMove plain = move_at(ts + 0.1f, 0, 0, 0);
    plain.location = repl::quantize_vector(100, 0.0, -10.0, 0.0);
    h.fx.world->registry().move(kHorse, base.x, base.y, base.z, 90.0);
    plain.base = emit::FIntrepidNetworkGUID{};
    plain.base->ObjectId = kHorse + 4;
    plain.base->ServerId = kServerId;
    plain.base->Randomizer = kRandomizer;
    const auto by_id = h.send_move(plain);
    CHECK(answered_with_ack(by_id, ts + 0.1f) && close(place_of(h.s->movement().good()), repl::Vec3d{base.x + 10.0, base.y, base.z}) && h.s->moves_based_unplaced() == 0,
          "a plain reference to the horse's cylinder is placed too (offset (0, -10, 0) at yaw 90 degrees: 10 cm east)");
}
AOC_REGISTER_TEST(test_a_move_on_a_mount_is_put_where_its_yaw_says);

static void test_moves_on_a_base_worldd_cannot_place_are_followed() {
    using namespace based_move_checks;
    std::printf("moves on a base worldd cannot place: followed from the move before, judged like any move\n");
    Harness h;
    h.send_move(move_at(1.0f, kX, kY, kZ));
    constexpr uint64_t kHorse = 0x4000;
    CHECK(add_mount(h, kHorse, kX + 20.0, kY, kZ, 90.0), "(the horse is in the registry)");
    const repl::Vec3d start = place_of(h.s->movement().good());

    // the first move on a component worldd does not place stays where the player was
    const auto first = h.send_move(move_on(1.1f, component_of(kHorse, "CharacterMesh0"), repl::Vec3d{3.0, 4.0, 5.0}));
    CHECK(answered_with_ack(first, 1.1f) && close(place_of(h.s->movement().good()), start) && h.s->moves_refused() == 0 && h.s->corrections_sent() == 0 &&
              h.s->moves_based() == 1 && h.s->moves_based_unplaced() == 1 && h.s->moves_based_held() == 1,
          "the first move on a component worldd cannot place is acknowledged and the player stays where he was (a relative location is no place to move to)");

    // each move after it on the same base moves the player by what the offset changed by: 10 cm a move, 200 cm/s
    float ts = 1.1f;
    bool all_acked = true;
    for (int i = 1; i <= 5; ++i) {
        ts += 0.05f;
        all_acked = answered_with_ack(h.send_move(move_on(ts, component_of(kHorse, "CharacterMesh0"), repl::Vec3d{3.0 + 10.0 * i, 4.0, 5.0})), ts) && all_acked;
    }
    CHECK(all_acked && close(place_of(h.s->movement().good()), repl::Vec3d{start.x + 50.0, start.y, start.z}) && h.s->moves_refused() == 0 && h.s->moves_based() == 6 &&
              h.s->moves_based_held() == 1,
          "the five moves after it carry the player 50 cm along, as the offset changed (the base is taken to stand still), all acknowledged");

    // a different base starts again; an actor worldd does not hold is such a base too
    const repl::Vec3d followed = place_of(h.s->movement().good());
    ts += 0.05f;
    const auto other = h.send_move(move_on(ts, component_of(0x7770, "CollisionCylinder"), repl::Vec3d{-900.0, 40.0, 7.0}));
    CHECK(answered_with_ack(other, ts) && close(place_of(h.s->movement().good()), followed) && h.s->moves_based_held() == 2,
          "a move on another base (an actor worldd does not hold) stays in place again, wherever its offset is");

    // stepping off: an absolute move that goes on from the followed place is accepted without a correction
    ts += 0.05f;
    const auto off = h.send_move(move_at(ts, followed.x + 10.0, followed.y, followed.z));
    CHECK(answered_with_ack(off, ts) && close(place_of(h.s->movement().good()), repl::Vec3d{followed.x + 10.0, followed.y, followed.z}) && h.s->corrections_sent() == 0 &&
              h.s->moves_refused() == 0,
          "the first move off the base (an absolute place 10 cm on) is acknowledged: the followed place and the player's agree, no jump and no correction");
}
AOC_REGISTER_TEST(test_moves_on_a_base_worldd_cannot_place_are_followed);

static void test_a_jump_on_a_followed_base_is_refused_like_any_jump() {
    using namespace based_move_checks;
    std::printf("a jump on a base worldd cannot place is refused like any other jump\n");
    Harness h;
    h.send_move(move_at(1.0f, kX, kY, kZ));
    const repl::InlineObject prop = component_of(0x6600, "StaticMeshComponent0");
    float ts = 1.0f;
    for (int i = 0; i < 3; ++i) {
        ts += 0.05f;
        h.send_move(move_on(ts, prop, repl::Vec3d{100.0 + 10.0 * i, 0, 0}));
    }
    const GoodPosition before = h.s->movement().good();
    ts += 0.05f;
    const auto jump = h.send_move(move_on(ts, prop, repl::Vec3d{100.0 + 8000.0, 0, 0}));   // 80 m in 50 ms
    CHECK(jump.size() == 1 && correction_is(*jump[0], ts, before.x, before.y, before.z) && h.s->moves_refused() == 1 && h.s->movement().good() == before,
          "an offset that changes by 80 m in 50 ms moves the player too fast: refused, corrected to the last good position, the player stays");
    // the correction ended the following: the next move on the same base is the first on it again
    ts += 0.05f;
    const size_t held = h.s->moves_based_held();
    const auto next = h.send_move(move_on(ts, prop, repl::Vec3d{140.0, 0, 0}));
    CHECK(answered_with_ack(next, ts) && h.s->moves_based_held() == held + 1 && h.s->movement().good().x == before.x + 0.0 && h.s->movement().good().time_stamp == ts,
          "and the next move on that base is a first one again: acknowledged, in place");
}
AOC_REGISTER_TEST(test_a_jump_on_a_followed_base_is_refused_like_any_jump);

static void test_a_client_naming_a_new_base_with_every_move_is_put_back() {
    using namespace based_move_checks;
    std::printf("a client that names a new base with every move is put back after %zu of them\n", PlayerSession::kBasedHeldCap);
    constexpr size_t kCap = PlayerSession::kBasedHeldCap;
    {
        Harness h;
        h.send_move(move_at(1.0f, kX, kY, kZ));
        const GoodPosition start = h.s->movement().good();
        float ts = 1.0f;
        size_t acked = 0, corrected = 0;
        for (size_t i = 0; i <= kCap; ++i) {
            ts += 0.02f;
            // a base it never named before, each time: its place can be neither read nor followed
            for (const auto* d : h.send_move(move_on(ts, component_of(0x9000 + 2 * i, "Hull"), repl::Vec3d{300.0 * i, 0, 0}))) {
                if (is_ack(*d, ts)) ++acked;
                else if (correction_is(*d, ts, start.x, start.y, start.z)) ++corrected;
            }
        }
        CHECK(acked == kCap && corrected == 1 && h.s->moves_based_cut() == 1 && close(place_of(h.s->movement().good()), place_of(start)) && h.s->moves_refused() == 0,
              "the first 120 are acknowledged and stay in place; the 121st is answered with a correction to the last good position (it is not a refusal)");
        // a run is counted from the last move whose place is known: the next one starts from nothing
        ts += 0.02f;
        const auto after = h.send_move(move_on(ts, component_of(0x9900, "Hull"), repl::Vec3d{0, 0, 0}));
        CHECK(answered_with_ack(after, ts), "and the run starts again after it");
    }
    {
        Harness h;
        h.send_move(move_at(1.0f, kX, kY, kZ));
        float ts = 1.0f;
        size_t fixes = 0;
        for (size_t i = 0; i < 100; ++i) {
            ts += 0.02f;
            for (const auto* d : h.send_move(move_on(ts, component_of(0x9000 + 2 * i, "Hull"), repl::Vec3d{300.0 * i, 0, 0}))) fixes += as_correction(*d) ? 1 : 0;
        }
        ts += 0.02f;
        const float plain_ts = ts;
        const auto plain = h.send_move(move_at(plain_ts, kX, kY, kZ));   // a move whose place the server knows
        CHECK(answered_with_ack(plain, plain_ts), "(the ordinary move between the runs is acknowledged)");
        for (size_t i = 0; i < 100; ++i) {
            ts += 0.02f;
            for (const auto* d : h.send_move(move_on(ts, component_of(0xa000 + 2 * i, "Hull"), repl::Vec3d{300.0 * i, 0, 0}))) fixes += as_correction(*d) ? 1 : 0;
        }
        CHECK(fixes == 0 && h.s->moves_based_cut() == 0 && h.s->moves_based_held() == 200,
              "100 moves on new bases, one ordinary move, 100 more: no correction (the count starts again at a move whose place the server knows)");
    }
    {
        // a datagram that carries the 121st move and one more: the move after it is accepted in place, and the correction goes out all the same
        Harness h;
        h.send_move(move_at(1.0f, kX, kY, kZ));
        const GoodPosition start = h.s->movement().good();
        float ts = 1.0f;
        for (size_t i = 0; i < kCap; ++i) {
            ts += 0.02f;
            h.send_move(move_on(ts, component_of(0xb000 + 2 * i, "Hull"), repl::Vec3d{0, 0, 0}));
        }
        const float cut_ts = ts + 0.02f, next_ts = ts + 0.04f;
        size_t fixes = 0;
        bool next_acked = false;
        for (const auto* d : h.send_move(client_moves({move_on(cut_ts, component_of(0xc000, "Hull"), repl::Vec3d{0, 0, 0}), move_on(next_ts, component_of(0xc002, "Hull"), repl::Vec3d{0, 0, 0})}))) {
            if (correction_is(*d, cut_ts, start.x, start.y, start.z)) ++fixes;
            else if (is_ack(*d, next_ts)) next_acked = true;
        }
        CHECK(fixes == 1 && next_acked && h.s->moves_based_cut() == 1,
              "a datagram with the 121st move and another: the move after it is acknowledged, and the correction goes out all the same (a client cannot cancel it by packing a move behind it)");
    }
}
AOC_REGISTER_TEST(test_a_client_naming_a_new_base_with_every_move_is_put_back);

static void test_a_packet_that_repeats_a_move_on_a_base_counts_it_once() {
    using namespace based_move_checks;
    std::printf("a pending move the game repeats is not judged or counted twice\n");
    Harness h;
    h.send_move(move_at(1.0f, kX, kY, kZ));
    const repl::InlineObject prop = component_of(0x6600, "StaticMeshComponent0");
    const repl::NetworkMove a = move_on(1.05f, prop, repl::Vec3d{100.0, 0, 0});
    const repl::NetworkMove b = move_on(1.10f, prop, repl::Vec3d{110.0, 0, 0});
    const repl::NetworkMove c = move_on(1.15f, prop, repl::Vec3d{120.0, 0, 0});
    h.send_move(a);
    repl::MovePacked packet;   // the next packet: the move before as the pending move, and the new one
    packet.pending = a;
    packet.new_move = b;
    // (the chains a send returns point into the harness's list, which the next send may move: each is read before it)
    const bool first_acked = answered_with_ack(h.send_move(client_move(packet)), 1.10f);
    packet.pending = b;
    packet.new_move = c;
    const bool second_acked = answered_with_ack(h.send_move(client_move(packet)), 1.15f);
    CHECK(first_acked && second_acked, "each packet is acknowledged, with its new move's TimeStamp");
    CHECK(h.s->moves_based() == 3 && h.s->moves_based_held() == 1 && h.s->moves_stale() == 0,
          "three moves on one base, each sent twice (as the new move and then as the pending move): counted three times, the first held, the others followed");
    CHECK(close(place_of(h.s->movement().good()), repl::Vec3d{kX + 20.0, kY, kZ}), "and the player went on by the two changes of the offset (10 cm each)");
    const auto again = h.send_move(client_move(packet));   // the same datagram twice
    CHECK(again.empty() && h.s->moves_based() == 3 && h.s->moves_stale() == 1 && close(place_of(h.s->movement().good()), repl::Vec3d{kX + 20.0, kY, kZ}),
          "a datagram that arrives twice is dropped as stale: nothing counted, nothing moved, whatever offset its moves carry");
}
AOC_REGISTER_TEST(test_a_packet_that_repeats_a_move_on_a_base_counts_it_once);

static void test_moves_that_do_not_read_are_counted() {
    using namespace based_move_checks;
    std::printf("moves that do not read, and what the position line says\n");
    Harness h;
    h.send_move(move_at(1.0f, kX, kY, kZ));
    CHECK(h.s->based_move_summary().empty(), "(a session with no such moves says nothing about them)");
    const schema::ClassSchema& cls = class_named("PlayerPawn_C");
    const schema::NetField* f = cls.field("ServerMovePacked");
    BunchWriter params, field, bunch;
    params.write_bit(0);   // a zeroed struct: the parameter is not there
    CHECK(f && repl::write_field(cls, f->index, params, field) != 0, "(a zeroed ServerMovePacked is built)");
    repl::BlockHeader header;
    repl::write_content_block(bunch, header, field, /*server_form=*/false);
    h.send_move(bunch);
    h.send_move(bunch);
    CHECK(h.s->moves_unreadable() == 2 && h.s->moves_refused() == 0, "a move that does not read is counted (the log says the first and every 100th)");
    const repl::InlineObject prop = component_of(0x6600, "StaticMeshComponent0\nforged line");
    h.send_move(move_on(1.05f, prop, repl::Vec3d{100.0, 0, 0}));
    h.send_move(move_on(1.10f, prop, repl::Vec3d{110.0, 0, 0}));
    const std::string summary = h.s->based_move_summary();
    if (summary != ", 2 unreadable, 2 on a movement base (2 not placed)") std::printf("    (the summary reads \"%s\")\n", summary.c_str());
    CHECK(summary == ", 2 unreadable, 2 on a movement base (2 not placed)", "the position line gets the counts after the moves dropped as stale");
}
AOC_REGISTER_TEST(test_moves_that_do_not_read_are_counted);

static void test_the_log_of_a_base_cannot_be_forged_by_its_name() {
    using namespace based_move_checks;
    std::printf("a base's name from the client cannot start a log line of its own\n");
    Harness h;
    h.send_move(move_at(1.0f, kX, kY, kZ));
    g_log.str("");
    h.send_move(move_on(1.05f, component_of(0x6600, "Mesh\n[info] [session] forged: all is well"), repl::Vec3d{1, 2, 3}));
    spdlog::default_logger()->flush();
    const std::string log = g_log.str();
    CHECK(log.find("a move on a movement base (") != std::string::npos && log.find("\n[info] [session] forged") == std::string::npos && log.find("Mesh?[info]") != std::string::npos,
          "the line names the base with the newline made harmless");
}
AOC_REGISTER_TEST(test_the_log_of_a_base_cannot_be_forged_by_its_name);
