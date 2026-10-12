// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/wire/hit_confirmation.h
//
//  Server-to-client hit confirmations.
//
//  SUPERSEDED by protocol/repl/hit_codec.h and hit_rpc.h (plan D2), which read and write the struct as the retail server sent
//  it (7,115 captured calls, bit for bit).  What is written below is the first-guess form: it frames the parameter as "a send
//  bit, a uint16 count per TArray" and always leaves Targets empty, whereas the retail bits are an 11-bit hit count, a 15-bit
//  length per blob and, in the blob, up to 40 FHitTargetInst.  Nothing outside test_hit_confirmation uses this header.
//
//    S>C  AAoCPlayerController::ClientHitConfirmationsReliable   PC field 171
//         AAoCPlayerController::ClientHitConfirmations           PC field 170
//
//  Field index: AAoCPlayerController's 908 net functions sorted by name put
//  these at 53/54; base = APlayerController's 98 fields + 19 AAoCPC props
//  = 117 (98 + 19 + 908 = 1025 = the SerializeInt max every ch=3 RPC uses;
//  the same formula gives ClientSetDialogue=258, ServerChooseDialogueOption
//  =424, ServerSelectDialogueChoice=834 as already used in game_server.h).
//
//  UAoCAbilityComponent::ProxyMulticastHitConfirmation (ability field 21) is
//  NOT client-callable: its FunctionFlags (0x00080C60) have no NetClient /
//  NetMulticast bit, only AoC's 0x20 "proxy" flag, so the client rejects it
//  ("Rejected RPC function due to access rights").
//
//  RPC parameter: FSerializedClientHitConfirmations { TArray<
//  FSerializedClientHitConfirmation{ TArray<uint8> SerializedData }> }.
//  RepLayout RPC encoding: send bit, uint16 array count per TArray.
//
//  SerializedData is the client's own FArchive format, read with a plain
//  FBitReader (sub_1463E2F90 -> sub_1463E1750 -> sub_1463E1870):
//    FHitInst: InstGuid, RecordGuid, SourceRecordType, SourceRecordGuid
//              (int64 each), bPvPForceFlag (bool = uint32), Instigator and
//              Source (FIntrepidNetGUID, 4x uint32, sub_14141E960), Targets
//              (SerializeBits count, 7 bits, max 40; then FHitTargetInst each
//              via sub_1463E1DA0 — not implemented, count is always 0 here).
//              DelayTime and SynchronizedTime are not serialized.
//    bPredictedOnAutonomous, bPredictedOnSimulated (bool = uint32 each).
// ============================================================================
#pragma once

#include "protocol/wire/ue5_primitives.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace aoc { namespace protocol { namespace wire {

constexpr uint32_t kPcFieldIdx_ClientHitConfirmations         = 170;
constexpr uint32_t kPcFieldIdx_ClientHitConfirmationsReliable = 171;

struct NetGuid128 {
    uint32_t object_lo  = 0;
    uint32_t object_hi  = 0;
    uint32_t server_id  = 0;
    uint32_t randomizer = 0;
};
// Cyb x C-O-A

struct HitConfirmation {
    uint64_t   inst_guid          = 0;
    uint64_t   record_guid        = 0;
    uint64_t   source_record_type = 0;
    uint64_t   source_record_guid = 0;
    bool       pvp_force          = false;
    NetGuid128 instigator;
    NetGuid128 source;
    bool       predicted_on_autonomous = false;
    bool       predicted_on_simulated  = false;
};

namespace detail {

inline void write_guid128(uint8_t* buf, size_t cap, size_t& off, const NetGuid128& g) {
    ::ue5::write_bits(buf, cap, off, g.object_lo, 32);
    ::ue5::write_bits(buf, cap, off, g.object_hi, 32);
    ::ue5::write_bits(buf, cap, off, g.server_id, 32);
    ::ue5::write_bits(buf, cap, off, g.randomizer, 32);
}

inline size_t sip_bit_count(uint64_t v) {
    size_t n = 8;
    while (v >>= 7) n += 8;
    return n;
}

inline size_t serialize_int_bit_count(uint32_t value, uint32_t max_val) {
    size_t n = 0;
    uint32_t written = 0;
    for (uint32_t mask = 1; mask != 0 && written + mask < max_val; mask <<= 1) {
        if (value & mask) written |= mask;
        ++n;
    }
    return n;
}

} // namespace detail

/// One SerializedData blob, byte-padded.
inline std::vector<uint8_t> serialize_hit_confirmation(const HitConfirmation& hc) {
    constexpr size_t kBits = 4 * 64 + 32 + 2 * 128 + 7 + 2 * 32;  // 615, no targets
    std::vector<uint8_t> out((kBits + 7) / 8, 0);
    size_t off = 0;
    uint8_t* b = out.data();
    const size_t cap = out.size();
    ::ue5::write_bits(b, cap, off, hc.inst_guid, 64);
    ::ue5::write_bits(b, cap, off, hc.record_guid, 64);
    ::ue5::write_bits(b, cap, off, hc.source_record_type, 64);
    ::ue5::write_bits(b, cap, off, hc.source_record_guid, 64);
    ::ue5::write_bits(b, cap, off, hc.pvp_force ? 1 : 0, 32);
    detail::write_guid128(b, cap, off, hc.instigator);
    detail::write_guid128(b, cap, off, hc.source);
    ::ue5::write_bits(b, cap, off, 0, 7);                          // Targets count
    ::ue5::write_bits(b, cap, off, hc.predicted_on_autonomous ? 1 : 0, 32);
    ::ue5::write_bits(b, cap, off, hc.predicted_on_simulated ? 1 : 0, 32);
    return out;
}

/// Writes a complete ch=3 actor content block for ClientHitConfirmations
/// (reliable -> field 171, else 170) carrying `blobs`.  Returns bits written.
inline size_t write_client_hit_confirmations(uint8_t* buf, size_t cap, size_t& off,
                                             const std::vector<std::vector<uint8_t>>& blobs,
                                             bool reliable) {
    size_t param_bits = 1 + 16;                                     // send bit + count
    for (const auto& blob : blobs) param_bits += 16 + 8 * blob.size();

    const uint32_t field = reliable ? kPcFieldIdx_ClientHitConfirmationsReliable
                                    : kPcFieldIdx_ClientHitConfirmations;
    const size_t cb_bits = detail::serialize_int_bit_count(field, 1025) +
                           detail::sip_bit_count(param_bits) + param_bits;

    const size_t start = off;
    ::ue5::write_bits(buf, cap, off, 0, 1);                         // bHasRepLayout
    ::ue5::write_bits(buf, cap, off, 1, 1);                         // bIsActor (the PC)
    ::ue5::write_sip(buf, cap, off, cb_bits);
    ::ue5::write_serialize_int(buf, cap, off, field, 1025);
    ::ue5::write_sip(buf, cap, off, param_bits);

    ::ue5::write_bits(buf, cap, off, 1, 1);                         // Hits send bit
    ::ue5::write_bits(buf, cap, off, blobs.size(), 16);             // HitConfirmations num
    for (const auto& blob : blobs) {
        ::ue5::write_bits(buf, cap, off, blob.size(), 16);          // SerializedData num
        for (uint8_t byte : blob) ::ue5::write_bits(buf, cap, off, byte, 8);
    }
    return off - start;
}

}}} // namespace aoc::protocol::wire
