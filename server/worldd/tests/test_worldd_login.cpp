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
#include <map>
#include <mutex>
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

/// A gateway that redeems any ticket for the character it is asked for, and counts the redeems of each ticket.
struct EchoGateway : svc::GatewayApi {
    std::mutex mu;
    std::map<std::string, int> redeems;
    svc::RedeemResult redeem_ticket(const std::string& ticket, const std::string& character, uint32_t) override {
        std::lock_guard<std::mutex> lock(mu);
        ++redeems[ticket];
        return svc::RedeemResult{svc::CallStatus::Ok, 3, character};
    }
    int redeemed(const std::string& ticket) {
        std::lock_guard<std::mutex> lock(mu);
        return redeems[ticket];
    }
};

/// A character service that answers any character and counts the loads of each.
struct EchoCharacters : svc::CharacterApi {
    std::mutex mu;
    std::map<std::string, int> loads;
    svc::CharacterResult get_character(const std::string& id) override {
        std::lock_guard<std::mutex> lock(mu);
        ++loads[id];
        svc::CharacterResult result;
        result.status = svc::CallStatus::Ok;
        result.character.id = id;
        result.character.account_id = 3;
        result.character.name = "echo";
        return result;
    }
    svc::CallStatus save_position(const std::string&, const svc::Position&) override { return svc::CallStatus::Ok; }
    int loaded(const std::string& id) {
        std::lock_guard<std::mutex> lock(mu);
        return loads[id];
    }
};

/// What a worker handed back, by session id.
struct Handed {
    std::mutex mu;
    std::map<uint64_t, LoginResult> results;
    bool has(uint64_t id) {
        std::lock_guard<std::mutex> lock(mu);
        return results.count(id) != 0;
    }
    LoginResult get(uint64_t id) {
        std::lock_guard<std::mutex> lock(mu);
        return results[id];
    }
    bool wait_for(uint64_t id, std::chrono::milliseconds how_long) {
        const auto until = std::chrono::steady_clock::now() + how_long;
        while (!has(id) && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return has(id);
    }
};

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

    std::printf("barrier\n");
    {
        // the log-in waits for the world to say the character has nothing unanswered, then loads
        FakeGateway gw;
        FakeCharacters ch;
        LoginService svc(gw, ch);
        std::atomic<bool> pending{true};
        std::atomic<int> asked{0};
        LoginService::Barrier barrier;
        barrier.writes_pending = [&](const std::string& id) {
            ++asked;
            return pending.load() && id == kCharacter;
        };
        barrier.bound = std::chrono::seconds(5);
        barrier.poll = std::chrono::milliseconds(2);
        svc.set_barrier(barrier);
        std::thread release([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            CHECK(ch.calls == 0, "the character is not loaded while something of it is unanswered");
            pending = false;
        });
        const auto began = std::chrono::steady_clock::now();
        const LoginResult r = svc.login(request(), 7);
        release.join();
        CHECK(r.accepted && ch.calls == 1, "the log-in goes on and loads once the world says it is clear");
        CHECK(std::chrono::steady_clock::now() - began >= std::chrono::milliseconds(70) && asked > 2, "after waiting for it, asking again each time");
        CHECK(gw.calls == 1, "the ticket was redeemed first");
    }
    {
        // past the bound the log-in is refused with the try-again text and nothing is loaded
        FakeGateway gw;
        FakeCharacters ch;
        LoginService svc(gw, ch);
        LoginService::Barrier barrier;
        barrier.writes_pending = [](const std::string&) { return true; };
        barrier.bound = std::chrono::milliseconds(100);
        barrier.poll = std::chrono::milliseconds(2);
        svc.set_barrier(barrier);
        const auto began = std::chrono::steady_clock::now();
        const LoginResult r = svc.login(request(), 7);
        CHECK(!r.accepted && r.error == login_text::kServiceUnavailable, "past the bound it is refused with the try-again text");
        CHECK(ch.calls == 0, "and nothing is loaded");
        CHECK(std::chrono::steady_clock::now() - began >= std::chrono::milliseconds(90), "after waiting for the bound");
    }
    {
        // a character with nothing unanswered is not held at all; a log-in that is refused before the character is read never asks the barrier
        FakeGateway gw;
        FakeCharacters ch;
        LoginService svc(gw, ch);
        std::atomic<int> asked{0};
        LoginService::Barrier barrier;
        barrier.writes_pending = [&](const std::string&) {
            ++asked;
            return false;
        };
        svc.set_barrier(barrier);
        const auto began = std::chrono::steady_clock::now();
        CHECK(svc.login(request(), 7).accepted && asked == 1 && ch.calls == 1, "a clear character is loaded at once");
        CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds(2), "without a wait");
        CHECK(!svc.login({kCharacter, "not-hex"}, 7).accepted && asked == 1, "a malformed ticket does not reach the barrier");
        gw.result.status = svc::CallStatus::Refused;
        CHECK(!svc.login(request(), 7).accepted && asked == 1, "a refused ticket does not hold this thread: the barrier is not asked");
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

