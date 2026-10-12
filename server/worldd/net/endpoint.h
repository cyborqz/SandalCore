// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/endpoint.h
//
//  worldd's side of the UDP port (architecture spec §3.2 net/Endpoint):
//  the stateless handshake, one session per client address, and the login.
//
//      Initial   -> Challenge (cookie over timestamp and address; no state kept)
//      Response  -> verify the cookie, create the session, send the Ack,
//                   start the login with the character id and ticket the
//                   extension carries
//      data      -> the session's Connection; channel 0 to its ControlChannel
//
//  A character logs in at most once: when a second login for it succeeds,
//  the first session is kicked.  The endpoint does no I/O itself: datagrams
//  go out through `send`, and the caller owns the socket and the clock, so
//  tests can drive it in memory.  Not thread-safe except post_login_result.
//
//  A client whose first round trip takes over a second retries its Initial
//  and answers the newer Challenge too.  The newer Response replaces the
//  session, and when it carries the same character id and ticket the login
//  goes on with the new session: a login in progress is not submitted again
//  and one that succeeded is applied to it, because the ticket is spent by
//  the first redemption and the gateway would refuse a second.  An older
//  Response than the session's is ignored.
//
//  At most EndpointConfig::max_sessions sessions exist at once: beyond it an
//  Initial gets no Challenge and a Response no Ack, and nothing is allocated;
//  an address that already has a session may still replace it.
//
//  After NMT_Join (plan A5, with EndpointConfig::players set) the session
//  starts a PlayerSession: the player's bootstrap goes out on actor channels,
//  the client's bunches on them are answered, and the connection's ack events
//  are fed back (session/player_session.h).  Without players (the A4 tests) the
//  client is left at the loading screen after Join.  When content.db did not
//  load (players->ready() false) a handshake is refused before its ticket is
//  redeemed: the client's Hello gets NMT_Failure with login_text::kContentMissing.
//  A player whose setup is refused at Join, or whose join cannot be sent, is
//  told login_text::kLoginFailed and closed (login_text::kRaceUnsupported when
//  the setup refused the character's race).  A session that is closing gets
//  no more answers on its actor channels.  A datagram's acknowledgements are
//  applied before its bunches.  An exception out of one session's handling
//  (a datagram or a tick) drops that session and no other.
//
//  Plan A6: every way a session with a player ends (the client quit, timed out or broke
//  the protocol, a kick, a replaced session) goes through remove(), which calls the
//  player's finish(): its last good position is handed to the PositionSink.  worldd's
//  shutdown calls finish_players() for the sessions still open, then waits (bounded)
//  for the saver.  tick() also gives each player the clock for the periodic save.
//
//  Rate limit (plan A7 review): a session's datagrams are a token bucket of bytes on the caller's clock,
//  EndpointConfig::max_session_bytes_per_second a second (256 KB: 30 retail captures show the client sending 1-3 KB a second,
//  5 KB in the busiest second), holding session_burst_seconds' worth; a datagram that finds the bucket short is dropped before
//  anything reads it (the first one of a session is a warning, the rest are counted), so a flood costs a bucket check,
//  not a parse.  Handshake datagrams are not counted: the cookie already makes a client prove its address.
// ============================================================================
#pragma once
// [C-Y-B - CoA]

#include "login/login_service.h"
#include "net/connection.h"
#include "net/control_channel.h"
#include "net/cookie.h"
#include "net/udp_socket.h"
#include "session/player_session.h"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aoc { namespace worldd {

struct EndpointConfig {
    WorldInfo world;
    /// The spawn location in the handshake extension; the original server
    /// sent this one (Lyneth).  The character's own position comes with A5.
    std::array<double, 3> spawn = {-777664.5, 616578.7, 15952.0};
    /// How long a closing session keeps resending its Failure.
    std::chrono::milliseconds close_linger{2000};
    /// A session that has not been sent Welcome by then is dropped, whatever
    /// it keeps sending; a refusal before Hello shortens it to close_linger.
    std::chrono::seconds login_deadline{Connection::kLoginTimeout};
    /// The most sessions at once (closing ones count); 0 is no limit.
    size_t max_sessions = 1024;
    /// What the player sessions share (content.db, the GUID allocator ...); null: nothing is started after Join.
    /// Must outlive the Endpoint.
    PlayerWorld* players = nullptr;
    /// The most bytes a session's datagrams may bring a second (a token bucket; 0 is no limit) and how many seconds' worth it holds.
    uint32_t max_session_bytes_per_second = 262144;
    uint32_t session_burst_seconds = 2;
};

class Endpoint {
public:
    using Send = std::function<void(const Address&, const std::vector<uint8_t>&)>;
    /// The least time between two log lines about handshakes refused for the session cap.
    static constexpr std::chrono::seconds kLimitLogInterval{10};

    Endpoint(EndpointConfig cfg, CookieSigner& signer, LoginQueue& logins, Send send,
             CookieSigner::RandomFill random, Clock::time_point start);

