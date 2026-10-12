// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/conn/handshake.h
//
//  The stateless connect handshake (UE5 StatelessConnectHandlerComponent)
//  with Intrepid's 94-byte extension, as the AoC client and the original
//  server speak it (2026-09-28 capture; legacy src/net/game_server.h ue5_hs).
//
//      client  Initial   (type 0)  extension: marker, character id, join ticket
//      server  Challenge (type 1)  timestamp + cookie, the server's extension
//      client  Response  (type 2)  echoes timestamp, cookie and extension
//      server  Ack       (type 3)  timestamp -1, the same cookie and extension
//
//  Packet layout (LSB-first bits):
//      MagicHeader:32 SessionId:2 ClientId:3 bHandshake:1 bRestart:1
//      MinVersion:8 CurVersion:8 PacketType:8 SentCount:8
//      NetworkVersion:32 NetworkFeatures:16 SecretId:1 Timestamp:64(double)
//      Cookie:20 bytes, extension: 94 bytes, padding bytes, termination bit
//
//  The client derives both initial packet sequences from the cookie (see
//  initial_sequences), so whoever builds the cookie chooses them.
//
//  LAYER: Protocol / conn
// ============================================================================
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace aoc { namespace protocol { namespace conn {

enum class HandshakeType : uint8_t {
    Initial = 0, Challenge = 1, Response = 2, Ack = 3,
    Restart = 4, RestartResponse = 5, VersionUpgrade = 6,
};

constexpr size_t kCookieSize = 20;
constexpr size_t kIntrepidExtSize = 94;
constexpr std::array<uint8_t, 6> kIntrepidMarker = {0x7f, 0x7f, 0x7f, 0x7f, 0xff, 0x7f};

/// Padding after the extension, as the original server sends it:
/// Challenge 153 bytes, Ack 147 bytes.
constexpr size_t kChallengePaddingBytes = 15;
constexpr size_t kAckPaddingBytes = 9;

/// Intrepid's handshake extension; the same layout in all four packets.
///   [0, 22)  Initial: the marker and 16 zero bytes.  Server packets:
///            server-chosen bytes; the first 6 are the connection's custom
///            field, which both sides then put in every data packet.
///   [22, 38) character id (FGuid bytes)
///   [38, 54) Initial: zeros.  Server packets: server-chosen bytes.
///   [54, 70) join ticket (FGuid bytes)
///   [70, 94) spawn location: three little-endian doubles (Initial: zeros)
struct IntrepidExt {
    std::array<uint8_t, kIntrepidExtSize> bytes{};

    bool has_initial_marker() const;
    std::array<uint8_t, 16> character_id() const;
    std::array<uint8_t, 16> ticket() const;
    std::array<uint8_t, 6> custom_field() const;
    std::array<double, 3> spawn() const;

    /// The extension a server sends: its 38 chosen bytes around the ids from
    /// the client's Initial, and a spawn location.
    static IntrepidExt for_server(const IntrepidExt& initial,
                                  const std::array<uint8_t, 38>& server_bytes,
                                  const std::array<double, 3>& spawn);
};
// (C:Y:B ~ C-O-A)

struct HandshakePacket {
    uint32_t magic_header = 0;
    uint8_t  session_id = 0;        // 2 bits
    uint8_t  client_id = 0;         // 3 bits
    bool     restart = false;
    uint8_t  min_version = 0;
    uint8_t  cur_version = 0;
    HandshakeType type = HandshakeType::Initial;
    uint8_t  sent_count = 0;
    uint32_t network_version = 0;
    uint16_t network_features = 0;
    uint8_t  secret_id = 0;         // 1 bit
    double   timestamp = 0.0;
    std::array<uint8_t, kCookieSize> cookie{};
    std::optional<IntrepidExt> ext;  // present when 94 bytes follow the cookie
    std::vector<uint8_t> padding;    // whole bytes after the extension
};

/// True if the packet's handshake bit (bit 37) is set.
bool is_handshake(const uint8_t* data, size_t len);

/// Parses a handshake packet; nullopt if it is not one or is too short.
std::optional<HandshakePacket> parse_handshake(const uint8_t* data, size_t len);

/// Serializes `p` (with its padding bytes) and the termination bit.
std::vector<uint8_t> build_handshake(const HandshakePacket& p);

struct InitialSequences {
    uint16_t server;  // first sequence the server sends
    uint16_t client;  // first sequence the client sends
};

/// The client reads both initial packet sequences from the cookie: bytes
/// [0, 2) and [2, 4), little-endian, masked to 14 bits.  In the capture,
/// cookie b9 37 fe bd gave 14265 and 15870.
InitialSequences initial_sequences(const std::array<uint8_t, kCookieSize>& cookie);

}}} // namespace aoc::protocol::conn
