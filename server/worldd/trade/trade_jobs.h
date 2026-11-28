// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  trade/trade_jobs.h
//
//  Asynchronous worker for atomic two-character player trade commits.
//  Owned by PlayerWorld, off the network/world tick thread:
//    - single worker thread executing PositionSink::commit_player_trade_now
//    - thread-safe saving(character_id) tracking open commits until settlement
//    - Dekker-safe login handshake preventing a commit from racing a pending login
//    - non-head-of-line exponential retry on unclear gRPC outcomes (Unavailable/Failed)
//    - fail-closed undecided answer when retries are exhausted (never overwrites DB)
// ============================================================================
#pragma once

#include "session/position_saver.h"
#include "svc/services.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace aoc::worldd::trade {

struct TradeJobsConfig {
    int                       max_attempts = 6;
    std::chrono::milliseconds retry_delay{500};
    std::chrono::milliseconds retry_cap{4000};
};

struct TradeCommitAnswer {
    svc::CallStatus status = svc::CallStatus::Failed;
    bool            replayed = false;
    bool            undecided = false;   // retries exhausted on unclear status; caller must fail closed
    int             attempts = 0;
    std::string     reason;
};

class TradeJobs {
public:
    using LoginCheck = std::function<bool(const std::string&)>;

    explicit TradeJobs(TradeJobsConfig config = {});
    ~TradeJobs();
    TradeJobs(const TradeJobs&) = delete;
    TradeJobs& operator=(const TradeJobs&) = delete;

    /// Installs the predicate checked under gate_mu_ after incrementing open_commits_ (Dekker handshake with login_began).
    void set_login_check(LoginCheck check);

    /// Updates retry config (takes effect for subsequent retries; intended for tests before commits run).
    void set_config(TradeJobsConfig config);

    /// Submits an atomic CommitPlayerTrade job for (req.side_a.character_id, req.side_b.character_id).
    /// Increments open_commits_ for both characters BEFORE checking login_pending_; if either login is pending
    /// (or sink is null / worker stopped), decrements both and returns 0.
    uint64_t submit(svc::CommitTradeRequest request, PositionSink* sink);

    /// Takes a settled commit answer by ticket; returns false if still in flight or queued.
    bool take(uint64_t ticket, TradeCommitAnswer& out);

    /// Releases the open_commits_ hold for both characters AFTER synchronous in-memory swap or orphan save handoff.
    void release(const std::string& char_a, const std::string& char_b);

    /// Thread-safe: true while at least one unsettled trade commit involves character_id.
    bool saving(const std::string& character_id) const;

    /// Thread-safe: true while any trade commit remains unsettled.
    bool has_open_commits() const;

    /// Waits up to `max_wait` until either a finished answer is ready in `done_` or no job is queued/in flight.
    void wait_for_progress(std::chrono::milliseconds max_wait);

    /// Stops the worker thread cleanly (called automatically by destructor).
    void stop();

private:
    struct PendingJob {
        uint64_t                              ticket = 0;
        svc::CommitTradeRequest               request;
        PositionSink*                         sink = nullptr;
        int                                   attempts = 0;
        std::chrono::steady_clock::time_point ready_at{};
    };

    void run();
    std::chrono::milliseconds backoff_for(int attempt) const;

    mutable std::mutex             gate_mu_;
    std::map<std::string, size_t>  open_commits_;
    LoginCheck                     login_pending_;

    mutable std::mutex                     mu_;
    TradeJobsConfig                        config_;
    std::condition_variable                cv_;
    std::condition_variable                done_cv_;
    std::deque<PendingJob>                 queue_;
    std::map<uint64_t, TradeCommitAnswer>  done_;
    uint64_t                               next_ticket_ = 1;
    bool                                   in_flight_ = false;
    bool                                   stop_ = false;
    std::thread                            thread_;
};

}  // namespace aoc::worldd::trade
