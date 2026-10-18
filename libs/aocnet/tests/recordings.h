// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  libs/aocnet/tests/recordings.h
//
//  Where the tests that compare against recorded packets find their recordings. The repository ships none: they are read from the folder
//  named by the environment variable AOC_RECORDINGS_DIR, and a test that needs one prints "SKIP: no recordings" and exits 0 when the variable
//  is not set or the file is not in that folder.
//
//  The files a test looks for, all in that one folder:
//    session.bin   every server packet of one recorded session (a container that starts with the magic "RPLY", version 1)
//    packet_a.bin  one raw server packet: the update of the HUD character name
//    packet_b.bin  one raw server packet: the spawn of the player pawn
//    bunch.bin     one reassembled bunch payload: the spawn of the player controller
// ============================================================================
#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace recordings {

inline constexpr const char* kEnv = "AOC_RECORDINGS_DIR";
inline constexpr const char* kSession = "session.bin";
inline constexpr const char* kPacketA = "packet_a.bin";
inline constexpr const char* kPacketB = "packet_b.bin";
inline constexpr const char* kBunch = "bunch.bin";

/// The path of recording `name` in the folder AOC_RECORDINGS_DIR names; an empty string when the variable is not set or the file is not there.
inline std::string find(const char* name) {
    const char* dir = std::getenv(kEnv);
    if (!dir || !*dir) return {};
    std::error_code ec;
    const std::filesystem::path p = std::filesystem::path(dir) / name;
    return std::filesystem::is_regular_file(p, ec) ? p.string() : std::string();
}

/// Prints why recording `name` cannot be used, the way every test that skips for want of one does (the line the runner counts as a skip).
inline void print_skip(const char* name) {
    const char* dir = std::getenv(kEnv);
    if (!dir || !*dir) std::printf("  [skip] SKIP: no recordings (set %s to the folder that holds them)\n", kEnv);
    else std::printf("  [skip] SKIP: no recordings (%s is not in the folder %s names)\n", name, kEnv);
}

/// The end of a test that has nothing to check without recording `name`: the skip line, an empty count, and the exit code 0.
inline int skip_test(const char* name) {
    print_skip(name);
    std::printf("\n0 passed, 0 failed\n");
    return 0;
}

}  // namespace recordings
