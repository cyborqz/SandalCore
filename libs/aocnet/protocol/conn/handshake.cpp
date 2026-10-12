// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/conn/handshake.cpp
// ============================================================================
#include "protocol/conn/handshake.h"
#include "protocol/conn/packet_framing.h"
#include "protocol/conn/sequence.h"
#include "protocol/emit/bunch_writer.h"
#include "protocol/wire/ue5_primitives.h"
#include <cstring>

namespace aoc { namespace protocol { namespace conn {

namespace {

constexpr size_t kCharacterIdOffset = 22;
constexpr size_t kServerBytes2Offset = 38;
constexpr size_t kTicketOffset = 54;
constexpr size_t kSpawnOffset = 70;
constexpr size_t kOuterBits = 38;
constexpr size_t kFixedBits = kOuterBits + 1 + 8 + 8 + 8 + 8 + 32 + 16 + 1 + 64 + kCookieSize * 8;  // 344

template <size_t N>
std::array<uint8_t, N> slice(const std::array<uint8_t, kIntrepidExtSize>& b, size_t off) {
    std::array<uint8_t, N> out{};
    std::memcpy(out.data(), b.data() + off, N);
    return out;
}

}  // namespace

bool IntrepidExt::has_initial_marker() const {
    return std::memcmp(bytes.data(), kIntrepidMarker.data(), kIntrepidMarker.size()) == 0;
}

std::array<uint8_t, 16> IntrepidExt::character_id() const { return slice<16>(bytes, kCharacterIdOffset); }
std::array<uint8_t, 16> IntrepidExt::ticket() const { return slice<16>(bytes, kTicketOffset); }
std::array<uint8_t, 6> IntrepidExt::custom_field() const { return slice<6>(bytes, 0); }

std::array<double, 3> IntrepidExt::spawn() const {
    std::array<double, 3> xyz{};
    std::memcpy(xyz.data(), bytes.data() + kSpawnOffset, sizeof(xyz));
    return xyz;
}

IntrepidExt IntrepidExt::for_server(const IntrepidExt& initial,
                                    const std::array<uint8_t, 38>& server_bytes,
                                    const std::array<double, 3>& spawn) {
    IntrepidExt e;
    std::memcpy(e.bytes.data(), server_bytes.data(), kCharacterIdOffset);
    const auto id = initial.character_id();
    std::memcpy(e.bytes.data() + kCharacterIdOffset, id.data(), id.size());
    std::memcpy(e.bytes.data() + kServerBytes2Offset, server_bytes.data() + kCharacterIdOffset, 16);
    const auto ticket = initial.ticket();
    std::memcpy(e.bytes.data() + kTicketOffset, ticket.data(), ticket.size());
    std::memcpy(e.bytes.data() + kSpawnOffset, spawn.data(), sizeof(spawn));
    return e;
}

bool is_handshake(const uint8_t* data, size_t len) {
    if (len * 8 < kOuterBits) return false;
    return ::ue5::read_bits_at(data, len, 37, 1) != 0;
}

std::optional<HandshakePacket> parse_handshake(const uint8_t* data, size_t len) {
    if (len == 0 || len > kMaxPacketBytes) return std::nullopt;  // nothing, or larger than any datagram
    if (data[len - 1] == 0) return std::nullopt;  // no termination bit
    const size_t end = ::ue5::strip_termination(data, len);
    if (end < kFixedBits || !is_handshake(data, len)) return std::nullopt;
    size_t off = 0;
    auto rd = [&](int n) { return ::ue5::read_bits(data, len, off, n); };
    HandshakePacket p;
    p.magic_header = static_cast<uint32_t>(rd(32));
    p.session_id = static_cast<uint8_t>(rd(2));
    p.client_id = static_cast<uint8_t>(rd(3));
    rd(1);  // bHandshake
    p.restart = rd(1) != 0;
    p.min_version = static_cast<uint8_t>(rd(8));
    p.cur_version = static_cast<uint8_t>(rd(8));
    p.type = static_cast<HandshakeType>(rd(8));
    p.sent_count = static_cast<uint8_t>(rd(8));
    p.network_version = static_cast<uint32_t>(rd(32));
    p.network_features = static_cast<uint16_t>(rd(16));
    p.secret_id = static_cast<uint8_t>(rd(1));
    const uint64_t ts = rd(64);
    std::memcpy(&p.timestamp, &ts, sizeof(ts));
    for (auto& b : p.cookie) b = static_cast<uint8_t>(rd(8));
    if (end - off >= kIntrepidExtSize * 8) {
        IntrepidExt e;
        for (auto& b : e.bytes) b = static_cast<uint8_t>(rd(8));
        p.ext = e;
    }
    while (off + 8 <= end) p.padding.push_back(static_cast<uint8_t>(rd(8)));
    return p;
}
// (CoA : C Y B)

std::vector<uint8_t> build_handshake(const HandshakePacket& p) {
    emit::BunchWriter w(256);
    w.write_uint32(p.magic_header);
    w.write_bits(p.session_id, 2);
    w.write_bits(p.client_id, 3);
    w.write_bit(1);  // bHandshake
    w.write_bit(p.restart ? 1 : 0);
    w.write_uint8(p.min_version);
    w.write_uint8(p.cur_version);
    w.write_uint8(static_cast<uint8_t>(p.type));
    w.write_uint8(p.sent_count);
    w.write_uint32(p.network_version);
    w.write_uint16(p.network_features);
    w.write_bits(p.secret_id, 1);
    uint64_t ts = 0;
    std::memcpy(&ts, &p.timestamp, sizeof(ts));
    w.write_uint64(ts);
    for (uint8_t b : p.cookie) w.write_uint8(b);
    if (p.ext) for (uint8_t b : p.ext->bytes) w.write_uint8(b);
    for (uint8_t b : p.padding) w.write_uint8(b);
    w.write_bit(1);  // termination
    return std::vector<uint8_t>(w.data(), w.data() + w.byte_size());
}

InitialSequences initial_sequences(const std::array<uint8_t, kCookieSize>& cookie) {
    const uint16_t server = static_cast<uint16_t>(cookie[0] | (cookie[1] << 8));
    const uint16_t client = static_cast<uint16_t>(cookie[2] | (cookie[3] << 8));
    return {static_cast<uint16_t>(server & kSeqMask), static_cast<uint16_t>(client & kSeqMask)};
}

}}} // namespace aoc::protocol::conn