    void on_datagram(const Address& from, const uint8_t* data, size_t len, Clock::time_point now);

    /// Called from any thread when a login finishes.
    void post_login_result(uint64_t session_id, LoginResult result);

    /// Applies finished logins, sends what connections have queued, and
    /// drops sessions that closed, timed out or finished closing.
    void tick(Clock::time_point now);

    /// Ends every session with `text` (server shutdown); keep ticking for
    /// close_linger so the messages go out.
    void kick_all(const std::string& text, Clock::time_point now);

    /// Forgets the session of `from` at once, without a word to the client (the caller's barrier caught an exception
    /// from it); nothing when there is none.
    void drop(const Address& from, const char* why);

    /// Ends every player's session as far as saving goes (Session::player->finish): worldd is stopping and the sessions
    /// may not be removed before it exits. Positions go to the PositionSink; sessions are left as they are.
    void finish_players(const char* why);

    size_t session_count() const { return sessions_.size(); }
    /// Datagrams of established sessions dropped for the rate limit.
    size_t datagrams_dropped_for_rate() const { return rate_dropped_; }
    /// The address of the session a character is logged in on, if any.
    std::optional<Address> session_of(const std::string& character_id) const;
    /// The player of the session at `a`, once it has joined (tests, diagnostics); nullptr when there is none.
    const PlayerSession* player_at(const Address& a) const;
    /// The players that have joined and are not closing, in the order their sessions began, with their connections: what the GM travel tools (session/gm_travel.h) act on.
    /// Network thread only, like everything of the Endpoint; the handles are good until the next tick or datagram.
    std::vector<PlayerHandle> players();

private:
    struct Session {
        uint64_t id = 0;                  // the id its login was submitted under (by_id_); a session that took
                                          // over a replaced session's login keeps that session's
        Address address;
        Cookie cookie{};
        double cookie_timestamp = 0;      // the Challenge's timestamp behind the cookie
        Clock::time_point deadline{};     // dropped if not welcomed by then
        std::vector<uint8_t> ack_packet;  // sent again for a repeated Response
        Connection connection;
        ControlChannel control;
        LoginRequest request;
        std::string character_name;
        std::unique_ptr<PlayerSession> player;   // after Join, with EndpointConfig::players
        bool carried_items_differ = false;       // it took over a playing session's inventory and that differs from the record's (plan B2)
        bool logged_in = false;
        bool closing = false;
        Clock::time_point close_by{};
        double byte_tokens = 0;           // the rate limit's bucket, in bytes (full at the session's first datagram)
        Clock::time_point byte_at{};
        bool rate_started = false;
        size_t rate_dropped = 0;

        Session(const ConnectionConfig& cc, Clock::time_point now, WorldInfo world, std::string challenge)
            : connection(cc, now), control(std::move(world), std::move(challenge)) {}
    };

    void on_handshake(const Address& from, const uint8_t* data, size_t len, Clock::time_point now);
    void apply(Session& s, ControlChannel::Reply reply, Clock::time_point now);
    /// The client's Join: builds the player and queues its join sequence (refuses the login when that fails).
    void start_player(Session& s, Clock::time_point now);
    /// Applies a finished login. By value: when the character is already playing on another session, the new session
    /// starts from where that one is (its last good position goes into the record) and that session's position is
    /// handed over before the new one can save anything.
    void on_login_result(Session& s, LoginResult result, Clock::time_point now);
    void remove(const Address& a, const char* why);
    /// Whether the session cap refuses a handshake from `from` (an address with a session is never refused:
    /// its newer Response replaces the session).  Counts the refusal for the log.
    bool session_limit_reached(const Address& from, Clock::time_point now);
    /// Writes one line for the handshakes refused for the cap since the last line, at most one every
    /// kLimitLogInterval; tick calls it too, so the last refusals of a burst are not left out.
    void log_refused_for_limit(Clock::time_point now);
    double seconds(Clock::time_point t) const;
    std::string random_challenge();
    /// Takes `len` bytes from the session's bucket (refilled for the time since the last datagram); false when it holds fewer.
    bool within_rate(Session& s, const Address& from, size_t len, Clock::time_point now);
// [CoA + C.Y.B]

    EndpointConfig cfg_;
    CookieSigner& signer_;
    LoginQueue& logins_;
    Send send_;
    CookieSigner::RandomFill random_;
    Clock::time_point start_;
    uint64_t next_session_id_ = 1;
    std::unordered_map<Address, std::unique_ptr<Session>, AddressHash> sessions_;
    std::unordered_map<std::string, Address> by_character_;
    std::unordered_map<uint64_t, Address> by_id_;  // Session::id -> its address: where a login result goes
    size_t refused_for_limit_ = 0;                 // handshakes refused for the cap, not in a log line yet
    size_t rate_dropped_ = 0;                      // datagrams dropped for the rate limit, all sessions
    std::optional<Clock::time_point> limit_logged_;

    std::mutex results_mu_;
    std::vector<std::pair<uint64_t, LoginResult>> results_;
};

}} // namespace aoc::worldd
