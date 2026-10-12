// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/replayout/encoders/scalar_codec.cpp
// ============================================================================
#include "protocol/emit/replayout/encoders/scalar_codec.h"

#include "spdlog/spdlog.h"

#include <cstring>

namespace aoc { namespace protocol { namespace emit { namespace replayout {

// ─── Bool (1 bit) ─────────────────────────────────────────────────────

PropertyValue decode_fbool(::aoc::protocol::wire::PacketReader& r) {
    if (r.overflowed()) return {};
    int b = r.read_bit();
    if (r.overflowed()) return {};
    return PropertyValue::make_bool(b != 0);
}

bool encode_fbool(const PropertyValue& v, BunchWriter& w) {
    const bool* b = std::get_if<bool>(&v.payload);
    if (!b) {
        spdlog::error("[replayout/encode_fbool] payload is not bool");
        return false;
    }
    w.write_bit(*b ? 1 : 0);
    return true;
}

// ─── Byte (8 bits) ────────────────────────────────────────────────────

PropertyValue decode_fbyte(::aoc::protocol::wire::PacketReader& r) {
    if (r.overflowed()) return {};
    uint8_t x = r.read_uint8();
    if (r.overflowed()) return {};
    PropertyValue v;
    v.type = FPropertyType::Byte;
    v.payload = x;
    return v;
}

bool encode_fbyte(const PropertyValue& v, BunchWriter& w) {
    const uint8_t* x = std::get_if<uint8_t>(&v.payload);
    if (!x) {
        spdlog::error("[replayout/encode_fbyte] payload is not uint8");
        return false;
    }
    w.write_uint8(*x);
    return true;
}

// ─── Int (32 bits, signed) ────────────────────────────────────────────

PropertyValue decode_fint(::aoc::protocol::wire::PacketReader& r) {
    if (r.overflowed()) return {};
    int32_t x = static_cast<int32_t>(r.read_uint32());
    if (r.overflowed()) return {};
    return PropertyValue::make_int(x);
}

bool encode_fint(const PropertyValue& v, BunchWriter& w) {
    const int32_t* x = std::get_if<int32_t>(&v.payload);
    if (!x) {
        spdlog::error("[replayout/encode_fint] payload is not int32");
        return false;
    }
    w.write_int32(*x);
    return true;
}
// CoA captures, C | Y | B code

// ─── Int64 (64 bits, signed) ──────────────────────────────────────────

PropertyValue decode_fint64(::aoc::protocol::wire::PacketReader& r) {
    if (r.overflowed()) return {};
    int64_t x = static_cast<int64_t>(r.read_uint64());
    if (r.overflowed()) return {};
    PropertyValue v;
    v.type = FPropertyType::Int64;
    v.payload = x;
    return v;
}

bool encode_fint64(const PropertyValue& v, BunchWriter& w) {
    const int64_t* x = std::get_if<int64_t>(&v.payload);
    if (!x) {
        spdlog::error("[replayout/encode_fint64] payload is not int64");
        return false;
    }
    w.write_uint64(static_cast<uint64_t>(*x));
    return true;
}

// ─── Float (32 bits, IEEE 754) ────────────────────────────────────────

PropertyValue decode_ffloat(::aoc::protocol::wire::PacketReader& r) {
    if (r.overflowed()) return {};
    uint32_t bits = r.read_uint32();
    if (r.overflowed()) return {};
    float out;
    std::memcpy(&out, &bits, sizeof(out));  // strict-alias-safe punning
    PropertyValue v;
    v.type = FPropertyType::Float;
    v.payload = out;
    return v;
}

bool encode_ffloat(const PropertyValue& v, BunchWriter& w) {
    const float* x = std::get_if<float>(&v.payload);
    if (!x) {
        spdlog::error("[replayout/encode_ffloat] payload is not float");
        return false;
    }
    uint32_t bits;
    std::memcpy(&bits, x, sizeof(bits));
    w.write_uint32(bits);
    return true;
}

// ─── Double (64 bits, IEEE 754) ───────────────────────────────────────

PropertyValue decode_fdouble(::aoc::protocol::wire::PacketReader& r) {
    if (r.overflowed()) return {};
    uint64_t bits = r.read_uint64();
    if (r.overflowed()) return {};
    double out;
    std::memcpy(&out, &bits, sizeof(out));
    PropertyValue v;
    v.type = FPropertyType::Double;
    v.payload = out;
    return v;
}

bool encode_fdouble(const PropertyValue& v, BunchWriter& w) {
    const double* x = std::get_if<double>(&v.payload);
    if (!x) {
        spdlog::error("[replayout/encode_fdouble] payload is not double");
        return false;
    }
    uint64_t bits;
    std::memcpy(&bits, x, sizeof(bits));
    w.write_uint64(bits);
    return true;
}

}}}} // namespace aoc::protocol::emit::replayout
