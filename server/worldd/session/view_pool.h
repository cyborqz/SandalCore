// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/view_pool.h
//
//  The threads that run the players' view passes side by side (worldd --view-threads N).
//
//  A view pass reads the world's registry and writes nothing but its own view and its own connection, so the passes of different players can run at the same time.  The server's tick
//  (net/endpoint.cpp) runs them in three phases when the pool is on: every session's own work in turn (phase 1: the abilities, the mounts, the quests ... and the decision whether the
//  view's pass is due, which takes or moves a phase of the views' shared count), then all the due passes at once on the pool (phase 2, with the registry frozen: nothing may change it),
//  then every session's flush in turn (phase 3).  This file is the pool of phase 2.
//
//  `threads` counts the threads that run passes, the network thread's among them: 1 starts no thread and runs the passes inline, in order (the reference the threaded runs are compared
//  with), 4 starts three.  Each thread works through a job at a time from one shared count; the workers sleep between ticks.  While a job runs, view_worker() is the index of the thread
//  (so that the update memos and counters of session/shared_updates.h are the thread's own) and tick_profile() is a profile of the thread's own, added to the network thread's profile in the
//  order of the threads when the passes are done.
//
//  A call of run() ends when its jobs have ended, not when every worker has woken: each call has a record of its own (how far the jobs have got), and a worker that the system has not
//  scheduled yet, or that wakes after the call has ended, finds no job in it.  So one thread that is held back on a busy machine delays the tick by no more than the job it is in.
// ============================================================================
#pragma once

#include "net/tick_profile.h"
#include "session/view_worker.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aoc::worldd {

class ViewPool {
public:
    /// What a job ended with.  `failed` is set for every exception out of the job, whatever its text (an exception can carry none).
    struct JobError {
        bool        failed = false;
        std::string what;
    };

    /// 1 to kViewWorkers threads in all (more are taken as kViewWorkers).
    explicit ViewPool(unsigned threads);
    ~ViewPool();
    ViewPool(const ViewPool&) = delete;
    ViewPool& operator=(const ViewPool&) = delete;

    unsigned threads() const { return threads_; }

    /// Runs fn(i) for every i in [0, count) on the pool's threads, the caller's among them, and returns when every job has ended.  An exception out of fn(i) is caught and put in (*errors)[i]
    /// when `errors` is given (the caller sized it to `count`); no other job is stopped.  Only one thread calls run, and not from inside a job.
    void run(size_t count, const std::function<void(size_t)>& fn, std::vector<JobError>* errors = nullptr);

    /// The seconds the threads spent in jobs since the pool was made (the sum over the threads), and how many jobs they ran, and how many of them ran on a worker (not the caller's thread).
    double   busy_seconds() const;
    uint64_t jobs_run() const { return jobs_.load(std::memory_order_relaxed); }
    uint64_t worker_jobs() const { return worker_jobs_.load(std::memory_order_relaxed); }

    /// For the tests: with a seed other than 0 every job waits a pseudo-random time (up to about 300 microseconds) before it starts, so that the threads interleave differently on every run.
    void set_jitter(uint64_t seed) { jitter_ = seed; }
    /// For the tests: every worker calls gate(its index) when it wakes for a run, before it takes a job.  A gate that waits holds the worker back, as a busy machine can.  Set it while no run is on.
    void set_worker_gate(std::function<void(unsigned)> gate) { gate_ = std::move(gate); }

private:
    /// One call of run(): the jobs and how far they have got.  A worker keeps the record of the run it woke for, so a late worker never touches the next call's jobs.
    struct Run {
        const std::function<void(size_t)>* fn = nullptr;
        size_t                             count = 0;
        std::vector<JobError>*             errors = nullptr;
        std::atomic<size_t>                next{0};   // the next job to take
        std::atomic<size_t>                done{0};   // the jobs that have ended
    };

    void worker_main(unsigned index);
    /// Takes jobs of `r` until none are left (this thread's profile and index are set while it does).
    void work(unsigned index, Run& r);

    unsigned                 threads_;
    std::vector<std::thread> workers_;
    std::mutex               mu_;
    std::condition_variable  wake_, finished_;
    uint64_t                 generation_ = 0;   // counts the runs; guarded by mu_
    bool                     stop_ = false;
    std::shared_ptr<Run>     current_;          // the run in progress (the last one once it has ended); guarded by mu_
    std::vector<TickProfile> profiles_;         // one for each thread
    std::vector<double>      busy_;             // seconds in jobs, one for each thread (written by the thread alone)
    std::atomic<uint64_t>    jobs_{0};
    std::atomic<uint64_t>    worker_jobs_{0};
    uint64_t                 jitter_ = 0;
    std::function<void(unsigned)> gate_;
};

}  // namespace aoc::worldd
