// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/view_worker.h
//
//  Which of the threads that run the players' view passes the calling thread is (session/view_pool.h): 0 for the network thread (and for every program that has no pool), 1 and up for the
//  pool's workers.  The update that views share (session/shared_updates.h) keeps one memo per worker on each actor and one set of counters per worker, so that no two threads write the
//  same memory.
// ============================================================================
#pragma once

#include <cstddef>

namespace aoc::worldd {

/// The most threads that run view passes at once (the network thread and seven workers).
inline constexpr size_t kViewWorkers = 8;

/// The index of the calling thread among the view threads.
inline unsigned& view_worker() {
    thread_local unsigned worker = 0;
    return worker;
}

}  // namespace aoc::worldd
