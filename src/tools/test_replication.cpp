// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tools/test_replication.cpp
//
//  Session D' exit-criterion test.  Wires simulation (Session D) + replication
//  (Session D') end-to-end with a mock PacketEmitter that records every
//  emission.  Then scripts a scenario and verifies the right packets
//  were queued for the right clients in the right order.
// ============================================================================
#include "world/events/world_events.h"
#include "world/simulation/world_clock.h"
#include "world/simulation/actor_registry.h"
#include "world/replication/packet_emitter.h"
#include "world/replication/visibility_manager.h"
#include "world/replication/broadcast_manager.h"
#include <cstdio>
#include <vector>

using namespace aoc;
using namespace aoc::protocol;
using namespace aoc::world;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

/// Mock emitter — records every call without sending real packets.
class RecordingEmitter : public replication::IPacketEmitter {
public:
    struct SpawnRecord   { std::string client; uint64_t netguid; };
    struct DeltaRecord   { std::string client; uint64_t netguid;
                           std::vector<replication::ChangedProperty> changes; };
    struct DestroyRecord { std::string client; uint64_t netguid; };
    struct MoveRecord    { std::string client; uint64_t netguid;
                           emit::FVector3 loc; emit::FVector3 vel; };

    std::vector<SpawnRecord>   spawns;
    std::vector<DeltaRecord>   deltas;
    std::vector<DestroyRecord> destroys;
    std::vector<MoveRecord>    moves;

    void emit_spawn(const std::string& c, const simulation::SimulationActor& a) override {
        spawns.push_back({c, a.netguid});
    }
    void emit_property_delta(const std::string& c, const simulation::SimulationActor& a,
                             const std::vector<replication::ChangedProperty>& ch) override {
        deltas.push_back({c, a.netguid, ch});
    }
    void emit_destroy(const std::string& c, uint64_t guid) override {
        destroys.push_back({c, guid});
    }
    void emit_movement(const std::string& c, uint64_t guid,
                       emit::FVector3 loc, emit::FVector3 vel) override {
        moves.push_back({c, guid, loc, vel});
    }
// ~ C | O | A :: C | Y | B ~

    void clear() { spawns.clear(); deltas.clear(); destroys.clear(); moves.clear(); }
};

