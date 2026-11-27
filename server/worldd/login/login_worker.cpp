// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  login/login_worker.cpp
// ============================================================================
#include "login/login_worker.h"

#include <algorithm>
#include <chrono>

namespace aoc { namespace worldd {

LoginWorker::LoginWorker(LoginService& service, uint32_t server_id, Done done)
    : service_(service), server_id_(server_id), done_(std::move(done)), thread_([this] { run(); }) {}

LoginWorker::~LoginWorker() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void LoginWorker::submit(uint64_t session_id, LoginRequest request) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        LoginService::Attempt attempt;
        attempt.session_id = session_id;
        attempt.request = std::move(request);
        queue_.push_back(std::move(attempt));
    }
    cv_.notify_one();
}

void LoginWorker::run() {
    std::unique_lock<std::mutex> lock(mu_);
    for (;;) {
        if (stop_) return;
        // The first login that may be looked at now: one that waits for its character is passed over until its time.
        const auto now = std::chrono::steady_clock::now();
        auto wake = std::chrono::steady_clock::time_point::max();
        auto pick = queue_.end();
        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
            if (it->not_before <= now) {
                pick = it;
                break;
            }
            wake = std::min(wake, it->not_before);
        }
        if (pick == queue_.end()) {
            if (wake == std::chrono::steady_clock::time_point::max()) cv_.wait(lock);
            else cv_.wait_until(lock, wake);
            continue;
        }
        LoginService::Attempt attempt = std::move(*pick);
        queue_.erase(pick);
        lock.unlock();
        LoginResult result;
        const bool finished = service_.advance(attempt, server_id_, result);
        if (finished) done_(attempt.session_id, std::move(result));
        lock.lock();
        if (!finished) queue_.push_back(std::move(attempt));   // (it waits for its character: the others go first)
    }
}

}} // namespace aoc::worldd
