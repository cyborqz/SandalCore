// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/movement_presence_rig.h: players seeing each other, through the Endpoint (the helpers of movement_presence_*_tests.inc)
//
//  Several players log in and phase in through the Endpoint of test_worldd_movement.cpp (its Rig, Joined and handshake), with the view passes of the players on `threads` threads (0: none,
//  the loop as it was), and each step of the rig is ONE tick of the Endpoint for all of them: every client sends a datagram (a move, or only its acknowledgements) and every packet the
//  server answers is read by its client.  The registry of the world is read for the Player actors (ActorKind::Player: a player as the other players are shown it), and every packet a
//  client is sent can be searched for a text.
//  Included by the .inc files that use it, inside test_worldd_movement.cpp, after the program's own helpers.
// ============================================================================
#pragma once

/// The world, the Endpoint and its character service, with the players' view passes on `threads` threads.
struct PresenceEndpoints {
    Fixture                         fx;
    FakeCharacters                  api;
    std::unique_ptr<CharacterSaver> saver;
    EndpointConfig                  cfg;
    std::unique_ptr<Rig>            rig;
    /// `real`: the world runs on the content.db AOC_CONTENT_DB names (the ability tables a scenario needs are only there).
    explicit PresenceEndpoints(unsigned threads, bool real = false) : fx(real) {
        saver = std::make_unique<CharacterSaver>(api, quick());
        fx.world->set_position_sink(saver.get());
        cfg.players = fx.world.get();
        cfg.view_threads = threads;
        rig = std::make_unique<Rig>(cfg);
    }
    ~PresenceEndpoints() {
        rig.reset();
        saver.reset();
    }
};

/// A character other than cyb: its own id, name and account, so that two of them are in the world at once (a second login of one character kicks the first).
static svc::Character presence_record(const std::string& id, const std::string& name, int account) {
    svc::Character c = cyb_record();
    c.id = id;
    c.name = name;
    c.account_id = account;
    return c;
}

/// A 32-digit character id that ends in `n` (1 to 15).
static std::string presence_id(int n) {
    std::string id(32, '0');
    id[31] = "0123456789abcdef"[n & 15];
    id[30] = "0123456789abcdef"[(n >> 4) & 15];
    return id;
}

/// Whether the bytes of `needle` are in `packet` at any bit offset: the replicator packs its bits, so a text is rarely on a byte boundary.
static bool presence_has_text(const std::vector<uint8_t>& packet, const std::string& needle) {
    if (needle.empty() || packet.size() < needle.size()) return false;
    std::vector<uint8_t> shifted(packet.size());
    for (int shift = 0; shift < 8; ++shift) {
        for (size_t i = 0; i < packet.size(); ++i) {
            const unsigned low = static_cast<unsigned>(packet[i]) >> shift;
            const unsigned high = (shift == 0 || i + 1 >= packet.size()) ? 0u : (static_cast<unsigned>(packet[i + 1]) << (8 - shift));
            shifted[i] = static_cast<uint8_t>((low | high) & 0xFFu);
        }
        if (std::search(shifted.begin(), shifted.end(), needle.begin(), needle.end()) != shifted.end()) return true;
    }
    return false;
}

/// ... as one byte of text, and as two (UTF-16, which a string with a character past ASCII is written in).
static bool presence_has_name(const std::vector<uint8_t>& packet, const std::string& text) {
    std::string wide;
    for (const char ch : text) {
        wide += ch;
        wide += '\0';
    }
    return presence_has_text(packet, text) || presence_has_text(packet, wide);
}

/// One client of the Endpoint: its address, its datagram for the next step (`out`), its clock for the moves and the place its moves put it.
struct PresenceClient {
    Address                  addr;
    Joined                   j;
    std::string              name;
    std::vector<fake::Bunch> out;
    float                    ts = 0;
    double                   x = kX, y = kY, z = kZ;
    std::vector<std::string> needles;     // texts to look for in every packet the client is sent
    size_t                   hits = 0;    // ... and how many packets held one
    size_t                   packets = 0;
    explicit PresenceClient(uint16_t port, std::string who) : addr{0x7F000001u, port}, name(std::move(who)) {}
};

/// The packets the server answered `c`, read by its client (and searched for its texts).
static void presence_drain(Rig& rig, PresenceClient& c) {
    for (const auto& p : rig.take(c.addr)) {
        ++c.packets;
        for (const std::string& n : c.needles) {
            if (presence_has_name(p, n)) ++c.hits;
        }
        const auto r = c.j.client->receive(p);
        for (const auto& d : r.delivered) c.j.got.push_back(d);
    }
}

/// One tick of the Endpoint at `now`: every client in `all` sends the datagram it has ready (a move, or only its acknowledgements), the Endpoint ticks, every client reads its packets.
static void presence_step(Rig& rig, const std::vector<PresenceClient*>& all, Clock::time_point now) {
    for (PresenceClient* c : all) {
        const auto pkt = c->j.client->packet(std::move(c->out));
        c->out.clear();
        rig.ep.on_datagram(c->addr, pkt.data(), pkt.size(), now);
    }
    rig.ep.tick(now);
    for (PresenceClient* c : all) presence_drain(rig, *c);
}

