// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_server_guid_allocator.cpp
// ============================================================================
#include "protocol/server_guid_allocator.h"
#include <cstdio>
#include <set>
#include <thread>
#include <vector>

using aoc::protocol::ServerGuidAllocator;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

int main() {
    ServerGuidAllocator a(7, 0x6ee87ee4u);
    const auto g1 = a.next();
    const auto g2 = a.next();
    CHECK(g1.ObjectId == 0x01000000ull, "the first ObjectId is 0x01000000");
    CHECK(g2.ObjectId == g1.ObjectId + 2, "ObjectIds step by 2");
    CHECK(g1.ServerId == 7 && g2.ServerId == 7, "every GUID carries the server's id");
    CHECK(g1.Randomizer == 0x6ee87ee4u && g2.Randomizer == 0x6ee87ee4u, "and the run's randomizer");
    CHECK(a.server_id() == 7 && a.randomizer() == 0x6ee87ee4u, "accessors");

    ServerGuidAllocator shared(9, 1);
    std::vector<std::vector<uint64_t>> got(4);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&, t] { for (int i = 0; i < 1000; ++i) got[t].push_back(shared.next().ObjectId); });
    for (auto& th : threads) th.join();
    std::set<uint64_t> all;
    bool even = true;
    for (const auto& v : got)
        for (uint64_t id : v) { all.insert(id); even = even && (id % 2 == 0); }
    CHECK(all.size() == 4000, "4 threads x 1000 GUIDs are all distinct");
    CHECK(even, "and all even");

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
