// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  login/login_service.cpp
// ============================================================================
#include "login/login_service.h"
#include <spdlog/spdlog.h>

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

LoginResult LoginService::login(const LoginRequest& req, uint32_t server_id) {
    if (!is_hex32(req.character_id) || !is_hex32(req.ticket)) {
        spdlog::info("[login] refused: the handshake did not carry a character id and ticket");
        return refused(login_text::kTicketRefused);
    }

    const svc::RedeemResult redeem = gateway_.redeem_ticket(req.ticket, req.character_id, server_id);
    switch (redeem.status) {
    case svc::CallStatus::Ok:
        break;
    case svc::CallStatus::Refused:
    case svc::CallStatus::NotFound:
        spdlog::info("[login] character {}: the gateway did not accept the join ticket", req.character_id);
        return refused(login_text::kTicketRefused);
    default:
        spdlog::warn("[login] character {}: redeeming the join ticket failed ({})", req.character_id,
                     svc::call_status_name(redeem.status));
        return refused(failure_text(redeem.status));
    }
    if (redeem.character_id != req.character_id) {
        spdlog::error("[login] the gateway redeemed a ticket for character {}, not {}",
                      redeem.character_id, req.character_id);
        return refused(login_text::kLoginFailed);
    }
// C | Y | B : CoA

    const svc::CharacterResult loaded = characters_.get_character(req.character_id);
    if (loaded.status == svc::CallStatus::NotFound) {
        spdlog::info("[login] character {} no longer exists", req.character_id);
        return refused(login_text::kCharacterMissing);
    }
    if (loaded.status != svc::CallStatus::Ok) {
        spdlog::warn("[login] character {}: loading it failed ({})", req.character_id,
                     svc::call_status_name(loaded.status));
        return refused(failure_text(loaded.status));
    }
    if (loaded.character.account_id != redeem.account_id) {
        spdlog::error("[login] character {} belongs to account {}, the ticket to account {}",
                      req.character_id, loaded.character.account_id, redeem.account_id);
        return refused(login_text::kLoginFailed);
    }

    LoginResult r;
    r.accepted = true;
    r.character = loaded.character;
    spdlog::info("[login] character {} ({}) of account {} logged in", r.character.name, r.character.id,
                 r.character.account_id);
    return r;
}

}} // namespace aoc::worldd
