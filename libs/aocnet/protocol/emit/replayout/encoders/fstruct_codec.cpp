// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/replayout/encoders/fstruct_codec.cpp
// ============================================================================
#include "protocol/emit/replayout/encoders/fstruct_codec.h"
#include "protocol/emit/replayout/encoder.h"       // encode_property
#include "protocol/emit/replayout/decoder.h"       // decode_property
#include "protocol/emit/replayout/encoders/scalar_codec.h"  // for double helper

#include "spdlog/spdlog.h"

#include <cstring>
#include <functional>
#include <unordered_map>

namespace aoc { namespace protocol { namespace emit { namespace replayout {

namespace {

using AtomicDecoder = PropertyValue (*)(::aoc::protocol::wire::PacketReader&);
using AtomicEncoder = bool           (*)(const PropertyValue&, BunchWriter&);

struct AtomicCodec {
    AtomicDecoder decode;
    AtomicEncoder encode;
};

// ── Registry of atomic-struct codecs ────────────────────────────────────
// Keyed by the struct's CANONICAL name (NOT the property name).  Since
// our catalog currently stores the property name (not the struct type
// name), we match against both property names and known struct type
// aliases.  When the catalog gains a proper `struct_type_name` field,
// this becomes a clean lookup.
//
// For now we allow the property name to serve as a lookup key too —
// so "ReplicatedMovement" (property on AActor of type FRepMovement) maps
// to the FRepMovement codec.
const std::unordered_map<std::string, AtomicCodec>& codec_registry() {
    static const std::unordered_map<std::string, AtomicCodec> R = {
        // ─── FVector family — named by struct type ────────────────────
        {"Vector",                      {decode_fvector,                encode_fvector}},
        {"Vector_NetQuantize",          {decode_fvector_netquantize,    encode_fvector_netquantize}},
        {"Vector_NetQuantize10",        {decode_fvector_netquantize10,  encode_fvector_netquantize10}},
        {"Vector_NetQuantize100",       {decode_fvector_netquantize100, encode_fvector_netquantize100}},
// (C Y B) (C O A)

        // ─── FRotator ────────────────────────────────────────────────
        {"Rotator",                     {decode_frotator,               encode_frotator}},

        // ─── FRepMovement family ─────────────────────────────────────
        {"RepMovement",                 {decode_frepmovement,           encode_frepmovement}},
        {"ReplicatedMovement",          {decode_frepmovement,           encode_frepmovement}},

        // ─── FUniqueNetIdRepl ────────────────────────────────────────
        {"UniqueNetIdRepl",             {decode_funiquenetidrepl,       encode_funiquenetidrepl}},
        {"UniqueId",                    {decode_funiquenetidrepl,       encode_funiquenetidrepl}},

        // ─── Properties typed as specific variants ───────────────────
        // TargetViewRotation (APlayerController) is typed FRotator_NetQuantize
        // (verify via divergence) — default to plain FRotator for now.
        {"TargetViewRotation",          {decode_frotator,               encode_frotator}},
        // SpawnLocation (APlayerController) is typed FVector_NetQuantize10
        // per UE5 source.
        {"SpawnLocation",               {decode_fvector_netquantize10,  encode_fvector_netquantize10}},
    };
    return R;
}

} // namespace

// ─── Top-level dispatcher ───────────────────────────────────────────────

PropertyValue decode_fstruct(const ReplicatedPropertyDesc& desc,
                              ::aoc::protocol::wire::PacketReader& reader) {
    const auto& R = codec_registry();
    auto it = R.find(desc.name);
    if (it != R.end()) {
        return it->second.decode(reader);
    }

    if (!desc.sub_cmds.empty()) {
        StructValue sv;
        sv.fields.reserve(desc.sub_cmds.size());
        for (const auto& sub : desc.sub_cmds) {
            sv.fields.push_back(decode_property(sub, reader));
        }
        return PropertyValue::make_struct(std::move(sv));
    }

    spdlog::warn("[replayout/decode_fstruct] struct '{}' has no atomic codec "
                 "AND no sub_cmds — cannot determine bit length, returning "
                 "empty.  Register a codec in fstruct_codec.cpp or expand "
                 "sub_cmds in catalog.cpp.", desc.name);
    return {};
}

bool encode_fstruct(const ReplicatedPropertyDesc& desc,
                     const PropertyValue& value,
                     BunchWriter& writer) {
    const auto& R = codec_registry();
    auto it = R.find(desc.name);
    if (it != R.end()) {
        return it->second.encode(value, writer);
    }

    if (!desc.sub_cmds.empty()) {
        const StructValue* sv = std::get_if<StructValue>(&value.payload);
        if (!sv) {
            spdlog::error("[replayout/encode_fstruct] struct '{}' expected "
                          "StructValue payload", desc.name);
            return false;
        }
        if (sv->fields.size() != desc.sub_cmds.size()) {
            spdlog::error("[replayout/encode_fstruct] struct '{}' field count "
                          "mismatch ({} fields vs {} sub_cmds)",
                          desc.name, sv->fields.size(), desc.sub_cmds.size());
            return false;
        }
        for (size_t i = 0; i < desc.sub_cmds.size(); ++i) {
            if (!encode_property(desc.sub_cmds[i], sv->fields[i], writer)) {
                return false;
            }
        }
        return true;
    }

    spdlog::warn("[replayout/encode_fstruct] struct '{}' has no atomic codec "
                 "AND no sub_cmds — refusing to guess.  Register a codec.",
                 desc.name);
    return false;
}

// ─── FVector / FRotator — 3× double (UE5.1+) ──────────────────────────

namespace {

PropertyValue read_triple_double(::aoc::protocol::wire::PacketReader& r) {
    StructValue sv;
    sv.fields.reserve(3);
    for (int i = 0; i < 3; ++i) {
        if (r.overflowed()) return {};
        uint64_t bits = r.read_uint64();
        if (r.overflowed()) return {};
        double d;
        std::memcpy(&d, &bits, sizeof(d));
        PropertyValue v;
        v.type = FPropertyType::Double;
        v.payload = d;
        sv.fields.push_back(std::move(v));
    }
    return PropertyValue::make_struct(std::move(sv));
}

bool write_triple_double(const PropertyValue& value, BunchWriter& w,
                          const char* label) {
    const StructValue* sv = std::get_if<StructValue>(&value.payload);
    if (!sv || sv->fields.size() != 3) {
        spdlog::error("[replayout/{}] expected 3-field StructValue", label);
        return false;
    }
    for (const auto& f : sv->fields) {
        const double* d = std::get_if<double>(&f.payload);
        if (!d) {
            spdlog::error("[replayout/{}] field not a double", label);
            return false;
        }
        uint64_t bits;
        std::memcpy(&bits, d, sizeof(bits));
        w.write_uint64(bits);
    }
    return true;
}

} // namespace

PropertyValue decode_fvector(::aoc::protocol::wire::PacketReader& r) {
    return read_triple_double(r);
}
bool encode_fvector(const PropertyValue& v, BunchWriter& w) {
    return write_triple_double(v, w, "encode_fvector");
}
// C | O | A with C Y B

PropertyValue decode_frotator(::aoc::protocol::wire::PacketReader& r) {
    return read_triple_double(r);
}
bool encode_frotator(const PropertyValue& v, BunchWriter& w) {
    return write_triple_double(v, w, "encode_frotator");
}

// ─── FVector_NetQuantize* — stubs ────────────────────────────────────
//
// These use UE5's WritePackedVector<Precision, MaxBits>:
//   bit 0          : "all zero?" flag
//   bits 1..2      : component byte-count (SerializeInt(max MaxBits+1))
//   remaining      : per-component packed ints, with sign bit + magnitude
//
// Fleshing this out requires matching UE5's integer packing exactly.
// We'll do it when we see divergence on SpawnLocation or similar.
// For now, stub returns false so the RawBits fallback kicks in when the
// caller handles it — but the caller MUST supply explicit bit length
// from another mechanism.

PropertyValue decode_fvector_netquantize(::aoc::protocol::wire::PacketReader&) {
    spdlog::warn("[replayout/decode_fvector_netquantize] not implemented yet");
    return {};
}
bool encode_fvector_netquantize(const PropertyValue&, BunchWriter&) {
    spdlog::warn("[replayout/encode_fvector_netquantize] not implemented yet");
    return false;
}

PropertyValue decode_fvector_netquantize10(::aoc::protocol::wire::PacketReader&) {
    spdlog::warn("[replayout/decode_fvector_netquantize10] not implemented yet");
    return {};
}
bool encode_fvector_netquantize10(const PropertyValue&, BunchWriter&) {
    spdlog::warn("[replayout/encode_fvector_netquantize10] not implemented yet");
    return false;
}

PropertyValue decode_fvector_netquantize100(::aoc::protocol::wire::PacketReader&) {
    spdlog::warn("[replayout/decode_fvector_netquantize100] not implemented yet");
    return {};
}
bool encode_fvector_netquantize100(const PropertyValue&, BunchWriter&) {
    spdlog::warn("[replayout/encode_fvector_netquantize100] not implemented yet");
    return false;
}

// ─── FRepMovement — stub ────────────────────────────────────────────

PropertyValue decode_frepmovement(::aoc::protocol::wire::PacketReader&) {
    spdlog::warn("[replayout/decode_frepmovement] not implemented yet — "
                 "property would need its mask bit cleared or a real codec");
    return {};
}
bool encode_frepmovement(const PropertyValue&, BunchWriter&) {
    spdlog::warn("[replayout/encode_frepmovement] not implemented yet");
    return false;
}

// ─── FUniqueNetIdRepl — stub ────────────────────────────────────────

PropertyValue decode_funiquenetidrepl(::aoc::protocol::wire::PacketReader&) {
    spdlog::warn("[replayout/decode_funiquenetidrepl] not implemented yet");
    return {};
}
bool encode_funiquenetidrepl(const PropertyValue&, BunchWriter&) {
    spdlog::warn("[replayout/encode_funiquenetidrepl] not implemented yet");
    return false;
}

}}}} // namespace aoc::protocol::emit::replayout
