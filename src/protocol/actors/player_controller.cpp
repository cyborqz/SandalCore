// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/actors/player_controller.cpp
//
//  First actor builder (Phase 3.6).  Emits a complete PlayerController
//  ActorOpen bunch payload for a live player, using the formats decoded
//  from UE5 5.7 source:
//      * PackageMapClient::SerializeNewActor  (actor GUIDs + spawn transform)
//      * FRotator::SerializeCompressedShort   (rotation)
//      * FVector_NetQuantize10                (locations — Phase 3.7)
//
//  The builder is intentionally additive: not yet wired into the live
//  send path.  Phase 3.7 will add a BootstrapSequence hook that swaps
//  the captured PlayerController bunch with this synthesized one when
//  available.  Tests can call build() directly and compare output bytes.
//
//  Byte-identity goal:
//    When build() is called with a profile that matches the captured
//    player's state, the first ~720 bits of the output should match
//    the captured bunch exactly.  The final ~2,580 bits (property
//    values) are spliced from the captured data unchanged until
//    Phase M5 decodes them per-field.
// ============================================================================
#include "protocol/actors/player_controller.h"
#include "protocol/bootstrap/bootstrap_data.h"

#include <cstring>

namespace aoc { namespace protocol { namespace actors { namespace player_controller {

// ─── Constants observed in the captured PlayerController bunch ──────────
// (pkt 14287, payload starting at bit 206, decoded in Phase 3.3)
//
// These match the values the real AoC server wrote for the captured
// player.  For fresh live players we reuse the same Archetype/Level
// GUIDs because those identify the class BP + persistent level — both
// shared across every player in the same world.
// ─────────────────────────────────────────────────────────────────────────

// How many RepLayout fields the PlayerController class has (the "411" in
// "411-bit field mask").  Class-specific constant; every PC bunch uses it.
constexpr uint32_t kNumRepLayoutExports = 411u;
// -- CoA . C.Y.B --

// NetGUID for the PlayerController CLASS blueprint.  All PlayerController
// instances reference this archetype.  Bare ref (no path): the client
// already has the class loaded.
constexpr uint64_t kArchetypeNetGuid = 120u;
constexpr uint8_t  kArchetypeFlags   = 0x72u;  // bNoLoad=1

// NetGUID for the persistent Level.  Same for every actor spawned into
// this level.  Flag byte has bHasNetworkChecksum=1 set in the high-order
// position but, per UE5 InternalLoadObject semantics, the 32-bit checksum
// is only emitted WHEN bHasPath is also set.  For this bare reference
// (bHasPath=0) the checksum is NOT serialized.
constexpr uint64_t kLevelNetGuid = 10u;
constexpr uint8_t  kLevelFlags   = 0xd6u;  // bNoLoad=1, bHasNetworkChecksum=1
                                           // (high bits are AoC-specific
                                           // and don't affect parsing)

// Default Actor NetGUID used by the captured replay (single-player session).
// The captured bunch encodes this value with a REDUNDANT 2-byte SIP
// (byte0=0x03 continuation=1, byte1=0x00 continuation=0).  We match this
// exact encoding so the builder produces byte-identical output; a real
// live server can switch to the minimal 1-byte encoding freely.
constexpr uint64_t kDefaultActorNetGuid = 1u;
constexpr uint8_t  kDefaultActorFlags   = 0x00u;

// Captured rotator for the PlayerController's spawn orientation.  Only
// Pitch is non-zero (24266, which decodes to ~133.3°).  When the profile
// supplies no rotation we emit these exact values to keep the bunch
// byte-identical to the capture.
constexpr uint16_t kCapturedRotatorPitch = 24266u;
constexpr uint16_t kCapturedRotatorYaw   = 0u;
constexpr uint16_t kCapturedRotatorRoll  = 0u;

// ─── Captured-payload splice region ─────────────────────────────────────
// The SerializeNewActor body of the captured PC ended around bit offset
// ~720 into the payload (SerializeNewActor start=bit 444, decoded length
// varies by 4 flagged vectors).  Everything after that is the 68 initial
// property values.
//
// We extract those bits from bootstrap_data.h at build time so we don't
// need a separate captured-bytes table.  The player_controller packet
// is at bootstrap index 22 (orig_seq 14287).
// ─────────────────────────────────────────────────────────────────────────
constexpr std::size_t kCapturedPacketIndex = 22u;

// Bit offset within the captured bunch payload where SerializeNewActor
// ENDS and property values begin.  Determined via decode_pc_precise.py
// in Phase 3.7 — the captured bunch's SerializeNewActor body occupies
// bits 444..727 of the payload (starting after the 411-bit field mask),
// so property values begin at bit 728.
//
// Bit 728 = end of Scale flag (Scale was bWasSerialized=0, 1 bit) and
// the start of the Velocity field which is logically still part of
// SerializeNewActor.  For Phase 3.7 our builder emits through Scale
// inclusive, then splices Velocity + property values together from the
// captured data.  This keeps the output byte-identical without needing
// to decode Velocity precisely (that's Phase 3.8).
constexpr std::size_t kPropertyValuesStartBit = 522u; // = 444 prefix + 78 SNA fields
constexpr std::size_t kPayloadTotalBits       = 3302u;
constexpr std::size_t kSpliceFromCapturedBits =
    kPayloadTotalBits - kPropertyValuesStartBit;  // = 2780 bits

// The capture's bunch payload starts at bit 206 of the raw packet.
constexpr std::size_t kBunchPayloadStartBit = 206u;

// Helper: append bits [src_bit_offset .. src_bit_offset + n_bits) from the
// captured raw packet into `out`.
static void splice_captured_bits(BunchBuffer& out,
                                 std::size_t src_bit_offset,
                                 std::size_t n_bits) {
    const auto& pkt = bootstrap_data::kPackets[kCapturedPacketIndex];
    for (std::size_t i = 0; i < n_bits; ++i) {
        const std::size_t bp = src_bit_offset + i;
        if ((bp >> 3) >= pkt.raw_size) break;
        const uint64_t bit = (pkt.raw[bp >> 3] >> (bp & 7)) & 1ULL;
        out.write_bits(bit, 1);
    }
}

// ─── The builder ─────────────────────────────────────────────────────────

bool build(BunchBuffer& out, const CharacterProfile& profile) {
    // [1] bHasRepLayoutExport = 1 — PC uses the RepLayout export format.
    out.write_bits(1u, 1);

    // [2] NumExports = 411 (uint32 LE).
    out.write_bits(kNumRepLayoutExports, 32);

    // [3] 411-bit field-mask.  Phase 3.6: copy the EXACT mask from the
    //     captured bunch (68 of the 411 bits are set).  Phase M5 will let
    //     us understand which fields the mask actually enables and build
    //     a profile-driven mask.
    //
    //     Mask begins at bit 33 of the captured payload
    //     (= raw bit kBunchPayloadStartBit + 33 = 239).
    constexpr std::size_t kMaskStartBitInRaw = kBunchPayloadStartBit + 33u;
    splice_captured_bits(out, kMaskStartBitInRaw,
                         kNumRepLayoutExports /* = 411 */);

    // [4] Actor NetGUID — SIP-encoded integer + flag byte.
    //     PHASE 3.7 FIX: the captured bunch uses a REDUNDANT 2-byte SIP
    //     encoding for value 1 (byte0=0x03 cont=1, byte1=0x00 cont=0).
    //     To produce byte-identical output we write the same encoding
    //     explicitly.  A cleaner minimal encoding (single 0x02 byte) is
    //     also valid UE5 wire format — we'd use that in Phase 3.8 when
    //     we stop splicing captured property bytes.
    const uint64_t actor_guid = profile.actor_netguid_hint != 0
        ? profile.actor_netguid_hint
        : kDefaultActorNetGuid;
    if (profile.actor_netguid_hint == 0) {
        // Replicate the captured 2-byte SIP exactly.
        out.write_bits(0x03u, 8);  // byte0: data=1, continuation=1
        out.write_bits(0x00u, 8);  // byte1: data=0, continuation=0 → value=1
    } else {
        out.write_sip(actor_guid);
    }
    out.write_bits(kDefaultActorFlags, 8);

    // [5] Archetype NetGUID — the PlayerController class BP.
    out.write_sip(kArchetypeNetGuid);
    out.write_bits(kArchetypeFlags, 8);

    // [6] Level NetGUID — the persistent level.  Flag has bHasNetworkChecksum=1
    //     but stock UE5 only emits the 32-bit checksum when bHasPath is also
    //     set (which it isn't here).  No checksum bytes follow.
    out.write_sip(kLevelNetGuid);
    out.write_bits(kLevelFlags, 8);
    // NO checksum written — bHasPath=0 in flag byte.

    // [7] Location — captured PC has bWasSerialized=false (PC spawns at
    //     the origin; its visible position comes from the possessed pawn).
    const bool serialize_location = false;  // TODO Phase 3.8: from profile
    out.write_conditional_vector_stub(serialize_location);
// (CoA : C | Y | B)

    // [8] Rotation — bSerialize=true + compressed rotator.  If the profile
    //     supplies a non-zero rotation, quantize it; otherwise emit the
    //     captured rotator to stay byte-identical with the replay.
    out.write_bits(1u, 1);  // bSerializeRotation = true
    uint16_t pitch = kCapturedRotatorPitch;
    uint16_t yaw   = kCapturedRotatorYaw;
    uint16_t roll  = kCapturedRotatorRoll;
    const bool profile_has_rotation =
        profile.spawn_rotation.pitch != 0.0f ||
        profile.spawn_rotation.yaw   != 0.0f ||
        profile.spawn_rotation.roll  != 0.0f;
    if (profile_has_rotation) {
        auto to_short = [](float deg) -> uint16_t {
            // UE5 CompressAxisToShort: map [0, 360) -> [0, 65536)
            return static_cast<uint16_t>((deg / 360.0f) * 65536.0f);
        };
        pitch = to_short(profile.spawn_rotation.pitch);
        yaw   = to_short(profile.spawn_rotation.yaw);
        roll  = to_short(profile.spawn_rotation.roll);
    }
    out.write_compressed_rotator(pitch, yaw, roll);

    // [9] Scale flag = false (captured used default 1,1,1).
    out.write_conditional_vector_stub(/*serialized=*/false);

    // [10] Velocity + [11] Property values — everything from here to the
    //      end of the bunch is spliced from the captured data.
    //
    //      Why stop at Scale and splice the rest?  Velocity serialization
    //      uses packed-quantized-vector format whose exact layout depends
    //      on the engine net version.  We haven't pinned that down yet
    //      (Phase 3.8), so we pass through the captured bits unchanged.
    //      Property values (~2,580 bits) are Phase M5 territory.
    //
    //      The splice count (kSpliceFromCapturedBits = 2780) is chosen
    //      so the total output is exactly 3302 bits — byte-identical to
    //      the captured bunch when the profile matches capture defaults.
    splice_captured_bits(out,
                         kBunchPayloadStartBit + kPropertyValuesStartBit,
                         kSpliceFromCapturedBits);

    // The `profile.name`, `archetype_id`, `race_id`, `gender_id` are NOT
    // used here yet — they live INSIDE the property-values block whose
    // format we haven't decoded.  Patching those fields is the job of
    // the `personalization/` module (Phase A track, separate from this
    // synthesis track).

    (void)profile;  // silence unused-variable warning until Phase 3.7.
    return true;
}

}}}} // namespace aoc::protocol::actors::player_controller
