// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/conn/sequence.h
//
//  Wrap-around sequence arithmetic for the two counters a connection keeps:
//  14-bit packet sequences (FNetPacketNotify) and channel sequences
//  (reliable bunches; the control channel's are 10 bits on the wire).
//
//  Ported from UE5 TSequenceNumber and UNetConnection's MakeRelative /
//  BestSignedDifference.
//
//  LAYER: Protocol / conn
// ============================================================================
#pragma once

#include <cstdint>

namespace aoc { namespace protocol { namespace conn {

/// Packet sequence numbers are 14 bits on the wire.
constexpr uint16_t kSeqBits = 14;
constexpr uint16_t kSeqMask = (1u << kSeqBits) - 1;  // 0x3FFF
constexpr uint16_t kSeqHalf = 1u << (kSeqBits - 1);

/// `a - b` in 14-bit sequence space, as a signed distance: positive when `a`
/// is newer than `b` (UE5 TSequenceNumber::Diff).
inline int seq_diff(uint16_t a, uint16_t b) {
    const int d = static_cast<int>((a - b) & kSeqMask);
    return d >= kSeqHalf ? d - (kSeqMask + 1) : d;
}

/// True when `a` is newer than `b` (UE5 TSequenceNumber::operator>).
inline bool seq_greater(uint16_t a, uint16_t b) {
    return a != b && ((a - b) & kSeqMask) < kSeqHalf;
}

inline uint16_t seq_add(uint16_t a, int n) {
    return static_cast<uint16_t>((a + n) & kSeqMask);
}

/// UE5 BestSignedDifference: the signed distance from `reference` to `value`
/// in a space of `max` values (a power of two).
inline int best_signed_difference(int value, int reference, int max) {
    return ((value - reference + max / 2) & (max - 1)) - max / 2;
}

/// UE5 MakeRelative: expands a wrapped `value` (e.g. a 10-bit channel
/// sequence off the wire) to the full counter nearest `reference`.
inline int make_relative(int value, int reference, int max) {
    return reference + best_signed_difference(value, reference, max);
}

}}} // namespace aoc::protocol::conn
