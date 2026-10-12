// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  libs/aocnet/tests/test_serialize_new_actor.cpp
//
//  The spawn record at the head of an actor-channel open, checked bit for
//  bit against the original server's PlayerController open
//  (fixtures/replay_data.bin pkt#22, ch=3 chSeq 954/955; the export block
//  fills the first partial bunch, so the record starts the second):
//    UPackageMapClient::SerializeNewActor   actor, archetype, level GUIDs;
//      location (UE5 quantized vector: SerializeInt(scaled<<6|bits, 128) +
//      3 two's-complement components), rotation/scale/velocity flags
//    APlayerController::OnSerializeNewActor NetPlayerIndex(8) +
//      ClientHandshakeId(32)
//  tools/decode_actor_open.py parses all 743 captured opens with this layout.
// ============================================================================
#include "protocol/emit/actor_builder.h"
#include "protocol/emit/bunch_writer.h"
#include "protocol/schema/schema_registry.h"

#include <cstdio>

using namespace aoc::protocol;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

// The first 508 bits after the export block of captured pkt#22 (PC open).
static const uint8_t kCapturedPcSpawnRecord[] = {
    0x9a, 0xcc, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x00, 0xe4, 0x7e, 0xe8, 0x6e,
    0x33, 0x3c, 0x2a, 0x91, 0xf5, 0xd7, 0x9f, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x19, 0xe2, 0xc5, 0x6a, 0xfe, 0x6a, 0x2f, 0xe4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x63, 0xf7, 0xac, 0x12, 0x37, 0x2a, 0xbc, 0x40, 0xde, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
constexpr size_t kCapturedPcSpawnRecordBits = 508;
constexpr size_t kSerializeNewActorBits = 468;   // the rest is the PlayerController's 40 bits

static bool same_bits(const uint8_t* a, const uint8_t* b, size_t n, size_t* first_diff) {
    for (size_t i = 0; i < n; ++i) {
        if (((a[i >> 3] >> (i & 7)) & 1) != ((b[i >> 3] >> (i & 7)) & 1)) {
            *first_diff = i;
            return false;
        }
    }
    return true;
}
// ~ C Y B - C-O-A ~

static emit::ActorRuntime captured_pc_runtime() {
    emit::ActorRuntime rt;
    rt.type                 = schema::ActorType::PlayerController;
    rt.actor_netguid        = 10341530;
    rt.actor_server_id      = 60;
    rt.actor_randomizer     = 0x6ee87ee4;
    rt.archetype_netguid    = 3503756484819958835ULL;   // Default__AoCPlayerControllerBP_C
    rt.level_netguid        = 16442478405498561049ULL;  // Verra_World_Master PersistentLevel
    rt.serialize_location   = true;
    rt.quantize_location    = true;
    rt.location_scaled_x    = -7776645;                 // world cm x10: (-777664.5, 616578.7, 15952.0)
    rt.location_scaled_y    = 6165787;
    rt.location_scaled_z    = 159520;
    return rt;
}

static void test_pc_spawn_record() {
    std::printf("PlayerController spawn record vs captured pkt#22\n");
    schema::SchemaRegistry::instance().load_all();
    const auto* pc = schema::SchemaRegistry::instance().get_schema(schema::ActorType::PlayerController);
    CHECK(pc != nullptr, "PlayerController schema loads");
    if (!pc) return;

    emit::BunchWriter bw;
    const size_t sna = emit::write_serialize_new_actor(bw, captured_pc_runtime(), *pc);
    CHECK(sna == kSerializeNewActorBits, "SerializeNewActor is 468 bits (location 81, flags 3)");
    emit::write_player_controller_open_fields(bw, /*net_player_index=*/0, /*client_handshake_id=*/0);
    CHECK(bw.bit_pos() == kCapturedPcSpawnRecordBits, "with the PlayerController fields: 508 bits");

    size_t diff = 0;
    const bool same = bw.bit_pos() == kCapturedPcSpawnRecordBits &&
                      same_bits(bw.data(), kCapturedPcSpawnRecord, kCapturedPcSpawnRecordBits, &diff);
    if (!same && bw.bit_pos() == kCapturedPcSpawnRecordBits)
        std::printf("         first differing bit: %zu\n", diff);
    CHECK(same, "every bit equals the original server's spawn record");
}

static void test_zero_vector() {
    std::printf("zero vector (default scale/velocity payload)\n");
    emit::BunchWriter bw;
    emit::write_quantized_vector(bw, 0, 0, 0);
    // SerializeInt(1<<6 | 1, 128) = 7 bits, then 3 one-bit zero components.
    CHECK(bw.bit_pos() == 10, "7-bit header + 3 x 1 bit");
    CHECK(bw.data()[0] == 0x41 && (bw.data()[1] & 0x03) == 0, "header = scaled, 1 bit per component");
}

int main() {
    test_pc_spawn_record();
    test_zero_vector();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
