// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_endpoint.cpp: net/endpoint driven in memory
//
//  The handshake starts from the retail client's captured Initial (character
//  cyb); from there a fake client takes the session through NMT.  Logins are
//  answered by hand through post_login_result.
// ============================================================================
#include "capture_packets.h"
#include "fake_client.h"
#include "net/endpoint.h"
#include "protocol/wire/ue5_primitives.h"
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <cstdio>
#include <sstream>

using namespace aoc::worldd;
namespace conn = aoc::protocol::conn;
using std::chrono::milliseconds;
using std::chrono::seconds;

static std::ostringstream g_log;  // what the endpoint logs (main points spdlog at it)
static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static const std::string kCyb = "bbec5c86eb101fe5f38c87f0fa22e7d0";
static const std::string kTicket = "9ddc8eac805cc2fc3e75df52b74613c8";  // the captured Initial's
static const Clock::time_point t0{seconds(5000)};
static const Address kA{0x7F000001u, 50510};
static const Address kB{0x7F000001u, 50511};
static const Address kC{0x7F000001u, 50512};

static uint8_t g_counter = 1;
static void counting_random(uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; ++i) out[i] = g_counter++;
}
// C | O | A - Cyb

struct FakeLogins : LoginQueue {
    std::vector<std::pair<uint64_t, LoginRequest>> submitted;
    void submit(uint64_t id, LoginRequest r) override { submitted.emplace_back(id, std::move(r)); }
};

struct Rig {
    CookieSigner signer{counting_random};
    FakeLogins logins;
    std::vector<std::pair<Address, std::vector<uint8_t>>> sent;
    Endpoint ep;

    explicit Rig(EndpointConfig cfg = {})
        : ep(std::move(cfg), signer, logins,
             [this](const Address& a, const std::vector<uint8_t>& p) { sent.emplace_back(a, p); },
             counting_random, t0) {}

    std::vector<std::vector<uint8_t>> take(const Address& a) {
        std::vector<std::vector<uint8_t>> out;
        std::vector<std::pair<Address, std::vector<uint8_t>>> rest;
        for (auto& [to, p] : sent) {
            if (to == a) out.push_back(p);
            else rest.emplace_back(to, p);
        }
        sent.swap(rest);
        return out;
    }
};

/// A client that has finished the handshake with the rig at `addr`.
struct Joined {
    std::optional<conn::HandshakePacket> challenge;
    std::optional<conn::HandshakePacket> ack;
    std::vector<uint8_t> response;  // the Response bytes, for replays
    std::optional<fake::Client> client;
    uint64_t session = 0;
};

static std::optional<conn::HandshakePacket> parse(const std::vector<uint8_t>& b) {
    return conn::parse_handshake(b.data(), b.size());
}

/// The captured Initial with the ticket's first byte, and the character id's, flipped by the masks (the
/// extension holds the character id at byte 22 and the ticket at 54; both are echoed in the Challenge).
static std::vector<uint8_t> initial_with(uint8_t ticket_mask, uint8_t character_mask) {
    const auto bytes = capture::bytes(capture::kInitial);
    auto p = conn::parse_handshake(bytes.data(), bytes.size());
    p->ext->bytes[22] ^= character_mask;
    p->ext->bytes[54] ^= ticket_mask;
    return conn::build_handshake(*p);
}

/// Initial -> Challenge -> Response -> Ack from `addr`, starting from `initial` (the captured one by default).
static Joined handshake(Rig& rig, const Address& addr, Clock::time_point now,
                        std::vector<uint8_t> initial = capture::bytes(capture::kInitial)) {
    Joined j;
    rig.take(addr);  // what an earlier session of this address left unread
    rig.ep.on_datagram(addr, initial.data(), initial.size(), now);
    auto out = rig.take(addr);
    if (out.size() != 1) return j;
    j.challenge = parse(out[0]);
    conn::HandshakePacket resp = *j.challenge;
    resp.type = conn::HandshakeType::Response;
    resp.sent_count = 2;
    resp.padding.assign(11, 0x5A);
    j.response = conn::build_handshake(resp);
    rig.ep.on_datagram(addr, j.response.data(), j.response.size(), now);
    out = rig.take(addr);
    if (out.size() != 1) return j;
    j.ack = parse(out[0]);
    const auto seqs = conn::initial_sequences(j.challenge->cookie);
    j.client.emplace(conn::outer_header_of(*j.challenge), j.challenge->ext->custom_field(), seqs.server, seqs.client);
    j.session = rig.logins.submitted.empty() ? 0 : rig.logins.submitted.back().first;
    return j;
}

/// What the client gets of the packets sent to it so far.
static std::vector<fake::Received> drain(Rig& rig, Joined& j, const Address& addr) {
    std::vector<fake::Received> got;
    for (const auto& p : rig.take(addr)) got.push_back(j.client->receive(p));
    return got;
}

