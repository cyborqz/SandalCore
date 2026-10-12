// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tools/test_simulation.cpp
//
//  Session D exit-criterion test.  Verifies the Simulation Layer works in
//  isolation:
//    - ActorRegistry spawn/destroy round-trip
//    - Event bus fires correct events in correct order
//    - No replication concerns (we don't instantiate any replication code)
//    - Property mutations trigger PropertyChanged events
//    - Location mutations trigger ActorMoved events
//    - Per architectural correction 1: simulation has NO awareness of clients
// ============================================================================
#include "world/events/world_events.h"
#include "world/simulation/world_clock.h"
#include "world/simulation/actor_registry.h"
#include <cstdio>
#include <atomic>

using namespace aoc;
using namespace aoc::protocol;
using namespace aoc::world;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

int main() {
    std::printf("=== Session D simulation-layer test ===\n");

    events::EventBus bus;
    simulation::WorldClock clock;
    simulation::ActorRegistry registry(bus, clock);

    // ── Set up listeners to record events ──
    std::atomic<int> spawn_count{0};
    std::atomic<int> destroy_count{0};
    std::atomic<int> prop_changed_count{0};
    std::atomic<int> moved_count{0};
// ~ C.O.A & Cyb ~

    uint64_t last_spawn_guid = 0;
    int last_prop_component = 999;
    uint32_t last_prop_handle = 0;

    bus.subscribe<events::ActorSpawned>([&](const events::ActorSpawned& e) {
        spawn_count++;
        last_spawn_guid = e.netguid;
    });
    bus.subscribe<events::ActorDestroyed>([&](const events::ActorDestroyed&) {
        destroy_count++;
    });
    bus.subscribe<events::PropertyChanged>([&](const events::PropertyChanged& e) {
        prop_changed_count++;
        last_prop_component = e.component_index;
        last_prop_handle = e.handle;
    });
    bus.subscribe<events::ActorMoved>([&](const events::ActorMoved&) {
        moved_count++;
    });

    CHECK(bus.listener_count() == 4, "4 listeners registered");

    // ── Spawn 2 actors ──
    simulation::SimulationActor pc;
    pc.netguid = 0x01000000;
    pc.type = schema::ActorType::PlayerController;
    pc.owner_client_key = "client_A";
    pc.runtime.actor_netguid = pc.netguid;
    pc.set_root(3, emit::SchemaValue::make_bool(false));  // bIsGM = false initially
    pc.set_root(13, emit::SchemaValue::make_u32(17748));  // Cleric

    CHECK(registry.spawn(std::move(pc)), "spawn PC returns true");
    CHECK(spawn_count.load() == 1, "ActorSpawned event fired");
    CHECK(last_spawn_guid == 0x01000000, "spawn event carries correct netguid");

    simulation::SimulationActor pawn;
    pawn.netguid = 0x01000001;
    pawn.type = schema::ActorType::Pawn;
    pawn.owner_client_key = "client_A";
    pawn.runtime.actor_netguid = pawn.netguid;
    // CharacterName on component 2
    pawn.set_component(2, 1, emit::SchemaValue::make_fstring("Hatemost"));

    CHECK(registry.spawn(std::move(pawn)), "spawn Pawn returns true");
    CHECK(spawn_count.load() == 2, "second ActorSpawned fired");
    CHECK(registry.size() == 2, "registry has 2 actors");

    // ── Duplicate spawn must fail ──
    simulation::SimulationActor dup;
    dup.netguid = 0x01000000;
    CHECK(!registry.spawn(std::move(dup)), "duplicate netguid spawn returns false");
    CHECK(spawn_count.load() == 2, "duplicate spawn did NOT fire an event");

    // ── Mutate properties ──
    // Change bIsGM: 0 → 1  (no-op if same value; should emit on change)
    bool changed = registry.set_root_property(
        0x01000000, 3, emit::SchemaValue::make_bool(true));
    CHECK(changed, "bIsGM 0→1 returns changed=true");
    CHECK(prop_changed_count.load() == 1, "PropertyChanged fired once for bIsGM");
    CHECK(last_prop_component == -1, "event for root property has component_index=-1");
    CHECK(last_prop_handle == 3, "event carries correct handle");

    // Redundant write: same value, should be no-op
    changed = registry.set_root_property(
        0x01000000, 3, emit::SchemaValue::make_bool(true));
    CHECK(!changed, "bIsGM 1→1 returns changed=false (no-op)");
    CHECK(prop_changed_count.load() == 1, "no event fired for no-op write");

    // Change component property
    changed = registry.set_component_property(
        0x01000001, 2, 1, emit::SchemaValue::make_fstring("Hatemost2"));
    CHECK(changed, "CharacterName rename returns changed=true");
    CHECK(prop_changed_count.load() == 2, "PropertyChanged fired for component update");
    CHECK(last_prop_component == 2, "component index preserved");
    CHECK(last_prop_handle == 1, "component handle preserved");

    // ── Move ──
    emit::FVector3 new_loc{100.0f, 200.0f, 50.0f};
    emit::FVector3 vel{1.0f, 0.0f, 0.0f};
    bool moved = registry.set_location(0x01000001, new_loc, vel);
    CHECK(moved, "set_location returns true");
    CHECK(moved_count.load() == 1, "ActorMoved fired");

    // ── Verify authoritative state ──
    const simulation::SimulationActor* pc_now = registry.get(0x01000000);
    CHECK(pc_now != nullptr, "PC still in registry");
    if (pc_now) {
        auto* bIsGM = pc_now->get_root(3);
        CHECK(bIsGM && bIsGM->b == true, "bIsGM is now true (authoritatively)");
        auto* arch = pc_now->get_root(13);
        CHECK(arch && arch->u == 17748, "CharacterArchetype is still Cleric");
    }

    const simulation::SimulationActor* pawn_now = registry.get(0x01000001);
    if (pawn_now) {
        auto* name = pawn_now->get_component(2, 1);
        CHECK(name && name->str == "Hatemost2", "CharacterName is Hatemost2");
        CHECK(pawn_now->runtime.spawn_location.x == 100.0f,
              "spawn_location updated to new coordinates");
    }
// == C Y B | CoA ==

    // ── Snapshot for replication-tick pattern ──
    auto snap = registry.snapshot();
    CHECK(snap.size() == 2, "snapshot() returns 2 actors");

    // ── Destroy ──
    registry.destroy(0x01000000);
    CHECK(destroy_count.load() == 1, "ActorDestroyed fired");
    CHECK(registry.size() == 1, "registry size after destroy");
    CHECK(registry.get(0x01000000) == nullptr, "destroyed actor no longer retrievable");

    // Double-destroy is no-op
    registry.destroy(0x01000000);
    CHECK(destroy_count.load() == 1, "second destroy fires no event");

    // ── Tick configurability (correction 3) ──
    simulation::TickConfig cfg;
    CHECK(cfg.simulation_hz == 30, "default simulation_hz = 30");
    CHECK(cfg.replication_hz == 20, "default replication_hz = 20");
    cfg.simulation_hz = 60;
    CHECK(cfg.simulation_hz == 60, "simulation_hz is mutable");

    auto interval = simulation::WorldClock::interval_us(20);
    CHECK(interval.count() == 50000, "20Hz → 50ms interval");

    std::printf("\n=== Summary ===\n");
    std::printf("  Passed: %d\n", g_pass);
    std::printf("  Failed: %d\n", g_fail);
    std::printf("  Result: %s\n", g_fail == 0 ? "ALL PASSED" : "FAILURES");
    std::printf("\n  Events tally: spawn=%d destroy=%d prop_changed=%d moved=%d\n",
                spawn_count.load(), destroy_count.load(),
                prop_changed_count.load(), moved_count.load());
    return g_fail == 0 ? 0 : 1;
}
