// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/cookie.cpp
// ============================================================================
#include "net/cookie.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace aoc { namespace worldd {

void secure_random(uint8_t* out, size_t n) {
    if (RAND_bytes(out, static_cast<int>(n)) != 1)
        throw std::runtime_error("RAND_bytes failed");
}

CookieSigner::CookieSigner(RandomFill random) : random_(std::move(random)) {
    for (auto& s : secrets_) random_(s.data(), s.size());
}

void CookieSigner::update(double now_seconds) {
    if (now_seconds - last_rotation_ < kSecretLifetimeSeconds) return;
    current_ ^= 1;
    random_(secrets_[current_].data(), secrets_[current_].size());
    last_rotation_ = now_seconds;
}

Cookie CookieSigner::sign(uint8_t secret_id, double timestamp, const std::string& address) const {
    std::vector<uint8_t> input(sizeof(timestamp) + address.size());
    std::memcpy(input.data(), &timestamp, sizeof(timestamp));
    std::memcpy(input.data() + sizeof(timestamp), address.data(), address.size());
    Cookie out{};
    unsigned int len = static_cast<unsigned int>(out.size());
    const auto& secret = secrets_[secret_id & 1];
    HMAC(EVP_sha1(), secret.data(), static_cast<int>(secret.size()), input.data(), input.size(),
         out.data(), &len);
    return out;
}

bool CookieSigner::verify(uint8_t secret_id, double timestamp, const std::string& address,
                          const Cookie& cookie, double now_seconds) const {
    if (secret_id > 1) return false;
    const double age = now_seconds - timestamp;
    if (!(age >= 0.0 && age <= kMaxCookieAgeSeconds)) return false;  // also rejects NaN
    const Cookie want = sign(secret_id, timestamp, address);
    return CRYPTO_memcmp(want.data(), cookie.data(), want.size()) == 0;
}

}} // namespace aoc::worldd
