// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/cookie.h
//
//  The handshake cookie (UE5 StatelessConnectHandlerComponent): HMAC-SHA1
//  of the Challenge's timestamp and the client's address under one of two
//  rotating secrets.  The server keeps no state for a client until its
//  Response echoes a cookie that verifies, so spoofed Initials cost nothing.
//  The cookie's first 4 bytes also pick both initial packet sequences
//  (protocol/conn/handshake.h initial_sequences).
// ============================================================================
#pragma once

#include "protocol/conn/handshake.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace aoc { namespace worldd {

using Cookie = std::array<uint8_t, protocol::conn::kCookieSize>;

/// Cryptographically random bytes (OpenSSL RAND_bytes).
void secure_random(uint8_t* out, size_t n);

class CookieSigner {
public:
    static constexpr double kSecretLifetimeSeconds = 15.0;
    static constexpr double kMaxCookieAgeSeconds = 40.0;
    using RandomFill = std::function<void(uint8_t*, size_t)>;

    explicit CookieSigner(RandomFill random = secure_random);

    /// Replaces the older secret once kSecretLifetimeSeconds have passed; the
    /// previous one stays valid for Responses in flight.
    void update(double now_seconds);

    /// The secret new Challenges use (0 or 1).
    uint8_t secret_id() const { return current_; }

    Cookie sign(uint8_t secret_id, double timestamp, const std::string& address) const;

    /// True if `cookie` is what sign() gives for these inputs and the
    /// timestamp is at most kMaxCookieAgeSeconds old (and not in the future).
    bool verify(uint8_t secret_id, double timestamp, const std::string& address,
                const Cookie& cookie, double now_seconds) const;

private:
    RandomFill random_;
    std::array<std::array<uint8_t, 64>, 2> secrets_{};
    uint8_t current_ = 0;
    double last_rotation_ = 0;
};

}} // namespace aoc::worldd
