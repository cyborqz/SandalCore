// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/server_guid_allocator.h
//
//  Mints the NetGUIDs of the objects a world server creates.
//
//  AoC's NetGUID is FIntrepidNetworkGUID {ObjectId:64, ServerId:32,
//  Randomizer:32} (emit/intrepid_netguid.h): the ServerId names the server
//  that minted the object, so every world server mints under the id mesh
//  assigned it and GUIDs from different servers never collide.  The capture's
//  dynamic GUIDs look like that (pawn 10341538 | server 60 | 0x6ee87ee4).
//
//  - ObjectIds are even: the client reads bit 0 as "an inline export
//    follows" (net_guid_allocator.h, PM52).  They start at kFirstObjectId,
//    clear of the static GUIDs (ServerId 0, small ObjectIds), and step by 2.
//  - The randomizer is one value per server run, so a restarted server that
//    gets the same ServerId still mints GUIDs the client has not seen.
//
//  Thread-safe.
//
//  LAYER: Protocol
// ============================================================================
#pragma once

#include "protocol/emit/intrepid_netguid.h"
#include <atomic>
#include <cstdint>

namespace aoc { namespace protocol {

class ServerGuidAllocator {
public:
    static constexpr uint64_t kFirstObjectId = 0x01000000;

    ServerGuidAllocator(uint32_t server_id, uint32_t randomizer)
        : server_id_(server_id), randomizer_(randomizer), next_object_id_(kFirstObjectId) {}

    emit::FIntrepidNetworkGUID next() {
        emit::FIntrepidNetworkGUID g;
        g.ObjectId = next_object_id_.fetch_add(2, std::memory_order_relaxed);
        g.ServerId = server_id_;
        g.Randomizer = randomizer_;
        return g;
    }

    uint32_t server_id() const { return server_id_; }
    uint32_t randomizer() const { return randomizer_; }

private:
    const uint32_t server_id_;
    const uint32_t randomizer_;
    std::atomic<uint64_t> next_object_id_;
};

}} // namespace aoc::protocol
