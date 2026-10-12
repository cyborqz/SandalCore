// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  libs/aocnet/tests/test_rep_layout_writer.cpp
//
//  RepLayout property blocks as the AoC client reads them, checked against
//  the original server's PlayerPawn_C (actor 10341538) updates on ch=19 of
//  replay_data.bin:
//    pkt#107  after possession: AuthServerIDReplicated, bHidden, RemoteRole,
//             Instigator, PlayerState, Controller (477 bits)
//    pkt#2523 bHidden cleared once the client finished loading (18 bits)
//    pkt#7706 RemoteViewPitch16 (33 bits)
//  and the PlayerController's CharacterLoadTracker on ch=3 (pkt#46, #72).
// ============================================================================
#include "protocol/emit/net_serializers.h"
#include "protocol/emit/rep_layout_writer.h"
#include "protocol/schema/actor_rep_handles.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace aoc::protocol::emit;
namespace rep = aoc::protocol::schema;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static FIntrepidNetworkGUID replay_guid(uint64_t object_id) {
    FIntrepidNetworkGUID g;
    g.ObjectId   = object_id;
    g.ServerId   = 60;
    g.Randomizer = 0x6ee87ee4;
    return g;
}
// ~ C O A :: Cyb ~

/// The first `bits` bits of `w`, packed LSB-first, as lowercase hex.
static std::string hex_of(const BunchWriter& w, size_t bits) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < (bits + 7) / 8; ++i) {
        uint8_t b = w.data()[i];
        if (i == bits / 8 && bits % 8) b &= static_cast<uint8_t>((1u << (bits % 8)) - 1);
        s += digits[b >> 4];
        s += digits[b & 15];
    }
    return s;
}

static void test_pkt107_possession_update() {
    std::printf("pkt#107 pawn update after possession\n");
    BunchWriter w;
    RepLayoutWriter props(w);
    props.write_uint32(rep::actor_rep::AuthServerIDReplicated, 0);
    props.write_bool(rep::actor_rep::bHidden, true);
    props.write_enum(rep::actor_rep::RemoteRole,
                     static_cast<uint32_t>(rep::ENetRole::AutonomousProxy), rep::kNetRoleBits);
    props.write_object(rep::actor_rep::Instigator, replay_guid(10341538));
    props.write_object(rep::pawn_rep::PlayerState, replay_guid(10341528));
    props.write_object(rep::pawn_rep::Controller, replay_guid(10341530));
    props.finish();

    CHECK(props.ok(), "handles ascend");
    CHECK(w.bit_pos() == 477, "block is 477 bits");
    CHECK(hex_of(w, w.bit_pos()) ==
          "04000000000c42884494b913000000008007000080dc0fdd4d0593b913000000008007000080dc0fdd8d45"
          "93b913000000008007000080dc0fdd0d00",
          "bits match the original server's block");
}

static void test_pkt2523_unhide() {
    std::printf("pkt#2523 bHidden cleared\n");
    BunchWriter w;
    RepLayoutWriter props(w);
    props.write_bool(rep::actor_rep::bHidden, false);
    props.finish();
    CHECK(w.bit_pos() == 18, "block is 18 bits");
    CHECK(hex_of(w, w.bit_pos()) == "0c0000", "bits match the original server's block");
}

static void test_pkt7706_view_pitch() {
    std::printf("pkt#7706 RemoteViewPitch16\n");
    BunchWriter w;
    RepLayoutWriter props(w);
    props.write_uint16(rep::pawn_rep::RemoteViewPitch16, 61115);
    props.finish();
    CHECK(w.bit_pos() == 33, "block is 33 bits");
    CHECK(hex_of(w, w.bit_pos()) == "4c76dd0100", "bits match the original server's block");
}

static std::vector<bool> flags_from(const char* bits) {
    std::vector<bool> flags;
    for (const char* c = bits; *c; ++c) flags.push_back(*c == '1');
    return flags;
}

static void test_pc_load_tracker() {
    std::printf("PC CharacterLoadTracker (ch=3 pkt#46 and pkt#72)\n");
    BunchWriter first;
    {
        RepLayoutWriter props(first);
        write_character_load_tracker(props, rep::pc_rep::CharacterLoadTracker,
                                     flags_from("1000000000000000000000000"));
        props.finish();
    }
    CHECK(first.bit_pos() == 823, "pkt#46 block is 823 bits");
    CHECK(hex_of(first, first.bit_pos()) ==
          "ccb2" + std::string(202, '0'), "pkt#46 bits match the original server's block");

    BunchWriter loaded;
    {
        RepLayoutWriter props(loaded);
        write_character_load_tracker(props, rep::pc_rep::CharacterLoadTracker,
                                     flags_from("1011111111111111111111111"));
        props.finish();
    }
    std::string expect = "ccb2000000000000";
    for (int i = 0; i < 23; ++i) expect += "00800000";
    expect += "000000";
    CHECK(hex_of(loaded, loaded.bit_pos()) == expect, "pkt#72 bits match the original server's block");
}

static void test_descending_handles_flagged() {
    std::printf("handles out of order\n");
    BunchWriter w;
    RepLayoutWriter props(w);
    props.write_bool(rep::actor_rep::bHidden, false);
    props.write_bool(rep::actor_rep::bReplicateMovement, false);
    CHECK(!props.ok(), "a lower handle after a higher one is flagged");
}

static void test_actor_content_block() {
    std::printf("actor content block wrapper\n");
    BunchWriter props_bits;
    RepLayoutWriter props(props_bits);
    props.write_bool(rep::actor_rep::bHidden, false);
    props.finish();

    BunchWriter w;
    const size_t bits = write_actor_property_block(w, props_bits);
    // bHasRepLayout=1, bIsActor=1, SIP(18) = 8 bits, then the 18-bit block.
    CHECK(bits == 2 + 8 + 18, "header + payload is 28 bits");
    CHECK(w.bit_pos() == bits, "writer advanced by the returned count");
    CHECK(hex_of(w, bits) == "93300000", "bHasRepLayout, bIsActor, SIP size, payload");
}

int main() {
    test_pkt107_possession_update();
    test_pkt2523_unhide();
    test_pkt7706_view_pitch();
    test_pc_load_tracker();
    test_descending_handles_flagged();
    test_actor_content_block();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
