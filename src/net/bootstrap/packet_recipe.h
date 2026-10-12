// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/bootstrap/packet_recipe.h
//
//  The three-mode emission framework.  Every packet in the 100-packet
//  bootstrap plan is represented by one PacketRecipe instance:
//
//    StaticPacketRecipe  — emit captured bytes verbatim.  Zero changes.
//                          Used for packets with no dynamic content.
//
//    PatchedPacketRecipe — emit captured bytes with specific bit ranges
//                          replaced by CharacterProfile-derived values.
//                          Used for packets like pkt#22 (PC ActorOpen)
//                          where the exports + SNA structure is static
//                          but actor_netguid and spawn location are
//                          dynamic.
//
//    NativePacketRecipe  — emit fully programmatic output (via
//                          ActorBuilder etc.).  Byte-identity tested
//                          against the captured fixture.  Used for
//                          the PC ActorOpen once we've calibrated
//                          ActorBuilder (test_pc_spawn_diff passes).
//
//  The BootstrapRunner iterates a vector<unique_ptr<PacketRecipe>> in
//  order, calls recipe->build(profile, allocator, replay_data) per
//  step, and ships each result via IGameServerHost::send_bunch_packet.
//
//  LAYER:   net / bootstrap
//  SESSION: M2
// ============================================================================
#pragma once

#include "net/bootstrap/character_profile.h"
#include "net/bootstrap/netguid_allocator.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Forward-decl: ReplayData is defined at global namespace in game_server.h
struct ReplayData;

namespace aoc { namespace net { namespace bootstrap {

/// Build context passed to each recipe's build() method.
struct BuildContext {
    const CharacterProfile&   profile;
    const NetGUIDAllocator&   alloc;
    const ::ReplayData&       replay;     // for splice-mode extraction
};

/// Abstract base — every recipe produces a raw bunch-bit stream ready
/// for IGameServerHost::send_bunch_packet.  Returns {bits, bit_count}
/// pair; bits.size() * 8 >= bit_count (last byte may have padding).
struct BunchBits {
    std::vector<uint8_t> bytes;
    std::size_t          bit_count;
};

class PacketRecipe {
public:
    virtual ~PacketRecipe() = default;

    virtual BunchBits build(const BuildContext& ctx) const = 0;

    /// Short human-readable name for logging.  e.g. "pkt#22 PC ActorOpen"
    virtual const char* name() const = 0;

    /// Which captured packet (index into ReplayData::packets) does this
    /// correspond to?  Used for diagnostics + as the splice source for
    /// Static/Patched recipes.  Returns -1 for pure-native recipes that
    /// don't map to a single captured packet.
    virtual int32_t replay_pkt_idx() const = 0;
};

// ── Recipe 1: Static — pure splice ──────────────────────────────────
// ~ C-O-A - Cyb ~

/// Emits the captured packet's bunch bit stream unchanged.  The bunch
/// bits are extracted from the ReplayData at build time; the packet's
/// seq/ack/header gets rewritten by send_bunch_packet.
class StaticPacketRecipe : public PacketRecipe {
public:
    StaticPacketRecipe(int32_t replay_idx, std::string display_name)
        : replay_idx_(replay_idx), name_(std::move(display_name)) {}

    BunchBits build(const BuildContext& ctx) const override;
    const char* name() const override { return name_.c_str(); }
    int32_t replay_pkt_idx() const override { return replay_idx_; }

private:
    int32_t     replay_idx_;
    std::string name_;
};

// ── Recipe 2: Patched — splice with field overrides ──────────────────

/// A single dynamic field replacement within a captured packet.
/// `bit_offset` is measured from bit 0 of the extracted bunch stream
/// (NOT from the raw captured packet — the splice extracts out the
/// packet header first).
struct FieldPatch {
    std::string field_name;
    std::size_t bit_offset;
    std::size_t bit_width;   ///< for fixed-width fields (int, NetGUID); 0 for variable (FString)

    /// Encode the dynamic value as a bit stream ready to be written
    /// at `bit_offset`.  For fixed-width fields, returned bits.size()
    /// * 8 must be >= bit_width (extra bits ignored).  For FStrings
    /// (bit_width=0), the caller is responsible for ensuring the
    /// new value is the same byte count as the captured one, OR the
    /// surrounding packet gets re-fragmented.
    std::function<BunchBits(const BuildContext&)> encode;
};

class PatchedPacketRecipe : public PacketRecipe {
public:
    PatchedPacketRecipe(int32_t replay_idx,
                         std::string display_name,
                         std::vector<FieldPatch> patches)
        : replay_idx_(replay_idx),
          name_(std::move(display_name)),
          patches_(std::move(patches)) {}

    BunchBits build(const BuildContext& ctx) const override;
    const char* name() const override { return name_.c_str(); }
    int32_t replay_pkt_idx() const override { return replay_idx_; }

private:
    int32_t                 replay_idx_;
    std::string             name_;
    std::vector<FieldPatch> patches_;
};

// ── Recipe 3: Native — fully programmatic ────────────────────────────

/// Builder callback that produces the complete bunch bits from profile
/// + allocator, bypassing the replay data entirely.
using NativeBuilder = std::function<BunchBits(const BuildContext&)>;

class NativePacketRecipe : public PacketRecipe {
public:
    NativePacketRecipe(std::string display_name,
                        int32_t corresponding_replay_idx,
                        NativeBuilder builder)
        : name_(std::move(display_name)),
          replay_idx_(corresponding_replay_idx),
          builder_(std::move(builder)) {}

    BunchBits build(const BuildContext& ctx) const override {
        return builder_(ctx);
    }
    const char* name() const override { return name_.c_str(); }
    int32_t replay_pkt_idx() const override { return replay_idx_; }

private:
    std::string   name_;
    int32_t       replay_idx_;
    NativeBuilder builder_;
};

// ── Bit-patching utilities ──────────────────────────────────────────

/// Overwrite `dst` bits [dst_bit_off .. dst_bit_off + bit_count) with
/// bits read from `src` starting at bit 0.  LSB-first, same convention
/// as BunchWriter / the replay extractor.
void patch_bits_in_place(std::vector<uint8_t>& dst,
                          std::size_t            dst_bit_off,
                          const uint8_t*         src,
                          std::size_t            bit_count);

/// Encode an FIntrepidNetworkGUID (128 bits) LSB-first.
BunchBits encode_intrepid_netguid(const IntrepidNetGUID& g);

/// Encode a SerializePackedVector-style position (3 × location_bits
/// offset-binary ints).  Does NOT include the 5-bit BitsNeeded prefix
/// or the preceding bSerializeLocation + bQuantizeLocation flags —
/// those are captured-static, only the three coordinate values change.
BunchBits encode_location_body(int32_t x, int32_t y, int32_t z, uint8_t location_bits);

/// Encode a UE5 FString (int32 len LE + ASCII + NUL).
BunchBits encode_fstring(const std::string& s);

}}} // namespace aoc::net::bootstrap
