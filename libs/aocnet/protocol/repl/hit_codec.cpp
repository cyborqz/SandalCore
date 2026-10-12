// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/hit_codec.cpp
//
//  See hit_codec.h.
// ============================================================================
#include "protocol/repl/hit_codec.h"

#include "protocol/repl/codecs.h"

#include <bit>

namespace aoc { namespace protocol { namespace repl {

namespace {

using emit::BunchWriter;
using wire::PacketReader;

constexpr size_t kGuidBits = 128;
constexpr size_t kVectorBits = 3 * 64;
constexpr size_t kStatModBits = 64 + 32 + 32;
constexpr size_t kHitModBits = 64 + kGuidBits;
constexpr size_t kInterruptBits = 64;
// A target with no arrays: HitComp, two vectors, the three counts (5, 32, 5), bAttemptedInterrupt and Flags.
constexpr size_t kTargetMinBits = kGuidBits + 2 * kVectorBits + 5 + 32 + 32 + 8 + 5;
// A hit with no targets: four int64, bPvPForceFlag, two guids, the 7-bit count, the two prediction flags.
constexpr size_t kInstanceMinBits = 4 * 64 + 32 + 2 * kGuidBits + 7 + 2 * 32;
constexpr int    kHitsCountBits = 11;    // CeilLogTwo(1999 + 1)
constexpr int    kBlobLengthBits = 15;   // CeilLogTwo(19999 + 1)

// ---- primitives -------------------------------------------------------------

/// A UBOOL: a bool as 32 bits, which the archive refuses above 1.
std::optional<bool> read_ubool(PacketReader& r) {
    const uint32_t v = r.read_uint32();
    if (overran(r) || v > 1) return std::nullopt;
    return v != 0;
}
// [Cyb - C | O | A]

void write_ubool(BunchWriter& out, bool b) { out.write_uint32(b ? 1u : 0u); }

emit::FIntrepidNetworkGUID read_guid(PacketReader& r) {
    emit::FIntrepidNetworkGUID g;
    const uint32_t lo = r.read_uint32();
    const uint32_t hi = r.read_uint32();
    g.ObjectId = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
    g.ServerId = r.read_uint32();
    g.Randomizer = r.read_uint32();
    return g;
}

Vec3d read_vector(PacketReader& r) {
    Vec3d v;
    v.x = std::bit_cast<double>(r.read_uint64());
    v.y = std::bit_cast<double>(r.read_uint64());
    v.z = std::bit_cast<double>(r.read_uint64());
    return v;
}

void write_vector(BunchWriter& out, const Vec3d& v) {
    out.write_uint64(std::bit_cast<uint64_t>(v.x));
    out.write_uint64(std::bit_cast<uint64_t>(v.y));
    out.write_uint64(std::bit_cast<uint64_t>(v.z));
}

/// A count of `bits` bits within `maximum` that, at `min_bits_each` bits an element, the bits left can hold.
std::optional<size_t> read_small_count(PacketReader& r, int bits, size_t maximum, size_t min_bits_each) {
    const size_t n = static_cast<size_t>(r.read_bits(bits));
    if (overran(r) || n > maximum || n * min_bits_each > r.remaining_bits()) return std::nullopt;
    return n;
}

// ---- one target ---------------------------------------------------------------

std::optional<HitTarget> read_target(PacketReader& r) {
    HitTarget t;
    t.hit_comp = read_guid(r);
    t.impact_location = read_vector(r);
    t.impact_normal = read_vector(r);
    if (overran(r)) return std::nullopt;

    const auto n_stat = read_small_count(r, 5, kMaxHitStatMods, kStatModBits);
    if (!n_stat) return std::nullopt;
    t.stat_mods.reserve(*n_stat);
    for (size_t i = 0; i < *n_stat; ++i) {
        HitStatMod m;
        m.guid = static_cast<int64_t>(r.read_uint64());
        m.value = std::bit_cast<float>(r.read_uint32());
        m.effective_value = std::bit_cast<float>(r.read_uint32());
        t.stat_mods.push_back(m);
    }

    const uint32_t n_interrupt = r.read_uint32();   // an int32 in the exe: a negative count is refused too
    if (overran(r) || n_interrupt > kMaxHitInterrupts ||
        static_cast<uint64_t>(n_interrupt) * kInterruptBits > r.remaining_bits())
        return std::nullopt;
    t.interrupts.reserve(n_interrupt);
    for (uint32_t i = 0; i < n_interrupt; ++i) t.interrupts.push_back(static_cast<int64_t>(r.read_uint64()));

    const auto attempted = read_ubool(r);
    if (!attempted) return std::nullopt;
    t.attempted_interrupt = *attempted;
    t.flags = r.read_uint8();
// ~ C | O | A - Cyb ~

    const auto n_mod = read_small_count(r, 5, kMaxHitHitMods, kHitModBits);
    if (!n_mod) return std::nullopt;
    t.hit_mods.reserve(*n_mod);
    for (size_t i = 0; i < *n_mod; ++i) {
        HitModInst m;
        m.guid = static_cast<int64_t>(r.read_uint64());
        m.owner = read_guid(r);
        t.hit_mods.push_back(m);
    }
    if (overran(r)) return std::nullopt;
    return t;
}

bool target_fits(const HitTarget& t) {
    return t.stat_mods.size() <= kMaxHitStatMods && t.interrupts.size() <= kMaxHitInterrupts && t.hit_mods.size() <= kMaxHitHitMods;
}

size_t target_bits(const HitTarget& t) {
    return kTargetMinBits + t.stat_mods.size() * kStatModBits + t.interrupts.size() * kInterruptBits + t.hit_mods.size() * kHitModBits;
}

void write_target(BunchWriter& out, const HitTarget& t) {
    emit::write_intrepid_guid(out, t.hit_comp);
    write_vector(out, t.impact_location);
    write_vector(out, t.impact_normal);
    out.write_bits(t.stat_mods.size(), 5);
    for (const HitStatMod& m : t.stat_mods) {
        out.write_uint64(static_cast<uint64_t>(m.guid));
        out.write_uint32(std::bit_cast<uint32_t>(m.value));
        out.write_uint32(std::bit_cast<uint32_t>(m.effective_value));
    }
    out.write_uint32(static_cast<uint32_t>(t.interrupts.size()));
    for (const int64_t a : t.interrupts) out.write_uint64(static_cast<uint64_t>(a));
    write_ubool(out, t.attempted_interrupt);
    out.write_uint8(t.flags);
    out.write_bits(t.hit_mods.size(), 5);
    for (const HitModInst& m : t.hit_mods) {
        out.write_uint64(static_cast<uint64_t>(m.guid));
        emit::write_intrepid_guid(out, m.owner);
    }
}

// ---- one hit ------------------------------------------------------------------

std::optional<HitInstance> read_instance(PacketReader& r) {
    HitInstance h;
    h.inst_guid = static_cast<int64_t>(r.read_uint64());
    h.record_guid = static_cast<int64_t>(r.read_uint64());
    h.source_record_type = static_cast<int64_t>(r.read_uint64());
    h.source_record_guid = static_cast<int64_t>(r.read_uint64());
    const auto pvp = read_ubool(r);
    if (!pvp) return std::nullopt;
    h.pvp_force = *pvp;
    h.instigator = read_guid(r);
    h.source = read_guid(r);
    const auto n = read_small_count(r, 7, kMaxHitTargets, kTargetMinBits);
    if (!n) return std::nullopt;
    h.targets.reserve(*n);
    for (size_t i = 0; i < *n; ++i) {
        auto t = read_target(r);
        if (!t) return std::nullopt;
        h.targets.push_back(std::move(*t));
    }
    const auto autonomous = read_ubool(r);
    const auto simulated = autonomous ? read_ubool(r) : std::nullopt;
    if (!autonomous || !simulated) return std::nullopt;
    h.predicted_on_autonomous = *autonomous;
    h.predicted_on_simulated = *simulated;
    return h;
}

void write_instance(BunchWriter& out, const HitInstance& h) {
    out.write_uint64(static_cast<uint64_t>(h.inst_guid));
    out.write_uint64(static_cast<uint64_t>(h.record_guid));
    out.write_uint64(static_cast<uint64_t>(h.source_record_type));
    out.write_uint64(static_cast<uint64_t>(h.source_record_guid));
    write_ubool(out, h.pvp_force);
    emit::write_intrepid_guid(out, h.instigator);
    emit::write_intrepid_guid(out, h.source);
    out.write_bits(h.targets.size(), 7);
    for (const HitTarget& t : h.targets) write_target(out, t);
    write_ubool(out, h.predicted_on_autonomous);
    write_ubool(out, h.predicted_on_simulated);
}

}  // namespace

// ---- the blob -------------------------------------------------------------------

size_t hit_instance_bits(const HitInstance& hit) {
    size_t bits = kInstanceMinBits;
    for (const HitTarget& t : hit.targets) bits += target_bits(t);
    return bits;
}

bool hit_instance_fits(const HitInstance& hit) {
    if (hit.targets.size() > kMaxHitTargets) return false;
    for (const HitTarget& t : hit.targets) {
        if (!target_fits(t)) return false;
    }
    return (hit_instance_bits(hit) + 7) / 8 <= kMaxHitBlobBytes;
}

std::optional<std::vector<uint8_t>> serialize_hit_instance(const HitInstance& hit) {
    if (!hit_instance_fits(hit)) return std::nullopt;
    BunchWriter w(hit_instance_bits(hit) / 8 + 8);
    write_instance(w, hit);
    return std::vector<uint8_t>(w.data(), w.data() + w.byte_size());
}
// CoA : C_Y_B

std::optional<HitInstance> deserialize_hit_instance(const uint8_t* data, size_t bytes) {
    if (!data || bytes > kMaxHitBlobBytes) return std::nullopt;
    PacketReader r(data, bytes, bytes * 8);
    auto h = read_instance(r);
    if (!h || overran(r)) return std::nullopt;
    // The exe's writer pads the last byte with zero bits and ends the blob there.
    const size_t left = bytes * 8 - r.pos();
    if (left >= 8) return std::nullopt;
    if (left && r.read_bits(static_cast<int>(left)) != 0) return std::nullopt;
    return h;
}

// ---- the struct -----------------------------------------------------------------

bool hit_confirmations_fit(const HitConfirmations& c) {
    if (c.hits.size() > kMaxHitConfirmations) return false;
    for (const HitInstance& h : c.hits) {
        if (!hit_instance_fits(h)) return false;
    }
    return true;
}

std::optional<HitConfirmations> read_hit_confirmations(PacketReader& r) {
    const size_t n = static_cast<size_t>(r.read_bits(kHitsCountBits));
    // Each hit is at least a 15-bit length and the smallest blob.
    if (overran(r) || n > kMaxHitConfirmations || n * (kBlobLengthBits + ((kInstanceMinBits + 7) / 8) * 8) > r.remaining_bits())
        return std::nullopt;
    HitConfirmations c;
    c.hits.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const size_t len = static_cast<size_t>(r.read_bits(kBlobLengthBits));
        if (overran(r) || len > kMaxHitBlobBytes || len * 8 > r.remaining_bits()) return std::nullopt;
        std::vector<uint8_t> blob(len);
        for (uint8_t& b : blob) b = r.read_uint8();
        auto h = deserialize_hit_instance(blob.data(), blob.size());
        if (!h) return std::nullopt;
        c.hits.push_back(std::move(*h));
    }
    if (overran(r)) return std::nullopt;
    return c;
}

bool write_hit_confirmations(BunchWriter& out, const HitConfirmations& c) {
    if (!hit_confirmations_fit(c)) return false;
    // Serialize every blob first: a hit that does not fit leaves `out` untouched (hit_confirmations_fit has checked them).
    std::vector<std::vector<uint8_t>> blobs;
    blobs.reserve(c.hits.size());
    for (const HitInstance& h : c.hits) {
        auto blob = serialize_hit_instance(h);
        if (!blob) return false;
        blobs.push_back(std::move(*blob));
    }
    out.write_bits(c.hits.size(), kHitsCountBits);
    for (const auto& blob : blobs) {
        out.write_bits(blob.size(), kBlobLengthBits);
        for (const uint8_t b : blob) out.write_uint8(b);
    }
    return true;
}

}}}  // namespace aoc::protocol::repl
