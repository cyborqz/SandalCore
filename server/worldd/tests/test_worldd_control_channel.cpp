// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_control_channel.cpp: net/control_channel's login sequence
// ============================================================================
#include "net/control_channel.h"
#include <cstdio>

using namespace aoc::worldd;
namespace conn = aoc::protocol::conn;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

using State = ControlChannel::State;

static conn::ControlMessage msg(conn::Nmt type) {
    conn::ControlMessage m;
    m.type = type;
    return m;
}

static std::optional<conn::ControlMessage> decode(const ControlChannel::Reply& r, size_t i = 0) {
    if (i >= r.messages.size()) return std::nullopt;
    const auto& w = r.messages[i];
    return conn::parse_control_message(w.data(), w.byte_size(), 0, w.bit_pos());
}

static LoginResult accepted() {
    LoginResult r;
    r.accepted = true;
    r.character.name = "cyb";
    return r;
}

static LoginResult refused(const char* text) {
    LoginResult r;
    r.error = text;
    return r;
}

int main() {
    std::printf("login finishes before NMT_Login\n");
    {
        ControlChannel c(WorldInfo{}, "50995344");
        auto r = c.on_message(msg(conn::Nmt::Hello));
        auto m = decode(r);
        CHECK(m && m->type == conn::Nmt::Challenge && m->challenge == "50995344" && !r.close,
              "Hello -> Challenge with our challenge string");
        CHECK(c.state() == State::AwaitLogin, "then waits for Login");
        CHECK(c.on_login(accepted()).messages.empty(), "a login result before Login sends nothing");
        r = c.on_message(msg(conn::Nmt::Login));
        m = decode(r);
        CHECK(m && m->type == conn::Nmt::Welcome && m->level == WorldInfo{}.level &&
              m->game_mode == WorldInfo{}.game_mode && m->redirect_url.empty(),
              "Login -> Welcome with Verra and the AoC game mode");
        CHECK(c.state() == State::AwaitJoin, "then waits for Join");
        conn::ControlMessage speed = msg(conn::Nmt::Netspeed);
        speed.net_speed = 200000;
        CHECK(c.on_message(speed).messages.empty() && c.net_speed() == 200000, "Netspeed is recorded");
        r = c.on_message(msg(conn::Nmt::Join));
        CHECK(r.joined && r.messages.empty() && c.state() == State::Playing, "Join: playing");
    }
// (C Y B | CoA)

    std::printf("login still running at NMT_Login\n");
    {
        ControlChannel c(WorldInfo{}, "X");
        c.on_message(msg(conn::Nmt::Hello));
        auto r = c.on_message(msg(conn::Nmt::Login));
        CHECK(r.messages.empty() && c.state() == State::LoggingIn, "Login waits for the login");
        r = c.on_login(accepted());
        auto m = decode(r);
        CHECK(m && m->type == conn::Nmt::Welcome, "the result sends Welcome");
    }
    {
        ControlChannel c(WorldInfo{}, "X");
        c.on_message(msg(conn::Nmt::Hello));
        c.on_message(msg(conn::Nmt::Login));
        auto r = c.on_login(refused(login_text::kTicketRefused));
        auto m = decode(r);
        CHECK(m && m->type == conn::Nmt::Failure && m->error == login_text::kTicketRefused && r.close,
              "a refusal then sends Failure and closes");
        CHECK(c.state() == State::Closed, "closed");
    }

    std::printf("refused early\n");
    {
        ControlChannel c(WorldInfo{}, "X");
        CHECK(c.on_login(refused(login_text::kTicketRefused)).messages.empty(),
              "a refusal before Hello waits: channel 0 is not open yet");
        auto r = c.on_message(msg(conn::Nmt::Hello));
        auto m = decode(r);
        CHECK(m && m->type == conn::Nmt::Failure && r.close, "Hello then gets Failure instead of Challenge");
    }
    {
        ControlChannel c(WorldInfo{}, "X");
        c.on_message(msg(conn::Nmt::Hello));
        auto r = c.on_login(refused(login_text::kServiceUnavailable));
        auto m = decode(r);
        CHECK(m && m->type == conn::Nmt::Failure && m->error == login_text::kServiceUnavailable && r.close,
              "a refusal after Hello sends Failure at once");
    }

    std::printf("kick and stray messages\n");
    {
        ControlChannel c(WorldInfo{}, "X");
        auto r = c.kick(login_text::kLoggedInElsewhere);
        CHECK(r.messages.empty() && r.close, "a kick before Hello just closes");
    }
    {
        ControlChannel c(WorldInfo{}, "X");
        c.on_message(msg(conn::Nmt::Hello));
        c.on_login(accepted());
        c.on_message(msg(conn::Nmt::Login));
        c.on_message(msg(conn::Nmt::Join));
        auto r = c.kick(login_text::kLoggedInElsewhere);
        auto m = decode(r);
        CHECK(m && m->type == conn::Nmt::Failure && m->error == login_text::kLoggedInElsewhere && r.close,
              "a kick while playing sends Failure and closes");
        CHECK(c.kick("again").messages.empty() && !c.kick("again").close, "a second kick does nothing");
        CHECK(c.on_message(msg(conn::Nmt::Hello)).messages.empty(), "messages after closing are ignored");
    }
    {
        ControlChannel c(WorldInfo{}, "X");
        CHECK(c.on_message(msg(conn::Nmt::Login)).messages.empty() && c.state() == State::AwaitHello,
              "Login before Hello is ignored");
        CHECK(c.on_message(msg(conn::Nmt::Join)).messages.empty(), "Join before Welcome is ignored");
        c.on_message(msg(conn::Nmt::Hello));
        CHECK(c.on_message(msg(conn::Nmt::Hello)).messages.empty(), "a second Hello is ignored");
        CHECK(c.on_message(msg(conn::Nmt::GameSpecific)).messages.empty(), "other messages are ignored");
        c.on_login(accepted());
        CHECK(c.on_login(refused("late")).messages.empty(), "only the first login result counts");
    }
// CoA and C Y B

    std::printf("an accepted login before Hello\n");
    {
        ControlChannel c(WorldInfo{}, "X");
        CHECK(c.on_login(accepted()).messages.empty() && c.state() == State::AwaitHello, "the result waits for Hello");
        auto r = c.on_message(msg(conn::Nmt::Hello));
        auto m = decode(r);
        CHECK(m && m->type == conn::Nmt::Challenge && !r.close && c.state() == State::AwaitLogin,
              "Hello still gets the Challenge");
        r = c.on_message(msg(conn::Nmt::Login));
        m = decode(r);
        CHECK(m && m->type == conn::Nmt::Welcome && c.state() == State::AwaitJoin, "and Login gets Welcome at once");
        CHECK(c.on_message(msg(conn::Nmt::Join)).joined, "Join");
        CHECK(!c.on_message(msg(conn::Nmt::Join)).joined && c.state() == State::Playing,
              "a second Join is not reported again");
    }

    std::printf("after a refusal\n");
    {
        ControlChannel c(WorldInfo{}, "X");
        c.on_message(msg(conn::Nmt::Hello));
        c.on_message(msg(conn::Nmt::Login));
        auto r = c.on_login(refused("why"));
        auto m = decode(r);
        CHECK(m && m->type == conn::Nmt::Failure && m->error == "why" && r.close && c.state() == State::Closed,
              "the refusal's text goes into Failure");
        CHECK(!c.on_message(msg(conn::Nmt::Join)).joined, "Join after Closed is ignored");
        CHECK(c.on_login(accepted()).messages.empty(), "a second result after Closed is ignored");
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
