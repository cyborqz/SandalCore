// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/codecs.cpp
//
//  See codecs.h.  Every reader mirrors tools/ghidra/schema_check.py; every
//  writer is its inverse and is proven bit for bit by test_repl_codecs.cpp and
//  test_repl_rep_block.cpp against the original server's blocks.
// ============================================================================
#include "protocol/repl/codecs.h"

#include "protocol/emit/net_serializers.h"
#include "protocol/repl/hit_codec.h"
#include "protocol/repl/vendor_codec.h"
#include "protocol/repl/callout_codec.h"
#include "protocol/repl/channeling_codec.h"
#include "protocol/repl/item_codec.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <set>
#include <tuple>

namespace aoc { namespace protocol { namespace repl {

namespace {

using emit::BunchWriter;
using wire::PacketReader;

// ---- UTF-8 <-> UTF-16 (the wire's UCS-2 is UTF-16 code units) ---------------

bool utf8_to_utf16(const std::string& s, std::u16string& out) {
    size_t i = 0;
    while (i < s.size()) {
        const uint8_t c = static_cast<uint8_t>(s[i]);
        uint32_t cp;
        size_t n;
        if (c < 0x80)                { cp = c;        n = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
        else return false;
        if (i + n > s.size()) return false;
        for (size_t k = 1; k < n; ++k) {
            const uint8_t cc = static_cast<uint8_t>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) ||
            cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
        i += n;
    }
    return true;
}
// (C Y B ~ CoA)

bool utf16_to_utf8(const std::u16string& u, std::string& out) {
    for (size_t i = 0; i < u.size(); ++i) {
        uint32_t cp = u[i];
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (i + 1 >= u.size() || u[i + 1] < 0xDC00 || u[i + 1] > 0xDFFF) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (u[i + 1] - 0xDC00);
            ++i;
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return false;
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return true;
}

/// GetBitsNeeded (QuantizedVectorSerialization.cpp): the two's-complement
/// width of `v`, at least 1.
uint32_t bits_needed(int64_t v) {
    uint64_t m = static_cast<uint64_t>(v ^ (v >> 63));
    uint32_t n = 1;
    while (m) { m >>= 1; ++n; }
    return n;
}

double read_double(PacketReader& r) { return std::bit_cast<double>(r.read_uint64()); }
void   write_double(BunchWriter& out, double d) { out.write_uint64(std::bit_cast<uint64_t>(d)); }

// ---- the structs ------------------------------------------------------------

enum class S {
    Unknown, Vec3, Vec2, Ticks, Rotator, Quantized, Normal, RepMovement, LoadTracker, UniqueId, Tag, TagContainer,
    StatInt32, Quat, GuidToBool, Alignment, AppearanceId, Customization, ItemInstance, ItemStorageEvent, HitConfirmations,
    MovementInput, VendorPurchase, VendorTransaction, NodeTax, LocalizedMessage, ChannelingData,
};

S find_struct(std::string_view n) {
    if (n == "FVector" || n == "FVector3d") return S::Vec3;
    if (n == "FVector2D" || n == "FVector2d") return S::Vec2;
    if (n == "FDateTime" || n == "FTimespan") return S::Ticks;
    if (n == "FRotator") return S::Rotator;
    if (n == "FVector_NetQuantize" || n == "FVector_NetQuantize10" || n == "FVector_NetQuantize100") return S::Quantized;
    if (n == "FVector_NetQuantizeNormal") return S::Normal;
    if (n == "FRepMovement") return S::RepMovement;
    if (n == "FCharacterLoadTracker") return S::LoadTracker;
    if (n == "FUniqueNetIdRepl") return S::UniqueId;
    if (n == "FGameplayTag") return S::Tag;
    if (n == "FGameplayTagContainer") return S::TagContainer;
    if (n == "FStatInt32InstRep") return S::StatInt32;
    if (n == "FQuat") return S::Quat;
    if (n == "FGuidToBoolWrapper") return S::GuidToBool;
    if (n == "FAlignmentContainer") return S::Alignment;
    if (n == "FAppearanceId") return S::AppearanceId;
    if (n == "FCharacterCustomizationSaveData") return S::Customization;
    if (n == "FItemInstance") return S::ItemInstance;
    if (n == "FItemStorageEvent") return S::ItemStorageEvent;
    if (n == "FSerializedClientHitConfirmations") return S::HitConfirmations;
    if (n == "FPlayerMovementInput") return S::MovementInput;
    if (n == "FVendorPurchaseCart") return S::VendorPurchase;
    if (n == "FVendorTransactionCart") return S::VendorTransaction;
    if (n == "FNodeTaxDetails") return S::NodeTax;
    if (n == "FLocalizedMessage") return S::LocalizedMessage;
    if (n == "FChannelingData") return S::ChannelingData;
    return S::Unknown;
}

/// FRotator::SerializeCompressed (8 bits) / SerializeCompressedShort (16):
/// per axis a non-zero bit and the value.
std::optional<Rot> read_rotator(PacketReader& r, int bits) {
    Rot rot;
    uint16_t* axes[3] = {&rot.pitch, &rot.yaw, &rot.roll};
    for (uint16_t* a : axes) {
        if (r.read_bit()) *a = static_cast<uint16_t>(r.read_bits(bits));
    }
    if (overran(r)) return std::nullopt;
    return rot;
}

bool write_rotator(BunchWriter& out, const Rot& rot, int bits) {
    const uint16_t axes[3] = {rot.pitch, rot.yaw, rot.roll};
    for (uint16_t a : axes) {
        if (bits < 16 && (a >> bits) != 0) return false;
        out.write_bit(a != 0);
        if (a != 0) out.write_bits(a, bits);
    }
    return true;
}

std::optional<Value> read_rep_movement(PacketReader& r, const CodecOptions& opts) {
    RepMovement m;
    const uint32_t flags = static_cast<uint32_t>(r.read_bits(4));
    m.simulated_physic_sleep = (flags & 1) != 0;
    m.rep_physics = (flags & 2) != 0;
    auto loc = read_quantized_vector(r);
    auto rot = read_rotator(r, opts.rep_movement_short_rotation ? 16 : 8);
    auto vel = read_quantized_vector(r);
    if (!loc || !rot || !vel) return std::nullopt;
    m.location = *loc;
    m.rotation = *rot;
    m.linear_velocity = *vel;
    if (m.rep_physics) {
        auto ang = read_quantized_vector(r);
        if (!ang) return std::nullopt;
        m.angular_velocity = *ang;
    }
    if (flags & 4) {
        const auto v = r.read_sip();
        if (!v || *v > UINT32_MAX) return std::nullopt;
        m.server_frame = static_cast<uint32_t>(*v);
    }
    if (flags & 8) {
        const auto v = r.read_sip();
        if (!v || *v > UINT32_MAX) return std::nullopt;
        m.server_physics_handle = static_cast<uint32_t>(*v);
    }
    if (r.read_bit()) {
        auto acc = read_quantized_vector(r);
        if (!acc) return std::nullopt;
        m.acceleration = *acc;
    }
    if (overran(r)) return std::nullopt;
    return Value(std::move(m));
}
// (CoA | C-Y-B)

bool write_rep_movement(BunchWriter& out, const RepMovement& m, const CodecOptions& opts) {
    const uint32_t flags = (m.simulated_physic_sleep ? 1u : 0u) | (m.rep_physics ? 2u : 0u) |
                           (m.server_frame ? 4u : 0u) | (m.server_physics_handle ? 8u : 0u);
    out.write_bits(flags, 4);
    if (!write_quantized_vector(out, m.location)) return false;
    if (!write_rotator(out, m.rotation, opts.rep_movement_short_rotation ? 16 : 8)) return false;
    if (!write_quantized_vector(out, m.linear_velocity)) return false;
    if (m.rep_physics && !write_quantized_vector(out, m.angular_velocity)) return false;
    if (m.server_frame) out.write_sip(*m.server_frame);
    if (m.server_physics_handle) out.write_sip(*m.server_physics_handle);
    out.write_bit(m.acceleration.has_value());
    if (m.acceleration && !write_quantized_vector(out, *m.acceleration)) return false;
    return true;
}

std::optional<Value> read_unique_net_id(PacketReader& r) {
    UniqueNetId id;
    id.flags = r.read_uint8();
    if (overran(r)) return std::nullopt;
    const uint32_t type_hash = id.flags >> 3;
    if (id.flags & 1) {                                    // IsEncoded
        if (id.flags & 2) return Value(std::move(id));     // IsEmpty
        if (type_hash == 30) {                             // TypeHash_V2: EOnlineServices + TArray<uint8>
            id.services = r.read_uint8();
            const int32_t n = static_cast<int32_t>(r.read_uint32());
            if (n < 0 || static_cast<size_t>(n) * 8 > r.remaining_bits()) return std::nullopt;
            for (int32_t i = 0; i < n; ++i) id.bytes.push_back(r.read_uint8());
        } else {
            if (type_hash == 31) {                         // TypeHash_Other: the type name
                auto type = read_fstring(r);
                if (!type) return std::nullopt;
                id.type = std::move(*type);
            }
            const uint32_t n = r.read_uint8();
            for (uint32_t i = 0; i < n; ++i) id.bytes.push_back(r.read_uint8());
        }
    } else {
        if (type_hash == 31) {
            auto type = read_fstring(r);
            if (!type) return std::nullopt;
            id.type = std::move(*type);
        }
        auto s = read_fstring(r);
        if (!s) return std::nullopt;
        id.id = std::move(*s);
    }
    if (overran(r)) return std::nullopt;
    return Value(std::move(id));
}

bool write_unique_net_id(BunchWriter& out, const UniqueNetId& id) {
    out.write_uint8(id.flags);
    const uint32_t type_hash = id.flags >> 3;
    if (id.flags & 1) {
        if (id.flags & 2) return true;
        if (type_hash == 30) {
            out.write_uint8(id.services);
            out.write_int32(static_cast<int32_t>(id.bytes.size()));
        } else {
            if (type_hash == 31 && !write_fstring(out, id.type)) return false;
            if (id.bytes.size() > 255) return false;
            out.write_uint8(static_cast<uint8_t>(id.bytes.size()));
        }
        out.write_bytes(id.bytes.data(), id.bytes.size());
        return true;
    }
    if (type_hash == 31 && !write_fstring(out, id.type)) return false;
    return write_fstring(out, id.id);
}
// [C-O-A :: C-Y-B]

/// True when write_unique_net_id writes `id`: the parts the flags say go out, each one that fits (an encoded id of
/// a non-V2 type has a byte for its length).
bool unique_net_id_fits(const UniqueNetId& id) {
    const uint32_t type_hash = id.flags >> 3;
    if (id.flags & 1) {
        if (id.flags & 2) return true;
        if (type_hash == 30) return true;
        if (type_hash == 31 && !fstring_fits(id.type)) return false;
        return id.bytes.size() <= 255;
    }
    if (type_hash == 31 && !fstring_fits(id.type)) return false;
    return fstring_fits(id.id);
}

/// True when write_rep_movement writes `m`, its rotation aside (that depends on the object's rotation quantization).
bool rep_movement_fits(const RepMovement& m) {
    return quant_vec_fits(m.location) && quant_vec_fits(m.linear_velocity) && (!m.rep_physics || quant_vec_fits(m.angular_velocity)) &&
           (!m.acceleration || quant_vec_fits(*m.acceleration));
}


// ---- AoC's structs: the shared pieces ---------------------------------------

/// An element count as `Ar << int32 Num` writes it.  Refused when negative or
/// when what is left of the input cannot hold that many elements of at least
/// `min_bits` each (a hostile count then costs no memory).
std::optional<uint32_t> read_count(PacketReader& r, uint64_t min_bits) {
    const int32_t n = static_cast<int32_t>(r.read_uint32());
    if (overran(r) || n < 0 || static_cast<uint64_t>(n) * min_bits > r.remaining_bits()) return std::nullopt;
    return static_cast<uint32_t>(n);
}

bool write_count(BunchWriter& out, size_t n) {
    if (n > static_cast<size_t>(INT32_MAX)) return false;
    out.write_int32(static_cast<int32_t>(n));
    return true;
}

/// `Ar << FGuid`: A, B, C, D as four int32 (0x14141e960).
GuidValue read_guid(PacketReader& r) {
    GuidValue g;
    g.a = r.read_uint32();
    g.b = r.read_uint32();
    g.c = r.read_uint32();
    g.d = r.read_uint32();
    return g;
}

void write_guid(BunchWriter& out, const GuidValue& g) {
    out.write_uint32(g.a);
    out.write_uint32(g.b);
    out.write_uint32(g.c);
    out.write_uint32(g.d);
}

auto guid_key(const GuidValue& g) { return std::make_tuple(g.a, g.b, g.c, g.d); }

/// True when no two elements of `v` have the same `key` (a TMap / TSet holds each key once; a
/// duplicate would collapse on the client).
template <class T, class Key>
bool unique_keys(const std::vector<T>& v, Key key) {
    if (v.empty()) return true;
    std::set<decltype(key(v.front()))> seen;
    for (const T& e : v) {
        if (!seen.insert(key(e)).second) return false;
    }
    return true;
}

// FGuidToBoolWrapper: `Ar << TMap<FGuid, bool>` (0x145a7d730).
std::optional<Value> read_guid_to_bool(PacketReader& r) {
    const auto n = read_count(r, 128 + 32);
    if (!n) return std::nullopt;
    GuidToBoolWrapper w;
    for (uint32_t i = 0; i < *n; ++i) {
        const GuidValue g = read_guid(r);
        const uint32_t b = r.read_uint32();
        if (b > 1) return std::nullopt;   // the archive's bool: 0 or 1, else an error
        w.pairs.emplace_back(g, b != 0);
    }
    if (overran(r)) return std::nullopt;
    return Value(std::move(w));
}

bool write_guid_to_bool(BunchWriter& out, const GuidToBoolWrapper& w) {
    if (!write_count(out, w.pairs.size())) return false;
    for (const auto& [g, b] : w.pairs) {
        write_guid(out, g);
        out.write_uint32(b ? 1u : 0u);
    }
    return true;
}

bool guid_to_bool_fits(const GuidToBoolWrapper& w) {
    return unique_keys(w.pairs, [](const auto& p) { return guid_key(p.first); });
}

// FAlignmentContainer (0x145e77630): the two maps in turn.
std::optional<Value> read_alignment(PacketReader& r) {
    AlignmentContainer c;
    const auto n = read_count(r, 64 + 32);
    if (!n) return std::nullopt;
    for (uint32_t i = 0; i < *n; ++i) {
        AlignmentContainer::Instance inst;
        inst.key = r.read_uint64();
        const auto m = read_count(r, 128);
        if (!m) return std::nullopt;
        for (uint32_t j = 0; j < *m; ++j) inst.guids.push_back(read_guid(r));
        c.instances.push_back(std::move(inst));
    }
    const auto t = read_count(r, 32 + 3 * 64);
    if (!t) return std::nullopt;
    for (uint32_t i = 0; i < *t; ++i) {
        AlignmentContainer::Timer timer;
        timer.key = r.read_uint32();
        timer.set_time_stamp = r.read_uint64();
        timer.alignment_id_a = r.read_uint64();
        timer.alignment_id_b = r.read_uint64();
        c.timers.push_back(timer);
    }
    if (overran(r)) return std::nullopt;
    return Value(std::move(c));
}

bool write_alignment(BunchWriter& out, const AlignmentContainer& c) {
    if (!write_count(out, c.instances.size())) return false;
    for (const auto& inst : c.instances) {
        out.write_uint64(inst.key);
        if (!write_count(out, inst.guids.size())) return false;
        for (const GuidValue& g : inst.guids) write_guid(out, g);
    }
    if (!write_count(out, c.timers.size())) return false;
    for (const auto& t : c.timers) {
        out.write_uint32(t.key);
        out.write_uint64(t.set_time_stamp);
        out.write_uint64(t.alignment_id_a);
        out.write_uint64(t.alignment_id_b);
    }
    return true;
}

bool alignment_fits(const AlignmentContainer& c) {
    if (!unique_keys(c.instances, [](const auto& i) { return i.key; })) return false;
    if (!unique_keys(c.timers, [](const auto& t) { return t.key; })) return false;
    for (const auto& inst : c.instances) {
        if (!unique_keys(inst.guids, [](const GuidValue& g) { return guid_key(g); })) return false;
    }
    return true;
}

// FCharacterCustomizationSaveData (0x1463d6290).
// == C-Y-B | C O A ==

/// The wire width of each CustomizationField, in wire order (the reflection types: floats and int32 are
/// 32 bits, ids and colors int64).
constexpr uint8_t kCustomizationBits[kCustomizationFieldCount] = {
    32, 32, 32, 32, 32,               // SkinColorHue .. SkinSet
    64, 64, 64, 64, 64, 64, 64,       // EyeColor .. HeadHairTipColor
    32, 32, 32,                       // HeadHairLength, Contrast, Gradient
    64, 64, 64,                       // FacialHairLip, Chin, Cheek
    32, 32, 32, 32,                   // FacialHairLipLength .. EyelashLength
    64, 64,                           // FacialHairRootColor, TipColor
    32, 32,                           // FacialHairContrast, Gradient
    64, 32, 64, 32,                   // RacialHorns, RacialHornsLength, NailColor, NailOpacity
};

/// The full FCharacterCustomizationDecalData (0x1463da010): 53 bytes.
CustomizationDecal read_decal(PacketReader& r) {
    CustomizationDecal d;
    d.guid = r.read_uint64();
    d.strength = r.read_uint32();
    d.scale_u = r.read_uint32();
    d.scale_v = r.read_uint32();
    d.slot_index = r.read_uint8();
    d.merge_section_material_base = r.read_uint64();
    d.offset_u = r.read_uint32();
    d.offset_v = r.read_uint32();
    d.angle = r.read_uint32();
    d.color = r.read_uint64();
    d.discoloration = r.read_uint32();
    return d;
}

void write_decal(BunchWriter& out, const CustomizationDecal& d) {
    out.write_uint64(d.guid);
    out.write_uint32(d.strength);
    out.write_uint32(d.scale_u);
    out.write_uint32(d.scale_v);
    out.write_uint8(d.slot_index);
    out.write_uint64(d.merge_section_material_base);
    out.write_uint32(d.offset_u);
    out.write_uint32(d.offset_v);
    out.write_uint32(d.angle);
    out.write_uint64(d.color);
    out.write_uint32(d.discoloration);
}

std::optional<Value> read_customization(PacketReader& r, const CodecOptions& opts) {
    CharacterCustomization c;
    if (r.read_bit()) c.preset_guid = r.read_uint64();
    if (r.read_bit()) c.random_seed = r.read_uint32();
    const bool preset_known = opts.customization_preset_known && opts.customization_preset_known(c.preset_guid.value_or(0));
    if (!preset_known) {
        CharacterCustomization::RaceGender rg;
        rg.race = r.read_uint8();       // 0x1463d638a reads +0xf3 (Race) before +0xf2 (Gender)
        rg.gender = r.read_uint8();
        c.race_gender = rg;
    }
    for (size_t i = 0; i < kCustomizationFieldCount; ++i) {
        if (r.read_bit()) c.deltas[static_cast<CustomizationField>(i)] = r.read_bits(kCustomizationBits[i]);
    }
    if (overran(r)) return std::nullopt;
    if (r.read_bit()) {                                     // DecalData
        const auto n = read_count(r, 1);
        if (!n) return std::nullopt;
        std::vector<std::optional<CustomizationDecal>> v;
        for (uint32_t i = 0; i < *n; ++i) {
            if (r.read_bit()) v.push_back(read_decal(r));
            else v.push_back(std::nullopt);
        }
        c.decals = std::move(v);
    }
    if (overran(r)) return std::nullopt;
    if (r.read_bit()) {                                     // DecalBlendGroups
        const auto n = read_count(r, 1);
        if (!n) return std::nullopt;
        std::vector<std::optional<CustomizationBlendGroup>> v;
        for (uint32_t i = 0; i < *n; ++i) {
            if (r.read_bit()) {
                CustomizationBlendGroup g;
                g.decal_appearance_guid = r.read_uint64();
                g.color_guid = r.read_uint64();
                g.intensity = r.read_uint32();
                v.push_back(g);
            } else {
                v.push_back(std::nullopt);
            }
        }
        c.blend_groups = std::move(v);
    }
    if (overran(r)) return std::nullopt;
    std::optional<bool>* bools[2] = {&c.helmet_visible, &c.cape_visible};
    for (std::optional<bool>* b : bools) {                  // bIsHelmetVisible, bIsCapeVisible
        if (r.read_bit()) {
            const uint32_t v = r.read_uint32();
            if (v > 1) return std::nullopt;
            *b = v != 0;
        }
    }
    if (overran(r)) return std::nullopt;
    if (r.read_bit()) {                                     // SectionsValues
        const auto n = read_count(r, 8 + 3 * 64);
        if (!n) return std::nullopt;
        std::vector<CustomizationSection> v;
        for (uint32_t i = 0; i < *n; ++i) {
            CustomizationSection s;
            s.key = r.read_uint8();
            s.value.x = read_double(r);
            s.value.y = read_double(r);
            s.value.z = read_double(r);
            v.push_back(s);
        }
        c.sections_values = std::move(v);
    }
    if (overran(r)) return std::nullopt;
    if (r.read_bit()) {                                     // FaceMorphWeightMaps: at least a hardcoded FName (9 bits) + a float
        const auto n = read_count(r, 9 + 32);
        if (!n) return std::nullopt;
        std::vector<CustomizationMorph> v;
        for (uint32_t i = 0; i < *n; ++i) {
            auto name = read_name(r);
            if (!name) return std::nullopt;
            CustomizationMorph m;
            m.name = std::move(*name);
            m.weight = r.read_uint32();
            v.push_back(std::move(m));
        }
        c.face_morph_weights = std::move(v);
    }
    if (overran(r)) return std::nullopt;
    return Value(std::move(c));
}
// C.O.A captures, C:Y:B code

bool write_customization(BunchWriter& out, const CharacterCustomization& c) {
    out.write_bit(c.preset_guid.has_value());
    if (c.preset_guid) out.write_uint64(*c.preset_guid);
    out.write_bit(c.random_seed.has_value());
    if (c.random_seed) out.write_uint32(*c.random_seed);
    if (c.race_gender) {
        out.write_uint8(c.race_gender->race);
        out.write_uint8(c.race_gender->gender);
    }
    for (size_t i = 0; i < kCustomizationFieldCount; ++i) {
        const auto it = c.deltas.find(static_cast<CustomizationField>(i));
        out.write_bit(it != c.deltas.end());
        if (it != c.deltas.end()) out.write_bits(it->second, kCustomizationBits[i]);
    }
    out.write_bit(c.decals.has_value());
    if (c.decals) {
        if (!write_count(out, c.decals->size())) return false;
        for (const auto& d : *c.decals) {
            out.write_bit(d.has_value());
            if (d) write_decal(out, *d);
        }
    }
    out.write_bit(c.blend_groups.has_value());
    if (c.blend_groups) {
        if (!write_count(out, c.blend_groups->size())) return false;
        for (const auto& g : *c.blend_groups) {
            out.write_bit(g.has_value());
            if (g) {
                out.write_uint64(g->decal_appearance_guid);
                out.write_uint64(g->color_guid);
                out.write_uint32(g->intensity);
            }
        }
    }
    for (const std::optional<bool>* b : {&c.helmet_visible, &c.cape_visible}) {
        out.write_bit(b->has_value());
        if (*b) out.write_uint32(**b ? 1u : 0u);
    }
    out.write_bit(c.sections_values.has_value());
    if (c.sections_values) {
        if (!write_count(out, c.sections_values->size())) return false;
        for (const CustomizationSection& s : *c.sections_values) {
            out.write_uint8(s.key);
            write_double(out, s.value.x);
            write_double(out, s.value.y);
            write_double(out, s.value.z);
        }
    }
    out.write_bit(c.face_morph_weights.has_value());
    if (c.face_morph_weights) {
        if (!write_count(out, c.face_morph_weights->size())) return false;
        for (const CustomizationMorph& m : *c.face_morph_weights) {
            if (!write_name(out, m.name)) return false;
            out.write_uint32(m.weight);
        }
    }
    return true;
}

bool customization_fits(const CharacterCustomization& c) {
    for (const auto& [field, bits] : c.deltas) {
        const size_t i = static_cast<size_t>(field);
        if (i >= kCustomizationFieldCount) return false;
        if (kCustomizationBits[i] < 64 && (bits >> kCustomizationBits[i]) != 0) return false;
    }
    if (c.sections_values && !unique_keys(*c.sections_values, [](const CustomizationSection& s) { return s.key; })) return false;
    if (c.face_morph_weights) {
        const auto& v = *c.face_morph_weights;
        if (!std::all_of(v.begin(), v.end(), [](const CustomizationMorph& m) { return name_fits(m.name); })) return false;
        if (!unique_keys(v, [](const CustomizationMorph& m) {
                return std::make_tuple(m.name.hardcoded, m.name.index, m.name.text, m.name.number);
            }))
            return false;
    }
    return true;
}

}  // namespace

// ---- primitives -------------------------------------------------------------

bool valid_utf8(const std::string& s) {
    std::u16string u;
    return utf8_to_utf16(s, u);
}

std::optional<std::string> read_fstring(PacketReader& r) {
    const int32_t n = static_cast<int32_t>(r.read_uint32());
    if (overran(r)) return std::nullopt;
    if (n == 0) return std::string();
    if (n == INT32_MIN) return std::nullopt;
    const uint32_t len = static_cast<uint32_t>(n < 0 ? -n : n);   // with the terminating NUL
    if (len > kMaxFStringChars + 1) return std::nullopt;            // a bound on hostile lengths, far above any replicated string
    if (n > 0) {
        std::string s;
        for (uint32_t i = 0; i < len; ++i) {
            const uint8_t c = r.read_uint8();
            if (overran(r)) return std::nullopt;
            if (i + 1 == len) {
                if (c != 0) return std::nullopt;             // not NUL-terminated
            } else {
                if (c == 0 || c >= 0x80) return std::nullopt;  // an embedded NUL or a code page byte: not carried
                s.push_back(static_cast<char>(c));
            }
        }
        return s;
    }
    std::u16string u;
    for (uint32_t i = 0; i < len; ++i) {
        const uint16_t c = static_cast<uint16_t>(r.read_bits(16));
        if (overran(r)) return std::nullopt;
        if (i + 1 == len) {
            if (c != 0) return std::nullopt;
        } else {
            if (c == 0) return std::nullopt;
            u.push_back(static_cast<char16_t>(c));
        }
    }
    std::string s;
    if (!utf16_to_utf8(u, s)) return std::nullopt;
    return s;
}
// -- C.O.A . Cyb --

bool fstring_fits(const std::string& s) {
    if (s.empty()) return true;
    if (s.find('\0') != std::string::npos) return false;
    const bool ascii = std::all_of(s.begin(), s.end(), [](char c) { return static_cast<uint8_t>(c) < 0x80; });
    if (ascii) return s.size() <= kMaxFStringChars;
    std::u16string u;
    return utf8_to_utf16(s, u) && u.size() <= kMaxFStringChars;
}

bool name_fits(const NameValue& n) { return n.hardcoded || fstring_fits(n.text); }

bool text_fits(const TextValue& t) {
    if (t.history == 0xFF) return !t.has_string || fstring_fits(t.source);
    if (t.history == 0) return fstring_fits(t.ns) && fstring_fits(t.key) && fstring_fits(t.source);
    return false;
}

bool write_fstring(BunchWriter& out, const std::string& utf8) {
    if (!fstring_fits(utf8)) return false;
    if (utf8.empty()) {
        out.write_int32(0);
        return true;
    }
    const bool ascii = std::all_of(utf8.begin(), utf8.end(), [](char c) { return static_cast<uint8_t>(c) < 0x80; });
    if (ascii) {
        out.write_int32(static_cast<int32_t>(utf8.size()) + 1);
        out.write_bytes(reinterpret_cast<const uint8_t*>(utf8.data()), utf8.size());
        out.write_uint8(0);
        return true;
    }
    std::u16string u;
    if (!utf8_to_utf16(utf8, u)) return false;
    out.write_int32(-(static_cast<int32_t>(u.size()) + 1));
    for (char16_t c : u) out.write_uint16(c);
    out.write_uint16(0);
    return true;
}

std::optional<NameValue> read_name(PacketReader& r) {
    NameValue n;
    if (r.read_bit()) {
        const auto idx = r.read_sip();
        if (!idx || *idx > UINT32_MAX) return std::nullopt;
        n.hardcoded = true;
        n.index = static_cast<uint32_t>(*idx);
    } else {
        auto s = read_fstring(r);
        if (!s) return std::nullopt;
        n.text = std::move(*s);
        n.number = static_cast<int32_t>(r.read_uint32());
    }
    if (overran(r)) return std::nullopt;
    return n;
}

bool write_name(BunchWriter& out, const NameValue& n) {
    out.write_bit(n.hardcoded);
    if (n.hardcoded) {
        out.write_sip(n.index);
        return true;
    }
    if (!write_fstring(out, n.text)) return false;
    out.write_int32(n.number);
    return true;
}
// [C-Y-B - CoA]

std::optional<TextValue> read_text(PacketReader& r) {
    TextValue t;
    t.flags = r.read_uint32();
    t.history = r.read_uint8();
    if (t.history == 0xFF) {
        t.has_string = r.read_uint32() != 0;
        if (t.has_string) {
            auto s = read_fstring(r);
            if (!s) return std::nullopt;
            t.source = std::move(*s);
        }
    } else if (t.history == 0) {
        auto ns = read_fstring(r);
        auto key = read_fstring(r);
        auto src = read_fstring(r);
        if (!ns || !key || !src) return std::nullopt;
        t.ns = std::move(*ns);
        t.key = std::move(*key);
        t.source = std::move(*src);
    } else {
        return std::nullopt;   // other histories are not decoded
    }
    if (overran(r)) return std::nullopt;
    return t;
}

bool write_text(BunchWriter& out, const TextValue& t) {
    if (t.history != 0xFF && t.history != 0) return false;
    out.write_uint32(t.flags);
    out.write_uint8(t.history);
    if (t.history == 0xFF) {
        out.write_uint32(t.has_string ? 1u : 0u);
        return !t.has_string || write_fstring(out, t.source);
    }
    return write_fstring(out, t.ns) && write_fstring(out, t.key) && write_fstring(out, t.source);
}

std::optional<emit::FIntrepidNetworkGUID> read_object(PacketReader& r) {
    emit::FIntrepidNetworkGUID g;
    const uint32_t lo = r.read_uint32();
    const uint32_t hi = r.read_uint32();
    g.ObjectId = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
    g.ServerId = r.read_uint32();
    g.Randomizer = r.read_uint32();
    if (overran(r) || g.ObjectId == 1) return std::nullopt;
    return g;
}

bool write_object(BunchWriter& out, const emit::FIntrepidNetworkGUID& g) {
    if (g.ObjectId == 1) return false;
    emit::write_intrepid_guid(out, g);
    return true;
}

namespace {
constexpr uint8_t kExportHasPath = 1, kExportNoLoad = 2, kExportHasChecksum = 4;
}

std::optional<Value> read_object_value(PacketReader& r, bool allow_inline) {
    emit::FIntrepidNetworkGUID g;
    const uint32_t lo = r.read_uint32();
    const uint32_t hi = r.read_uint32();
    g.ObjectId = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
    g.ServerId = r.read_uint32();
    g.Randomizer = r.read_uint32();
    if (overran(r)) return std::nullopt;
    if (g.ObjectId != 1) return Value(g);
    if (!allow_inline) return std::nullopt;
    InlineObject o;
    o.flags = r.read_uint8();
    if (overran(r) || (o.flags & ~(kExportHasPath | kExportNoLoad | kExportHasChecksum))) return std::nullopt;
    if ((o.flags & kExportHasChecksum) && !(o.flags & kExportHasPath)) return std::nullopt;   // UE reads the checksum with the path only
    if (o.flags & kExportHasPath) {
        auto outer = read_object(r);   // the outer is a plain reference: a nested inline export is refused
        auto path = read_fstring(r);
        if (!outer || !path || path->empty()) return std::nullopt;
        o.outer = *outer;
        o.path = std::move(*path);
        if (o.flags & kExportHasChecksum) o.checksum = r.read_uint32();
    }
    if (overran(r)) return std::nullopt;
    return Value(std::move(o));
}

bool inline_object_fits(const InlineObject& o) {
    if (o.flags & ~(kExportHasPath | kExportNoLoad | kExportHasChecksum)) return false;
    if (o.flags & kExportHasPath) {
        if (o.path.empty() || !fstring_fits(o.path) || o.outer.ObjectId == 1) return false;
        return (o.flags & kExportHasChecksum) || o.checksum == 0;
    }
    // no path: no outer, no name, and no checksum (UE reads the checksum with the path only)
    return !(o.flags & kExportHasChecksum) && o.path.empty() && o.checksum == 0 && o.outer == emit::FIntrepidNetworkGUID{};
}
// ~ Cyb - C.O.A ~

bool write_inline_object(BunchWriter& out, const InlineObject& o) {
    if (!inline_object_fits(o)) return false;
    emit::FIntrepidNetworkGUID root;
    root.ObjectId = 1;
    emit::write_intrepid_guid(out, root);
    out.write_uint8(o.flags);
    if (o.flags & kExportHasPath) {
        emit::write_intrepid_guid(out, o.outer);
        if (!write_fstring(out, o.path)) return false;
        if (o.flags & kExportHasChecksum) out.write_uint32(o.checksum);
    }
    return true;
}

std::optional<SoftObjectValue> read_soft_object(PacketReader& r) {
    SoftObjectValue s;
    if (r.read_bit()) {
        auto p = read_fstring(r);
        if (!p) return std::nullopt;
        s.is_path = true;
        s.path = std::move(*p);
    } else {
        auto o = read_object(r);
        if (!o) return std::nullopt;
        s.object = *o;
    }
    if (overran(r)) return std::nullopt;
    return s;
}

bool write_soft_object(BunchWriter& out, const SoftObjectValue& s) {
    out.write_bit(s.is_path);
    return s.is_path ? write_fstring(out, s.path) : write_object(out, s.object);
}

// ---- quantized vectors ------------------------------------------------------

std::optional<QuantVec> read_quantized_vector(PacketReader& r) {
    QuantVec q;
    const uint32_t info = r.read_serialize_int(1u << 7);
    const uint32_t bits = info & 63, extra = info >> 6;
    if (bits) {
        int64_t* comps[3] = {&q.x, &q.y, &q.z};
        for (int64_t* c : comps) {
            const uint64_t v = r.read_bits(static_cast<int>(bits));
            const uint64_t sign = 1ull << (bits - 1);
            *c = static_cast<int64_t>((v ^ sign) - sign);
        }
        q.mode = extra ? QuantVec::Mode::Scaled : QuantVec::Mode::Unscaled;
    } else if (extra) {
        q.mode = QuantVec::Mode::RawDouble;
        q.x = static_cast<int64_t>(r.read_uint64());
        q.y = static_cast<int64_t>(r.read_uint64());
        q.z = static_cast<int64_t>(r.read_uint64());
    } else {
        q.mode = QuantVec::Mode::RawFloat;
        q.x = r.read_uint32();
        q.y = r.read_uint32();
        q.z = r.read_uint32();
    }
    if (overran(r)) return std::nullopt;
    return q;
}

bool quant_vec_fits(const QuantVec& q) {
    switch (q.mode) {
        case QuantVec::Mode::Scaled:
        case QuantVec::Mode::Unscaled:
            return std::max(bits_needed(q.x), std::max(bits_needed(q.y), bits_needed(q.z))) <= 63;
        case QuantVec::Mode::RawDouble:
            return true;
        case QuantVec::Mode::RawFloat:
            return q.x >= 0 && q.y >= 0 && q.z >= 0 && q.x <= UINT32_MAX && q.y <= UINT32_MAX && q.z <= UINT32_MAX;
    }
    return false;
}

bool write_quantized_vector(BunchWriter& out, const QuantVec& q) {
    if (!quant_vec_fits(q)) return false;
    switch (q.mode) {
        case QuantVec::Mode::Scaled:
        case QuantVec::Mode::Unscaled: {
            const uint32_t bits = std::max(bits_needed(q.x), std::max(bits_needed(q.y), bits_needed(q.z)));
            out.write_serialize_int((q.mode == QuantVec::Mode::Scaled ? (1u << 6) : 0u) | bits, 1u << 7);
            const uint64_t mask = (1ull << bits) - 1;
            out.write_bits(static_cast<uint64_t>(q.x) & mask, static_cast<int>(bits));
            out.write_bits(static_cast<uint64_t>(q.y) & mask, static_cast<int>(bits));
            out.write_bits(static_cast<uint64_t>(q.z) & mask, static_cast<int>(bits));
            return true;
        }
        case QuantVec::Mode::RawDouble:
            out.write_serialize_int(1u << 6, 1u << 7);   // 0 bits, the 8-byte scalar flag
            out.write_uint64(static_cast<uint64_t>(q.x));
            out.write_uint64(static_cast<uint64_t>(q.y));
            out.write_uint64(static_cast<uint64_t>(q.z));
            return true;
        case QuantVec::Mode::RawFloat:
            out.write_serialize_int(0, 1u << 7);
            out.write_uint32(static_cast<uint32_t>(q.x));
            out.write_uint32(static_cast<uint32_t>(q.y));
            out.write_uint32(static_cast<uint32_t>(q.z));
            return true;
    }
    return false;
}

QuantVec quantize_vector(int32_t scale, double x, double y, double z) {
    // UE::Net::Private::WriteQuantizedVector (double): a non-finite vector goes out as zero;
    // beyond 2^62 after scaling the value goes out at full precision.
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) x = y = z = 0;
    constexpr double kMaxValueToScale = 4503599627370496.0;         // 2^52
    constexpr double kMaxScaledValue = 4611686018427387904.0;       // 2^62
    const double f = scale;
    const double sx = x * f, sy = y * f, sz = z * f;
    QuantVec q;
    if (std::max({std::fabs(sx), std::fabs(sy), std::fabs(sz)}) < kMaxScaledValue) {
        const bool scaled = std::min({std::fabs(x), std::fabs(y), std::fabs(z)}) < kMaxValueToScale;
        auto round = [](double d) { return static_cast<int64_t>(d + (d >= 0 ? 0.5 : -0.5)); };
        q.mode = scaled ? QuantVec::Mode::Scaled : QuantVec::Mode::Unscaled;
        q.x = round(scaled ? sx : x);
        q.y = round(scaled ? sy : y);
        q.z = round(scaled ? sz : z);
    } else {
        q.mode = QuantVec::Mode::RawDouble;
        q.x = static_cast<int64_t>(std::bit_cast<uint64_t>(x));
        q.y = static_cast<int64_t>(std::bit_cast<uint64_t>(y));
        q.z = static_cast<int64_t>(std::bit_cast<uint64_t>(z));
    }
    return q;
}

QuantVec quantize_location(const CodecOptions& o, double x, double y, double z) {
    return quantize_vector(o.rep_movement_location_scale, x, y, z);
}

QuantVec quantize_velocity(const CodecOptions& o, double x, double y, double z) {
    return quantize_vector(o.rep_movement_velocity_scale, x, y, z);
}

Vec3d dequantize_location(const CodecOptions& o, const QuantVec& v) { return dequantize_vector(v, o.rep_movement_location_scale); }

Vec3d dequantize_velocity(const CodecOptions& o, const QuantVec& v) { return dequantize_vector(v, o.rep_movement_velocity_scale); }

Vec3d dequantize_vector(const QuantVec& q, int32_t scale) {
    switch (q.mode) {
        case QuantVec::Mode::Scaled:
            return {static_cast<double>(q.x) / scale, static_cast<double>(q.y) / scale, static_cast<double>(q.z) / scale};
        case QuantVec::Mode::Unscaled:
            return {static_cast<double>(q.x), static_cast<double>(q.y), static_cast<double>(q.z)};
        case QuantVec::Mode::RawFloat:
            return {std::bit_cast<float>(static_cast<uint32_t>(q.x)), std::bit_cast<float>(static_cast<uint32_t>(q.y)),
                    std::bit_cast<float>(static_cast<uint32_t>(q.z))};
        case QuantVec::Mode::RawDouble:
            return {std::bit_cast<double>(static_cast<uint64_t>(q.x)), std::bit_cast<double>(static_cast<uint64_t>(q.y)),
                    std::bit_cast<double>(static_cast<uint64_t>(q.z))};
    }
    return {};
}

// ---- FQuat ------------------------------------------------------------------
// CoA.C Y B

Quat quat_to_wire(double x, double y, double z, double w) {
    // 0x141438440's saving branch.  The first test sums (x^2 + y^2) + (z^2 + w^2); the unit test and the
    // scale sum (x^2 + z^2) + (y^2 + w^2), the order its SIMD adds them in.
    constexpr double kSmallNumber = static_cast<double>(1.e-8f);       // UE_SMALL_NUMBER
    constexpr double kThreshNormalized = static_cast<double>(0.01f);   // UE_THRESH_QUAT_NORMALIZED
    if ((x * x + y * y) + (z * z + w * w) <= kSmallNumber) return Quat{};   // identity: XYZ 0, W 1
    const double sum = (x * x + z * z) + (y * y + w * w);
    if (kThreshNormalized < std::fabs(1.0 - sum)) {
        const double scale = 1.0 / std::sqrt(sum);
        x *= scale;
        y *= scale;
        z *= scale;
        w *= scale;
    }
    if (w < 0) {
        x = -x;
        y = -y;
        z = -z;
    }
    return Quat{x, y, z};
}

std::array<double, 4> quat_from_wire(const Quat& q) {
    double x = q.x, y = q.y, z = q.z, w;
    const double xyz2 = x * x + y * y + z * z;
    const double w2 = 1.0 - xyz2;
    if (w2 >= 0) {
        w = std::sqrt(w2);
    } else {
        w = 0;
        const double inv = 1.0 / std::sqrt(xyz2);
        x *= inv;
        y *= inv;
        z *= inv;
    }
    return {x, y, z, w};
}

// ---- NetSerialize structs ---------------------------------------------------

bool is_known_struct(std::string_view struct_name) { return find_struct(struct_name) != S::Unknown; }

bool struct_accepts(std::string_view struct_name, const Value& v) {
    switch (find_struct(struct_name)) {
        case S::Vec3:        return v.get_if<Vec3d>() != nullptr;
        case S::Vec2:        return v.get_if<Vec2d>() != nullptr;
        case S::Ticks:       return v.get_if<Ticks>() != nullptr;
        case S::Rotator:     return v.get_if<Rot>() != nullptr;
        case S::Quantized: {
            const auto* q = v.get_if<QuantVec>();
            return q && quant_vec_fits(*q);
        }
        case S::Normal:      return v.get_if<Normal16>() != nullptr;
        case S::RepMovement: {
            const auto* m = v.get_if<RepMovement>();
            return m && rep_movement_fits(*m);
        }
        case S::LoadTracker: {
            const auto* t = v.get_if<LoadTracker>();
            return t && t->flags.size() <= emit::kCharacterLoadTrackerMaxFlags;
        }
        case S::UniqueId: {
            const auto* id = v.get_if<UniqueNetId>();
            return id && unique_net_id_fits(*id);
        }
        case S::Tag: {
            const auto* t = v.get_if<GameplayTag>();
            return t && name_fits(t->name);
        }
        case S::TagContainer: {
            const auto* c = v.get_if<GameplayTagContainer>();
            if (!c || c->tags.size() > 63) return false;
            return std::all_of(c->tags.begin(), c->tags.end(), [](const NameValue& n) { return name_fits(n); });
        }
        case S::StatInt32: {
            const auto* t = v.get_if<StatInt32Inst>();
            return t && (t->extended || (t->base == 0 && t->equipment == 0));
        }
        case S::Quat:         return v.get_if<Quat>() != nullptr;
        case S::GuidToBool: {
            const auto* w = v.get_if<GuidToBoolWrapper>();
            return w && guid_to_bool_fits(*w);
        }
        case S::Alignment: {
            const auto* c = v.get_if<AlignmentContainer>();
            return c && alignment_fits(*c);
        }
        case S::AppearanceId: return v.get_if<AppearanceId>() != nullptr;
        case S::Customization: {
            const auto* c = v.get_if<CharacterCustomization>();
            return c && customization_fits(*c);
        }
        case S::ItemInstance: {
            const auto* i = v.get_if<ItemInstance>();
            return i && item_instance_fits(*i);
        }
        case S::ItemStorageEvent: {
            const auto* e = v.get_if<ItemStorageEvent>();
            return e && item_storage_event_fits(*e);
        }
        case S::HitConfirmations: {
            const auto* c = v.get_if<HitConfirmations>();
            return c && hit_confirmations_fit(*c);
        }
        case S::MovementInput: {
            const auto* m = v.get_if<PlayerMovementInput>();
            return m && m->bits <= 15;
        }
        case S::VendorPurchase: {
            const auto* c = v.get_if<VendorPurchaseCart>();
            return c && vendor_purchase_cart_fits(*c);
        }
        case S::VendorTransaction: {
            const auto* c = v.get_if<VendorTransactionCart>();
            return c && vendor_transaction_cart_fits(*c);
        }
        case S::NodeTax: {
            const auto* d = v.get_if<NodeTaxDetails>();
            return d && node_tax_details_fit(*d);
        }
        case S::LocalizedMessage: {
            const auto* m = v.get_if<LocalizedMessage>();
            return m && localized_message_fits(*m);
        }
        case S::ChannelingData: {
            const auto* d = v.get_if<ChannelingData>();
            return d && channeling_data_fits(*d);
        }
        case S::Unknown:     break;
    }
    return false;
}

std::optional<Value> zero_struct(std::string_view struct_name) {
    switch (find_struct(struct_name)) {
        case S::Vec3:         return Value(Vec3d{});
        case S::Vec2:         return Value(Vec2d{});
        case S::Ticks:        return Value(Ticks{});
        case S::Rotator:      return Value(Rot{});
        case S::Quantized:    return Value(QuantVec{});   // 0, 0, 0 at any scale
        case S::Tag:          return Value(GameplayTag{NameValue::hardcoded_name(0)});   // NAME_None
        case S::TagContainer: return Value(GameplayTagContainer{});
        case S::MovementInput: return Value(PlayerMovementInput{});   // no direction held
        case S::LocalizedMessage: return Value(LocalizedMessage{});     // no table, no key
        default:              break;
    }
    return std::nullopt;
}
// C Y B and CoA

std::optional<Value> read_struct(std::string_view struct_name, PacketReader& r, const CodecOptions& opts) {
    switch (find_struct(struct_name)) {
        case S::Vec3: {
            Vec3d v;
            v.x = read_double(r); v.y = read_double(r); v.z = read_double(r);
            if (overran(r)) return std::nullopt;
            return Value(v);
        }
        case S::Vec2: {
            Vec2d v;
            v.x = read_double(r); v.y = read_double(r);
            if (overran(r)) return std::nullopt;
            return Value(v);
        }
        case S::Ticks: {
            Ticks t;
            t.ticks = static_cast<int64_t>(r.read_uint64());
            if (overran(r)) return std::nullopt;
            return Value(t);
        }
        case S::Rotator: {
            auto rot = read_rotator(r, 16);
            if (!rot) return std::nullopt;
            return Value(*rot);
        }
        case S::Quantized: {
            auto q = read_quantized_vector(r);
            if (!q) return std::nullopt;
            return Value(*q);
        }
        case S::Normal: {
            Normal16 n;
            n.x = static_cast<uint16_t>(r.read_bits(16));
            n.y = static_cast<uint16_t>(r.read_bits(16));
            n.z = static_cast<uint16_t>(r.read_bits(16));
            if (overran(r)) return std::nullopt;
            return Value(n);
        }
        case S::RepMovement:
            return read_rep_movement(r, opts);
        case S::LoadTracker: {
            const uint32_t n = static_cast<uint32_t>(r.read_bits(6));
            if (n > emit::kCharacterLoadTrackerMaxFlags) return std::nullopt;
            LoadTracker t;
            for (uint32_t i = 0; i < n; ++i) {
                const uint32_t f = r.read_uint32();
                if (f > 1) return std::nullopt;   // the writer's 0 or 1
                t.flags.push_back(f != 0);
            }
            if (overran(r)) return std::nullopt;
            return Value(std::move(t));
        }
        case S::UniqueId:
            return read_unique_net_id(r);
        case S::Tag: {
            auto n = read_name(r);
            if (!n) return std::nullopt;
            return Value(GameplayTag{std::move(*n)});
        }
        case S::TagContainer: {
            GameplayTagContainer c;
            if (!r.read_bit()) {
                const uint32_t n = static_cast<uint32_t>(r.read_bits(6));
                for (uint32_t i = 0; i < n; ++i) {
                    auto name = read_name(r);
                    if (!name) return std::nullopt;
                    c.tags.push_back(std::move(*name));
                }
            }
            if (overran(r)) return std::nullopt;
            return Value(std::move(c));
        }
        case S::StatInt32: {
            StatInt32Inst t;
            t.record = static_cast<int64_t>(r.read_uint64());
            t.value = r.read_uint32();
            if (opts.stat_int32_extended && opts.stat_int32_extended(t.record)) {
                t.extended = true;
                t.base = r.read_uint32();
                t.equipment = r.read_uint32();
            }
            if (overran(r)) return std::nullopt;
            return Value(t);
        }
        case S::Quat: {
            Quat q;
            q.x = read_double(r);
            q.y = read_double(r);
            q.z = read_double(r);
            if (overran(r)) return std::nullopt;
            return Value(q);
        }
        case S::GuidToBool:
            return read_guid_to_bool(r);
        case S::Alignment:
            return read_alignment(r);
        case S::AppearanceId: {
            AppearanceId a;
            a.type_id = r.read_uint64();        // 0x145919170 reads +8 (TypeId) before +0 (RecordGuid)
            a.record_guid = r.read_uint64();
            a.enchantment_level = r.read_uint8();
            a.item_equip_slot = r.read_uint8();
            if (overran(r)) return std::nullopt;
            return Value(a);
        }
        case S::Customization:
            return read_customization(r, opts);
        case S::ItemInstance: {
            auto item = read_item_instance(r);
            if (!item) return std::nullopt;
            return Value(std::move(*item));
        }
        case S::ItemStorageEvent: {
            auto event = read_item_storage_event(r);
            if (!event) return std::nullopt;
            return Value(std::move(*event));
        }
        case S::HitConfirmations: {
            auto hits = read_hit_confirmations(r);
            if (!hits) return std::nullopt;
            return Value(std::move(*hits));
        }
        case S::MovementInput: {
            PlayerMovementInput m;
            m.bits = static_cast<uint8_t>(r.read_bits(4));
            if (overran(r)) return std::nullopt;
            return Value(m);
        }
        case S::VendorPurchase: {
            auto c = read_vendor_purchase_cart(r);
            if (!c) return std::nullopt;
            return Value(std::move(*c));
        }
        case S::VendorTransaction: {
            auto c = read_vendor_transaction_cart(r);
            if (!c) return std::nullopt;
            return Value(std::move(*c));
        }
        case S::NodeTax: {
            auto d = read_node_tax_details(r);
            if (!d) return std::nullopt;
            return Value(std::move(*d));
        }
        case S::LocalizedMessage: {
            auto m = read_localized_message(r);
            if (!m) return std::nullopt;
            return Value(std::move(*m));
        }
        case S::ChannelingData: {
            auto d = read_channeling_data(r);
            if (!d) return std::nullopt;
            return Value(std::move(*d));
        }
        case S::Unknown:
            break;
    }
    return std::nullopt;
}
// C-O-A : C:Y:B

bool write_struct(std::string_view struct_name, const Value& v, BunchWriter& out, const CodecOptions& opts) {
    if (!struct_accepts(struct_name, v)) return false;
    switch (find_struct(struct_name)) {
        case S::Vec3: {
            const auto& a = *v.get_if<Vec3d>();
            write_double(out, a.x); write_double(out, a.y); write_double(out, a.z);
            return true;
        }
        case S::Vec2: {
            const auto& a = *v.get_if<Vec2d>();
            write_double(out, a.x); write_double(out, a.y);
            return true;
        }
        case S::Ticks:
            out.write_uint64(static_cast<uint64_t>(v.get_if<Ticks>()->ticks));
            return true;
        case S::Rotator:
            return write_rotator(out, *v.get_if<Rot>(), 16);
        case S::Quantized:
            return write_quantized_vector(out, *v.get_if<QuantVec>());
        case S::Normal: {
            const auto& n = *v.get_if<Normal16>();
            out.write_uint16(n.x); out.write_uint16(n.y); out.write_uint16(n.z);
            return true;
        }
        case S::RepMovement:
            return write_rep_movement(out, *v.get_if<RepMovement>(), opts);
        case S::LoadTracker:
            emit::write_character_load_tracker_value(out, v.get_if<LoadTracker>()->flags);
            return true;
        case S::UniqueId:
            return write_unique_net_id(out, *v.get_if<UniqueNetId>());
        case S::Tag:
            return write_name(out, v.get_if<GameplayTag>()->name);
        case S::TagContainer: {
            const auto& c = *v.get_if<GameplayTagContainer>();
            out.write_bit(c.tags.empty());
            if (c.tags.empty()) return true;
            out.write_bits(c.tags.size(), 6);
            for (const NameValue& n : c.tags) {
                if (!write_name(out, n)) return false;
            }
            return true;
        }
        case S::StatInt32: {
            const auto& t = *v.get_if<StatInt32Inst>();
            out.write_uint64(static_cast<uint64_t>(t.record));
            out.write_uint32(t.value);
            if (t.extended) {
                out.write_uint32(t.base);
                out.write_uint32(t.equipment);
            }
            return true;
        }
        case S::Quat: {
            const auto& q = *v.get_if<Quat>();
            write_double(out, q.x);
            write_double(out, q.y);
            write_double(out, q.z);
            return true;
        }
        case S::GuidToBool:
            return write_guid_to_bool(out, *v.get_if<GuidToBoolWrapper>());
        case S::Alignment:
            return write_alignment(out, *v.get_if<AlignmentContainer>());
        case S::AppearanceId: {
            const auto& a = *v.get_if<AppearanceId>();
            out.write_uint64(a.type_id);
            out.write_uint64(a.record_guid);
            out.write_uint8(a.enchantment_level);
            out.write_uint8(a.item_equip_slot);
            return true;
        }
        case S::Customization:
            return write_customization(out, *v.get_if<CharacterCustomization>());
        case S::ItemInstance:
            return write_item_instance(out, *v.get_if<ItemInstance>());
        case S::ItemStorageEvent:
            return write_item_storage_event(out, *v.get_if<ItemStorageEvent>());
        case S::HitConfirmations:
            return write_hit_confirmations(out, *v.get_if<HitConfirmations>());
        case S::MovementInput:
            out.write_bits(v.get_if<PlayerMovementInput>()->bits, 4);
            return true;
        case S::VendorPurchase:
            return write_vendor_purchase_cart(out, *v.get_if<VendorPurchaseCart>());
        case S::VendorTransaction:
            return write_vendor_transaction_cart(out, *v.get_if<VendorTransactionCart>());
        case S::NodeTax:
            return write_node_tax_details(out, *v.get_if<NodeTaxDetails>());
        case S::LocalizedMessage:
            return write_localized_message(out, *v.get_if<LocalizedMessage>());
        case S::ChannelingData:
            return write_channeling_data(out, *v.get_if<ChannelingData>());
        case S::Unknown:
            break;
    }
    return false;
}

}}}  // namespace aoc::protocol::repl
