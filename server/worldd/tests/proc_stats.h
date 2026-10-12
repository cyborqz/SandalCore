// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// proc_stats.h: the process's memory, for the tests that report what a thing costs (G1, stage 1, Part B).
#pragma once

#include <cstddef>
#include <cstdio>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#endif

namespace aoc::worldd::tests {

struct MemoryNow {
    double working_set_mb = 0;   // resident now
    double peak_mb = 0;          // the most resident so far
    double private_mb = 0;       // committed private bytes now
};

inline MemoryNow memory_now() {
    MemoryNow m;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX c{};
    c.cb = sizeof(c);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&c), sizeof(c))) {
        m.working_set_mb = static_cast<double>(c.WorkingSetSize) / 1048576.0;
        m.peak_mb = static_cast<double>(c.PeakWorkingSetSize) / 1048576.0;
        m.private_mb = static_cast<double>(c.PrivateUsage) / 1048576.0;
    }
#endif
    return m;
}

}  // namespace aoc::worldd::tests
