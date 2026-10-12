// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/captured_identity.h
//
//  Native mode cannot generate the player pawn's components yet (stats,
//  combat and character info, appearance, storage): with none of them the
//  client shows 0 health and never moves the pawn, although its input
//  reaches the movement component.  Until they can be generated, the PC and
//  the pawn take the captured session's NetGUIDs, so the captured pawn
//  channel (replay pkts 78-118, ch=19) applies to them unchanged: its
//  Owner / Controller references resolve against our native PC.
//
//  On by default; probe_captured_pawn.txt = 0 selects the minted GUIDs and
//  PlayerPawnEmitter's native open (no components).
// ============================================================================
#pragma once

#include "protocol/emit/intrepid_netguid.h"
#include <cstdint>
#include <cstdio>

namespace aoc { namespace net {

constexpr uint64_t kCapturedPcObjectId   = 10341530;
constexpr uint64_t kCapturedPawnObjectId = 10341538;
/// The captured pawn channel: ch=19, reliable chSeq 954-983 in pkts 78-118.
constexpr uint32_t kCapturedPawnChannel  = 19;
constexpr uint32_t kCapturedPawnFirstPkt = 78;
constexpr uint32_t kCapturedPawnLastPkt  = 118;
constexpr uint32_t kCapturedPawnNextChSeq = 984;

inline bool use_captured_player_identity() {
    std::FILE* fp = std::fopen("probe_captured_pawn.txt", "r");
    if (!fp) return true;
    int v = 1;
    if (std::fscanf(fp, "%d", &v) != 1) v = 1;
    std::fclose(fp);
    return v != 0;
}

/// A captured-session GUID: ServerId 60, the session's Randomizer.
inline protocol::emit::FIntrepidNetworkGUID captured_guid(uint64_t object_id) {
    protocol::emit::FIntrepidNetworkGUID g;
    g.ObjectId   = object_id;
    g.ServerId   = 60;
    g.Randomizer = 0x6ee87ee4;
    return g;
}

}}  // namespace aoc::net
