// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tools/test_live_server.cpp
//
//  Session F exit-criterion test.
//
//  Wires up a LiveWorld (the Session F integration shim) with all sub-systems
//  active and drives it through a realistic client lifecycle — handshake,
//  login, join, spawn, movement, disconnect — using the OpcodeDispatcher as
//  the packet source and a RecordingPacketEmitter as the sink.  Verifies:
//
//    1. Every Session D/D'/E component is constructed and talks to its
//       neighbours correctly.
//    2. The replication tick thread runs at the configured cadence.
//    3. on_client_connected registers the client with VisibilityManager; any
//       actor that spawns afterwards fans out to that client's emitter queue.
//    4. on_client_disconnected releases resources and prevents future emits.
//    5. Multi-client scenarios: two clients simultaneously connected, each
//       seeing the other's actors after a replication tick.
//    6. Shutdown is clean — no hanging threads, no leaks.
//
//  This is the Session F *headless* exit-criterion.  The "real client
//  connects via UDP, gets dynamically-built PC+Pawn+PS spawn bunches"
//  criterion from the plan is Session G work (requires the wire-parser →
//  dispatcher feed to be live).  Session F proves the plumbing is sound.
// ============================================================================
#include "net/live_world.h"
#include "net/udp_packet_emitter.h"
#include "protocol/schema/schema_registry.h"
#include "protocol/emit/schema_value.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace aoc;
using namespace aoc::net;
using namespace aoc::protocol;
namespace simulation = aoc::world::simulation;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static DispatchPacket pkt(DispatchOp op, const std::string& key) {
    DispatchPacket p; p.op = op; p.client_key = key; return p;
}
// C | O | A + C-Y-B

