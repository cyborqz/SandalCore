// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  login/login_service.cpp
// ============================================================================
#include "login/login_service.h"
#include <spdlog/spdlog.h>
#include <thread>

namespace aoc { namespace worldd {

namespace {

bool is_hex32(const std::string& s) {
    if (s.size() != 32) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

LoginResult refused(const char* text) {
    LoginResult r;
    r.error = text;
    return r;
}

// The player's text for a service failure other than a refusal.
const char* failure_text(svc::CallStatus s) {
    return s == svc::CallStatus::Unavailable ? login_text::kServiceUnavailable
                                             : login_text::kLoginFailed;
}

}  // namespace

LoginResult LoginService::login(const LoginRequest& req, uint32_t server_id, uint64_t session_id) {
    Attempt attempt;
    attempt.session_id = session_id;
    attempt.request = req;
    LoginResult result;
    while (!advance(attempt, server_id, result)) std::this_thread::sleep_until(attempt.not_before);
    return result;
}

bool LoginService::advance(Attempt& attempt, uint32_t server_id, LoginResult& out) {
    const LoginRequest& req = attempt.request;
    if (!attempt.redeemed) {
        if (!is_hex32(req.character_id) || !is_hex32(req.ticket)) {
            spdlog::info("[login] refused: the handshake did not carry a character id and ticket");
            out = refused(login_text::kTicketRefused);
            return true;
        }

        const svc::RedeemResult redeem = gateway_.redeem_ticket(req.ticket, req.character_id, server_id);
        switch (redeem.status) {
        case svc::CallStatus::Ok:
            break;
        case svc::CallStatus::Refused:
        case svc::CallStatus::NotFound:
            spdlog::info("[login] character {}: the gateway did not accept the join ticket", req.character_id);
            out = refused(login_text::kTicketRefused);
            return true;
        default:
            spdlog::warn("[login] character {}: redeeming the join ticket failed ({})", req.character_id,
                         svc::call_status_name(redeem.status));
            out = refused(failure_text(redeem.status));
            return true;
        }
        if (redeem.character_id != req.character_id) {
            spdlog::error("[login] the gateway redeemed a ticket for character {}, not {}",
                          redeem.character_id, req.character_id);
            out = refused(login_text::kLoginFailed);
            return true;
        }
// C | Y | B : CoA
        attempt.redeemed = true;
        attempt.account_id = redeem.account_id;
        if (barrier_.redeemed) barrier_.redeemed(attempt.session_id, req.character_id);   // (the ticket is the character's: mark it, then look for what is unanswered)
    }

    // The barrier: not before the ticket is redeemed (an unauthenticated id must not hold anything), and before the character is read.  A character that is held waits in the
    // worker's queue, not here: the caller asks again at not_before.
    if (barrier_.writes_pending && barrier_.writes_pending(req.character_id)) {
        const auto now = std::chrono::steady_clock::now();
        if (!attempt.held) {
            attempt.held = true;
            attempt.held_since = now;
        }
        if (now - attempt.held_since >= barrier_.bound) {
            spdlog::warn("[login] character {}: an earlier claim or save of it is still unanswered after {} ms: the login is refused (nothing is loaded)", req.character_id,
                         static_cast<long long>(barrier_.bound.count()));
            out = refused(login_text::kServiceUnavailable);
            return true;
        }
        attempt.not_before = now + barrier_.poll;
        return false;
    }
    if (attempt.held) {
        spdlog::info("[login] character {}: waited {} ms for its earlier claim or save", req.character_id,
                     static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - attempt.held_since).count()));
    }

    const svc::CharacterResult loaded = characters_.get_character(req.character_id);
    if (loaded.status == svc::CallStatus::NotFound) {
        spdlog::info("[login] character {} no longer exists", req.character_id);
        out = refused(login_text::kCharacterMissing);
        return true;
    }
    if (loaded.status != svc::CallStatus::Ok) {
        spdlog::warn("[login] character {}: loading it failed ({})", req.character_id,
                     svc::call_status_name(loaded.status));
        out = refused(failure_text(loaded.status));
        return true;
    }
    if (loaded.character.account_id != attempt.account_id) {
        spdlog::error("[login] character {} belongs to account {}, the ticket to account {}",
                      req.character_id, loaded.character.account_id, attempt.account_id);
        out = refused(login_text::kLoginFailed);
        return true;
    }

    LoginResult r;
    r.accepted = true;
    r.character = loaded.character;
    if (quests_) {   // quests, slice 1: the saved quest log; without it the character plays without quests and nothing is saved over what the service holds
        svc::QuestLogResult q = quests_->load_log(r.character.id);
        if (q.status == svc::CallStatus::Ok) r.character.quest_log = std::move(q.log);
        else spdlog::warn("[login] character {}: its quest log does not load ({}): it plays without quests", r.character.id, svc::call_status_name(q.status));
    }
    spdlog::info("[login] character {} ({}) of account {} logged in", r.character.name, r.character.id,
                 r.character.account_id);
    out = std::move(r);
    return true;
}

}} // namespace aoc::worldd
