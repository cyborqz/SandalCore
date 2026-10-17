// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_udp_socket.cpp: net/udp_socket over loopback
// ============================================================================
#include "net/udp_socket.h"
#include <cstdio>
#include <string>

using namespace aoc::worldd;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

int main() {
    std::printf("addresses\n");
    const auto a = parse_address("127.0.0.1:7777");
    CHECK(a && a->ip == 0x7F000001u && a->port == 7777, "parses 127.0.0.1:7777");
    CHECK(a && a->to_string() == "127.0.0.1:7777" && a->is_loopback(), "prints it back; loopback");
    CHECK(!parse_address("localhost:7777") && !parse_address("192.0.2.4") && !parse_address("192.0.2.256:1") &&
          !parse_address("192.0.2.4:70000") && !parse_address("192.0.2.4:5x"),
          "refuses names, missing or bad ports and octets");
    CHECK(!parse_address("4294967423.0.0.1:7777") && !parse_address("127.0.0.1:4294975073") &&
          !parse_address("-0.0.0.0:7777") && !parse_address("+127.0.0.1:7777") &&
          !parse_address(std::string("127.0.0.1:7777\0junk", 19)),
          "oversized numbers, signed fields and embedded NULs cannot become valid addresses");
    CHECK(!parse_address("192.0.2.1:1")->is_loopback(), "192.0.2.1 is not loopback");

    std::printf("sockets\n");
    std::string err;
    auto s1 = UdpSocket::open(*parse_address("127.0.0.1:0"), &err);
    auto s2 = UdpSocket::open(*parse_address("127.0.0.1:0"), &err);
    CHECK(s1 && s2, "two sockets bind to system-chosen ports");
    if (!s1 || !s2) return 1;
    const Address a1 = s1->local_address();
    const Address a2 = s2->local_address();
    CHECK(a1.port != 0 && a2.port != 0 && a1.port != a2.port, "local_address reports the chosen ports");

    auto busy = UdpSocket::open(a1, &err);
    CHECK(!busy && err.find("cannot bind") != std::string::npos, "binding a port in use fails with a reason");

    uint8_t buf[64];
    Address from;
    const auto none = s2->receive(buf, sizeof(buf), from, std::chrono::milliseconds(20));
    CHECK(none && *none == 0, "receive times out with 0");

    const uint8_t msg[] = {1, 2, 3, 4, 5};
    CHECK(s1->send(a2, msg, sizeof(msg)), "send");
    const auto n = s2->receive(buf, sizeof(buf), from, std::chrono::milliseconds(1000));
    CHECK(n && *n == 5 && buf[4] == 5 && from == a1, "the datagram arrives with its sender's address");

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
