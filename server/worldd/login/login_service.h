// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  login/login_service.h
//
//  A client's login on worldd (architecture spec §4 step 5): the character
//  id and join ticket from its handshake are redeemed with the gateway,
//  then the character is loaded from the character service.
//
//  Refusals carry the text the client shows (NMT_Failure).  The ticket is a
//  bearer secret until it is spent: it is never logged.
// ============================================================================
#pragma once

#include "svc/services.h"
#include <cstdint>
#include <string>

namespace aoc { namespace worldd {

/// What the client presented in its handshake, as 32 lowercase hex digits.
struct LoginRequest {
    std::string character_id;
    std::string ticket;
};

struct LoginResult {
    bool accepted = false;
    std::string error;          // for the player, when refused
    svc::Character character;   // when accepted
};

namespace login_text {
constexpr const char* kTicketRefused =
    "Your join ticket was not accepted. Return to character select and press Play again.";
constexpr const char* kServiceUnavailable =
    "The world server cannot reach the login service. Try again in a moment.";
constexpr const char* kCharacterMissing =
    "Your character could not be loaded. Return to character select and try again.";
constexpr const char* kLoginFailed = "Login failed on the world server.";
// The character's race is Tulnar or past the enum (bootstrap::SetupRefusal::UnsupportedRace); the log has the id.
constexpr const char* kRaceUnsupported = "This race is not supported on this server yet.";
// content.db did not load (plan A5): the world cannot value a character without it.
constexpr const char* kContentMissing =
    "The world server has no game data loaded (content.db), so it cannot let you in yet. Tell the server's operator.";
constexpr const char* kLoggedInElsewhere = "Your character has logged in again from another client.";
constexpr const char* kShuttingDown = "The world server is shutting down.";
}  // namespace login_text
// CoA captures, C.Y.B code

/// Where the endpoint submits logins; the answer comes back through
/// Endpoint::post_login_result.
class LoginQueue {
public:
    virtual ~LoginQueue() = default;
    virtual void submit(uint64_t session_id, LoginRequest request) = 0;
};

class LoginService {
public:
    LoginService(svc::GatewayApi& gateway, svc::CharacterApi& characters)
        : gateway_(gateway), characters_(characters) {}

    /// Redeems the ticket for this server and loads the character.  Blocks
    /// on the services: call it off the network thread.
    LoginResult login(const LoginRequest& req, uint32_t server_id);

private:
    svc::GatewayApi& gateway_;
    svc::CharacterApi& characters_;
};

}} // namespace aoc::worldd
