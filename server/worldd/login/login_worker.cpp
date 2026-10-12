// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  login/login_worker.cpp
// ============================================================================
#include "login/login_worker.h"

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
        queue_.emplace_back(session_id, std::move(request));
    }
    cv_.notify_one();
}

void LoginWorker::run() {
    for (;;) {
        std::pair<uint64_t, LoginRequest> job;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        done_(job.first, service_.login(job.second, server_id_));
    }
}

}} // namespace aoc::worldd