int main() {
    std::printf("=== Session D' replication-layer test ===\n");

    events::EventBus bus;
    simulation::WorldClock clock;
    simulation::ActorRegistry actors(bus, clock);
    replication::VisibilityManager vis;
    RecordingEmitter mock;
    replication::BroadcastManager broadcast(bus, actors, vis, mock);
    broadcast.install_subscriptions();

    // ── Scenario 1: first client connects, then actor spawns ──
    std::printf("\n--- Scenario 1: client A connects, then actor spawns ---\n");
    broadcast.add_client("clientA");
    CHECK(mock.spawns.empty(), "no spawn yet (no actors exist)");

    simulation::SimulationActor pc_a;
    pc_a.netguid = 0x01000000;
    pc_a.type = schema::ActorType::PlayerController;
    pc_a.owner_client_key = "clientA";
    pc_a.runtime.actor_netguid = pc_a.netguid;
    pc_a.set_root(3, emit::SchemaValue::make_bool(false));  // bIsGM
    actors.spawn(std::move(pc_a));

    broadcast.tick();
    CHECK(mock.spawns.size() == 1, "1 spawn emitted to clientA after tick");
    if (!mock.spawns.empty()) {
        CHECK(mock.spawns[0].client == "clientA", "spawn addressed to clientA");
        CHECK(mock.spawns[0].netguid == 0x01000000, "spawn carries correct netguid");
    }

    // Second tick with no changes → nothing new emitted
    size_t emits_before = broadcast.total_emits();
    broadcast.tick();
    CHECK(broadcast.total_emits() == emits_before,
          "idle tick produces no emits (actor unchanged, no visibility changes)");

    // ── Scenario 2: property change ──
    std::printf("\n--- Scenario 2: flip bIsGM, tick, verify delta ---\n");
    mock.clear();
    actors.set_root_property(0x01000000, 3, emit::SchemaValue::make_bool(true));
    CHECK(mock.deltas.empty(), "delta not yet emitted (needs tick)");

    broadcast.tick();
    CHECK(mock.deltas.size() == 1, "1 property_delta emitted after tick");
    if (!mock.deltas.empty()) {
        CHECK(mock.deltas[0].client == "clientA", "delta addressed to clientA");
        CHECK(mock.deltas[0].netguid == 0x01000000, "delta correct netguid");
        CHECK(mock.deltas[0].changes.size() == 1, "1 changed property");
        if (!mock.deltas[0].changes.empty()) {
            CHECK(mock.deltas[0].changes[0].component_index == -1, "change on root");
            CHECK(mock.deltas[0].changes[0].handle == 3, "changed handle is bIsGM (3)");
        }
    }

    // ── Scenario 3: second client joins, sees existing actor ──
    std::printf("\n--- Scenario 3: clientB joins, should see clientA's actor ---\n");
    mock.clear();
    broadcast.add_client("clientB");
    broadcast.tick();
    CHECK(mock.spawns.size() == 1, "clientB gets spawn bunch for existing actor");
    if (!mock.spawns.empty()) {
        CHECK(mock.spawns[0].client == "clientB", "spawn addressed to clientB");
        CHECK(mock.spawns[0].netguid == 0x01000000, "same actor id");
    }

    // ── Scenario 4: movement broadcasts to both clients ──
    std::printf("\n--- Scenario 4: actor moves, both clients receive update ---\n");
    mock.clear();
    emit::FVector3 new_loc{50.0f, 100.0f, 10.0f};
    emit::FVector3 vel{1.0f, 0.0f, 0.0f};
    actors.set_location(0x01000000, new_loc, vel);
    broadcast.tick();

    CHECK(mock.moves.size() == 2, "2 movement emits (one per client)");
    bool a_received = false, b_received = false;
    for (const auto& m : mock.moves) {
        if (m.client == "clientA") a_received = true;
        if (m.client == "clientB") b_received = true;
    }
    CHECK(a_received && b_received, "both clientA and clientB received movement");

    // ── Scenario 5: property change on actor visible to both ──
    std::printf("\n--- Scenario 5: property change fans out to both clients ---\n");
    mock.clear();
    actors.set_root_property(0x01000000, 3, emit::SchemaValue::make_bool(false));
    broadcast.tick();
    CHECK(mock.deltas.size() == 2, "property delta fans out to both clients");

    // ── Scenario 6: destroy propagates to all viewers ──
    std::printf("\n--- Scenario 6: destroy fans out to both viewers ---\n");
    mock.clear();
    actors.destroy(0x01000000);
    broadcast.tick();
    CHECK(mock.destroys.size() == 2, "destroy fanned out to both clients");

    // ── Scenario 7: client leaves, no residual emits ──
    std::printf("\n--- Scenario 7: clientB leaves ---\n");
    mock.clear();
    broadcast.remove_client("clientB");

    // Spawn a new actor; only clientA should receive
    simulation::SimulationActor npc;
    npc.netguid = 0x02000000;
    npc.type = schema::ActorType::NPC;
    actors.spawn(std::move(npc));
    broadcast.tick();

    CHECK(mock.spawns.size() == 1, "only 1 spawn emitted (clientB is gone)");
    if (!mock.spawns.empty()) {
        CHECK(mock.spawns[0].client == "clientA", "spawn went to clientA only");
    }

    std::printf("\n=== Summary ===\n");
    std::printf("  Passed: %d\n", g_pass);
    std::printf("  Failed: %d\n", g_fail);
    std::printf("  Result: %s\n", g_fail == 0 ? "ALL PASSED" : "FAILURES");
    std::printf("\n  Total emits across all scenarios: %zu\n", broadcast.total_emits());
    return g_fail == 0 ? 0 : 1;
}
