// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/test_worldd_elections.cpp
//
//  The mayoral elections and the citizenship dues as plain rules (world/elections.h, world/citizenship.h, vendor/node_tax.h), with no session and no game
//  data: the cycle and its calendar, the ballot, the instant runoff, the file, the citizens a node needs, the dues and the node's dates, the mayor's taxes.  The
//  checks of the real content.db (AOC_CONTENT_DB or dist/content/content.db) skip themselves without it, so CI runs this program.  The checks are registered
//  from elections_registered.inc (tests/README.md).
// ============================================================================
#include "world/citizenship.h"
#include "world/elections.h"
#include "vendor/node_tax.h"
#include "vendor/town_shops.h"

#include "test_registry.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace aoc::worldd::world;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

namespace {

constexpr int64_t kNode = 5444963087871838208;
constexpr int64_t kDay = 86400000;
constexpr int64_t kHour = 3600000;

// UTC times of the calendar the tests walk through
constexpr int64_t kNov10Noon = 1794312000000;   // 2026-11-10 12:00
constexpr int64_t kNov26 = 1795651200000;       // 2026-11-26 00:00: December's registration
constexpr int64_t kDec1 = 1796083200000;        // 2026-12-01 00:00: December's election
constexpr int64_t kDec27 = 1798329600000;       // 2026-12-27 00:00: January's registration
constexpr int64_t kJan1 = 1798761600000;        // 2027-01-01 00:00
constexpr int64_t kJan6 = 1799193600000;        // 2027-01-06 00:00: the election's end (through the end of hour 23 of the 5th)
constexpr int64_t kJan6Results = kJan6 + 1000;  // 2027-01-06 00:00:01: the results
constexpr int64_t kJan27 = 1801008000000;       // 2027-01-27 00:00: February's registration

ElectionPerson who(int64_t account, const char* id, const char* name, int64_t at = 0) { return ElectionPerson{account, id, name, at}; }

const char* const kAnn = "0000000000000000000000000000000a";
const char* const kBob = "0000000000000000000000000000000b";
const char* const kCy = "0000000000000000000000000000000c";
const char* const kDee = "0000000000000000000000000000000d";

#include "elections_registered.inc"

}  // namespace

int main() {
    g_fail += aoc_tests::run();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
