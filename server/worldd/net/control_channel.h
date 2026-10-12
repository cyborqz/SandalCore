// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/control_channel.h
//
//  The control channel's login sequence (architecture spec §3.2
//  net/ControlChannel), as logic without I/O.  The retail client sends, on
//  channel 0 (2026-09-28 capture):
//
//      Hello  -> we answer Challenge
//      Login  -> we answer Welcome (level, game mode) once its join ticket is
//                redeemed and the character loaded; Failure + close if not.  Its
//                ?Name= option and unique id are kept (login_message()): the
//                PlayerState carries them after Join (plan A5)
//      Netspeed
//      Join   -> the client has loaded the map: it is playing
//
//  The login runs in parallel (login/login_service.h): the handshake
//  carried the character id and ticket, so it usually finishes before Login
//  arrives; Welcome waits for it if not.  A refusal before Hello waits for
//  Hello, since the client opens channel 0.
// ============================================================================
#pragma once

#include "login/login_service.h"
#include "protocol/conn/nmt.h"
#include "protocol/emit/bunch_writer.h"
#include <optional>
#include <string>
#include <vector>

namespace aoc { namespace worldd {

struct WorldInfo {
    std::string level = "/Game/Levels/Verra_World_Master/Verra_World_Master";
    std::string game_mode = "/Game/GameBlueprints/AoCGameModeBaseBP.AoCGameModeBaseBP_C";
};

/// What the client's NMT_Login said about the connection (the first Login only).
struct LoginMessage {
    std::string request_url;       // "?Name=DESKTOP-EXAMPL2-FEDCBA9876543210FEDCBA9876543210"
    bool        has_unique_id = false;
    std::string unique_id;         // the FUniqueNetIdRepl's id string; the capture's equals the ?Name= value
};

class ControlChannel {
public:
    enum class State {
        AwaitHello,   // the client has not opened channel 0 yet
        AwaitLogin,   // Challenge sent
        LoggingIn,    // Login received, the login is still running
        AwaitJoin,    // Welcome sent, the client is loading the map
        Playing,      // Join received
        Closed,       // Failure sent or the client was dropped
    };

    /// What to send on channel 0 (reliable NMT payloads, in order), and
    /// whether to close the connection after them.
    struct Reply {
        std::vector<protocol::emit::BunchWriter> messages;
        bool close = false;
        bool joined = false;  // this message was the client's Join
    };
// [C O A + Cyb]

    ControlChannel(WorldInfo world, std::string challenge)
        : world_(std::move(world)), challenge_(std::move(challenge)) {}

    Reply on_message(const protocol::conn::ControlMessage& m);
    Reply on_login(const LoginResult& result);
    /// Ends the session with `text` shown to the player.
    Reply kick(const std::string& text);

    State state() const { return state_; }
    uint32_t net_speed() const { return net_speed_; }
    /// The login result this channel was given by on_login, if any.
    const std::optional<LoginResult>& login() const { return login_; }
    /// The client's NMT_Login, once it has arrived.
    const std::optional<LoginMessage>& login_message() const { return login_message_; }

private:
    Reply welcome_or_refuse();
    Reply refuse(const std::string& text);

    WorldInfo world_;
    std::string challenge_;
    State state_ = State::AwaitHello;
    std::optional<LoginResult> login_;
    std::optional<LoginMessage> login_message_;
    uint32_t net_speed_ = 0;
};

}} // namespace aoc::worldd
