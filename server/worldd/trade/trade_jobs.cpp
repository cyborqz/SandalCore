// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  trade/trade_jobs.cpp
// ============================================================================
#include "trade/trade_jobs.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>

namespace aoc::worldd::trade {

TradeJobs::TradeJobs(TradeJobsConfig config) : config_(config), thread_([this] { run(); }) {}

TradeJobs::~TradeJobs() { stop(); }

void TradeJobs::stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stop_) return;
        stop_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    done_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void TradeJobs::set_login_check(LoginCheck check) {
    std::lock_guard<std::mutex> lock(gate_mu_);
    login_pending_ = std::move(check);
}

void TradeJobs::set_config(TradeJobsConfig config) {
    std::lock_guard<std::mutex> lock(mu_);
    config_ = config;
}

uint64_t TradeJobs::submit(svc::CommitTradeRequest request, PositionSink* sink) {
    if (!sink || request.side_a.character_id.empty() || request.side_b.character_id.empty()) return 0;

    const std::string char_a = request.side_a.character_id;
    const std::string char_b = request.side_b.character_id;

    // Dekker-safe handshake: mark both characters as saving BEFORE checking login_pending_.
    {
        std::lock_guard<std::mutex> lock(gate_mu_);
        ++open_commits_[char_a];
        ++open_commits_[char_b];
        if (login_pending_ && (login_pending_(char_a) || login_pending_(char_b))) {
            auto dec = [this](const std::string& id) {
                const auto it = open_commits_.find(id);
                if (it == open_commits_.end()) return;
                if (--it->second == 0) open_commits_.erase(it);
            };
            dec(char_a);
            dec(char_b);
            return 0;
        }
    }

    uint64_t ticket = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stop_) {
            // Fall through to release outside mu_.
        } else {
            ticket = next_ticket_++;
            PendingJob job;
            job.ticket = ticket;
            job.request = std::move(request);
            job.sink = sink;
            job.ready_at = std::chrono::steady_clock::now();
            queue_.push_back(std::move(job));
        }
    }
    if (ticket == 0) {
        release(char_a, char_b);
        return 0;
    }
    cv_.notify_one();
    return ticket;
}

bool TradeJobs::take(uint64_t ticket, TradeCommitAnswer& out) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = done_.find(ticket);
    if (it == done_.end()) return false;
    out = std::move(it->second);
    done_.erase(it);
    return true;
}

void TradeJobs::release(const std::string& char_a, const std::string& char_b) {
    std::lock_guard<std::mutex> lock(gate_mu_);
    auto dec = [this](const std::string& id) {
        const auto it = open_commits_.find(id);
        if (it == open_commits_.end()) return;
        if (--it->second == 0) open_commits_.erase(it);
    };
    dec(char_a);
    dec(char_b);
}

bool TradeJobs::saving(const std::string& character_id) const {
    std::lock_guard<std::mutex> lock(gate_mu_);
    const auto it = open_commits_.find(character_id);
    return it != open_commits_.end() && it->second > 0;
}

bool TradeJobs::has_open_commits() const {
    std::lock_guard<std::mutex> lock(gate_mu_);
    return !open_commits_.empty();
}

void TradeJobs::wait_for_progress(std::chrono::milliseconds max_wait) {
    std::unique_lock<std::mutex> lock(mu_);
    done_cv_.wait_for(lock, max_wait, [this] {
        return stop_ || !done_.empty() || (queue_.empty() && !in_flight_);
    });
}

std::chrono::milliseconds TradeJobs::backoff_for(int attempt) const {
    // Expects caller holds mu_.
    const int64_t base = std::max<int64_t>(0, config_.retry_delay.count());
    const int64_t cap = std::max<int64_t>(base, config_.retry_cap.count());
    const int shift = std::clamp(attempt - 1, 0, 20);
    int64_t delay = base;
    for (int i = 0; i < shift; ++i) {
        if (delay >= cap) {
            delay = cap;
            break;
        }
        delay = std::min(cap, delay * 2);
    }
    return std::chrono::milliseconds(delay);
}

void TradeJobs::run() {
    for (;;) {
        PendingJob job;
        {
            std::unique_lock<std::mutex> lock(mu_);
            for (;;) {
                if (stop_) return;
                if (queue_.empty()) {
                    cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
                    continue;
                }
                const auto now = std::chrono::steady_clock::now();
                // Non-HOL scan: pick the first ready job so a backing-off retry never blocks other pairs.
                auto ready_it = queue_.end();
                auto earliest = queue_.front().ready_at;
                for (auto it = queue_.begin(); it != queue_.end(); ++it) {
                    if (it->ready_at <= now) {
                        ready_it = it;
                        break;
                    }
                    if (it->ready_at < earliest) earliest = it->ready_at;
                }
                if (ready_it != queue_.end()) {
                    job = std::move(*ready_it);
                    queue_.erase(ready_it);
                    in_flight_ = true;
                    break;
                }
                cv_.wait_until(lock, earliest);
            }
        }

        ++job.attempts;
        svc::CommitTradeResult res = job.sink->commit_player_trade_now(job.request);

        const bool unclear = (res.status == svc::CallStatus::Unavailable || res.status == svc::CallStatus::Failed);

        {
            std::lock_guard<std::mutex> lock(mu_);
            in_flight_ = false;
            const int max_attempts = std::max(1, config_.max_attempts);
            if (unclear && !stop_ && job.attempts < max_attempts) {
                const auto delay = backoff_for(job.attempts);
                spdlog::warn("[trade] commit {} ({} <-> {}) attempt {}/{} unclear ({}: {}); retrying in {} ms",
                             job.request.trade_id, job.request.side_a.character_id, job.request.side_b.character_id,
                             job.attempts, max_attempts, svc::call_status_name(res.status), res.reason, delay.count());
                job.ready_at = std::chrono::steady_clock::now() + delay;
                queue_.push_back(std::move(job));
                cv_.notify_one();
                continue;
            }

            TradeCommitAnswer answer;
            answer.status = res.status;
            answer.replayed = res.replayed;
            answer.attempts = job.attempts;
            answer.reason = std::move(res.reason);
            if (unclear) {
                answer.undecided = true;
                spdlog::critical("[trade] commit {} ({} <-> {}, items {}/{}, copper {}/{}) exhausted {} attempt(s) ({}: {}); failing closed without overwriting database",
                                 job.request.trade_id, job.request.side_a.character_id, job.request.side_b.character_id,
                                 job.request.items_a_to_b, job.request.items_b_to_a,
                                 job.request.copper_a_to_b, job.request.copper_b_to_a,
                                 job.attempts, svc::call_status_name(answer.status), answer.reason);
            }
            done_[job.ticket] = std::move(answer);
        }
        done_cv_.notify_all();
    }
}

}  // namespace aoc::worldd::trade
