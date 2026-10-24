// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/craft_codec.h  (crafting, steps 1 and 2: "start a processing job", "collect it"; header only)
//
//  FCraftItemState as the wire carries it: a crafting or processing job.  It is a NetSerialize struct, a bit stream read in the order below ("ubool" is a bool written as an int32,
//  1 at most; every other number is little endian):
//
//      int64   RecipeId                    the recipe (a design-data guid)
//      int64   SubRecipeId                 0 for none
//      double  StartTime                   whole seconds on the synchronized clock (the clock ClientSetTime sets)
//      float   Duration                    seconds
//      int32   Quantity
//      int32   n, then n x { int64 RecordId; int32 m, then m x { uint8 Rarity; int32 Count; ubool bFromWarehouse } }      GeneralResources: what was paid, by record and rarity band
//      int32   n, then the same                                                                                          FlexibleResources
//      int32   n, then n x { int32 k, then k resources as above; int32 j, then j FStrings }                              TagResources
//      FGuid   the ClientTransactionId of the request that started the job (A, B, C, D as int32)
//      FGuid   the JobGuid, made by the server
//      uint8   Status                      EProcessingJobState: Reserved 0, Pending 1, InProgress 2, Complete 3, Claimed 4, Cancelled 5
//      int32   n, then n FItemInstance     items (empty in every recorded state)
//      if Status == Complete: int32 n, then n x { int64 Guid, int64 TypeId, int64 Amount } (the experience rows), int32 n, then n FItemInstance (the products),
//                             int32, int32 (0 in every recording), int32 (the crafter's level)
//      ubool   always 1
//      uint8   always 0
//
//  A state with one resource of one rarity and nothing else is 856 bits; a Complete one with two experience rows and two plain products is 3,592 (856 + 32 + 2 x 192 + 32 + 2 x 1,096 + 96).
//  The tag resources and the item list are written empty and read only when empty (a state with more is refused, not skipped).
// ============================================================================
#pragma once

#include "protocol/emit/bunch_writer.h"
#include "protocol/repl/codecs.h"
#include "protocol/repl/item_codec.h"
#include "protocol/repl/value.h"
#include "protocol/wire/packet_reader.h"

#include <bit>
#include <optional>

