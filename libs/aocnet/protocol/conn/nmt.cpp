// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/conn/nmt.cpp
// ============================================================================
#include "protocol/conn/nmt.h"
#include "protocol/wire/packet_reader.h"

namespace aoc { namespace protocol { namespace conn {

const char* nmt_name(Nmt type) {
    switch (type) {
    case Nmt::Hello: return "Hello";
    case Nmt::Welcome: return "Welcome";
    case Nmt::Upgrade: return "Upgrade";
    case Nmt::Challenge: return "Challenge";
    case Nmt::Netspeed: return "Netspeed";
    case Nmt::Login: return "Login";
    case Nmt::Failure: return "Failure";
    case Nmt::Join: return "Join";
    case Nmt::JoinSplit: return "JoinSplit";
    case Nmt::Skip: return "Skip";
    case Nmt::Abort: return "Abort";
    case Nmt::PCSwap: return "PCSwap";
    case Nmt::ActorChannelFailure: return "ActorChannelFailure";
    case Nmt::DebugText: return "DebugText";
    case Nmt::NetGUIDAssign: return "NetGUIDAssign";
    case Nmt::SecurityViolation: return "SecurityViolation";
    case Nmt::GameSpecific: return "GameSpecific";
    case Nmt::EncryptionAck: return "EncryptionAck";
    case Nmt::CloseReason: return "CloseReason";
    case Nmt::NetPing: return "NetPing";
    }
    return "?";
}

namespace {

/// FUniqueNetIdRepl's encoding flags as the captured Login has them: type hash 1 (the NULL subsystem) << 3, not
/// encoded, so the id follows as an FString (repl/codecs.cpp read_unique_net_id is the general reader).
constexpr uint8_t kUniqueIdFlagsNull = 8;

}  // namespace
// ~ C | Y | B - C | O | A ~

std::optional<ControlMessage> parse_control_message(const uint8_t* data, size_t len,
                                                    size_t start, size_t bits) {
    if (bits < 8) return std::nullopt;
    wire::PacketReader r(data, len, start + bits);
    r.set_pos(start);
    ControlMessage m;
    m.type = static_cast<Nmt>(r.read_uint8());
    switch (m.type) {
    case Nmt::Hello: {
        m.little_endian = r.read_uint8();
        m.network_version = r.read_uint32();
        auto token = r.read_fstring();
        if (!token) return std::nullopt;
        m.encryption_token = *token;
        break;
    }
    case Nmt::Login: {
        auto response = r.read_fstring();
        auto url = response ? r.read_fstring() : std::nullopt;
        if (!url) return std::nullopt;
        m.client_response = *response;
        m.request_url = *url;
        // The unique id: FUniqueNetIdRepl flags 8 (type hash 1, not encoded: the id is a plain string), then the
        // platform name.  Read only when it is that form; anything else after the URL is left unread, as before.
        if (r.remaining_bits() >= 8) {
            const auto after_url = r.save();
            const uint8_t flags = r.read_uint8();
            if (flags == kUniqueIdFlagsNull) {
                auto id = r.read_fstring();
                auto platform = id ? r.read_fstring() : std::nullopt;
                if (platform && !r.overflowed()) {
                    m.has_unique_id = true;
                    m.unique_id = *id;
                    m.platform_name = *platform;
                }
            }
            if (!m.has_unique_id) r.restore(after_url);
        }
        break;
    }
    case Nmt::Netspeed:
        m.net_speed = r.read_uint32();
        break;
    case Nmt::Challenge: {
        auto challenge = r.read_fstring();
        if (!challenge) return std::nullopt;
        m.challenge = *challenge;
        break;
    }
    case Nmt::Welcome: {
        auto level = r.read_fstring();
        auto mode = level ? r.read_fstring() : std::nullopt;
        auto redirect = mode ? r.read_fstring() : std::nullopt;
        if (!redirect) return std::nullopt;
        m.level = *level;
        m.game_mode = *mode;
        m.redirect_url = *redirect;
        break;
    }
    case Nmt::Failure: {
        auto error = r.read_fstring();
        if (!error) return std::nullopt;
        m.error = *error;
        break;
    }
    default:
        break;
    }
    if (r.overflowed()) return std::nullopt;
    return m;
}

namespace {

// UE5 FString: an empty string is a bare int32 0 (the captured Welcome's
// redirect URL), otherwise int32 length with the NUL, the bytes, the NUL.
// (BunchWriter::write_fstring_ansi writes "" as length 1 and a NUL.)
void write_fstring(emit::BunchWriter& w, const std::string& s) {
    if (s.empty()) w.write_int32(0);
    else w.write_fstring_ansi(s);
}

}  // namespace

void write_nmt_challenge(emit::BunchWriter& w, const std::string& challenge) {
    w.write_uint8(static_cast<uint8_t>(Nmt::Challenge));
    write_fstring(w, challenge);
}

void write_nmt_welcome(emit::BunchWriter& w, const std::string& level,
                       const std::string& game_mode, const std::string& redirect_url) {
    w.write_uint8(static_cast<uint8_t>(Nmt::Welcome));
    write_fstring(w, level);
    write_fstring(w, game_mode);
    write_fstring(w, redirect_url);
}

void write_nmt_failure(emit::BunchWriter& w, const std::string& error) {
    w.write_uint8(static_cast<uint8_t>(Nmt::Failure));
    write_fstring(w, error);
}

}}} // namespace aoc::protocol::conn
