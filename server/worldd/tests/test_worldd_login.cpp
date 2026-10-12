// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_login.cpp: login/login_service with fake services
//
//  Every outcome of redeem + load, the text the player gets, and that the
//  ticket never reaches the log.
// ============================================================================
#include "login/login_service.h"
#include "login/login_worker.h"
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <sstream>
#include <thread>

using namespace aoc::worldd;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static const std::string kCharacter = "bbec5c86eb101fe5f38c87f0fa22e7d0";
static const std::string kTicket = "0123456789abcdef0123456789abcdef";

struct FakeGateway : svc::GatewayApi {
    svc::RedeemResult result{svc::CallStatus::Ok, 3, kCharacter};
    int calls = 0;
    std::string ticket, character;
    uint32_t server = 0;
    svc::RedeemResult redeem_ticket(const std::string& t, const std::string& c, uint32_t s) override {
        ++calls;
        ticket = t;
        character = c;
        server = s;
        return result;
    }
};
// (C.Y.B ~ C.O.A)

struct FakeCharacters : svc::CharacterApi {
    svc::CharacterResult result;
    int calls = 0;
    FakeCharacters() {
        result.status = svc::CallStatus::Ok;
        result.character.id = kCharacter;
        result.character.account_id = 3;
        result.character.name = "cyb";
    }
    svc::CharacterResult get_character(const std::string&) override {
        ++calls;
        return result;
    }
    svc::CallStatus save_position(const std::string&, const svc::Position&) override { return svc::CallStatus::Ok; }
};

static LoginRequest request() { return {kCharacter, kTicket}; }

int main() {
    std::ostringstream log;
    auto logger = std::make_shared<spdlog::logger>("test", std::make_shared<spdlog::sinks::ostream_sink_mt>(log));
    logger->set_level(spdlog::level::trace);
    spdlog::set_default_logger(logger);

    std::printf("accepted\n");
    {
        FakeGateway gw;
        FakeCharacters ch;
        LoginService svc(gw, ch);
        const LoginResult r = svc.login(request(), 7);
        CHECK(r.accepted && r.character.name == "cyb" && r.error.empty(), "a good ticket logs cyb in");
        CHECK(gw.ticket == kTicket && gw.character == kCharacter && gw.server == 7,
              "the gateway gets the ticket, the character and this server's id");
    }

    std::printf("refused\n");
    auto refused_with = [](svc::CallStatus redeem, svc::CallStatus load, const char* want, const char* msg) {
        FakeGateway gw;
        FakeCharacters ch;
        gw.result.status = redeem;
        ch.result.status = load;
        LoginService svc(gw, ch);
        const LoginResult r = svc.login(request(), 7);
        CHECK(!r.accepted && r.error == want, msg);
        return ch.calls;
    };
    int loads = refused_with(svc::CallStatus::Refused, svc::CallStatus::Ok, login_text::kTicketRefused,
                             "a refused ticket: the ticket text");
    CHECK(loads == 0, "and the character is not loaded");
    refused_with(svc::CallStatus::NotFound, svc::CallStatus::Ok, login_text::kTicketRefused,
                 "an unknown ticket: the ticket text");
    refused_with(svc::CallStatus::Unavailable, svc::CallStatus::Ok, login_text::kServiceUnavailable,
                 "gateway unreachable: try again shortly");
    refused_with(svc::CallStatus::Failed, svc::CallStatus::Ok, login_text::kLoginFailed,
                 "gateway error: login failed");
    refused_with(svc::CallStatus::Ok, svc::CallStatus::NotFound, login_text::kCharacterMissing,
                 "the character was deleted: character text");
    refused_with(svc::CallStatus::Ok, svc::CallStatus::Unavailable, login_text::kServiceUnavailable,
                 "character service unreachable: try again shortly");
    {
        FakeGateway gw;
        FakeCharacters ch;
        gw.result.character_id = "ffffffffffffffffffffffffffffffff";
        LoginService svc(gw, ch);
        CHECK(!svc.login(request(), 7).accepted, "a ticket redeemed for another character is refused");
    }
    {
        FakeGateway gw;
        FakeCharacters ch;
        ch.result.character.account_id = 4;
        LoginService svc(gw, ch);
        CHECK(!svc.login(request(), 7).accepted, "a character of another account is refused");
    }
    {
        FakeGateway gw;
        FakeCharacters ch;
        LoginService svc(gw, ch);
        CHECK(!svc.login({kCharacter, "not-hex"}, 7).accepted && gw.calls == 0,
              "a malformed ticket is refused without asking the gateway");
        CHECK(!svc.login({"ABC", kTicket}, 7).accepted && gw.calls == 0,
              "a malformed character id too");
    }

    std::printf("worker\n");
    {
        FakeGateway gw;
        FakeCharacters ch;
        LoginService svc(gw, ch);
        std::atomic<int> done{0};
        uint64_t got_id = 0;
        bool got_ok = false;
        {
            LoginWorker w(svc, 9, [&](uint64_t id, LoginResult r) {
                got_id = id;
                got_ok = r.accepted;
                ++done;
            });
            w.submit(42, request());
            for (int i = 0; i < 200 && done == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(done == 1 && got_id == 42 && got_ok && gw.server == 9,
              "the worker runs the login and reports it with its session id");
    }

    logger->flush();
    CHECK(log.str().find(kTicket) == std::string::npos, "the ticket never appears in the log");
    CHECK(log.str().find(kCharacter) != std::string::npos, "(the character id does)");

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
