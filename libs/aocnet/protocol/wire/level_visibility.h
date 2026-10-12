// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/wire/level_visibility.h
//
//  World Partition level-visibility RPCs on the PlayerController channel.
//
//    C>S  APlayerController::ServerUpdateLevelVisibility      FieldNetIndex 92
//    S>C  APlayerController::ClientAckUpdateLevelVisibility   FieldNetIndex 21
//
//  Field indices (derived 2026-09-28): APlayerController's 79 net functions
//  (FUNC_Net, no super function) sorted by name put SULV at 73, SUMLV at 74
//  and CALV at 2.  Observed C>S fn_idx 92/93 give base 19, which matches
//  the class-hierarchy count independently: AActor 13 props + 0 RPCs,
//  AController 2 props + 2 RPCs, APlayerController 2 props = 19.
//
//  SULV params (790 bits for a 25-char WP cell name), per RepLayout
//  SendPropertiesForRPC (1 send bit before every non-bool parameter):
//    send(1)=1
//    FUpdateLevelVisibilityLevelInfo::NetSerialize:
//      bArePackageAndFileTheSame(1) bIsVisible(1) flag3(1)
//      PackageName (FName: bHardcoded(1) + FString + int32 Number)
//      [FileName FName if !bArePackageAndFileTheSame]
//      VisibilityRequestId — remaining bits (9 in every capture, all 0)
//  flag3 is bTryMakeVisible or bSkipCloseOnError (both exist in this
//  build); it was 0 in all 222+ captured requests across 9 runs.
// ============================================================================
#pragma once

#include "protocol/wire/ue5_primitives.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aoc { namespace protocol { namespace wire {

// SerializeInt max for PlayerController field headers (C>S and S>C).
constexpr uint32_t kPcMaxFieldIdx = 1025;
constexpr uint32_t kPcFieldIdx_ClientAckUpdateLevelVisibility       = 21;
constexpr uint32_t kPcFieldIdx_ServerUpdateLevelVisibility          = 92;
constexpr uint32_t kPcFieldIdx_ServerUpdateMultipleLevelsVisibility = 93;

struct LevelVisibilityRequest {
    std::string package_name;
    uint32_t    package_number = 0;
    bool        has_file_name  = false;
    std::string file_name;
    uint32_t    file_number    = 0;
    bool        is_visible     = false;
    bool        flag3          = false;
    // Serialized FNetLevelVisibilityTransactionId, echoed verbatim in the ack.
    std::vector<bool> transaction_bits;
};

namespace detail {
// CoA and C_Y_B

// Soft (non-hardcoded) FName as written by UPackageMap::StaticSerializeName:
// bHardcoded(1)=0, FString (int32 length incl. NUL; negative = UTF-16),
// int32 Number.  Package paths are never hardcoded ENames.
inline bool read_soft_fname(const uint8_t* data, size_t data_len, size_t& off,
                            size_t end, std::string& name, uint32_t& number) {
    if (off + 1 + 32 > end) return false;
    if (::ue5::read_bits(data, data_len, off, 1) != 0) return false;
    int32_t len = static_cast<int32_t>(::ue5::read_bits(data, data_len, off, 32));
    const bool wide = len < 0;
    const uint32_t chars = static_cast<uint32_t>(wide ? -static_cast<int64_t>(len) : len);
    const size_t char_bits = wide ? 16 : 8;
    if (chars == 0 || chars > 1024 || off + chars * char_bits + 32 > end) return false;
    name.clear();
    for (uint32_t i = 0; i < chars; ++i) {
        auto c = ::ue5::read_bits(data, data_len, off, static_cast<int>(char_bits));
        if (c != 0) name += static_cast<char>(c);
    }
    number = static_cast<uint32_t>(::ue5::read_bits(data, data_len, off, 32));
    return true;
}

inline void write_soft_fname(uint8_t* buf, size_t cap, size_t& off,
                             const std::string& name, uint32_t number) {
    ::ue5::write_bits(buf, cap, off, 0, 1);
    ::ue5::write_bits(buf, cap, off, static_cast<uint32_t>(name.size() + 1), 32);
    for (char c : name)
        ::ue5::write_bits(buf, cap, off, static_cast<uint8_t>(c), 8);
    ::ue5::write_bits(buf, cap, off, 0, 8);
    ::ue5::write_bits(buf, cap, off, number, 32);
}

// Bit count of SerializeInt(value, max_val) — value-dependent, mirrors
// ue5::write_serialize_int's loop.
inline size_t serialize_int_bits(uint32_t value, uint32_t max_val) {
    size_t n = 0;
    uint32_t written = 0;
    for (uint32_t mask = 1; mask != 0 && written + mask < max_val; mask <<= 1) {
        if (value & mask) written |= mask;
        ++n;
    }
    return n;
}

inline size_t sip_bits(uint64_t v) {
    size_t n = 8;
    while (v >>= 7) n += 8;
    return n;
}

} // namespace detail