/// Sends `bunches` from the client, ticks, and returns what the client got.
static std::vector<fake::Received> exchange(Rig& rig, Joined& j, const Address& addr,
                                            std::vector<fake::Bunch> bunches, Clock::time_point now) {
    const auto pkt = j.client->packet(std::move(bunches));
    rig.ep.on_datagram(addr, pkt.data(), pkt.size(), now);
    rig.ep.tick(now);
    return drain(rig, j, addr);
}
// ~ C-Y-B :: C | O | A ~

static bool has(const std::vector<fake::Received>& got, conn::Nmt type, bool* closed = nullptr) {
    bool found = false;
    for (const auto& r : got) {
        for (const auto& m : r.messages) found = found || m.type == type;
        if (closed && r.closed) *closed = true;
    }
    return found;
}

static bool any_valid(const std::vector<fake::Received>& got) {
    for (const auto& r : got)
        if (r.valid) return true;
    return false;
}

static LoginResult accepted() {
    LoginResult r;
    r.accepted = true;
    r.character.id = kCyb;
    r.character.name = "cyb";
    r.character.account_id = 3;
    return r;
}

/// Takes `j` through Hello, an accepted login and Login -> Welcome.
static void log_in(Rig& rig, Joined& j, const Address& addr, Clock::time_point now) {
    exchange(rig, j, addr, {j.client->hello()}, now);
    rig.ep.post_login_result(j.session, accepted());
    rig.ep.tick(now + milliseconds(1));
    drain(rig, j, addr);
    exchange(rig, j, addr, {j.client->login()}, now + milliseconds(2));
}

static void test_handshake() {
    std::printf("handshake\n");
    Rig rig;
    const auto initial = capture::bytes(capture::kInitial);
    rig.ep.on_datagram(kA, initial.data(), initial.size(), t0);
    auto out = rig.take(kA);
    const auto ch = out.size() == 1 ? parse(out[0]) : std::nullopt;
    CHECK(ch && ch->type == conn::HandshakeType::Challenge && out[0].size() == 153,
          "Initial -> a 153-byte Challenge, as the original server sent");
    CHECK(ch && ch->ext && ::ue5::guid_to_hex(ch->ext->character_id().data()) == kCyb &&
          ::ue5::guid_to_hex(ch->ext->ticket().data()) == kTicket && ch->ext->spawn()[0] == -777664.5,
          "the Challenge echoes the character id and ticket, with the Lyneth spawn");
    CHECK(ch && rig.signer.verify(ch->secret_id, ch->timestamp, kA.to_string(), ch->cookie, 1.0),
          "its cookie verifies for this address");
    CHECK(rig.ep.session_count() == 0, "no state is kept for an Initial");

    Rig rig2;
    Joined j = handshake(rig2, kA, t0);
    CHECK(j.ack && j.ack->type == conn::HandshakeType::Ack && j.ack->timestamp == -1.0 &&
          j.ack->cookie == j.challenge->cookie,
          "Response -> Ack: timestamp -1, the same cookie");
    CHECK(rig2.ep.session_count() == 1, "one session");
    CHECK(rig2.logins.submitted.size() == 1 && rig2.logins.submitted[0].second.character_id == kCyb &&
          rig2.logins.submitted[0].second.ticket == kTicket,
          "the login starts with cyb's id and the ticket, as hex");

    conn::HandshakePacket resp = *j.challenge;
    resp.type = conn::HandshakeType::Response;
    resp.sent_count = 2;
    const auto again = conn::build_handshake(resp);
    rig2.ep.on_datagram(kA, again.data(), again.size(), t0 + milliseconds(100));
    auto acks = rig2.take(kA);
    CHECK(acks.size() == 1 && parse(acks[0])->type == conn::HandshakeType::Ack && rig2.ep.session_count() == 1 &&
          rig2.logins.submitted.size() == 1,
          "a repeated Response gets the Ack again, no second session or login");

    conn::HandshakePacket forged = resp;
    forged.cookie[0] ^= 0xFF;
    const auto forged_bytes = conn::build_handshake(forged);
    rig2.ep.on_datagram(kB, forged_bytes.data(), forged_bytes.size(), t0);
    CHECK(rig2.take(kB).empty() && rig2.ep.session_count() == 1, "a Response with a forged cookie is ignored");

    Rig rig3;
    rig3.ep.on_datagram(kB, initial.data(), initial.size(), t0);
    auto chal = parse(rig3.take(kB)[0]);
    conn::HandshakePacket late = *chal;
    late.type = conn::HandshakeType::Response;
    const auto late_bytes = conn::build_handshake(late);
    rig3.ep.on_datagram(kB, late_bytes.data(), late_bytes.size(), t0 + seconds(41));
    CHECK(rig3.take(kB).empty() && rig3.ep.session_count() == 0, "a Response after 40 s is ignored");

    const std::vector<uint8_t> junk = {1, 2, 3};
    rig3.ep.on_datagram(kA, junk.data(), junk.size(), t0);
    CHECK(rig3.take(kA).empty(), "junk from an unknown address is ignored");
}

