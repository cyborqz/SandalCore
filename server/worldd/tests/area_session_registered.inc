// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/area_session_registered.inc: the check files of test_worldd_area_session that register their tests with AOC_REGISTER_TEST (see tests/README.md)
//  One #include line per file; a new feature adds its line at the END. Two branches that add a line at the same time both keep it (the merge=union rule
//  of .gitattributes), so this list does not conflict. The order of the lines does not matter.
// ============================================================================
#include "area_session_gm_vendor_tests.inc"
#include "area_session_craft_tests.inc"
#include "friend_calls_tests.inc"
#include "friend_store_race_tests.inc"
#include "friend_login_window_tests.inc"
#include "friend_lists_save_tests.inc"
#include "friend_refriend_tests.inc"
#include "friend_name_check_tests.inc"
#include "friend_unchanged_list_tests.inc"
#include "friend_removal_load_tests.inc"
#include "friend_unknown_line_tests.inc"
#include "friend_wire_edges_tests.inc"
