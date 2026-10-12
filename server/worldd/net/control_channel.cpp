// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/control_channel.cpp
// ============================================================================
#include "net/control_channel.h"
#include <spdlog/spdlog.h>

namespace aoc { namespace worldd {

namespace conn = protocol::conn;
namespace emit = protocol::emit;

ControlChannel::Reply ControlChannel::refuse(const std::string& text) {
    Reply r;
    emit::BunchWriter w;
    conn::write_nmt_failure(w, text);
    r.messages.push_back(std::move(w));
    r.close = true;
    state_ = State::Closed;
    return r;
}

ControlChannel::Reply ControlChannel::welcome_or_refuse() {
    if (!login_->accepted) return refuse(login_->error);
    Reply r;
    emit::BunchWriter w;
    conn::write_nmt_welcome(w, world_.level, world_.game_mode, "");
    r.messages.push_back(std::move(w));
    state_ = State::AwaitJoin;
    return r;
}

ControlChannel::Reply ControlChannel::on_message(const conn::ControlMessage& m) {
    switch (m.type) {
    case conn::Nmt::Hello:
        if (state_ != State::AwaitHello) break;
        if (login_ && !login_->accepted) return refuse(login_->error);
        {
            Reply r;
            emit::BunchWriter w;
            conn::write_nmt_challenge(w, challenge_);
            r.messages.push_back(std::move(w));
            state_ = State::AwaitLogin;
            return r;
        }
    case conn::Nmt::Login:
        if (state_ != State::AwaitLogin) break;
        login_message_ = LoginMessage{m.request_url, m.has_unique_id, m.unique_id};
        if (login_) return welcome_or_refuse();
        state_ = State::LoggingIn;
        return {};
    case conn::Nmt::Netspeed:
        net_speed_ = m.net_speed;
        return {};
    case conn::Nmt::Join:
        if (state_ != State::AwaitJoin) break;
        state_ = State::Playing;
        {
            Reply r;
            r.joined = true;
            return r;
        }
    default:
        break;
    }
    spdlog::debug("[control] ignoring NMT_{} in state {}", conn::nmt_name(m.type), static_cast<int>(state_));
    return {};
}
// -- C Y B x CoA --

ControlChannel::Reply ControlChannel::on_login(const LoginResult& result) {
    if (login_ || state_ == State::Closed) return {};
    login_ = result;
    if (state_ == State::LoggingIn) return welcome_or_refuse();
    if (state_ == State::AwaitLogin && !result.accepted) return refuse(result.error);
    return {};  // AwaitHello: answered when Hello comes
}

ControlChannel::Reply ControlChannel::kick(const std::string& text) {
    if (state_ == State::Closed) return {};
    if (state_ == State::AwaitHello) {
        state_ = State::Closed;
        Reply r;
        r.close = true;
        return r;
    }
    return refuse(text);
}

}} // namespace aoc::worldd