static void test_login_sequence() {
    std::printf("login\n");
    Rig rig;
    Joined j = handshake(rig, kA, t0);
    auto got = exchange(rig, j, kA, {j.client->hello()}, t0 + milliseconds(10));
    CHECK(has(got, conn::Nmt::Challenge), "Hello -> Challenge");
    const auto seqs = conn::initial_sequences(j.challenge->cookie);
    bool seq_ok = false;
    for (const auto& r : got)
        for (const auto& b : r.bunches) seq_ok = seq_ok || b.ch_sequence == (seqs.server % 1024 + 1) % 1024;
    CHECK(seq_ok, "its channel sequence is the server's initial sequence % 1024 + 1");

    rig.ep.post_login_result(j.session, accepted());
    rig.ep.tick(t0 + milliseconds(20));
    rig.take(kA);
    got = exchange(rig, j, kA, {j.client->login()}, t0 + milliseconds(30));
    CHECK(has(got, conn::Nmt::Welcome), "Login -> Welcome once the login is accepted");
    CHECK(rig.ep.session_of(kCyb) == kA, "cyb is logged in on this session");
    got = exchange(rig, j, kA, {j.client->netspeed(200000), j.client->nmt(conn::Nmt::Join)},
                   t0 + milliseconds(40));
    CHECK(!has(got, conn::Nmt::Failure), "Netspeed and Join are accepted");
    rig.ep.tick(t0 + seconds(59));
    CHECK(rig.ep.session_count() == 1, "a playing client may be quiet for up to 60 s");

    got = exchange(rig, j, kA, {j.client->close_channel()}, t0 + seconds(59) + milliseconds(500));
    CHECK(rig.ep.session_count() == 0 && !rig.ep.session_of(kCyb), "the client's quit ends the session at once");
}

static void test_refused_and_duplicate() {
    std::printf("refusals\n");
    {
        Rig rig;
        Joined j = handshake(rig, kA, t0);
        LoginResult no;
        no.error = login_text::kTicketRefused;
        rig.ep.post_login_result(j.session, no);
        rig.ep.tick(t0 + milliseconds(1));
        bool closed = false;
        auto got = exchange(rig, j, kA, {j.client->hello()}, t0 + milliseconds(10));
        CHECK(has(got, conn::Nmt::Failure, &closed) && closed,
              "a refused login: Hello gets Failure and the channel closes");
        std::string text;
        for (const auto& r : got)
            for (const auto& m : r.messages)
                if (m.type == conn::Nmt::Failure) text = m.error;
        CHECK(text == login_text::kTicketRefused, "with the refusal's text");
        exchange(rig, j, kA, {}, t0 + milliseconds(20));  // the client acks
        CHECK(rig.ep.session_count() == 0, "the session goes once the client has the Failure");
    }
    {
        Rig rig;
        Joined a = handshake(rig, kA, t0);
        exchange(rig, a, kA, {a.client->hello()}, t0);
        rig.ep.post_login_result(a.session, accepted());
        exchange(rig, a, kA, {a.client->login()}, t0 + milliseconds(10));
        Joined b = handshake(rig, kB, t0 + milliseconds(20));
        rig.ep.post_login_result(b.session, accepted());
        rig.ep.tick(t0 + milliseconds(30));
        bool closed = false;
        std::vector<fake::Received> got_a;
        for (const auto& p : rig.take(kA)) got_a.push_back(a.client->receive(p));
        std::string text;
        for (const auto& r : got_a) {
            closed = closed || r.closed;
            for (const auto& m : r.messages)
                if (m.type == conn::Nmt::Failure) text = m.error;
        }
        CHECK(text == login_text::kLoggedInElsewhere && closed,
              "a second login of cyb kicks the first client with the text for it");
        CHECK(rig.ep.session_of(kCyb) == kB, "cyb now belongs to the second session");
        rig.ep.tick(t0 + seconds(3));
        CHECK(rig.ep.session_count() == 1, "the first session is gone after the linger");
    }
    {
        Rig rig;
        handshake(rig, kA, t0);
        rig.ep.tick(t0 + seconds(121));
        CHECK(rig.ep.session_count() == 0, "a silent client times out after 120 s");
    }
    {
        Rig rig;
        Joined j = handshake(rig, kA, t0);
        exchange(rig, j, kA, {j.client->hello()}, t0);
        rig.ep.kick_all(login_text::kShuttingDown, t0 + milliseconds(5));
        rig.ep.tick(t0 + milliseconds(6));
        std::vector<fake::Received> got;
        for (const auto& p : rig.take(kA)) got.push_back(j.client->receive(p));
        CHECK(has(got, conn::Nmt::Failure), "kick_all tells connected clients");
    }
}
// (C-Y-B + CoA)

