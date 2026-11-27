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
#include <chrono>
#include <cstdint>
#include <functional>
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
    /// `quests` (optional): the quest service; a character's saved quest log is read from it at the login.
    LoginService(svc::GatewayApi& gateway, svc::CharacterApi& characters, svc::QuestApi* quests = nullptr)
        : gateway_(gateway), characters_(characters), quests_(quests) {}

    /// The log-in barrier: before the character is read, the log-in waits until `writes_pending(character id)` is false (the world answers true while the character has a mail
    /// claim without its answer, in a session or on the mail worker, or a write still queued or running in the saver), so a log-in never starts from bags that such a write can still
    /// change.  The wait never holds the log-in thread: advance() answers "wait" with a time, and the LoginWorker serves the other log-ins until then.  Past `bound` the log-in is
    /// refused with the "try again" text and nothing is loaded.
    struct Barrier {
        /// Called on the log-in's thread right after the gateway redeemed the ticket for the character, before the barrier is asked: the world marks the character (an id named in a
        /// handshake that was never redeemed marks nothing).  Whoever applies or refuses the log-in's result ends the mark.
        std::function<void(uint64_t session_id, const std::string& character_id)> redeemed;
        std::function<bool(const std::string&)> writes_pending;
        std::chrono::milliseconds                bound{20000};   // well under the client's 120 s for a log-in
        std::chrono::milliseconds                poll{25};
    };
    void set_barrier(Barrier barrier) { barrier_ = std::move(barrier); }

    /// One log-in on its way: what advance() remembers between its calls, so a log-in that waits is not started over (its ticket is redeemed once).
    struct Attempt {
        uint64_t     session_id = 0;
        LoginRequest request;
        bool         redeemed = false;   // the ticket was redeemed for the character
        int64_t      account_id = 0;     // the account the gateway redeemed it for
        bool         held = false;       // the barrier held it at least once
        std::chrono::steady_clock::time_point held_since{};   // when it first did
        std::chrono::steady_clock::time_point not_before{};   // a held log-in is looked at again then
    };

    /// Takes the log-in as far as it goes without waiting.  True: it is finished and `out` is its result.  False: the barrier holds it; ask again at `attempt.not_before` (the
    /// LoginWorker does, between other log-ins).  Blocks only on the services' own calls: call it off the network thread.
    bool advance(Attempt& attempt, uint32_t server_id, LoginResult& out);

    /// Redeems the ticket for this server and loads the character, sleeping where the barrier holds it (a caller with other log-ins to serve uses advance() instead).
    LoginResult login(const LoginRequest& req, uint32_t server_id, uint64_t session_id = 0);

private:
    svc::GatewayApi& gateway_;
    svc::CharacterApi& characters_;
    svc::QuestApi*     quests_ = nullptr;
    Barrier            barrier_;
};

}} // namespace aoc::worldd
