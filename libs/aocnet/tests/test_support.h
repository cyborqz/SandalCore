// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  libs/aocnet/tests/test_support.h
//
//  Helpers the plan D1 / D2 tests share (test_repl_hits, test_repl_leveling): hex and bit strings, running a reader on a heap
//  buffer of exactly the bytes it needs (an address sanitizer build reports an over-read), a seeded xorshift for the fuzzers,
//  and the canonical text pieces of the generators' dumps (hit_wire.py, leveling_wire.py).
// ============================================================================
#pragma once

#include "protocol/emit/bunch_writer.h"
#include "protocol/emit/intrepid_netguid.h"
#include "protocol/schema/generated/index.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace tsupport {

inline int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); tsupport::g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); tsupport::g_fail++; } \
} while (0)

inline const aoc::protocol::schema::ClassSchema& schema_named(const char* name) {
    const aoc::protocol::schema::ClassSchema* s = aoc::protocol::schema::gen::find_class_schema(name);
    if (!s) { std::printf("  [FAIL] no schema %s\n", name); std::exit(2); }
    return *s;
}
// built by C O A with C_Y_B

inline std::vector<uint8_t> bytes_of_hex(const char* hex) {
    auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    std::vector<uint8_t> out;
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) out.push_back(static_cast<uint8_t>(nib(hex[i]) << 4 | nib(hex[i + 1])));
    return out;
}

/// A bit string: what the tests cut and patch (LSB first, as on the wire).
struct Bits {
    std::vector<bool> b;
    Bits() = default;
    Bits(const std::vector<uint8_t>& bytes, size_t n) { for (size_t i = 0; i < n; ++i) b.push_back((bytes[i >> 3] >> (i & 7)) & 1); }
    explicit Bits(const aoc::protocol::emit::BunchWriter& w) : Bits(w.bytes(), w.bit_pos()) {}
    size_t size() const { return b.size(); }
    std::vector<uint8_t> bytes() const {
        std::vector<uint8_t> out((b.size() + 7) / 8, 0);
        for (size_t i = 0; i < b.size(); ++i) out[i >> 3] |= static_cast<uint8_t>(b[i] ? 1 : 0) << (i & 7);
        return out;
    }
    Bits slice(size_t from, size_t n) const { Bits r; r.b.assign(b.begin() + from, b.begin() + from + n); return r; }
    void set(size_t at, uint64_t v, int n) { for (int i = 0; i < n; ++i) b[at + i] = (v >> i) & 1; }
    uint64_t get(size_t at, int n) const { uint64_t v = 0; for (int i = 0; i < n; ++i) v |= static_cast<uint64_t>(b[at + i] ? 1 : 0) << i; return v; }
    void insert(size_t at, uint64_t v, int n) { for (int i = 0; i < n; ++i) b.insert(b.begin() + at + i, (v >> i) & 1); }
    void erase(size_t at, size_t n) { b.erase(b.begin() + at, b.begin() + at + n); }
    void append(uint64_t v, int n) { for (int i = 0; i < n; ++i) b.push_back((v >> i) & 1); }
    bool operator==(const Bits& o) const { return b == o.b; }
};

inline Bits bits_of_hex(const char* hex, size_t n) { return Bits(bytes_of_hex(hex), n); }

/// Runs `f(data, bytes, bits)` on a heap buffer of exactly the bytes the bits need.
template <class F>
inline auto with_exact(const Bits& bits, F f) {
    const std::vector<uint8_t> v = bits.bytes();
    std::unique_ptr<uint8_t[]> exact(new uint8_t[v.empty() ? 1 : v.size()]);
    if (!v.empty()) std::memcpy(exact.get(), v.data(), v.size());
    return f(static_cast<const uint8_t*>(exact.get()), v.size(), bits.size());
}

/// A seeded xorshift64: the fuzzers are deterministic.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    uint64_t below(uint64_t n) { return next() % n; }
};

// ---- canonical text pieces (the same text the Python decoders make) --------------------------------------------------------

inline std::string num(uint64_t v) { return std::to_string(v); }
inline std::string snum(int64_t v) { return std::to_string(v); }

inline std::string hexn(uint64_t v, int digits) {
    static const char* d = "0123456789abcdef";
    std::string s(static_cast<size_t>(digits), '0');
    for (int i = digits - 1; i >= 0; --i, v >>= 4) s[static_cast<size_t>(i)] = d[v & 15];
    return s;
}

inline std::string join(const std::vector<std::string>& v, const char* sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

/// An FIntrepidNetGUID as four uint32 words in hex: ObjectId low, high, ServerId, Randomizer.
inline std::string guid_words(const aoc::protocol::emit::FIntrepidNetworkGUID& g) {
    return hexn(g.ObjectId & 0xffffffffu, 8) + hexn(g.ObjectId >> 32, 8) + hexn(g.ServerId, 8) + hexn(g.Randomizer, 8);
}

}  // namespace tsupport