static void test_second_response_from_one_address() {
    std::printf("a second Response from the same address\n");
    {
        // A client whose first round trip is slow retries its Initial and answers the newer Challenge as well.  It is
        // the same login (same character id and ticket): the ticket is redeemed once, and the gateway would refuse
        // it the second time.  The login in progress, or done, goes on with the new session.
        Rig rig;
        Joined a = handshake(rig, kA, t0);
        log_in(rig, a, kA, t0);
        CHECK(rig.ep.session_of(kCyb) == kA, "A is logged in");
        Joined a2 = handshake(rig, kA, t0 + milliseconds(1500));  // the client retried its Initial
        CHECK(a2.ack && rig.ep.session_count() == 1 && rig.logins.submitted.size() == 1,
              "a newer Response with the same character and ticket replaces the session and redeems nothing again");
        CHECK(rig.ep.session_of(kCyb) == kA, "the completed login is carried over: cyb is still logged in");
        auto got = exchange(rig, a2, kA, {a2.client->hello()}, t0 + milliseconds(1510));
        CHECK(has(got, conn::Nmt::Challenge), "the new session answers Hello");
        got = exchange(rig, a2, kA, {a2.client->login()}, t0 + milliseconds(1520));
        CHECK(has(got, conn::Nmt::Welcome), "and Login with Welcome, with no login result posted for it");
        rig.ep.post_login_result(a.session, accepted());  // a late duplicate does no harm
        rig.ep.tick(t0 + milliseconds(1530));
        CHECK(rig.ep.session_count() == 1 && rig.ep.session_of(kCyb) == kA, "a duplicate result changes nothing");
    }
    {
        Rig rig;
        Joined a = handshake(rig, kA, t0);  // the login is submitted and not answered yet
        Joined a2 = handshake(rig, kA, t0 + milliseconds(1500));
        CHECK(a2.ack && rig.ep.session_count() == 1 && rig.logins.submitted.size() == 1 && a2.session == a.session,
              "a retry while the login is pending: one session, one redeem");
        auto got = exchange(rig, a2, kA, {a2.client->hello()}, t0 + milliseconds(1510));
        CHECK(has(got, conn::Nmt::Challenge), "the new session answers Hello");
        rig.ep.post_login_result(a.session, accepted());  // the one login's answer
        rig.ep.tick(t0 + milliseconds(1520));
        CHECK(rig.ep.session_of(kCyb) == kA, "it goes to the new session");
        got = exchange(rig, a2, kA, {a2.client->login()}, t0 + milliseconds(1530));
        CHECK(has(got, conn::Nmt::Welcome), "which answers Login with Welcome");

        Joined a3 = handshake(rig, kA, t0 + milliseconds(3000));  // and once more, with the login done
        CHECK(a3.ack && rig.ep.session_count() == 1 && rig.logins.submitted.size() == 1 &&
                  rig.ep.session_of(kCyb) == kA,
              "a second retry after the login finished: still one redeem, still logged in");
    }
    {
        // Initial, Challenge A, the retried Initial, Challenge B, and the client answers B only: no session to
        // replace, one redeem.
        Rig rig;
        const auto initial = capture::bytes(capture::kInitial);
        rig.ep.on_datagram(kA, initial.data(), initial.size(), t0);
        const auto first = parse(rig.take(kA).at(0));
        rig.ep.on_datagram(kA, initial.data(), initial.size(), t0 + milliseconds(1000));
        const auto second = parse(rig.take(kA).at(0));
        CHECK(first && second && second->timestamp > first->timestamp, "the retried Initial gets a newer Challenge");
        conn::HandshakePacket resp = *second;
        resp.type = conn::HandshakeType::Response;
        resp.sent_count = 2;
        const auto bytes = conn::build_handshake(resp);
        rig.ep.on_datagram(kA, bytes.data(), bytes.size(), t0 + milliseconds(1010));
        CHECK(rig.take(kA).size() == 1 && rig.ep.session_count() == 1 && rig.logins.submitted.size() == 1,
              "a Response to the second Challenge: one session, one redeem");
        Joined j;
        j.client.emplace(conn::outer_header_of(*second), second->ext->custom_field(),
                         conn::initial_sequences(second->cookie).server, conn::initial_sequences(second->cookie).client);
        j.session = rig.logins.submitted.back().first;
        log_in(rig, j, kA, t0 + milliseconds(1020));
        CHECK(rig.ep.session_of(kCyb) == kA, "the login completes");
    }
    {
        // Not the same login: another ticket, or another character, is redeemed for itself, and the replaced
        // session's result is dropped.
        Rig rig;
        Joined a = handshake(rig, kA, t0);
        Joined a2 = handshake(rig, kA, t0 + milliseconds(1500), initial_with(0x01, 0));  // another ticket
        CHECK(a2.ack && rig.ep.session_count() == 1 && rig.logins.submitted.size() == 2 &&
                  rig.logins.submitted[1].second.ticket != kTicket && a2.session != a.session,
              "a newer Response with another ticket is a new login: redeemed");
        rig.ep.post_login_result(a.session, accepted());
        rig.ep.tick(t0 + milliseconds(1600));
        CHECK(!rig.ep.session_of(kCyb), "a result for the replaced session is dropped");
        rig.ep.post_login_result(a2.session, accepted());
        rig.ep.tick(t0 + milliseconds(1700));
        CHECK(rig.ep.session_of(kCyb) == kA, "the new session's result counts");

        Joined a3 = handshake(rig, kA, t0 + milliseconds(3000), initial_with(0, 0x01));  // another character
        CHECK(a3.ack && rig.logins.submitted.size() == 3 && rig.logins.submitted[2].second.character_id != kCyb,
              "a newer Response for another character is redeemed too");
        CHECK(!rig.ep.session_of(kCyb), "and the replaced session's character mapping is gone");
    }
    {
        // A refused login, or a session that was kicked, is not carried over: the new session asks again (a
        // refusal for a service being down need not repeat, and a kicked session must not take its character
        // back from the client that replaced it).
        Rig rig;
        Joined a = handshake(rig, kA, t0);
        LoginResult no;
        no.error = login_text::kServiceUnavailable;
        rig.ep.post_login_result(a.session, no);
        rig.ep.tick(t0 + milliseconds(1));
        Joined a2 = handshake(rig, kA, t0 + milliseconds(1500));
        CHECK(a2.ack && rig.logins.submitted.size() == 2, "a refused login is submitted again by the retry");

        Rig rig2;
        Joined x = handshake(rig2, kA, t0);
        log_in(rig2, x, kA, t0);
        Joined y = handshake(rig2, kB, t0 + milliseconds(20));
        rig2.ep.post_login_result(y.session, accepted());
        rig2.ep.tick(t0 + milliseconds(30));
        CHECK(rig2.ep.session_of(kCyb) == kB, "B took cyb; A is kicked");
        Joined x2 = handshake(rig2, kA, t0 + milliseconds(1500));
        CHECK(x2.ack && rig2.logins.submitted.size() == 3 && rig2.ep.session_of(kCyb) == kB,
              "the kicked session's retry does not take cyb back: it is a new login");
    }
    {
        Rig rig;
        Joined a = handshake(rig, kA, t0);
        Joined b = handshake(rig, kA, t0 + milliseconds(500));  // the live session, a newer cookie
        auto got = exchange(rig, b, kA, {b.client->hello()}, t0 + milliseconds(510));
        CHECK(has(got, conn::Nmt::Challenge), "the live session answers Hello");

        rig.ep.on_datagram(kB, a.response.data(), a.response.size(), t0 + milliseconds(590));
        CHECK(rig.take(kB).empty(), "the first Response replayed from another address is ignored");

        rig.ep.on_datagram(kA, a.response.data(), a.response.size(), t0 + milliseconds(600));  // replayed, older
        CHECK(rig.take(kA).empty() && rig.ep.session_count() == 1 && rig.logins.submitted.size() == 1,
              "an older Response than the live session's is ignored: no new session, no second redeem");
        got = exchange(rig, b, kA, {b.client->login()}, t0 + milliseconds(700));
        CHECK(any_valid(got), "the live client's packets are still accepted afterwards");
    }
}

