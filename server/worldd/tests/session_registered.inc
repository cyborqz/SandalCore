// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/session_registered.inc
//
//  the check files of test_worldd_session that register their tests with AOC_REGISTER_TEST (see tests/README.md).  One #include line per file; a new feature adds its line at the END.  Two branches that add a line at the same time both keep it (the merge=union rule of .gitattributes), so this list does not conflict.  The order of the lines is the order the tests run in.
// ============================================================================

#include "session_identity_tests.inc"
#include "session_join_tests.inc"
#include "session_requests_tests.inc"
#include "session_budgets_tests.inc"
#include "session_endpoint_tests.inc"
#include "session_chat_tests.inc"
#include "session_ignore_tests.inc"
#include "session_party_tests.inc"
#include "session_friends_tests.inc"
#include "session_chat_fixes_tests.inc"