    std::printf("worker and barrier\n");
    {
        // A's character has an unanswered claim, B's has none: the one log-in thread serves B while A waits, and A goes on when its claim is answered
        const std::string char_a = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", char_b = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
        const std::string ticket_a = "11111111111111111111111111111111", ticket_b = "22222222222222222222222222222222";
        EchoGateway gw;
        EchoCharacters ch;
        LoginService svc(gw, ch);
        std::atomic<bool> a_held{true};
        LoginService::Barrier barrier;
        barrier.writes_pending = [&](const std::string& id) { return id == char_a && a_held.load(); };
        barrier.bound = std::chrono::seconds(10);
        barrier.poll = std::chrono::milliseconds(5);
        svc.set_barrier(barrier);
        Handed handed;
        LoginWorker w(svc, 9, [&](uint64_t id, LoginResult r) {
            std::lock_guard<std::mutex> lock(handed.mu);
            handed.results[id] = std::move(r);
        });
        w.submit(1, {char_a, ticket_a});
        w.submit(2, {char_b, ticket_b});
        CHECK(handed.wait_for(2, std::chrono::seconds(2)), "B is answered while A waits");
        CHECK(handed.has(2) && handed.get(2).accepted && ch.loaded(char_b) == 1, "B is accepted and loaded once");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK(!handed.has(1) && ch.loaded(char_a) == 0, "A has no answer yet and nothing of it is loaded");
        CHECK(gw.redeemed(ticket_a) == 1, "A's ticket was redeemed once, however many times it was looked at");
        a_held = false;
        CHECK(handed.wait_for(1, std::chrono::seconds(2)), "A is answered once its claim is");
        CHECK(handed.get(1).accepted && ch.loaded(char_a) == 1 && gw.redeemed(ticket_a) == 1, "A is accepted, loaded once, its ticket redeemed once");
    }
    {
        // past the bound A is refused with the try-again text and nothing is loaded; B is not held up by it
        const std::string char_a = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", char_b = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
        const std::string ticket_a = "11111111111111111111111111111111", ticket_b = "22222222222222222222222222222222";
        EchoGateway gw;
        EchoCharacters ch;
        LoginService svc(gw, ch);
        LoginService::Barrier barrier;
        barrier.writes_pending = [&](const std::string& id) { return id == char_a; };
        barrier.bound = std::chrono::milliseconds(200);
        barrier.poll = std::chrono::milliseconds(5);
        svc.set_barrier(barrier);
        Handed handed;
        LoginWorker w(svc, 9, [&](uint64_t id, LoginResult r) {
            std::lock_guard<std::mutex> lock(handed.mu);
            handed.results[id] = std::move(r);
        });
        const auto began = std::chrono::steady_clock::now();
        w.submit(1, {char_a, ticket_a});
        w.submit(2, {char_b, ticket_b});
        CHECK(handed.wait_for(2, std::chrono::seconds(2)) && std::chrono::steady_clock::now() - began < std::chrono::milliseconds(150), "B is answered long before A's bound");
        CHECK(handed.wait_for(1, std::chrono::seconds(5)), "A is answered at its bound");
        CHECK(!handed.get(1).accepted && handed.get(1).error == login_text::kServiceUnavailable, "A is refused with the try-again text");
        CHECK(std::chrono::steady_clock::now() - began >= std::chrono::milliseconds(180), "after waiting for the bound");
        CHECK(ch.loaded(char_a) == 0 && gw.redeemed(ticket_a) == 1, "nothing of A is loaded and its ticket was redeemed once");
    }

    logger->flush();
    CHECK(log.str().find(kTicket) == std::string::npos, "the ticket never appears in the log");
    CHECK(log.str().find(kCharacter) != std::string::npos, "(the character id does)");

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