static void test_session_cap() {
    std::printf("the session cap\n");
    EndpointConfig cfg;
    CHECK(cfg.max_sessions >= 256, "the default cap is generous");
    cfg.max_sessions = 2;
    Rig rig(cfg);
    const auto initial = capture::bytes(capture::kInitial);
// (C:Y:B : C | O | A)

    Joined a = handshake(rig, kA, t0);
    // C asks while there is room: the Challenge keeps no state.
    rig.ep.on_datagram(kC, initial.data(), initial.size(), t0);
    const auto chal_c = parse(rig.take(kC).at(0));
    Joined b = handshake(rig, kB, t0);
    CHECK(rig.ep.session_count() == 2, "two sessions fill the cap");

    conn::HandshakePacket resp = *chal_c;
    resp.type = conn::HandshakeType::Response;
    resp.sent_count = 2;
    const auto resp_c = conn::build_handshake(resp);
    rig.ep.on_datagram(kC, resp_c.data(), resp_c.size(), t0 + milliseconds(10));
    CHECK(rig.take(kC).empty() && rig.ep.session_count() == 2 && rig.logins.submitted.size() == 2,
          "a valid Response beyond the cap is refused: no Ack, no session, no login");

    rig.ep.on_datagram(kC, initial.data(), initial.size(), t0 + milliseconds(20));
    CHECK(rig.take(kC).empty(), "an Initial beyond the cap gets no Challenge");

    rig.ep.on_datagram(kA, initial.data(), initial.size(), t0 + milliseconds(30));
    const auto chal_a = rig.take(kA);
    CHECK(chal_a.size() == 1, "an address that has a session still gets its Challenge (a retry)");
    conn::HandshakePacket resp_a = *parse(chal_a[0]);
    resp_a.type = conn::HandshakeType::Response;
    resp_a.sent_count = 2;
    const auto resp_a_bytes = conn::build_handshake(resp_a);
    rig.ep.on_datagram(kA, resp_a_bytes.data(), resp_a_bytes.size(), t0 + milliseconds(40));
    CHECK(rig.take(kA).size() == 1 && rig.ep.session_count() == 2, "and its newer Response replaces the session at the cap");

    // A session ends; the next client gets in.
    exchange(rig, b, kB, {b.client->close_channel()}, t0 + milliseconds(50));
    CHECK(rig.ep.session_count() == 1, "B quits");
    Joined c = handshake(rig, kC, t0 + milliseconds(60));
    CHECK(c.ack && rig.ep.session_count() == 2, "C gets in once there is room");

    // The refusals are logged, at most one line every 10 s; tick writes the last of a burst.
    {
        EndpointConfig one;
        one.max_sessions = 1;
        Rig full(one);
        handshake(full, kA, t0);
        g_log.str("");
        for (int i = 0; i < 4; ++i) full.ep.on_datagram(kB, initial.data(), initial.size(), t0 + seconds(1) + milliseconds(i));
        const std::string first = g_log.str();
        CHECK(first.find("limit of 1 sessions is reached: refused 1 handshake") != std::string::npos &&
                  first.find("refused 1 handshake") == first.rfind("refused 1 handshake"),
              "the first refusal is logged at once, the rest of the burst held back");
        full.ep.tick(t0 + seconds(5));
        CHECK(g_log.str() == first, "a tick inside the interval writes nothing");
        full.ep.tick(t0 + seconds(12));
        CHECK(g_log.str().find("refused 3 handshake") != std::string::npos,
              "a tick after it writes the line for the 3 held back, though no refusal follows");
        const std::string after = g_log.str();
        full.ep.tick(t0 + seconds(30));
        CHECK(g_log.str() == after, "and only once");
    }

    // 0 means no cap.
    EndpointConfig open;
    open.max_sessions = 0;
    Rig rig2(open);
    for (uint16_t i = 0; i < 40; ++i) handshake(rig2, Address{0x7F000001u, static_cast<uint16_t>(41000 + i)}, t0);
    CHECK(rig2.ep.session_count() == 40, "a cap of 0 is no limit");
}

