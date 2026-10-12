// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/item_codec.cpp
//
//  See item_codec.h.
// ============================================================================
#include "protocol/repl/item_codec.h"

#include "protocol/repl/codecs.h"

#include <algorithm>
#include <bit>
#include <climits>

namespace aoc { namespace protocol { namespace repl {

namespace {

using emit::BunchWriter;
using wire::PacketReader;

// ---- primitives -------------------------------------------------------------

/// SerializeIntPacked of an int32: 7 bits a byte, bit 0 "more follows".  Refuses a value over 32 bits and any form but the
/// shortest (a zero payload byte after a continuation), so that reading and writing again gives the same bits.
std::optional<uint32_t> read_packed32(PacketReader& r) {
    uint64_t v = 0;
    for (int i = 0; i < 5; ++i) {
        const uint32_t b = static_cast<uint32_t>(r.read_bits(8));
        if (overran(r)) return std::nullopt;
        const uint32_t payload = b >> 1;
        v |= static_cast<uint64_t>(payload) << (7 * i);
        if (!(b & 1)) {
            if (i > 0 && payload == 0) return std::nullopt;   // not the shortest form
            if (v > UINT32_MAX) return std::nullopt;
            return static_cast<uint32_t>(v);
        }
    }
    return std::nullopt;                                       // a sixth byte
}

/// A UBOOL: a bool as 32 bits, which the archive refuses above 1.
std::optional<bool> read_ubool(PacketReader& r) {
    const uint32_t v = r.read_uint32();
    if (overran(r) || v > 1) return std::nullopt;
    return v != 0;
}

void write_ubool(BunchWriter& out, bool b) { out.write_uint32(b ? 1u : 0u); }

std::optional<GuidValue> read_guid(PacketReader& r) {
    GuidValue g;
    g.a = r.read_uint32();
    g.b = r.read_uint32();
    g.c = r.read_uint32();
    g.d = r.read_uint32();
    if (overran(r)) return std::nullopt;
    return g;
}
// built with C-Y-B and C.O.A

void write_guid(BunchWriter& out, const GuidValue& g) {
    out.write_uint32(g.a);
    out.write_uint32(g.b);
    out.write_uint32(g.c);
    out.write_uint32(g.d);
}

/// An int32 element count: within `maximum` and, at `min_bits_each` bits an element, no more than the bits left hold.
std::optional<uint32_t> read_count(PacketReader& r, size_t maximum, size_t min_bits_each) {
    const uint32_t n = r.read_uint32();
    if (overran(r) || n > maximum) return std::nullopt;
    if (static_cast<uint64_t>(n) * min_bits_each > r.remaining_bits()) return std::nullopt;
    return n;
}

double read_double(PacketReader& r) { return std::bit_cast<double>(r.read_uint64()); }
float  read_float(PacketReader& r) { return std::bit_cast<float>(r.read_uint32()); }
void   write_double(BunchWriter& out, double d) { out.write_uint64(std::bit_cast<uint64_t>(d)); }
void   write_float(BunchWriter& out, float f) { out.write_uint32(std::bit_cast<uint32_t>(f)); }

constexpr size_t kGemBits = 8 + 64 + 64 + 32 + 32;
constexpr size_t kStatBits = 64 + 32;
constexpr size_t kCooldownBits = 64 + 64 + 32;
constexpr size_t kCustomDataMinBits = 8 + 32;
constexpr size_t kEventSlotBits = 32 + 32 + 128;
constexpr size_t kContainerMinBits = 128 + 32 + 8;

// ---- containers ---------------------------------------------------------------

std::optional<StorageEventContainer> read_container(PacketReader& r) {
    StorageEventContainer c;
    auto object = read_object(r);
    if (!object) return std::nullopt;
    c.object = *object;
    const auto n = read_count(r, kMaxEventSlots, kEventSlotBits);
    if (!n) return std::nullopt;
    c.slots.reserve(*n);
    for (uint32_t i = 0; i < *n; ++i) {
        StorageEventSlot s;
        s.slot = static_cast<int32_t>(r.read_uint32());
        s.count = static_cast<int32_t>(r.read_uint32());
        auto g = read_guid(r);
        if (!g) return std::nullopt;
        s.item_guid = *g;
        c.slots.push_back(s);
    }
    c.status = r.read_uint8();
    if (overran(r)) return std::nullopt;
    return c;
}

bool container_fits(const StorageEventContainer& c) {
    return c.object.ObjectId != 1 && c.slots.size() <= kMaxEventSlots;
}

void write_container(BunchWriter& out, const StorageEventContainer& c) {
    emit::write_intrepid_guid(out, c.object);
    out.write_uint32(static_cast<uint32_t>(c.slots.size()));
    for (const StorageEventSlot& s : c.slots) {
        out.write_uint32(static_cast<uint32_t>(s.slot));
        out.write_uint32(static_cast<uint32_t>(s.count));
        write_guid(out, s.item_guid);
    }
    out.write_uint8(c.status);
}

}  // namespace

// ---- FItemInstance ------------------------------------------------------------

bool item_instance_fits(const ItemInstance& it) {
    if (!it.has_item) return it == ItemInstance{};
    if (it.gems.size() > kMaxItemGems || it.stats.size() > kMaxItemStats || it.cooldowns.size() > kMaxItemCooldowns ||
        it.custom_data.size() > kMaxItemCustomData)
        return false;
    return std::all_of(it.custom_data.begin(), it.custom_data.end(),
                       [](const PersistedCustomData& d) { return fstring_fits(d.text); });
}
// [C | O | A + Cyb]

std::optional<ItemInstance> read_item_instance(PacketReader& r) {
    ItemInstance it;
    const int has = r.read_bit();
    if (overran(r)) return std::nullopt;
    if (!has) return it;                       // the empty item
    it.has_item = true;
    it.record_id = r.read_uint64();
    const auto quantity = read_packed32(r);
    const auto quality = read_packed32(r);
    const auto enchantment = read_packed32(r);
    const auto tempering = read_packed32(r);
    if (!quantity || !quality || !enchantment || !tempering) return std::nullopt;
    it.quantity = *quantity;
    it.quality = *quality;
    it.enchantment_level = *enchantment;
    it.tempering_value = *tempering;

    const auto gems = read_count(r, kMaxItemGems, kGemBits);
    if (!gems) return std::nullopt;
    for (uint32_t i = 0; i < *gems; ++i) {
        SocketedGem g;
        g.socket_index = r.read_uint8();
        g.item_record_id = r.read_uint64();
        g.socket_record_id = r.read_uint64();
        g.quality = r.read_uint32();
        const auto flag = read_ubool(r);
        if (!flag) return std::nullopt;
        g.flag = *flag;
        it.gems.push_back(g);
    }

    it.durability = read_float(r);
    const auto stolen32 = read_ubool(r);
    auto guid = read_guid(r);
    it.creation_time = static_cast<int64_t>(r.read_uint64());
    auto crate = read_guid(r);
    const int stolen1 = r.read_bit();
    if (!stolen32 || !guid || !crate || overran(r)) return std::nullopt;
    if (stolen1 != (*stolen32 ? 1 : 0)) return std::nullopt;       // the exe overwrites one copy with the other
    it.stolen = stolen1 != 0;
    it.guid = *guid;
    it.crate_guid = *crate;

    const auto guid2 = read_guid(r);
    const int64_t creation2 = static_cast<int64_t>(r.read_uint64());
    if (!guid2 || *guid2 != it.guid || creation2 != it.creation_time) return std::nullopt;
    it.node_guid_purchased_from = r.read_uint64();
    const auto crate2 = read_guid(r);
    if (!crate2 || *crate2 != it.crate_guid) return std::nullopt;
    it.lock_expiration = static_cast<int64_t>(r.read_uint64());

    const uint32_t stats = static_cast<uint32_t>(r.read_bits(6));
    if (overran(r) || stats > kMaxItemStats) return std::nullopt;
    for (uint32_t i = 0; i < stats; ++i) {
        PersistedStat s;
        s.stat_id = r.read_uint64();
        s.value = read_float(r);
        it.stats.push_back(s);
    }

    const auto cooldowns = read_count(r, kMaxItemCooldowns, kCooldownBits);
    if (!cooldowns) return std::nullopt;
    for (uint32_t i = 0; i < *cooldowns; ++i) {
        PersistedCooldown c;
        c.record_guid = r.read_uint64();
        c.date_time_start = read_double(r);
        c.saved_charges = r.read_uint32();
        it.cooldowns.push_back(c);
    }

    const auto custom = read_count(r, kMaxItemCustomData, kCustomDataMinBits);
    if (!custom) return std::nullopt;
    for (uint32_t i = 0; i < *custom; ++i) {
        PersistedCustomData d;
        d.type = r.read_uint8();
        auto text = read_fstring(r);
        if (!text) return std::nullopt;
        d.text = std::move(*text);
        it.custom_data.push_back(std::move(d));
    }

    it.synchronized_timestamp = read_double(r);
    if (overran(r)) return std::nullopt;
    return it;
}

bool write_item_instance(BunchWriter& out, const ItemInstance& it) {
    if (!item_instance_fits(it)) return false;
    out.write_bit(it.has_item);
    if (!it.has_item) return true;
    out.write_uint64(it.record_id);
    out.write_sip(it.quantity);
    out.write_sip(it.quality);
    out.write_sip(it.enchantment_level);
    out.write_sip(it.tempering_value);
    out.write_uint32(static_cast<uint32_t>(it.gems.size()));
    for (const SocketedGem& g : it.gems) {
        out.write_uint8(g.socket_index);
        out.write_uint64(g.item_record_id);
        out.write_uint64(g.socket_record_id);
        out.write_uint32(g.quality);
        write_ubool(out, g.flag);
    }
    write_float(out, it.durability);
    write_ubool(out, it.stolen);
    write_guid(out, it.guid);
    out.write_uint64(static_cast<uint64_t>(it.creation_time));
    write_guid(out, it.crate_guid);
    out.write_bit(it.stolen);
    write_guid(out, it.guid);
    out.write_uint64(static_cast<uint64_t>(it.creation_time));
    out.write_uint64(it.node_guid_purchased_from);
    write_guid(out, it.crate_guid);
    out.write_uint64(static_cast<uint64_t>(it.lock_expiration));
    out.write_bits(it.stats.size(), 6);
    for (const PersistedStat& s : it.stats) {
        out.write_uint64(s.stat_id);
        write_float(out, s.value);
    }
    out.write_uint32(static_cast<uint32_t>(it.cooldowns.size()));
    for (const PersistedCooldown& c : it.cooldowns) {
        out.write_uint64(c.record_guid);
        write_double(out, c.date_time_start);
        out.write_uint32(c.saved_charges);
    }
    out.write_uint32(static_cast<uint32_t>(it.custom_data.size()));
    for (const PersistedCustomData& d : it.custom_data) {
        out.write_uint8(d.type);
        if (!write_fstring(out, d.text)) return false;   // cannot happen after item_instance_fits
    }
    write_double(out, it.synchronized_timestamp);
    return true;
}

// ---- FItemStorageEvent --------------------------------------------------------

bool item_storage_event_fits(const ItemStorageEvent& e) {
    if (!item_instance_fits(e.item) || !container_fits(e.move_source) || e.targets.size() > kMaxEventContainers)
        return false;
    return std::all_of(e.targets.begin(), e.targets.end(), container_fits);
}
// built by Cyb with C | O | A

std::optional<ItemStorageEvent> read_item_storage_event(PacketReader& r) {
    ItemStorageEvent e;
    auto source = read_container(r);
    if (!source) return std::nullopt;
    e.move_source = std::move(*source);
    auto item = read_item_instance(r);
    if (!item) return std::nullopt;
    e.item = std::move(*item);
    const auto optional_struct = read_ubool(r);
    if (!optional_struct || *optional_struct) return std::nullopt;   // set: a struct no capture shows, refused
    e.type = r.read_uint8();
    auto instigator = read_guid(r);
    if (!instigator) return std::nullopt;
    e.instigator = *instigator;
    const auto has_a = read_ubool(r);
    if (!has_a) return std::nullopt;
    if (*has_a) {
        auto g = read_guid(r);
        if (!g) return std::nullopt;
        e.extra_guid_a = *g;
    }
    const auto has_i64 = read_ubool(r);
    if (!has_i64) return std::nullopt;
    if (*has_i64) e.extra_i64 = r.read_uint64();
    const auto has_b = read_ubool(r);
    if (!has_b) return std::nullopt;
    if (*has_b) {
        auto g = read_guid(r);
        if (!g) return std::nullopt;
        e.extra_guid_b = *g;
    }
    const auto targets = read_count(r, kMaxEventContainers, kContainerMinBits);
    if (!targets) return std::nullopt;
    for (uint32_t i = 0; i < *targets; ++i) {
        auto c = read_container(r);
        if (!c) return std::nullopt;
        e.targets.push_back(std::move(*c));
    }
    if (overran(r)) return std::nullopt;
    return e;
}

bool write_item_storage_event(BunchWriter& out, const ItemStorageEvent& e) {
    if (!item_storage_event_fits(e)) return false;
    write_container(out, e.move_source);
    if (!write_item_instance(out, e.item)) return false;
    write_ubool(out, false);   // the optional struct: never set
    out.write_uint8(e.type);
    write_guid(out, e.instigator);
    write_ubool(out, e.extra_guid_a.has_value());
    if (e.extra_guid_a) write_guid(out, *e.extra_guid_a);
    write_ubool(out, e.extra_i64.has_value());
    if (e.extra_i64) out.write_uint64(*e.extra_i64);
    write_ubool(out, e.extra_guid_b.has_value());
    if (e.extra_guid_b) write_guid(out, *e.extra_guid_b);
    out.write_uint32(static_cast<uint32_t>(e.targets.size()));
    for (const StorageEventContainer& c : e.targets) write_container(out, c);
    return true;
}

}}}  // namespace aoc::protocol::repl