/// Decodes SULV parameters.  `param_off` is FActorRpcBunch::param_bit_offset
/// (the params-size SIP); `end_bit` is the end of the bunch payload.
inline bool parse_server_update_level_visibility(const uint8_t* data, size_t data_len,
                                                 size_t param_off, size_t end_bit,
                                                 LevelVisibilityRequest& out) {
    size_t off = param_off;
    const size_t param_bits = static_cast<size_t>(::ue5::read_sip(data, data_len, off));
    const size_t params_end = off + param_bits;
    if (params_end > end_bit || param_bits < 4) return false;

    // Send bit: 0 would mean a default-valued struct, i.e. no level at all.
    if (::ue5::read_bits(data, data_len, off, 1) == 0) return false;
    const bool same_file = ::ue5::read_bits(data, data_len, off, 1) != 0;
    out.is_visible       = ::ue5::read_bits(data, data_len, off, 1) != 0;
    out.flag3            = ::ue5::read_bits(data, data_len, off, 1) != 0;

    if (!detail::read_soft_fname(data, data_len, off, params_end,
                                 out.package_name, out.package_number))
        return false;
    out.has_file_name = !same_file;
    if (!same_file && !detail::read_soft_fname(data, data_len, off, params_end,
                                               out.file_name, out.file_number))
        return false;

    out.transaction_bits.clear();
    while (off < params_end)
        out.transaction_bits.push_back(::ue5::read_bits(data, data_len, off, 1) != 0);
    return true;
}

/// Writes a complete ch=3 actor content block (bHasRepLayout .. params) for
/// ClientAckUpdateLevelVisibility answering `req`.  Returns bits written.
///
/// Params (RepLayout SendPropertiesForRPC): send(1) + FName PackageName,
/// send(1) + TransactionId (request's bits, verbatim), bool (1 bit, no send
/// bit).  Structs always get send=1: CompareScriptStruct against a null
/// default is a mismatch.
inline size_t write_client_ack_update_level_visibility(uint8_t* buf, size_t cap, size_t& off,
                                                       const LevelVisibilityRequest& req,
                                                       bool can_make_visible) {
    std::vector<uint8_t> params(16 + req.package_name.size() + req.transaction_bits.size() / 8, 0);
    size_t p = 0;
    ::ue5::write_bits(params.data(), params.size(), p, 1, 1);
    detail::write_soft_fname(params.data(), params.size(), p, req.package_name, req.package_number);
    ::ue5::write_bits(params.data(), params.size(), p, 1, 1);
    for (bool bit : req.transaction_bits)
        ::ue5::write_bits(params.data(), params.size(), p, bit ? 1 : 0, 1);
    ::ue5::write_bits(params.data(), params.size(), p, can_make_visible ? 1 : 0, 1);

    const size_t field_bits = detail::serialize_int_bits(
        kPcFieldIdx_ClientAckUpdateLevelVisibility, kPcMaxFieldIdx);
    const size_t cb_bits    = field_bits + detail::sip_bits(p) + p;

    const size_t start = off;
    ::ue5::write_bits(buf, cap, off, 0, 1);  // bHasRepLayout
    ::ue5::write_bits(buf, cap, off, 1, 1);  // bIsActor (the PC itself)
    ::ue5::write_sip(buf, cap, off, cb_bits);
    ::ue5::write_serialize_int(buf, cap, off, kPcFieldIdx_ClientAckUpdateLevelVisibility,
                               kPcMaxFieldIdx);
    ::ue5::write_sip(buf, cap, off, p);
    size_t r = 0;
    for (size_t i = 0; i < p; ++i)
        ::ue5::write_bits(buf, cap, off, ::ue5::read_bits(params.data(), params.size(), r, 1), 1);
    return off - start;
}

}}} // namespace aoc::protocol::wire