static void test_login_deadline() {
    std::printf("sessions that never log in\n");
    {
        Rig rig;
        Joined j = handshake(rig, kA, t0);
        LoginResult no;
        no.error = login_text::kTicketRefused;
        rig.ep.post_login_result(j.session, no);
        rig.ep.tick(t0 + milliseconds(1));
        // No Hello ever, but an empty data packet every 30 s.
        for (int s = 30; s <= 600; s += 30) exchange(rig, j, kA, {}, t0 + seconds(s));
        CHECK(rig.ep.session_count() == 0,
              "a refused session that never sends Hello is dropped even if it keeps talking");
    }
    {
        Rig rig;
        Joined j = handshake(rig, kA, t0);
        LoginResult no;
        no.error = login_text::kTicketRefused;
        rig.ep.post_login_result(j.session, no);
        rig.ep.tick(t0 + milliseconds(1));
        exchange(rig, j, kA, {}, t0 + milliseconds(1900));
        CHECK(rig.ep.session_count() == 1, "a refused session waits for Hello for the close linger (2 s)");
        exchange(rig, j, kA, {}, t0 + milliseconds(2100));
        CHECK(rig.ep.session_count() == 0, "and is dropped after it");
    }
    {
        Rig rig;
        Joined j = handshake(rig, kA, t0);
        for (int s = 30; s <= 600; s += 30) exchange(rig, j, kA, {}, t0 + seconds(s));
        CHECK(rig.ep.session_count() == 0,
              "a session with no login result and no Hello is dropped within the login deadline even if it keeps talking");
    }
    {
        Rig rig;
        Joined j = handshake(rig, kA, t0);
        for (int s = 30; s < 120; s += 30) exchange(rig, j, kA, {}, t0 + seconds(s));
        CHECK(rig.ep.session_count() == 1, "up to 120 s the session may wait for its login");
        log_in(rig, j, kA, t0 + seconds(100));
        exchange(rig, j, kA, {}, t0 + seconds(119));
        exchange(rig, j, kA, {}, t0 + seconds(150));
        CHECK(rig.ep.session_count() == 1, "a session that has been sent Welcome is past the login deadline");
    }
}

