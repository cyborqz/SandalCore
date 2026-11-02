// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/movement_registered.inc: the check files of test_worldd_movement that register their tests with AOC_REGISTER_TEST (see tests/README.md)
//  One #include line per file; a new feature adds its line at the END. Two branches that add a line at the same time both keep it (the merge=union rule
//  of .gitattributes), so this list does not conflict. The order of the lines does not matter.
// ============================================================================
#include "movement_move_ack_tests.inc"
#include "movement_based_move_tests.inc"
#include "movement_view_threads_tests.inc"
