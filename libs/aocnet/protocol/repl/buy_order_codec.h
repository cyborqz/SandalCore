// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/buy_order_codec.h
//
//  ClientUpdateBuyOrderData's two structs, each its own NetSerialize, read from the client.  Archives: raw words, LSB first.
//
//  FClientBuyOrderData (0xb8; its own NetSerialize):
//    32 + n x possible          +0x00 an unreflected TArray, each: 64 the commodity (a CommodityDefRecordBase), 8 a byte (its
//                               ECommodityCategory [modelled]), then a TArray of options (cap 0x40000), each: 64 the recipe, then
//                               five min/max pairs: 16 16 (the contribution limit), 8 8 (the days), 16 16 (the reward), 32 32 (the cost), 16 16 (the quantity)
//                               [modelled: which 16-bit pair is which; the cost is the only 32-bit pair, the days the only 8-bit one], then a TArray of
//                               TArray<int64> (for each material requirement, the items it takes) and a TArray<int32> (each one's quantity)
//    32 + n x (128 + order)     +0x10 ActiveBuyOrders, a TMap<FGuid, FBuyOrderFulfillmentData>: each key, then the order's archive form
//                               64 CommodityId, 64 CommodityRecipeId, 64 RewardId, 64 TimeCreated, 32 + n x (64 + 32) Materials (item, quantity),
//                               32 ContributionLimit, 32 Requested, 32 VisiblyFulfilled, 64 NodeCost, 32 RewardQuantity, 32 TimeRemainingSeconds, and a 64-bit word
//                               at +0x38 that no property names (sent 0).  NumTimesLimitIncreased and ExpirationTimeInSeconds are not in it.
//    32 + n x ...               +0x60 ActiveBuyOrderCooldowns, a TMap<int64, a 0x60 struct>: sent empty
//    32                         +0xb0 MaxNumSlots
//  FPlayerBuyOrderContributions (0x58; its own NetSerialize):
//    32 + n x (128 + 32)        +0x00 CurrentContributions, a TMap<FGuid, int32>: an order and what the player gave it
//    32                         +0x50 RefreshContributionsTime
// ============================================================================
#pragma once

#include "protocol/emit/bunch_writer.h"
#include "protocol/repl/value.h"
#include "protocol/wire/packet_reader.h"

#include <cstdint>
#include <optional>

