// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  login/login_worker.h
//
//  Runs logins off the network thread: one worker thread takes submitted
//  logins in order, calls the services, and hands each result to `done`
//  (Endpoint::post_login_result).
// ============================================================================
#pragma once

#include "login/login_service.h"
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace aoc { namespace worldd {

class LoginWorker : public LoginQueue {
public:
    using Done = std::function<void(uint64_t session_id, LoginResult result)>;

    LoginWorker(LoginService& service, uint32_t server_id, Done done);
    ~LoginWorker() override;  // finishes the login in progress, drops the rest
    LoginWorker(const LoginWorker&) = delete;
    LoginWorker& operator=(const LoginWorker&) = delete;

    void submit(uint64_t session_id, LoginRequest request) override;

private:
    void run();

    LoginService& service_;
    const uint32_t server_id_;
    Done done_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::pair<uint64_t, LoginRequest>> queue_;
    bool stop_ = false;
    std::thread thread_;
};

}} // namespace aoc::worldd
