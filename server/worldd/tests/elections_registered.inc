// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/elections_registered.inc: the check files of test_worldd_elections that register their tests with AOC_REGISTER_TEST (see tests/README.md)
//  One #include line per file; a new feature adds its line at the END. Two branches that add a line at the same time both keep it (the merge=union rule
//  of .gitattributes), so this list does not conflict. The order of the lines does not matter.
// ============================================================================
#include "elections_mayors_tests.inc"
#include "elections_dues_tests.inc"
#include "elections_node_stores_tests.inc"
#include "elections_buy_orders_tests.inc"
#include "elections_citizen_shops_tests.inc"
#include "elections_reputation_tests.inc"
#include "elections_node_buildings_tests.inc"