static void test_results_and_kicks_before_hello() {
    std::printf("kicks and results\n");
    {
        Rig rig;
        Joined j = handshake(rig, kA, t0);
        rig.ep.kick_all(login_text::kShuttingDown, t0 + milliseconds(5));
        rig.ep.tick(t0 + milliseconds(6));
        bool closed = false;
        auto got = drain(rig, j, kA);
        CHECK(!has(got, conn::Nmt::Failure, &closed) && closed, "before Hello the kick sends only the close");
        rig.ep.post_login_result(j.session, accepted());
        rig.ep.tick(t0 + milliseconds(7));
        CHECK(!rig.ep.session_of(kCyb), "a login accepted after the kick does not register the character");
        exchange(rig, j, kA, {}, t0 + milliseconds(10));  // the client acks the close
        CHECK(rig.ep.session_count() == 0, "the session goes once the close is acknowledged");
    }
    {
        Rig rig;
        Joined a = handshake(rig, kA, t0);
        log_in(rig, a, kA, t0);
        CHECK(rig.ep.session_of(kCyb) == kA, "A has cyb");
        Joined b = handshake(rig, kB, t0 + milliseconds(20));
        exchange(rig, b, kB, {b.client->hello()}, t0 + milliseconds(21));
        rig.ep.post_login_result(b.session, accepted());
        rig.ep.tick(t0 + milliseconds(30));
        CHECK(rig.ep.session_of(kCyb) == kB, "B takes cyb; A is closing");
        Joined c = handshake(rig, kC, t0 + milliseconds(40));
        exchange(rig, c, kC, {c.client->hello()}, t0 + milliseconds(41));
        rig.ep.post_login_result(c.session, accepted());
        rig.ep.tick(t0 + milliseconds(50));
        CHECK(rig.ep.session_of(kCyb) == kC && rig.ep.session_count() == 3, "C takes cyb; A and B are closing");
        auto got_a = drain(rig, a, kA);
        auto got_b = drain(rig, b, kB);
        CHECK(has(got_a, conn::Nmt::Failure) && has(got_b, conn::Nmt::Failure), "both got the Failure");
        exchange(rig, a, kA, {}, t0 + milliseconds(60));
        exchange(rig, b, kB, {}, t0 + milliseconds(61));
        CHECK(rig.ep.session_count() == 1 && rig.ep.session_of(kCyb) == kC,
              "after A and B are gone, cyb still belongs to C");
    }
}
// [C O A x Cyb]

static void test_close_linger() {
    std::printf("the closing linger\n");
    {
        Rig rig;
        Joined j = handshake(rig, kA, t0);
        exchange(rig, j, kA, {j.client->hello()}, t0);
        rig.ep.kick_all(login_text::kShuttingDown, t0 + milliseconds(5));
        rig.ep.tick(t0 + milliseconds(6));
        CHECK(has(drain(rig, j, kA), conn::Nmt::Failure), "the Failure goes out at once");
        rig.ep.tick(t0 + milliseconds(1900));
        CHECK(rig.ep.session_count() == 1, "a silent client keeps the closing session until the linger");
        rig.ep.tick(t0 + milliseconds(2005));
        CHECK(rig.ep.session_count() == 0, "which ends it at 2 s");
    }
    {
        Rig rig;
        Joined j = handshake(rig, kA, t0);
        exchange(rig, j, kA, {j.client->hello()}, t0);
        rig.ep.kick_all(login_text::kShuttingDown, t0 + milliseconds(5));
        rig.ep.tick(t0 + milliseconds(6));
        rig.take(kA);  // the Failure's packet is lost
        rig.ep.tick(t0 + milliseconds(210));
        auto ka = drain(rig, j, kA);
        CHECK(ka.size() == 1 && ka[0].valid && ka[0].bunches.empty(), "a keep-alive follows at 200 ms");
        auto got = exchange(rig, j, kA, {}, t0 + milliseconds(220));  // reports the Failure's packet missing
        CHECK(has(got, conn::Nmt::Failure), "the lost Failure is sent again");
        exchange(rig, j, kA, {}, t0 + milliseconds(230));
        CHECK(rig.ep.session_count() == 0, "and the session goes once it is acknowledged");
    }
}

static void test_play_timeout_and_junk() {
    std::printf("after Join, and junk on the session\n");
    Rig rig;
    Joined j = handshake(rig, kA, t0);
    fake::Bunch odd;  // a complete NMT_Hello, but on channel 3
    odd.header.reliable = true;
    odd.header.control = true;
    odd.header.open = true;
    odd.header.channel = 3;
    odd.header.ch_sequence = 511;
    odd.payload.write_uint8(0);
    odd.payload.write_uint8(1);
    odd.payload.write_uint32(0xa42f624eu);
    odd.payload.write_int32(0);
    fake::Bunch bad = j.client->control([](aoc::protocol::emit::BunchWriter& w) {
        w.write_uint8(0);  // NMT_Hello with its fields cut short
        w.write_uint8(1);
    });
    auto junk = exchange(rig, j, kA, {odd, bad}, t0 + milliseconds(5));
    CHECK(rig.ep.session_count() == 1 && !has(junk, conn::Nmt::Challenge),
          "a Hello on channel 3 and an unreadable control message are ignored: no Challenge");
    log_in(rig, j, kA, t0 + milliseconds(10));
    auto got = exchange(rig, j, kA, {j.client->netspeed(200000), j.client->nmt(conn::Nmt::Join)},
                        t0 + milliseconds(40));
    CHECK(!has(got, conn::Nmt::Failure) && rig.ep.session_of(kCyb) == kA, "logged in and joined");
    rig.ep.tick(t0 + seconds(60));
    CHECK(rig.ep.session_count() == 1, "quiet for 60 s: still there");
    rig.ep.tick(t0 + seconds(61));
    CHECK(rig.ep.session_count() == 0 && !rig.ep.session_of(kCyb), "after Join a silent client times out at 60 s");
}

