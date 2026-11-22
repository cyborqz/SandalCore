// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/small_fixture_db.h: the small content.db of content_db_fixture.h,
//  made once for a test program (the Fixtures share its WorldData).
// ============================================================================
#pragma once

#include "content_db_fixture.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

inline const std::filesystem::path& small_fixture_db() {
    static const std::filesystem::path dir = content_fixture::make_temp_dir("worldd_session_");
    static const std::filesystem::path db = dir / "content.db";
    static const bool made = content_fixture::make_db(db);
    if (!made) {
        std::printf("  [FAIL] the small content.db cannot be made\n");
        std::exit(2);
    }
    return db;
}
