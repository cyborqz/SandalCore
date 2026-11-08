// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_registered.inc: the check files of test_worldd_combat that register their tests with AOC_REGISTER_TEST (see tests/README.md)
//  One #include line per file; a new feature adds its line at the END. Two branches that add a line at the same time both keep it (the merge=union rule
//  of .gitattributes), so this list does not conflict. The order of the lines does not matter.
// ============================================================================
#include "combat_hostile_tests.inc"
#include "combat_burn_tests.inc"
#include "combat_registry_tests.inc"
#include "combat_forged_requests_tests.inc"
#include "combat_table_errors_tests.inc"
#include "combat_fire_notify_tests.inc"
#include "combat_player_cast_tests.inc"
#include "combat_npc_hits_tests.inc"
#include "combat_creature_gates_tests.inc"
#include "combat_creature_swing_tests.inc"
#include "combat_humanoid_tests.inc"
#include "combat_dots_tests.inc"
#include "combat_dice_tests.inc"
#include "combat_cc_tests.inc"
#include "combat_wake_tests.inc"
#include "combat_removal_refund_tests.inc"
#include "combat_knockback_tests.inc"