static void test_rate_limit() {
    std::printf("a session's datagrams are rate limited\n");
    EndpointConfig cfg;
    cfg.max_session_bytes_per_second = 10000;
    cfg.session_burst_seconds = 2;
    Rig rig(cfg);
    Joined a = handshake(rig, kA, t0);
    Joined b = handshake(rig, kB, t0);
    const std::vector<uint8_t> junk(1000, 0x00);   // not a handshake: a datagram of an established session
    for (int i = 0; i < 100; ++i) rig.ep.on_datagram(kA, junk.data(), junk.size(), t0);
    CHECK(rig.ep.datagrams_dropped_for_rate() == 80, "a flood of 100 datagrams of 1,000 bytes at one instant: 20 KB (a 2 s bucket) get in, 80 are dropped");
    for (int i = 0; i < 30; ++i) rig.ep.on_datagram(kA, junk.data(), junk.size(), t0 + seconds(1));
    CHECK(rig.ep.datagrams_dropped_for_rate() == 80 + 20, "a second later the bucket has earned 10 KB: 10 more get in");
    for (int i = 0; i < 5; ++i) rig.ep.on_datagram(kB, junk.data(), junk.size(), t0 + seconds(1));
    CHECK(rig.ep.datagrams_dropped_for_rate() == 100, "another session has a bucket of its own: its first datagrams are not dropped");
    g_log.flush();
    const std::string text = g_log.str();
    const size_t first = text.find("datagrams come faster than 10000 bytes a second");
    CHECK(first != std::string::npos && text.find("datagrams come faster", first + 1) == std::string::npos, "the first drop of a session is a warning, once");
    // a client at its own pace is not touched: 60 datagrams of 150 bytes a second for 10 s
    Rig calm(cfg);
    Joined c = handshake(calm, kC, t0);
    const std::vector<uint8_t> small(150, 0x00);
    for (int i = 0; i < 600; ++i) calm.ep.on_datagram(kC, small.data(), small.size(), t0 + milliseconds(i * 17));
    CHECK(calm.ep.datagrams_dropped_for_rate() == 0, "600 datagrams of 150 bytes in 10 s (the retail client's pace, 9 KB a second) are all taken");
    // no limit: 0
    EndpointConfig open_cfg;
    open_cfg.max_session_bytes_per_second = 0;
    Rig open(open_cfg);
    Joined d = handshake(open, kA, t0);
    for (int i = 0; i < 1000; ++i) open.ep.on_datagram(kA, junk.data(), junk.size(), t0);
    CHECK(open.ep.datagrams_dropped_for_rate() == 0, "0 means no limit");
    (void)a; (void)b; (void)c; (void)d;
}
// [C Y B :: CoA]

static void test_protocol_breakers() {
    std::printf("a client that breaks the protocol\n");
    {
        Rig rig;
        Joined j = handshake(rig, kA, t0);
        Clock::time_point t = t0;
        exchange(rig, j, kA, {j.client->hello()}, t);
        j.client->netspeed(1);  // 512 is never sent
        size_t n = 0;
        while (n < 300 && rig.ep.session_count() == 1) {
            t += milliseconds(60);  // the ack after each packet keeps the packet window open
            exchange(rig, j, kA, {j.client->netspeed(static_cast<uint32_t>(2 + n))}, t);
            ++n;
        }
        CHECK(rig.ep.session_count() == 0 && n == 257,
              "the 257th early reliable bunch drops the session: too many held for a gap that never fills");
    }
    {
        Rig rig;
        Joined j = handshake(rig, kA, t0);
        Clock::time_point t = t0;
        exchange(rig, j, kA, {j.client->hello()}, t);
        auto part = [&](bool initial) {
            fake::Bunch b = j.client->control([](aoc::protocol::emit::BunchWriter& w) {
                for (int i = 0; i < 900; ++i) w.write_uint8(0x5A);
            });
            b.header.partial = true;
            b.header.partial_initial = initial;
            return b;
        };
        size_t n = 0;
        while (n < 100 && (n == 0 || rig.ep.session_count() == 1)) {
            t += milliseconds(60);
            exchange(rig, j, kA, {part(n == 0)}, t);
            ++n;
        }
        CHECK(rig.ep.session_count() == 0 && n == 73,
              "a reliable partial bunch over 64 KB drops the session (the 73rd 900-byte part)");
    }
}

int main() {
    std::ostringstream& log = g_log;
    auto logger = std::make_shared<spdlog::logger>("test", std::make_shared<spdlog::sinks::ostream_sink_mt>(log));
    logger->set_level(spdlog::level::trace);
    spdlog::set_default_logger(logger);

    test_handshake();
    test_login_sequence();
    test_refused_and_duplicate();
    test_second_response_from_one_address();
    test_session_cap();
    test_login_deadline();
    test_results_and_kicks_before_hello();
    test_close_linger();
    test_play_timeout_and_junk();
    test_rate_limit();
    test_protocol_breakers();

    logger->flush();
    CHECK(log.str().find(kTicket) == std::string::npos, "the ticket never appears in the log");
    CHECK(log.str().find(kCyb) != std::string::npos, "(the character id does)");

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