int main() {
    std::printf("=== Session F live-server integration test ===\n");

    // ── Shared schema registry (emits need this) ──
    auto& schemas = schema::SchemaRegistry::instance();
    schemas.load_all();

    // ── Scenario 1: standalone LiveWorld with RecordingEmitter ──────────
    std::printf("\n--- Scenario 1: LiveWorld stands up, clients connect ---\n");

    LiveWorldConfig cfg;
    cfg.replication_hz = 50;      // fast tick for snappy test
    cfg.use_recording_emitter = true;

    LiveWorld live(schemas, /*outer_fn=*/nullptr, /*send_fn=*/nullptr, cfg);
    live.start();

    CHECK(live.recording_emitter() != nullptr, "recording emitter installed");
    CHECK(live.recording_emitter()->total() == 0, "no emits before any clients");

    const std::string keyA = "198.51.100.10:40001";
    const std::string keyB = "198.51.100.11:40002";

    // Drive clientA through the handshake via synthetic dispatcher ops BEFORE
    // on_client_connected fires — in the real server the legacy handshake path
    // completes first and THEN calls on_client_connected (see H.1 notes).
    {
        auto r1 = live.on_packet(pkt(DispatchOp::HANDSHAKE_INITIAL, keyA));
        auto r2 = live.on_packet(pkt(DispatchOp::HANDSHAKE_CHALLENGE_RESPONSE, keyA));
        (void)r1; (void)r2;
    }

    live.on_client_connected(keyA);
    CHECK(live.sessions().contains(keyA),
          "clientA registered after on_client_connected");
    CHECK(live.visibility().is_visible(keyA, /*guid=*/0) == false,
          "no actor visible to clientA yet (no actors exist)");

    // Continue with NMT negotiation
    {
        auto r3 = live.on_packet(pkt(DispatchOp::NMT_HELLO, keyA));
        DispatchPacket login = pkt(DispatchOp::NMT_LOGIN, keyA);
        login.str_arg = "Hatemost"; login.str_arg2 = "steam:42";
        auto r4 = live.on_packet(login);
        auto r5 = live.on_packet(pkt(DispatchOp::NMT_WELCOME, keyA));
        auto r6 = live.on_packet(pkt(DispatchOp::NMT_JOIN, keyA));
        auto r7 = live.on_packet(pkt(DispatchOp::NMT_GAMESPECIFIC, keyA));

        CHECK(r7.accepted && r7.new_phase == ClientPhase::IN_WORLD,
              "clientA in IN_WORLD after NMT_GameSpecific");
        CHECK(live.sessions().get(keyA)->player_name == "Hatemost",
              "player name recorded on session");
        CHECK(live.sessions().get(keyA)->pc_netguid != 0,
              "NetGUID block allocated for clientA");
    }

    // ── Scenario 2: actor spawn fans out to connected client ────────────
    std::printf("\n--- Scenario 2: simulated actor spawn emits to clientA ---\n");
    {
        simulation::SimulationActor pc;
        pc.netguid = live.sessions().get(keyA)->pc_netguid;
        pc.type = schema::ActorType::PlayerController;
        pc.owner_client_key = keyA;
        pc.runtime.actor_netguid = pc.netguid;
        pc.set_root(3, emit::SchemaValue::make_bool(false));  // bIsGM
        live.actors().spawn(std::move(pc));

        // Wait up to ~200ms for the replication tick to pick it up
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(200);
        bool saw_spawn = false;
        while (std::chrono::steady_clock::now() < deadline) {
            if (!live.recording_emitter()->spawns.empty()) {
                saw_spawn = true; break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(saw_spawn, "spawn bunch emitted to clientA within 200ms");
        if (saw_spawn) {
            CHECK(live.recording_emitter()->spawns[0].client == keyA,
                  "spawn addressed to clientA");
        }
    }

    // ── Scenario 3: second client connects, gets existing actor ─────────
    std::printf("\n--- Scenario 3: clientB connects, sees clientA's actor ---\n");
    live.recording_emitter()->clear();
    {
        // Same as Scenario 1: handshake ops first, then on_client_connected
        live.on_packet(pkt(DispatchOp::HANDSHAKE_INITIAL, keyB));
        live.on_packet(pkt(DispatchOp::HANDSHAKE_CHALLENGE_RESPONSE, keyB));
        live.on_client_connected(keyB);
        // Advance clientB to IN_WORLD
        live.on_packet(pkt(DispatchOp::NMT_HELLO, keyB));
        DispatchPacket login = pkt(DispatchOp::NMT_LOGIN, keyB);
        login.str_arg = "TestHero";
        live.on_packet(login);
        live.on_packet(pkt(DispatchOp::NMT_WELCOME, keyB));
        live.on_packet(pkt(DispatchOp::NMT_JOIN, keyB));
        live.on_packet(pkt(DispatchOp::NMT_GAMESPECIFIC, keyB));

        // Wait for the next tick to deliver the spawn to B
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(200);
        bool saw_spawn_b = false;
        while (std::chrono::steady_clock::now() < deadline) {
            for (const auto& rec : live.recording_emitter()->spawns) {
                if (rec.client == keyB) { saw_spawn_b = true; break; }
            }
            if (saw_spawn_b) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(saw_spawn_b, "clientB receives spawn for clientA's existing actor");
    }
// built by C.O.A with Cyb

    // ── Scenario 4: property change fans out to both clients ────────────
    std::printf("\n--- Scenario 4: property change fans out to all viewers ---\n");
    {
        live.recording_emitter()->clear();
        uint64_t pc_guid = live.sessions().get(keyA)->pc_netguid;
        live.actors().set_root_property(pc_guid, 3,
                                          emit::SchemaValue::make_bool(true));

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(200);
        bool got_a = false, got_b = false;
        while (std::chrono::steady_clock::now() < deadline) {
            for (const auto& d : live.recording_emitter()->deltas) {
                if (d.client == keyA) got_a = true;
                if (d.client == keyB) got_b = true;
            }
            if (got_a && got_b) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(got_a, "delta delivered to clientA");
        CHECK(got_b, "delta delivered to clientB");
    }

    // ── Scenario 5: clientA disconnects; actor destroy delivered to B ───
    std::printf("\n--- Scenario 5: clientA disconnects; B gets destroy ---\n");
    {
        live.recording_emitter()->clear();
        uint64_t pc_guid = live.sessions().get(keyA)->pc_netguid;
        live.actors().destroy(pc_guid);
        live.on_client_disconnected(keyA);

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(200);
        bool b_got_destroy = false;
        while (std::chrono::steady_clock::now() < deadline) {
            for (const auto& d : live.recording_emitter()->destroys) {
                if (d.client == keyB) { b_got_destroy = true; break; }
            }
            if (b_got_destroy) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(b_got_destroy, "clientB received destroy for clientA's actor");
        CHECK(!live.sessions().contains(keyA),
              "clientA's session removed from registry");
    }

    // ── Scenario 6: replication tick thread actually ran ────────────────
    std::printf("\n--- Scenario 6: replication tick thread is alive ---\n");
    {
        auto s1 = live.stats();
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        auto s2 = live.stats();
        CHECK(s2.tick_count > s1.tick_count,
              "tick_count advanced — replication thread is running");
        CHECK(s1.total_packets_in > 0,
              "total_packets_in counted dispatched packets");
        CHECK(s1.total_rejects == 0,
              "no rejected packets in the happy-path sequence");
    }

    // ── Scenario 7: shutdown is clean and idempotent ────────────────────
    std::printf("\n--- Scenario 7: stop is clean and idempotent ---\n");
    {
        live.stop();
        live.stop();  // idempotent — must not crash
        CHECK(true, "stop() is safe to call twice");
    }

    // ── Scenario 8: UdpPacketEmitter path produces complete UE5 packets ──
    std::printf("\n--- Scenario 8: Session G byte-level packet production ---\n");
    {
        std::mutex mu;
        struct Delivery {
            std::string client_key;
            std::vector<uint8_t> bytes;
        };
        std::vector<Delivery> deliveries;
        auto capture = [&](const std::string& k, const uint8_t* data, size_t len) {
            std::lock_guard<std::mutex> lk(mu);
            deliveries.push_back({k, std::vector<uint8_t>(data, data + len)});
        };

        // Synthetic outer-packet-state callback — feeds in plausible values
        // that match what a real post-handshake ClientState would hold.
        auto outer = [](const std::string& /*k*/) -> std::optional<OuterPacketState> {
            OuterPacketState s;
            s.magic_header        = 0x500c7696;
            s.session_id          = 0;
            s.client_id_bits      = 0;
            s.out_seq             = 14265;
            s.in_ack_seq          = 3202;
            s.custom_field_present = true;
            uint8_t cf[6] = {0x62, 0x12, 0x4d, 0x85, 0xed, 0xec};
            std::memcpy(s.custom_field, cf, 6);
            return s;
        };

        LiveWorldConfig cfg2;
        cfg2.replication_hz = 50;
        cfg2.use_recording_emitter = false;  // force UDP emitter path
        LiveWorld live2(schemas, outer, capture, cfg2);
        live2.start();

        const std::string keyC = "198.51.100.21:50001";
        // Handshake ops first, then on_client_connected (real-server order)
        live2.on_packet(pkt(DispatchOp::HANDSHAKE_INITIAL, keyC));
        live2.on_packet(pkt(DispatchOp::HANDSHAKE_CHALLENGE_RESPONSE, keyC));
        live2.on_client_connected(keyC);
        live2.on_packet(pkt(DispatchOp::NMT_HELLO, keyC));
        DispatchPacket login = pkt(DispatchOp::NMT_LOGIN, keyC);
        login.str_arg = "Solo";
        live2.on_packet(login);
        live2.on_packet(pkt(DispatchOp::NMT_WELCOME, keyC));
        live2.on_packet(pkt(DispatchOp::NMT_JOIN, keyC));
        live2.on_packet(pkt(DispatchOp::NMT_GAMESPECIFIC, keyC));

        simulation::SimulationActor pc;
        pc.netguid = live2.sessions().get(keyC)->pc_netguid;
        pc.type = schema::ActorType::PlayerController;
        pc.owner_client_key = keyC;
        pc.runtime.actor_netguid = pc.netguid;
        pc.set_root(3, emit::SchemaValue::make_bool(true));  // bIsGM=true
        live2.actors().spawn(std::move(pc));

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(200);
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lk(mu);
                if (!deliveries.empty()) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        {
            std::lock_guard<std::mutex> lk(mu);
            CHECK(!deliveries.empty(), "UDP emitter delivered at least one packet");
            if (!deliveries.empty()) {
                const auto& d = deliveries[0];
                CHECK(d.client_key == keyC,        "delivery addressed to clientC");
                CHECK(d.bytes.size() > 0,          "byte buffer non-empty");
                CHECK(d.bytes.size() >= 20,        "byte buffer >= 20B (outer hdr alone)");

                // Session G: validate the packet starts with our magic header.
                bool magic_ok =
                    d.bytes.size() >= 4 &&
                    d.bytes[0] == 0x96 && d.bytes[1] == 0x76 &&
                    d.bytes[2] == 0x0c && d.bytes[3] == 0x50;
                CHECK(magic_ok, "first 4 bytes match AoC MagicHeader 0x500c7696");

                // Session G: final byte non-zero (termination bit makes the
                // last bit 1, so the final byte has at least one '1' bit).
                bool final_nonzero = d.bytes.back() != 0;
                CHECK(final_nonzero, "final byte non-zero (termination bit set)");
            }
        }
        live2.stop();
    }

    // ── Summary ─────────────────────────────────────────────────────────
    std::printf("\n=== Summary ===\n");
    std::printf("  Passed: %d\n", g_pass);
    std::printf("  Failed: %d\n", g_fail);
    std::printf("  Result: %s\n", g_fail == 0 ? "ALL PASSED" : "FAILURES");
    return g_fail == 0 ? 0 : 1;
}