namespace aoc { namespace protocol { namespace repl {

/// The most resources a state lists, the most rarities one resource lists, and a job's most units (the station gives 1 to 250).
constexpr size_t kMaxCraftResources = 64;
constexpr size_t kMaxCraftRarities = 8;
constexpr int32_t kMaxCraftCount = 1'000'000;
/// The most experience rows and products a Complete state lists.
constexpr size_t kMaxCraftExperience = 8;
constexpr size_t kMaxCraftProducts = 64;

inline bool craft_state_fits(const CraftItemState& s) {
    if (s.status > 5 || s.general.size() > kMaxCraftResources || s.flexible.size() > kMaxCraftResources) return false;
    for (const auto* list : {&s.general, &s.flexible}) {
        for (const CraftResource& r : *list) {
            if (r.rarities.size() > kMaxCraftRarities) return false;
            for (const CraftRarityCount& c : r.rarities) {
                if (c.rarity > 6 || c.count < 0 || c.count > kMaxCraftCount) return false;
            }
        }
    }
    // only a Complete state has the experience rows, the products and the level
    if (s.status != 3) return s.experience.empty() && s.products.empty() && s.unknown_a == 0 && s.unknown_b == 0 && s.level == 0;
    if (s.experience.size() > kMaxCraftExperience || s.products.size() > kMaxCraftProducts) return false;
    for (const ItemInstance& p : s.products) {
        if (!p.has_item || !item_instance_fits(p)) return false;
    }
    return true;
}

namespace craft_detail {

inline bool read_ubool(wire::PacketReader& r, bool& out) {
    const uint32_t v = r.read_uint32();
    if (overran(r) || v > 1) return false;
    out = v != 0;
    return true;
}

inline bool read_resources(wire::PacketReader& r, std::vector<CraftResource>& out) {
    const int32_t n = static_cast<int32_t>(r.read_uint32());
    // a resource is at least its record and an empty rarity list
    if (overran(r) || n < 0 || static_cast<size_t>(n) > kMaxCraftResources || static_cast<size_t>(n) * 96 > r.remaining_bits()) return false;
    out.reserve(static_cast<size_t>(n));
    for (int32_t i = 0; i < n; ++i) {
        CraftResource res;
        res.record = static_cast<int64_t>(r.read_uint64());
        const int32_t m = static_cast<int32_t>(r.read_uint32());
        if (overran(r) || m < 0 || static_cast<size_t>(m) > kMaxCraftRarities || static_cast<size_t>(m) * 72 > r.remaining_bits()) return false;
        res.rarities.reserve(static_cast<size_t>(m));
        for (int32_t k = 0; k < m; ++k) {
            CraftRarityCount c;
            c.rarity = r.read_uint8();
            c.count = static_cast<int32_t>(r.read_uint32());
            if (!read_ubool(r, c.from_warehouse) || c.rarity > 6 || c.count < 0 || c.count > kMaxCraftCount) return false;
            res.rarities.push_back(c);
        }
        out.push_back(std::move(res));
    }
    return !overran(r);
}

inline void write_resources(emit::BunchWriter& out, const std::vector<CraftResource>& list) {
    out.write_uint32(static_cast<uint32_t>(list.size()));
    for (const CraftResource& res : list) {
        out.write_uint64(static_cast<uint64_t>(res.record));
        out.write_uint32(static_cast<uint32_t>(res.rarities.size()));
        for (const CraftRarityCount& c : res.rarities) {
            out.write_uint8(c.rarity);
            out.write_uint32(static_cast<uint32_t>(c.count));
            out.write_uint32(c.from_warehouse ? 1u : 0u);
        }
    }
}

inline GuidValue read_guid(wire::PacketReader& r) {
    GuidValue g;
    g.a = r.read_uint32();
    g.b = r.read_uint32();
    g.c = r.read_uint32();
    g.d = r.read_uint32();
    return g;
}

inline void write_guid(emit::BunchWriter& out, const GuidValue& g) {
    out.write_uint32(g.a);
    out.write_uint32(g.b);
    out.write_uint32(g.c);
    out.write_uint32(g.d);
}

/// The members only a Complete state has: the experience rows, the products and the three numbers before the closing pair.
inline bool read_complete_tail(wire::PacketReader& r, CraftItemState& s) {
    const int32_t rows = static_cast<int32_t>(r.read_uint32());
    if (overran(r) || rows < 0 || static_cast<size_t>(rows) > kMaxCraftExperience || static_cast<size_t>(rows) * 192 > r.remaining_bits()) return false;
    for (int32_t i = 0; i < rows; ++i) {
        CraftExperience x;
        x.guid = static_cast<int64_t>(r.read_uint64());
        x.type = static_cast<int64_t>(r.read_uint64());
        x.amount = static_cast<int64_t>(r.read_uint64());
        s.experience.push_back(x);
    }
    const int32_t products = static_cast<int32_t>(r.read_uint32());
    // an item is at least its one "no item" bit; a hostile count must not buy memory
    if (overran(r) || products < 0 || static_cast<size_t>(products) > kMaxCraftProducts || static_cast<size_t>(products) > r.remaining_bits()) return false;
    for (int32_t i = 0; i < products; ++i) {
        auto item = read_item_instance(r);
        if (!item || !item->has_item) return false;
        s.products.push_back(std::move(*item));
    }
    s.unknown_a = static_cast<int32_t>(r.read_uint32());
    s.unknown_b = static_cast<int32_t>(r.read_uint32());
    s.level = static_cast<int32_t>(r.read_uint32());
    return !overran(r);
}

inline bool write_complete_tail(emit::BunchWriter& out, const CraftItemState& s) {
    out.write_uint32(static_cast<uint32_t>(s.experience.size()));
    for (const CraftExperience& x : s.experience) {
        out.write_uint64(static_cast<uint64_t>(x.guid));
        out.write_uint64(static_cast<uint64_t>(x.type));
        out.write_uint64(static_cast<uint64_t>(x.amount));
    }
    out.write_uint32(static_cast<uint32_t>(s.products.size()));
    for (const ItemInstance& p : s.products) {
        if (!write_item_instance(out, p)) return false;
    }
    out.write_uint32(static_cast<uint32_t>(s.unknown_a));
    out.write_uint32(static_cast<uint32_t>(s.unknown_b));
    out.write_uint32(static_cast<uint32_t>(s.level));
    return true;
}

}  // namespace craft_detail

inline std::optional<CraftItemState> read_craft_item_state(wire::PacketReader& r) {
    CraftItemState s;
    s.recipe = static_cast<int64_t>(r.read_uint64());
    s.sub_recipe = static_cast<int64_t>(r.read_uint64());
    s.start_time = std::bit_cast<double>(r.read_uint64());
    s.duration = std::bit_cast<float>(r.read_uint32());
    s.quantity = static_cast<int32_t>(r.read_uint32());
    if (overran(r) || !craft_detail::read_resources(r, s.general) || !craft_detail::read_resources(r, s.flexible)) return std::nullopt;
    if (r.read_uint32() != 0) return std::nullopt;   // tag resources: only the empty list is read
    s.transaction = craft_detail::read_guid(r);
    s.job = craft_detail::read_guid(r);
    s.status = r.read_uint8();
    if (overran(r) || s.status > 5) return std::nullopt;
    if (r.read_uint32() != 0) return std::nullopt;   // items: only the empty list is read
    if (s.status == 3 && !craft_detail::read_complete_tail(r, s)) return std::nullopt;
    if (!craft_detail::read_ubool(r, s.flag)) return std::nullopt;
    s.extra = r.read_uint8();
    if (overran(r)) return std::nullopt;
    return s;
}

inline bool write_craft_item_state(emit::BunchWriter& out, const CraftItemState& s) {
    if (!craft_state_fits(s)) return false;
    out.write_uint64(static_cast<uint64_t>(s.recipe));
    out.write_uint64(static_cast<uint64_t>(s.sub_recipe));
    out.write_uint64(std::bit_cast<uint64_t>(s.start_time));
    out.write_uint32(std::bit_cast<uint32_t>(s.duration));
    out.write_uint32(static_cast<uint32_t>(s.quantity));
    craft_detail::write_resources(out, s.general);
    craft_detail::write_resources(out, s.flexible);
    out.write_uint32(0);   // tag resources
    craft_detail::write_guid(out, s.transaction);
    craft_detail::write_guid(out, s.job);
    out.write_uint8(s.status);
    out.write_uint32(0);   // items
    if (s.status == 3 && !craft_detail::write_complete_tail(out, s)) return false;
    out.write_uint32(s.flag ? 1u : 0u);
    out.write_uint8(s.extra);
    return true;
}

}}}  // namespace aoc::protocol::repl
