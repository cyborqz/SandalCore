// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/conn/nmt.h
//
//  Control-channel messages (UE5 DataChannel.h NMT_*): the opcode byte and
//  the payloads worldd reads and writes during login.
//
//  Client → server, as the retail client sends them (2026-09-28 capture):
//      Hello     uint8 IsLittleEndian, uint32 NetworkVersion, FString EncryptionToken
//                (UE 5.6's Hello also carries EEngineNetworkRuntimeFeatures
//                after the token; the parser does not read it)
//      Login     FString ClientResponse, FString RequestURL, then the unique id
//                (FUniqueNetIdRepl, as the capture has it: a flags byte of 8, the NULL
//                online subsystem, and the id as an FString) and the platform name
//                (FString "NULL"): the e2e capture's Login is 1,032 bits of exactly that.
//                The unique id and platform are read when they are there and are of that
//                form (a Login without them, or with another encoding, still parses)
//      Netspeed  uint32 rate
//      Join      no payload, once the map has loaded
//  Server → client:
//      Challenge FString challenge
//      Welcome   FString level, FString game mode, FString redirect URL
//      Failure   FString error text; the client shows it and disconnects
//
//  LAYER: Protocol / conn
// ============================================================================
#pragma once

#include "protocol/emit/bunch_writer.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace aoc { namespace protocol { namespace conn {
// (Cyb ~ C-O-A)

enum class Nmt : uint8_t {
    Hello = 0,
    Welcome = 1,
    Upgrade = 2,
    Challenge = 3,
    Netspeed = 4,
    Login = 5,
    Failure = 6,
    Join = 9,
    JoinSplit = 10,
    Skip = 12,
    Abort = 13,
    PCSwap = 15,
    ActorChannelFailure = 16,
    DebugText = 17,
    NetGUIDAssign = 18,
    SecurityViolation = 19,
    GameSpecific = 20,
    EncryptionAck = 21,
    CloseReason = 23,
    NetPing = 24,
};

const char* nmt_name(Nmt type);

/// A control message.  Only the fields of its type are set.
struct ControlMessage {
    Nmt type = Nmt::Hello;
    // Hello
    uint8_t little_endian = 0;
    uint32_t network_version = 0;
    std::string encryption_token;
    // Login
    std::string client_response;
    std::string request_url;
    bool        has_unique_id = false;   // the unique id and platform name followed and were of the form above
    std::string unique_id;               // the FUniqueNetIdRepl's id string (flags 8)
    std::string platform_name;           // "NULL"
    // Netspeed
    uint32_t net_speed = 0;
    // Challenge
    std::string challenge;
    // Welcome
    std::string level;
    std::string game_mode;
    std::string redirect_url;
    // Failure
    std::string error;
};

/// Parses the control message in a bunch payload of `bits` bits starting at
/// bit `start` of `data`, in either direction.  nullopt if the payload is
/// empty or a field of a known type is cut short.  Other types return just
/// their opcode.
std::optional<ControlMessage> parse_control_message(const uint8_t* data, size_t len,
                                                    size_t start, size_t bits);

/// Payload writers: the opcode byte and the message's fields.
void write_nmt_challenge(emit::BunchWriter& w, const std::string& challenge);
void write_nmt_welcome(emit::BunchWriter& w, const std::string& level,
                       const std::string& game_mode, const std::string& redirect_url);
void write_nmt_failure(emit::BunchWriter& w, const std::string& error);

}}} // namespace aoc::protocol::conn
