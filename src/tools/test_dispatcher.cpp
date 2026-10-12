// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tools/test_dispatcher.cpp
//
//  Session E exit-criterion test.
//
//  "Driving the dispatcher with a sequence of simulated packets advances a
//   ClientSession through the correct state transitions, and each packet's
//   handler fires the right stats counter."
//
//  We intentionally DO NOT require the full UDP framing / bunch parser here
//  — Session F will swap in real parsing on top.  The point of Session E is
//  that the state machine is correct when given well-formed opcodes.
// ============================================================================
#include "net/client_session.h"
#include "net/session_registry.h"
#include "net/opcode_dispatcher.h"
#include "protocol/net_guid_allocator.h"
#include "protocol/wire/ue5_primitives.h"
#include <cstdio>
#include <string>

using namespace aoc;
using namespace aoc::net;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static DispatchPacket pkt(DispatchOp op, const std::string& key) {
    DispatchPacket p; p.op = op; p.client_key = key; return p;
}

int main() {
    std::printf("=== Session E dispatcher state-machine test ===\n");

    SessionRegistry registry;
    protocol::NetGuidAllocator allocator;
    OpcodeDispatcher dispatcher(registry, &allocator, /*event_bus=*/nullptr);

    const std::string keyA = "192.0.2.1:55001";
    const std::string keyB = "192.0.2.2:55002";

    // ── Scenario 1: happy path — full connect → in-world sequence for A ──
    std::printf("\n--- Scenario 1: clientA walks the full state machine ---\n");
// C-Y-B.C-O-A

    {
        // 1. Handshake initial → HANDSHAKE_IN_PROGRESS
        auto r = dispatcher.dispatch(pkt(DispatchOp::HANDSHAKE_INITIAL, keyA));
        CHECK(r.accepted,                              "handshake_initial accepted");
        CHECK(r.phase_changed,                         "phase changed on handshake_initial");
        CHECK(r.old_phase == ClientPhase::AWAITING_HANDSHAKE,
                                                        "was AWAITING_HANDSHAKE before");
        CHECK(r.new_phase == ClientPhase::HANDSHAKE_IN_PROGRESS,
                                                        "is HANDSHAKE_IN_PROGRESS after");
        CHECK(registry.contains(keyA),                 "session was created implicitly");
    }
    {
        // 2. Handshake response → NMT_NEGOTIATING
        auto r = dispatcher.dispatch(pkt(DispatchOp::HANDSHAKE_CHALLENGE_RESPONSE, keyA));
        CHECK(r.accepted,                               "handshake_response accepted");
        CHECK(r.new_phase == ClientPhase::NMT_NEGOTIATING, "now NMT_NEGOTIATING");
    }
    {
        // 3. NMT_Hello (no phase change) — just an acknowledgement from client
        auto r = dispatcher.dispatch(pkt(DispatchOp::NMT_HELLO, keyA));
        CHECK(r.accepted,                                   "nmt_hello accepted");
        CHECK(!r.phase_changed,                              "nmt_hello stays in NMT_NEGOTIATING");
        CHECK(r.new_phase == ClientPhase::NMT_NEGOTIATING,   "still NMT_NEGOTIATING");
    }
    {
        // 4. NMT_Login with player name → AUTHENTICATED
        DispatchPacket p = pkt(DispatchOp::NMT_LOGIN, keyA);
        p.str_arg  = "Hatemost";
        p.str_arg2 = "steam:12345";
        auto r = dispatcher.dispatch(p);
        CHECK(r.accepted,                               "nmt_login accepted");
        CHECK(r.new_phase == ClientPhase::AUTHENTICATED,"now AUTHENTICATED");
        ClientSession* cs = registry.get(keyA);
        CHECK(cs && cs->player_name == "Hatemost",      "player_name stored on session");
        CHECK(cs && cs->pc_netguid != 0,                "NetGUID block was allocated");
        CHECK(cs && cs->pawn_netguid == cs->pc_netguid + 2,
                                                        "pawn_netguid = pc+2 (stride 2)");
    }
    {
        // 5. Server sends NMT_Welcome (modeled as advance) → LOADING_MAP
        auto r = dispatcher.dispatch(pkt(DispatchOp::NMT_WELCOME, keyA));
        CHECK(r.accepted,                               "nmt_welcome accepted");
        CHECK(r.new_phase == ClientPhase::LOADING_MAP,  "now LOADING_MAP");
    }
    {
        // 6. Client NMT_Join → SPAWNING
        auto r = dispatcher.dispatch(pkt(DispatchOp::NMT_JOIN, keyA));
        CHECK(r.accepted,                               "nmt_join accepted");
        CHECK(r.new_phase == ClientPhase::SPAWNING,     "now SPAWNING");
    }
    {
        // 7. Client NMT_GameSpecific after spawn → IN_WORLD
        auto r = dispatcher.dispatch(pkt(DispatchOp::NMT_GAMESPECIFIC, keyA));
        CHECK(r.accepted,                               "nmt_game_specific accepted");
        CHECK(r.new_phase == ClientPhase::IN_WORLD,     "now IN_WORLD");
    }
    {
        // 8. Once in-world, movement packets flow freely without phase change
        auto r = dispatcher.dispatch(pkt(DispatchOp::ACTOR_MOVEMENT, keyA));
        CHECK(r.accepted,                               "actor_movement accepted in world");
        CHECK(!r.phase_changed,                         "actor_movement leaves phase unchanged");
        CHECK(r.new_phase == ClientPhase::IN_WORLD,     "still IN_WORLD");
    }
    {
        // 8b. In-world actor RPC packets are accepted and update ability state
        DispatchPacket p = pkt(DispatchOp::ACTOR_RPC, keyA);
        p.u64_arg = 10341564;
        p.fn_idx  = 188;
        p.str_arg = "ServerTriggerAbility";
        p.ability_guid = "2982C015-D50A7996-00000000-00000001";
        p.instance_id  = 2145386496;
        p.anim_index   = 0;
        auto r = dispatcher.dispatch(p);
        CHECK(r.accepted,                               "trigger_ability accepted in world");
        CHECK(!r.phase_changed,                         "trigger_ability leaves phase unchanged");
        CHECK(r.new_phase == ClientPhase::IN_WORLD,     "still IN_WORLD");
        CHECK(dispatcher.stats().actor_rpc == 1,        "actor_rpc stat incremented");
        CHECK(dispatcher.stats().ability_trigger == 1,  "ability_trigger stat incremented");
        ClientSession* cs = registry.get(keyA);
        CHECK(cs && cs->active_ability_guid_str == "2982C015-D50A7996-00000000-00000001",
                                                        "active_ability_guid_str set");
        CHECK(cs && cs->active_ability_instance_id == 2145386496,
                                                        "active_ability_instance_id set");
        CHECK(cs && cs->total_abilities_triggered == 1, "total_abilities_triggered = 1");
    }
    {
        // 8c. In-world ServerEndAbility is accepted and updates ability counters
        DispatchPacket p = pkt(DispatchOp::ACTOR_RPC, keyA);
        p.u64_arg = 10341564;
        p.fn_idx  = 216;
        p.str_arg = "ServerEndAbility";
        p.instance_id  = 120;
        auto r = dispatcher.dispatch(p);
        CHECK(r.accepted,                               "end_ability accepted in world");
        CHECK(!r.phase_changed,                         "end_ability leaves phase unchanged");
        CHECK(r.new_phase == ClientPhase::IN_WORLD,     "still IN_WORLD");
        CHECK(dispatcher.stats().actor_rpc == 2,        "actor_rpc stat = 2");
        CHECK(dispatcher.stats().ability_end == 1,      "ability_end stat incremented");
        ClientSession* cs = registry.get(keyA);
        CHECK(cs && cs->total_abilities_ended == 1,     "total_abilities_ended = 1");
    }
    {
        // 8d. In-world RequestHitVerification_Server is accepted and updates hit verification counters
        DispatchPacket p = pkt(DispatchOp::ACTOR_RPC, keyA);
        p.u64_arg       = 0x5429e688310d;
        p.hit_guid      = 0x5429e688317c;
        p.hit_component = "CollisionCylinder";
        p.fn_idx        = 246;
        p.str_arg       = "RequestHitVerification_Server";
        auto r = dispatcher.dispatch(p);
        CHECK(r.accepted,                               "hit_verification accepted in world");
        CHECK(!r.phase_changed,                         "hit_verification leaves phase unchanged");
        CHECK(r.new_phase == ClientPhase::IN_WORLD,     "still IN_WORLD");
        CHECK(dispatcher.stats().actor_rpc == 3,        "actor_rpc stat = 3");
        CHECK(dispatcher.stats().hit_verification == 1, "hit_verification stat incremented");
        ClientSession* cs = registry.get(keyA);
        CHECK(cs && cs->total_hits_verified == 1,       "total_hits_verified = 1");
        CHECK(cs && cs->last_verified_ability_guid == 0x5429e688310d,
                                                        "last_verified_ability_guid set");
        CHECK(cs && cs->last_verified_hit_guid == 0x5429e688317c,
                                                        "last_verified_hit_guid set");
        CHECK(cs && cs->last_verified_hit_component == "CollisionCylinder",
                                                        "last_verified_hit_component set");
        CHECK(cs && cs->total_hits_confirmed == 1,      "total_hits_confirmed = 1");
    }
    {
        // 8e. Contract-level wire test for UAoCAbilityComponent::ProxyMulticastHitConfirmation
        // Validates the exact 796-bit bunch structure, 21-bit field header, and 628-bit parameters
        uint8_t buf[256] = {};
        size_t off = 0;

        // ContentBlock header (147 bits)
        ue5::write_bits(buf, sizeof(buf), off, 0, 1);  // bHasRepLayout=0 (RPC)
        ue5::write_bits(buf, sizeof(buf), off, 0, 1);  // bIsActor=0 (SubObject)
        ue5::write_bits(buf, sizeof(buf), off, 0x9dccbc, 32); // UAoCAbilityComponent lo
        ue5::write_bits(buf, sizeof(buf), off, 0, 32);        // hi
        ue5::write_bits(buf, sizeof(buf), off, 60, 32);       // Srv
        ue5::write_bits(buf, sizeof(buf), off, 0x6ee87ee4, 32); // Rnd
        ue5::write_bits(buf, sizeof(buf), off, 1, 1);  // bStablyNamed=1 (Default subobject)
        ue5::write_sip(buf, sizeof(buf), off, 649);   // NumPayloadBits = 649 (21 + 628)
        CHECK(off == 147, "ProxyMulticastHitConfirmation ContentBlock header = 147 bits");

        // Field header (21 bits)
        ue5::write_bits(buf, sizeof(buf), off, 21, 5); // RepIndex=21 (5 bits: 0b10101)
        ue5::write_sip(buf, sizeof(buf), off, 628);   // NumFieldPayloadBits = 628
        CHECK(off == 168, "ProxyMulticastHitConfirmation Field header = 21 bits (cum 168)");

        // Parameters (628 bits)
        // Top-level Param 0 (HitInst): presence bit
        ue5::write_bits(buf, sizeof(buf), off, 1, 1);  // bSend=1 (presence bit for struct)
        // HitInst struct payload (625 bits)
        ue5::write_bits(buf, sizeof(buf), off, 1, 64); // InstGuid (64b)
        ue5::write_bits(buf, sizeof(buf), off, 0x5429e688317c01c6ULL, 64); // RecordGuid
        ue5::write_bits(buf, sizeof(buf), off, 0x01c5ULL, 64);             // SourceRecordType
        ue5::write_bits(buf, sizeof(buf), off, 0x5429e688310d01c5ULL, 64); // SourceRecordGuid
        ue5::write_bits(buf, sizeof(buf), off, 0, 1);                      // bPvPForceFlag (1b bool)
        ue5::write_bits(buf, sizeof(buf), off, 0, 32);                     // DelayTime (32b float)
        ue5::write_bits(buf, sizeof(buf), off, 88, 32); // Instigator NetGUID (128b)
        ue5::write_bits(buf, sizeof(buf), off, 0, 32);
        ue5::write_bits(buf, sizeof(buf), off, 60, 32);
        ue5::write_bits(buf, sizeof(buf), off, 0x6ee87ee4, 32);
        ue5::write_bits(buf, sizeof(buf), off, 88, 32); // Source NetGUID (128b)
        ue5::write_bits(buf, sizeof(buf), off, 0, 32);
        ue5::write_bits(buf, sizeof(buf), off, 60, 32);
        ue5::write_bits(buf, sizeof(buf), off, 0x6ee87ee4, 32);
        ue5::write_bits(buf, sizeof(buf), off, 0, 16); // Targets count (16b uint16 = 0)
        ue5::write_bits(buf, sizeof(buf), off, 0, 64); // SynchronizedTime (64b int64 = 0)
        // Top-level Param 1: bPredictedOnAutonomous (1b bool = 1)
        ue5::write_bits(buf, sizeof(buf), off, 1, 1);
        // Top-level Param 2: bPredictedOnSimulated (1b bool = 0)
        ue5::write_bits(buf, sizeof(buf), off, 0, 1);
        CHECK(off == 796, "ProxyMulticastHitConfirmation total bunch data = 796 bits");
        CHECK(off - 168 == 628, "ProxyMulticastHitConfirmation param bits = 628 bits");
    }
// [CoA :: C-Y-B]

    // ── Scenario 2: phase guards reject out-of-order packets ────────────────
    std::printf("\n--- Scenario 2: phase-guard rejections for clientB ---\n");
    {
        // No session exists for B yet; anything except HANDSHAKE_INITIAL is rejected
        auto r = dispatcher.dispatch(pkt(DispatchOp::NMT_LOGIN, keyB));
        CHECK(!r.accepted,                              "nmt_login before handshake rejected");
        CHECK(!registry.contains(keyB),                 "no session created on rejected op");
    }
    {
        // Create the session properly
        auto r = dispatcher.dispatch(pkt(DispatchOp::HANDSHAKE_INITIAL, keyB));
        CHECK(r.accepted,                               "handshake_initial accepted for B");
    }
    {
        // Trying NMT_Login before the challenge response should fail
        DispatchPacket p = pkt(DispatchOp::NMT_LOGIN, keyB);
        p.str_arg = "TestHero";
        auto r = dispatcher.dispatch(p);
        CHECK(!r.accepted,                              "nmt_login in HANDSHAKE_IN_PROGRESS rejected");
        CHECK(!r.phase_changed,                         "rejection leaves phase unchanged");
        CHECK(r.new_phase == ClientPhase::HANDSHAKE_IN_PROGRESS,
                                                        "still HANDSHAKE_IN_PROGRESS after rejection");
    }
    {
        // Trying actor movement in HANDSHAKE_IN_PROGRESS is rejected too
        auto r = dispatcher.dispatch(pkt(DispatchOp::ACTOR_MOVEMENT, keyB));
        CHECK(!r.accepted,                              "actor_movement pre-world rejected");
    }
    {
        // Fix up the phase and retry login with empty name → reject
        dispatcher.dispatch(pkt(DispatchOp::HANDSHAKE_CHALLENGE_RESPONSE, keyB));
        DispatchPacket p = pkt(DispatchOp::NMT_LOGIN, keyB);
        p.str_arg = "";
        auto r = dispatcher.dispatch(p);
        CHECK(!r.accepted,                              "nmt_login with empty name rejected");
    }

    // ── Scenario 3: disconnect + cleanup ────────────────────────────────────
    std::printf("\n--- Scenario 3: clientA disconnects, block released ---\n");
    {
        ClientSession* cs = registry.get(keyA);
        uint64_t owned_base = cs ? cs->netguid_block.base : 0;
        CHECK(owned_base != 0,                          "clientA has an allocated NetGUID block");
        CHECK(allocator.live_block_count() >= 1,        "allocator reports >=1 live block");

        auto r = dispatcher.dispatch(pkt(DispatchOp::CLIENT_DISCONNECT, keyA));
        CHECK(r.accepted,                               "client_disconnect accepted");
        CHECK(r.new_phase == ClientPhase::DISCONNECTING,"now DISCONNECTING");

        // Block is released on disconnect; the session persists until the
        // registry drops it explicitly.
        registry.remove(keyA);
        CHECK(!registry.contains(keyA),                 "session removed from registry");
    }

    // ── Scenario 4: NMT_Abort from anywhere goes to DISCONNECTING ───────────
    std::printf("\n--- Scenario 4: NMT_Abort works from any phase ---\n");
    {
        const std::string keyC = "192.0.2.3:55003";
        dispatcher.dispatch(pkt(DispatchOp::HANDSHAKE_INITIAL, keyC));
        auto r = dispatcher.dispatch(pkt(DispatchOp::NMT_ABORT, keyC));
        CHECK(r.accepted,                               "nmt_abort accepted mid-handshake");
        CHECK(r.new_phase == ClientPhase::DISCONNECTING,"mid-handshake abort → DISCONNECTING");
    }

    // ── Scenario 5: concurrent sessions stay isolated ──────────────────────
    std::printf("\n--- Scenario 5: sessions are independent ---\n");
    {
        const std::string keyD = "192.0.2.4:55004";
        const std::string keyE = "192.0.2.5:55005";
        dispatcher.dispatch(pkt(DispatchOp::HANDSHAKE_INITIAL, keyD));
        dispatcher.dispatch(pkt(DispatchOp::HANDSHAKE_INITIAL, keyE));
        dispatcher.dispatch(pkt(DispatchOp::HANDSHAKE_CHALLENGE_RESPONSE, keyD));
        // D is in NMT_NEGOTIATING, E is in HANDSHAKE_IN_PROGRESS
        ClientSession* csD = registry.get(keyD);
        ClientSession* csE = registry.get(keyE);
        CHECK(csD && csD->phase == ClientPhase::NMT_NEGOTIATING,
              "clientD in NMT_NEGOTIATING");
        CHECK(csE && csE->phase == ClientPhase::HANDSHAKE_IN_PROGRESS,
              "clientE in HANDSHAKE_IN_PROGRESS (not advanced by D)");
    }

    // ── Scenario 6: summary counts ──────────────────────────────────────────
    std::printf("\n--- Scenario 6: stats counters look right ---\n");
    {
        const auto& s = dispatcher.stats();
        CHECK(s.handshake_initial    >= 4,  "handshake_initial fired at least 4 times");
        CHECK(s.handshake_response   >= 2,  "handshake_response fired at least 2 times");
        CHECK(s.nmt_hello            >= 1,  "nmt_hello fired at least once");
        CHECK(s.nmt_login            >= 3,  "nmt_login fired at least 3 times (incl. rejects)");
        CHECK(s.nmt_welcome          >= 1,  "nmt_welcome fired");
        CHECK(s.nmt_join             >= 1,  "nmt_join fired");
        CHECK(s.nmt_game_specific    >= 1,  "nmt_game_specific fired");
        CHECK(s.actor_movement       >= 2,  "actor_movement fired (accepted + rejected)");
        CHECK(s.nmt_abort            >= 1,  "nmt_abort fired");
        CHECK(s.client_disconnect    >= 1,  "client_disconnect fired");
        CHECK(s.rejected_wrong_phase >= 3,  "rejected_wrong_phase counts out-of-order traffic");
        CHECK(s.rejected_missing_session >= 1, "rejected_missing_session counts no-session op");
    }

    // ── Scenario 7: registry introspection ──────────────────────────────────
    std::printf("\n--- Scenario 7: registry introspection works ---\n");
    {
        auto summaries = registry.summarize();
        CHECK(!summaries.empty(),                       "registry summarize() returns rows");
        size_t disconnecting_count = 0;
        for (const auto& s : summaries) {
            if (s.phase == ClientPhase::DISCONNECTING) disconnecting_count++;
        }
        CHECK(disconnecting_count >= 1,                 "at least one session in DISCONNECTING");
    }

    // ── Summary ─────────────────────────────────────────────────────────────
    std::printf("\n=== Summary ===\n");
    std::printf("  Passed: %d\n", g_pass);
    std::printf("  Failed: %d\n", g_fail);
    std::printf("  Result: %s\n", g_fail == 0 ? "ALL PASSED" : "FAILURES");
    std::printf("  Live sessions at exit: %zu\n", registry.size());
    std::printf("  Total NetGUID blocks issued: %llu\n",
                 static_cast<unsigned long long>(allocator.total_slots_issued()));
    return g_fail == 0 ? 0 : 1;
}
