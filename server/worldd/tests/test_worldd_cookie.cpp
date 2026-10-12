// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_cookie.cpp: net/cookie (HMAC-SHA1, rotating secrets, age)
// ============================================================================
#include "net/cookie.h"
#include <cstdio>
#include <cstring>

using namespace aoc::worldd;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

// Deterministic secrets: each call fills with the next byte value.
static uint8_t g_next = 1;
static void counting_random(uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; ++i) out[i] = g_next;
    ++g_next;
}

int main() {
    CookieSigner s(counting_random);
    const std::string addr = "127.0.0.1:50510";
    const Cookie c = s.sign(0, 10.0, addr);
    CHECK(c == s.sign(0, 10.0, addr), "the same inputs give the same cookie");
    CHECK(c != s.sign(1, 10.0, addr), "the other secret gives another cookie");
    CHECK(c != s.sign(0, 10.5, addr), "another timestamp gives another cookie");
    CHECK(c != s.sign(0, 10.0, "127.0.0.1:50511"), "another address gives another cookie");
// C.O.A captures, C Y B code

    CHECK(s.verify(0, 10.0, addr, c, 12.0), "verifies 2 s later");
    CHECK(s.verify(0, 10.0, addr, c, 50.0), "and at 40 s");
    CHECK(!s.verify(0, 10.0, addr, c, 50.5), "not after 40 s");
    CHECK(!s.verify(0, 10.0, addr, c, 9.0), "not with a timestamp in the future");
    CHECK(!s.verify(0, 10.0, "127.0.0.2:50510", c, 12.0), "not from another address");
    Cookie bad = c;
    bad[19] ^= 1;
    CHECK(!s.verify(0, 10.0, addr, bad, 12.0), "not with one bit changed");
    CHECK(!s.verify(2, 10.0, addr, c, 12.0), "not with a secret id other than 0 and 1");

    std::printf("rotation\n");
    CHECK(s.secret_id() == 0, "starts with secret 0");
    s.update(14.9);
    CHECK(s.secret_id() == 0, "no rotation before 15 s");
    s.update(15.0);
    CHECK(s.secret_id() == 1, "secret 1 after 15 s");
    CHECK(s.verify(0, 10.0, addr, c, 16.0), "a cookie from secret 0 still verifies");
    s.update(30.0);
    CHECK(s.secret_id() == 0, "back to secret 0, with new bytes");
    CHECK(!s.verify(0, 10.0, addr, c, 31.0), "the old secret-0 cookie no longer verifies");

    CookieSigner real;
    uint8_t r1[16] = {}, r2[16] = {};
    secure_random(r1, sizeof(r1));
    secure_random(r2, sizeof(r2));
    CHECK(std::memcmp(r1, r2, sizeof(r1)) != 0, "secure_random gives different bytes each time");

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