namespace aoc::protocol::repl {

constexpr size_t kMaxBuyOrderEntries = 4096;   // possible commodities, options, requirements, items of one, active orders, contributions: each count

inline bool buy_order_data_fits(const ClientBuyOrderData& d) {
    if (d.possible.size() > kMaxBuyOrderEntries || d.active.size() > kMaxBuyOrderEntries) return false;
    for (const BuyOrderPossible& p : d.possible) {
        if (p.options.size() > kMaxBuyOrderEntries) return false;
        for (const BuyOrderOption& o : p.options) {
            if (o.materials.size() > kMaxBuyOrderEntries || o.quantities.size() > kMaxBuyOrderEntries) return false;
            for (const auto& m : o.materials) {
                if (m.size() > kMaxBuyOrderEntries) return false;
            }
        }
    }
    for (const auto& [g, o] : d.active) {
        if (o.materials.size() > kMaxBuyOrderEntries) return false;
    }
    return true;
}

inline bool buy_order_contributions_fit(const PlayerBuyOrderContributions& c) { return c.contributions.size() <= kMaxBuyOrderEntries; }

inline void write_buy_order_guid(emit::BunchWriter& out, const GuidValue& g) {
    out.write_uint32(g.a);
    out.write_uint32(g.b);
    out.write_uint32(g.c);
    out.write_uint32(g.d);
}

inline GuidValue read_buy_order_guid(wire::PacketReader& r) {
    GuidValue g;
    g.a = r.read_uint32();
    g.b = r.read_uint32();
    g.c = r.read_uint32();
    g.d = r.read_uint32();
    return g;
}

/// FBuyOrderFulfillmentData's archive form.
inline void write_buy_order(emit::BunchWriter& out, const BuyOrderData& o) {
    out.write_uint64(static_cast<uint64_t>(o.commodity));
    out.write_uint64(static_cast<uint64_t>(o.recipe));
    out.write_uint64(static_cast<uint64_t>(o.reward));
    out.write_uint64(static_cast<uint64_t>(o.time_created));
    out.write_uint32(static_cast<uint32_t>(o.materials.size()));
    for (const BuyOrderMaterial& m : o.materials) {
        out.write_uint64(static_cast<uint64_t>(m.item));
        out.write_int32(m.quantity);
    }
    out.write_int32(o.contribution_limit);
    out.write_int32(o.requested);
    out.write_int32(o.visibly_fulfilled);
    out.write_uint64(static_cast<uint64_t>(o.node_cost));
    out.write_int32(o.reward_quantity);
    out.write_int32(o.time_remaining);
    out.write_uint64(static_cast<uint64_t>(o.extra));
}

inline std::optional<BuyOrderData> read_buy_order(wire::PacketReader& r) {
    BuyOrderData o;
    o.commodity = static_cast<int64_t>(r.read_uint64());
    o.recipe = static_cast<int64_t>(r.read_uint64());
    o.reward = static_cast<int64_t>(r.read_uint64());
    o.time_created = static_cast<int64_t>(r.read_uint64());
    const uint32_t n = r.read_uint32();
    if (n > kMaxBuyOrderEntries) return std::nullopt;
    for (uint32_t i = 0; i < n && !r.overflowed(); ++i) {
        BuyOrderMaterial m;
        m.item = static_cast<int64_t>(r.read_uint64());
        m.quantity = static_cast<int32_t>(r.read_uint32());
        o.materials.push_back(m);
    }
    o.contribution_limit = static_cast<int32_t>(r.read_uint32());
    o.requested = static_cast<int32_t>(r.read_uint32());
    o.visibly_fulfilled = static_cast<int32_t>(r.read_uint32());
    o.node_cost = static_cast<int64_t>(r.read_uint64());
    o.reward_quantity = static_cast<int32_t>(r.read_uint32());
    o.time_remaining = static_cast<int32_t>(r.read_uint32());
    o.extra = static_cast<int64_t>(r.read_uint64());
    if (r.overflowed()) return std::nullopt;
    return o;
}

inline bool write_buy_order_data(emit::BunchWriter& out, const ClientBuyOrderData& d) {
    if (!buy_order_data_fits(d)) return false;
    out.write_uint32(static_cast<uint32_t>(d.possible.size()));
    for (const BuyOrderPossible& p : d.possible) {
        out.write_uint64(static_cast<uint64_t>(p.commodity));
        out.write_uint8(p.category);
        out.write_uint32(static_cast<uint32_t>(p.options.size()));
        for (const BuyOrderOption& o : p.options) {
            out.write_uint64(static_cast<uint64_t>(o.recipe));
            out.write_bits(static_cast<uint16_t>(o.limit_min), 16);
            out.write_bits(static_cast<uint16_t>(o.limit_max), 16);
            out.write_uint8(o.days_min);
            out.write_uint8(o.days_max);
            out.write_bits(static_cast<uint16_t>(o.reward_min), 16);
            out.write_bits(static_cast<uint16_t>(o.reward_max), 16);
            out.write_int32(o.cost_min);
            out.write_int32(o.cost_max);
            out.write_bits(static_cast<uint16_t>(o.quantity_min), 16);
            out.write_bits(static_cast<uint16_t>(o.quantity_max), 16);
            out.write_uint32(static_cast<uint32_t>(o.materials.size()));
            for (const std::vector<int64_t>& items : o.materials) {
                out.write_uint32(static_cast<uint32_t>(items.size()));
                for (const int64_t item : items) out.write_uint64(static_cast<uint64_t>(item));
            }
            out.write_uint32(static_cast<uint32_t>(o.quantities.size()));
            for (const int32_t q : o.quantities) out.write_int32(q);
        }
    }
    out.write_uint32(static_cast<uint32_t>(d.active.size()));
    for (const auto& [g, o] : d.active) {
        write_buy_order_guid(out, g);
        write_buy_order(out, o);
    }
    out.write_uint32(0);   // ActiveBuyOrderCooldowns: none
    out.write_int32(d.max_slots);
    return true;
}

inline std::optional<ClientBuyOrderData> read_buy_order_data(wire::PacketReader& r) {
    ClientBuyOrderData d;
    const uint32_t np = r.read_uint32();
    if (np > kMaxBuyOrderEntries) return std::nullopt;
    for (uint32_t i = 0; i < np && !r.overflowed(); ++i) {
        BuyOrderPossible p;
        p.commodity = static_cast<int64_t>(r.read_uint64());
        p.category = static_cast<uint8_t>(r.read_bits(8));
        const uint32_t no = r.read_uint32();
        if (no > kMaxBuyOrderEntries) return std::nullopt;
        for (uint32_t k = 0; k < no && !r.overflowed(); ++k) {
            BuyOrderOption o;
            o.recipe = static_cast<int64_t>(r.read_uint64());
            o.limit_min = static_cast<int16_t>(r.read_bits(16));
            o.limit_max = static_cast<int16_t>(r.read_bits(16));
            o.days_min = static_cast<uint8_t>(r.read_bits(8));
            o.days_max = static_cast<uint8_t>(r.read_bits(8));
            o.reward_min = static_cast<int16_t>(r.read_bits(16));
            o.reward_max = static_cast<int16_t>(r.read_bits(16));
            o.cost_min = static_cast<int32_t>(r.read_uint32());
            o.cost_max = static_cast<int32_t>(r.read_uint32());
            o.quantity_min = static_cast<int16_t>(r.read_bits(16));
            o.quantity_max = static_cast<int16_t>(r.read_bits(16));
            const uint32_t nm = r.read_uint32();
            if (nm > kMaxBuyOrderEntries) return std::nullopt;
            for (uint32_t m = 0; m < nm && !r.overflowed(); ++m) {
                const uint32_t ni = r.read_uint32();
                if (ni > kMaxBuyOrderEntries) return std::nullopt;
                std::vector<int64_t> items;
                for (uint32_t j = 0; j < ni && !r.overflowed(); ++j) items.push_back(static_cast<int64_t>(r.read_uint64()));
                o.materials.push_back(std::move(items));
            }
            const uint32_t nq = r.read_uint32();
            if (nq > kMaxBuyOrderEntries) return std::nullopt;
            for (uint32_t j = 0; j < nq && !r.overflowed(); ++j) o.quantities.push_back(static_cast<int32_t>(r.read_uint32()));
            p.options.push_back(std::move(o));
        }
        d.possible.push_back(std::move(p));
    }
    const uint32_t na = r.read_uint32();
    if (na > kMaxBuyOrderEntries) return std::nullopt;
    for (uint32_t i = 0; i < na && !r.overflowed(); ++i) {
        const GuidValue g = read_buy_order_guid(r);
        auto o = read_buy_order(r);
        if (!o) return std::nullopt;
        d.active.emplace_back(g, std::move(*o));
    }
    if (r.read_uint32() != 0) return std::nullopt;   // (cooldowns: this side sends none and reads none)
    d.max_slots = static_cast<int32_t>(r.read_uint32());
    if (r.overflowed()) return std::nullopt;
    return d;
}

inline bool write_buy_order_contributions(emit::BunchWriter& out, const PlayerBuyOrderContributions& c) {
    if (!buy_order_contributions_fit(c)) return false;
    out.write_uint32(static_cast<uint32_t>(c.contributions.size()));
    for (const auto& [g, n] : c.contributions) {
        write_buy_order_guid(out, g);
        out.write_int32(n);
    }
    out.write_int32(c.refresh_time);
    return true;
}

inline std::optional<PlayerBuyOrderContributions> read_buy_order_contributions(wire::PacketReader& r) {
    PlayerBuyOrderContributions c;
    const uint32_t n = r.read_uint32();
    if (n > kMaxBuyOrderEntries) return std::nullopt;
    for (uint32_t i = 0; i < n && !r.overflowed(); ++i) {
        const GuidValue g = read_buy_order_guid(r);
        c.contributions.emplace_back(g, static_cast<int32_t>(r.read_uint32()));
    }
    c.refresh_time = static_cast<int32_t>(r.read_uint32());
    if (r.overflowed()) return std::nullopt;
    return c;
}

}  // namespace aoc::protocol::repl