/// `steps` idle steps of `gap` each, from `now` on.
static void presence_idle(Rig& rig, const std::vector<PresenceClient*>& all, Clock::time_point& now, int steps, milliseconds gap = milliseconds(40)) {
    for (int i = 0; i < steps; ++i) {
        now += gap;
        presence_step(rig, all, now);
    }
}

/// Handshake, Hello, Login (the ?Name= text is `login_name`), Join and the client's "ready to phase in": the player is in the world and its phase-in is on its way.  `c` joins `all`.
static void presence_join(Rig& rig, std::vector<PresenceClient*>& all, PresenceClient& c, const svc::Character& rec, Clock::time_point& now, const std::string& login_name = kMachine) {
    c.j = handshake(rig, c.addr, now);
    all.push_back(&c);
    c.out.push_back(c.j.client->hello());
    presence_step(rig, all, now);
    LoginResult r;
    r.accepted = true;
    r.character = rec;
    rig.ep.post_login_result(c.j.session, r);
    now += milliseconds(1);
    presence_step(rig, all, now);
    c.out.push_back(c.j.client->login(login_name, login_name));
    now += milliseconds(2);
    presence_step(rig, all, now);
    c.out.push_back(c.j.client->netspeed(200000));
    c.out.push_back(c.j.client->nmt(conn::Nmt::Join));
    now += milliseconds(38);
    presence_step(rig, all, now);
    BunchWriter ready;
    {
        const auto bytes = capture_blocks::bytes_of(capture_blocks::kClientReadyToPhaseIn.hex);
        ready.write_bit_range(bytes.data(), 0, capture_blocks::kClientReadyToPhaseIn.bits);
    }
    c.out.push_back(c.j.client->actor(5, ready, true));
    now += milliseconds(20);
    presence_step(rig, all, now);
}

/// Steps until the phase-in of `c` is built (nothing is left to send of the join) and returns how many steps it took (-1: not within `limit`).
static int presence_phase_in(Rig& rig, const std::vector<PresenceClient*>& all, PresenceClient& c, Clock::time_point& now, int limit = 200) {
    for (int i = 0; i < limit; ++i) {
        const PlayerSession* s = rig.ep.player_at(c.addr);
        if (s && s->join_pending() == 0 && s->replies_pending() == 0) return i;
        presence_idle(rig, all, now, 1);
    }
    return -1;
}

/// One move of `c`: its clock runs `dt` seconds and its place `dx` cm along x; the datagram goes with the next step.  `mode` is the movement mode the client claims.
static void presence_move(PresenceClient& c, float dt, double dx, uint8_t mode = repl::kMoveWalking) {
    c.ts += dt;
    c.x += dx;
    c.out.push_back(c.j.client->actor(5, client_move(move_at(c.ts, c.x, c.y, c.z, 8192, mode)), false));
}

// ---- the registry -------------------------------------------------------------------------------------------------------------------------

/// The player id of the session at `addr` (0: none).
static uint64_t presence_player_id(const Rig& rig, const Address& addr) {
    const PlayerSession* s = rig.ep.player_at(addr);
    return s ? static_cast<uint64_t>(s->bootstrap().setup().player_id) : 0;
}

/// The registry's actors of kind Player.
static std::vector<const world::WorldActor*> presence_actors(const world::ActorRegistry& reg) {
    std::vector<const world::WorldActor*> out;
    reg.for_each([&](const world::WorldActor& a) {
        if (a.kind == world::ActorKind::Player) out.push_back(&a);
    });
    return out;
}

/// The Player actor of the player `player_id` (its pawn); null when there is none.
static const world::WorldActor* presence_pawn(const world::ActorRegistry& reg, uint64_t player_id) {
    for (const world::WorldActor* a : presence_actors(reg)) {
        if (a->owner == player_id) return a;
    }
    return nullptr;
}

/// What a Player actor says of its pawn's motion: where ReplicatedMovement puts it, how fast it says it goes, whether it shows the walking input, and its movement mode.
struct PresenceMotion {
    double   x = 0, y = 0, z = 0;
    double   vx = 0, vy = 0;
    bool     moving = false;
    uint64_t mode = 0;
    bool     ok = false;
};

static PresenceMotion presence_motion(const world::WorldActor& a) {
    PresenceMotion out;
    const repl::ObjectState& o = a.actor.obj;
    if (!o.schema) return out;
    const repl::Value* v = o.state.get(*o.schema, "ReplicatedMovement");
    const auto* m = v ? v->get_if<repl::RepMovement>() : nullptr;
    if (!m) return out;
    const repl::Vec3d at = repl::dequantize_location(o.opts, m->location);
    const repl::Vec3d vel = repl::dequantize_velocity(o.opts, m->linear_velocity);
    out.x = at.x;
    out.y = at.y;
    out.z = at.z;
    out.vx = vel.x;
    out.vy = vel.y;
    if (const repl::Value* in = o.state.get(*o.schema, "CurrentMovementInput")) {
        if (const auto* input = in->get_if<repl::PlayerMovementInput>()) out.moving = input->bits != 0;
    }
    if (const repl::Value* mode = o.state.get(*o.schema, "ReplicatedMovementMode")) {
        if (const auto* u = mode->get_if<uint64_t>()) out.mode = *u;
    }
    out.ok = true;
    return out;
}

/// The Player actors' count, the way the checks print it.
static size_t presence_count(const world::ActorRegistry& reg) { return presence_actors(reg).size(); }

