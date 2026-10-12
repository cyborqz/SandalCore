// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  bootstrap/captured_bits.h
//
//  The two small types the generated captured_*.h headers use for what the
//  original server sent as bits: a hex string (bits LSB-first, the first `bits`
//  count) and a NetGUID.  The headers are written by tools/capture/
//  worldd_bootstrap.py; this one is by hand.
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace aoc::worldd::bootstrap::captured {

/// `bits` bits of `hex`, LSB first, as they sat in the capture.
struct CapturedBits {
    const char* name;
    size_t      bits;
    const char* hex;
};

/// A FIntrepidNetworkGUID.
struct CapturedGuid {
    uint64_t object_id;
    uint32_t server_id;
    uint32_t randomizer;
};

/// The bytes of a hex string (two digits a byte, lower case).
inline std::vector<uint8_t> bytes_of(const char* hex) {
    auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    std::vector<uint8_t> out;
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) out.push_back(static_cast<uint8_t>(nib(hex[i]) << 4 | nib(hex[i + 1])));
    return out;
}

inline std::vector<uint8_t> bytes_of(const CapturedBits& b) { return bytes_of(b.hex); }

}  // namespace aoc::worldd::bootstrap::captured
