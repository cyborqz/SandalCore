// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/endpoint.cpp
// ============================================================================
#include "net/endpoint.h"
#include "protocol/conn/handshake.h"
#include "protocol/conn/nmt.h"
#include "protocol/wire/ue5_primitives.h"
#include <algorithm>
#include <cstdio>
#include <optional>
#include <spdlog/spdlog.h>

namespace aoc { namespace worldd {

namespace conn = protocol::conn;

Endpoint::Endpoint(EndpointConfig cfg, CookieSigner& signer, LoginQueue& logins, Send send,
                   CookieSigner::RandomFill random, Clock::time_point start)
    : cfg_(std::move(cfg)), signer_(signer), logins_(logins), send_(std::move(send)),
      random_(std::move(random)), start_(start) {}

double Endpoint::seconds(Clock::time_point t) const {
    return std::chrono::duration<double>(t - start_).count();
}

std::string Endpoint::random_challenge() {
    uint8_t b[4];
    random_(b, sizeof(b));
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02X%02X%02X%02X", b[0], b[1], b[2], b[3]);
    return buf;
}

void Endpoint::on_datagram(const Address& from, const uint8_t* data, size_t len, Clock::time_point now) {
    if (conn::is_handshake(data, len)) {
        on_handshake(from, data, len, now);
        return;
    }
    auto it = sessions_.find(from);
    if (it == sessions_.end()) return;  // no handshake from this address
    Session& s = *it->second;
    if (!within_rate(s, from, len, now)) return;
    try {
        // The datagram's acknowledgements first, then its bunches: the retail client's first packet after the join
        // acknowledges the join and asks for the load in one, and the reply must leave out what was just acknowledged.
        const std::vector<InBunch> bunches = s.connection.receive(data, len, now);
        if (s.player) s.player->take_acks(s.connection);
        bool touched_player = false;
        for (const InBunch& b : bunches) {
            if (b.channel != 0) {
                if (s.player && !s.closing) {
                    s.player->on_bunch(s.connection, b, now);
                    touched_player = true;
                } else {
                    spdlog::debug("[endpoint] {}: ignoring a bunch on channel {}", from.to_string(), b.channel);
                }
                continue;
            }
            if (b.close || b.bits == 0) continue;
            const auto msg = conn::parse_control_message(b.data.data(), b.data.size(), 0, b.bits);
            if (!msg) {
                spdlog::debug("[endpoint] {}: unreadable control message", from.to_string());
                continue;
            }
            apply(s, s.control.on_message(*msg), now);
        }
        // The datagram is done: the one correction its refused moves left goes out (at most one per datagram).
        if (touched_player && s.player && !s.closing) s.player->end_of_datagram(s.connection);
    } catch (const std::exception& e) {
        // One session's fault stays its own: it is dropped, the others go on.
        spdlog::error("[endpoint] {} ({}): a datagram threw {}: dropping the session", from.to_string(), s.request.character_id, e.what());
        remove(from, "threw an exception");
    }
}
// Cyb / C-O-A

bool Endpoint::within_rate(Session& s, const Address& from, size_t len, Clock::time_point now) {
    const double rate = static_cast<double>(cfg_.max_session_bytes_per_second);
    if (rate <= 0) return true;
    const double capacity = rate * static_cast<double>(std::max<uint32_t>(cfg_.session_burst_seconds, 1));
    if (!s.rate_started) {
        s.rate_started = true;
        s.byte_tokens = capacity;
        s.byte_at = now;
    } else if (now > s.byte_at) {
        s.byte_tokens = std::min(capacity, s.byte_tokens + rate * std::chrono::duration<double>(now - s.byte_at).count());
        s.byte_at = now;
    }
    if (s.byte_tokens >= static_cast<double>(len)) {
        s.byte_tokens -= static_cast<double>(len);
        return true;
    }
    ++rate_dropped_;
    if (++s.rate_dropped == 1) {
        spdlog::warn("[endpoint] {} ({}): datagrams come faster than {} bytes a second: dropped (further ones are counted, not logged)", from.to_string(),
                     s.request.character_id, cfg_.max_session_bytes_per_second);
    }
    return false;
}

void Endpoint::drop(const Address& from, const char* why) { remove(from, why); }

void Endpoint::on_handshake(const Address& from, const uint8_t* data, size_t len, Clock::time_point now) {
    const auto p = conn::parse_handshake(data, len);
    if (!p || !p->ext) return;
    const double t = seconds(now);

    if (p->type == conn::HandshakeType::Initial) {
        if (!p->ext->has_initial_marker()) return;
        if (session_limit_reached(from, now)) return;  // no Challenge: the client would only be refused next
        conn::HandshakePacket c = *p;
        c.type = conn::HandshakeType::Challenge;
        c.restart = false;
        c.secret_id = signer_.secret_id();
        c.timestamp = t;
        c.cookie = signer_.sign(c.secret_id, t, from.to_string());
        std::array<uint8_t, 38> server_bytes{};
        random_(server_bytes.data(), server_bytes.size());
        c.ext = conn::IntrepidExt::for_server(*p->ext, server_bytes, cfg_.spawn);
        c.padding.assign(conn::kChallengePaddingBytes, 0);
        random_(c.padding.data(), c.padding.size());
        send_(from, conn::build_handshake(c));
        return;
    }
    if (p->type != conn::HandshakeType::Response) return;

    auto existing = sessions_.find(from);
    if (existing != sessions_.end() && existing->second->cookie == p->cookie) {
        send_(from, existing->second->ack_packet);  // our Ack was lost
        return;
    }
    if (existing != sessions_.end() && p->timestamp <= existing->second->cookie_timestamp) {
        // A replay of an earlier Response; a client that retried its Initial
        // answers a newer Challenge.
        spdlog::debug("[endpoint] {}: ignoring a Response older than its session", from.to_string());
        return;
    }
    if (!signer_.verify(p->secret_id, p->timestamp, from.to_string(), p->cookie, t)) {
        spdlog::debug("[endpoint] {}: Response with a wrong or expired cookie", from.to_string());
        return;
    }
    if (existing == sessions_.end() && session_limit_reached(from, now)) return;

    const std::string character_id = ::ue5::guid_to_hex(p->ext->character_id().data());
    const std::string ticket = ::ue5::guid_to_hex(p->ext->ticket().data());

    // A client that retried its Initial answers the newer Challenge with the character id and ticket of the
    // session it replaces: that login is already running or done, and the ticket was spent by it.  Carry it over
    // rather than redeem again (the gateway refuses a second redemption).  A refused login is asked again (a
    // service that was down may be up now; a spent ticket is refused again), and so is a session being closed:
    // one that was kicked for another client's login of the character must not take the character back.
    bool carried = false;
    uint64_t carried_id = 0;
    std::optional<LoginResult> carried_result;
    // A worldd without content.db cannot value a character: the login is refused before the ticket is redeemed.
    const bool no_content = cfg_.players && !cfg_.players->ready();
    if (existing != sessions_.end()) {
        const Session& old = *existing->second;
        const auto& result = old.control.login();
        if (!old.closing && old.request.character_id == character_id && old.request.ticket == ticket &&
            (!result || result->accepted)) {
            carried = true;
            carried_id = old.id;
            carried_result = result;
        }
        remove(from, carried ? "reconnected, keeping its login" : "reconnected");
    }

    const auto seqs = conn::initial_sequences(p->cookie);
    ConnectionConfig cc;
    cc.outer = conn::outer_header_of(*p);
    cc.custom_field = p->ext->custom_field();
    cc.server_initial_seq = seqs.server;
    cc.client_initial_seq = seqs.client;

    auto s = std::make_unique<Session>(cc, now, cfg_.world, random_challenge());
    s->id = carried ? carried_id : next_session_id_++;
    s->address = from;
    s->cookie = p->cookie;
    s->cookie_timestamp = p->timestamp;
    s->deadline = now + cfg_.login_deadline;
    s->request.character_id = character_id;
    s->request.ticket = ticket;

    conn::HandshakePacket a = *p;
    a.type = conn::HandshakeType::Ack;
    a.timestamp = -1.0;
    a.padding.assign(conn::kAckPaddingBytes, 0);
    random_(a.padding.data(), a.padding.size());
    s->ack_packet = conn::build_handshake(a);
    send_(from, s->ack_packet);
// C.Y.B x CoA

    Session& session = *s;
    by_id_[session.id] = from;
    if (carried) {
        spdlog::info("[endpoint] {} connected again for character {}: its login goes on", from.to_string(),
                     character_id);
    } else if (no_content) {
        spdlog::warn("[endpoint] {} connected for character {}: refused, worldd has no content.db ({})", from.to_string(), character_id,
                     cfg_.players->content_problem());
    } else {
        spdlog::info("[endpoint] {} connected for character {}", from.to_string(), character_id);
        logins_.submit(session.id, session.request);
    }
    sessions_[from] = std::move(s);
    // A pending login's result reaches the session by its id; one that finished is applied now.
    if (carried_result) on_login_result(session, *carried_result, now);
    if (!carried && no_content) {
        LoginResult refusal;
        refusal.error = login_text::kContentMissing;
        on_login_result(session, refusal, now);
    }
}

void Endpoint::post_login_result(uint64_t session_id, LoginResult result) {
    std::lock_guard<std::mutex> lock(results_mu_);
    results_.emplace_back(session_id, std::move(result));
}

void Endpoint::on_login_result(Session& s, LoginResult result, Clock::time_point now) {
    if (s.closing) return;  // already kicked or refused
    if (result.accepted) {
        auto other = by_character_.find(result.character.id);
        if (other != by_character_.end() && !(other->second == s.address)) {
            auto os = sessions_.find(other->second);
            if (os != sessions_.end()) {
                spdlog::info("[endpoint] character {} logged in again: dropping {}", result.character.id,
                             other->second.to_string());
                if (PlayerSession* old = os->second->player.get()) {
                    // The record in `result` was read when this login began: the old session knows where the character is
                    // now (its last periodic save may be a minute old, and its end is saved only now). The new session starts
                    // from that position, and the old one's hand-over goes to the saver now, before anything the new
                    // session saves.
                    const GoodPosition g = old->last_position();   // (a rider: beside its mount, on its feet; the mounts milestone, stage 3)
                    if (g.has_time_stamp) {
                        result.character.has_position = true;
                        result.character.position = {g.x, g.y, g.z, static_cast<float>(g.yaw_degrees)};
                    }
                    // the same goes for the items (plan B2): the record's are as old as the login's read, the old session's are the character's.
                    // When they differ from the record's, the old session's final save (handed to the saver below) is all that holds them in the
                    // database, and the saver may lose it: the new session is told, and saves its inventory when it ends even if the player
                    // never touches an item.
                    // (review A M3: the old session ends FIRST: its end persists a mount's Health into its inventory, and the new session must start from that)
                    old->finish("logged in elsewhere");
                    std::string unused;
                    const auto record_items = items::Inventory::from_items(result.character.items, unused);
                    s.carried_items_differ = !record_items || !(*record_items == old->inventory());
                    result.character.items = old->inventory().items();
                }
                apply(*os->second, os->second->control.kick(login_text::kLoggedInElsewhere), now);
            }
        }
        by_character_[result.character.id] = s.address;
        s.logged_in = true;
        s.character_name = result.character.name;
    }
    apply(s, s.control.on_login(result), now);
    if (!result.accepted && !s.closing) {
        // Refused before Hello: the Failure waits for Hello, but not for long.
        const auto soon = now + cfg_.close_linger;
        if (soon < s.deadline) s.deadline = soon;
    }
}

void Endpoint::apply(Session& s, ControlChannel::Reply reply, Clock::time_point now) {
    conn::BunchHeader control;
    control.reliable = true;
    control.channel = 0;
    for (const auto& m : reply.messages) {
        if (!s.connection.send(control, m)) reply.close = true;
    }
    if (reply.joined) {
        s.connection.set_playing();
        spdlog::info("[endpoint] character {} ({}) finished loading and joined", s.character_name,
                     s.request.character_id);
        start_player(s, now);
    }
    if (reply.close && !s.closing) {
        s.connection.send_close();
        s.closing = true;
        s.close_by = now + cfg_.close_linger;
    }
}

void Endpoint::start_player(Session& s, Clock::time_point now) {
    if (!cfg_.players || s.player || s.closing) return;
    const auto& login = s.control.login();
    if (!login || !login->accepted) return;   // cannot be: Join follows an accepted login
    std::vector<std::string> notes;
    const bootstrap::LoginIdentity identity = make_login_identity(s.control.login_message(), login->character, &notes);
    for (const std::string& note : notes) spdlog::info("[endpoint] character {} ({}): {}", s.character_name, s.request.character_id, note);
    std::string error;
    bootstrap::SetupRefusal refusal = bootstrap::SetupRefusal::None;
    s.player = PlayerSession::join(s.connection, *cfg_.players, login->character, identity, seconds(now), error, &refusal);
    if (s.player && s.carried_items_differ) s.player->inventory_may_be_unsaved();
    if (!s.player) {
        spdlog::error("[endpoint] character {} ({}) cannot join the world: {}", s.character_name, s.request.character_id, error);
        // The player's text: the one refusal a player can do something about (pick another character) is named.
        const char* text = refusal == bootstrap::SetupRefusal::UnsupportedRace ? login_text::kRaceUnsupported : login_text::kLoginFailed;
        apply(s, s.control.kick(text), now);
    }
}

void Endpoint::remove(const Address& a, const char* why) {
    auto it = sessions_.find(a);
    if (it == sessions_.end()) return;
    Session& s = *it->second;
    spdlog::info("[endpoint] {} ({}): {}", a.to_string(), s.request.character_id, why);
    if (s.player) s.player->finish(why);   // the last good position is saved whatever ended the session
    auto c = by_character_.find(s.request.character_id);
    if (c != by_character_.end() && c->second == a) by_character_.erase(c);
    auto i = by_id_.find(s.id);
    if (i != by_id_.end() && i->second == a) by_id_.erase(i);
    sessions_.erase(it);
}

bool Endpoint::session_limit_reached(const Address& from, Clock::time_point now) {
    if (cfg_.max_sessions == 0 || sessions_.size() < cfg_.max_sessions || sessions_.count(from)) return false;
    ++refused_for_limit_;
    log_refused_for_limit(now);
    return true;
}

void Endpoint::log_refused_for_limit(Clock::time_point now) {
    if (refused_for_limit_ == 0) return;
    if (limit_logged_ && now - *limit_logged_ < kLimitLogInterval) return;
    spdlog::warn("[endpoint] the limit of {} sessions is reached: refused {} handshake(s)", cfg_.max_sessions,
                 refused_for_limit_);
    refused_for_limit_ = 0;
    limit_logged_ = now;
}

void Endpoint::tick(Clock::time_point now) {
    signer_.update(seconds(now));
    if (cfg_.players) cfg_.players->tick_world(seconds(now));   // plan E1, stage 2: the NPCs walk

    std::vector<std::pair<uint64_t, LoginResult>> done;
    {
        std::lock_guard<std::mutex> lock(results_mu_);
        done.swap(results_);
    }
    for (auto& [id, result] : done) {
        const auto at = by_id_.find(id);
        if (at == by_id_.end()) continue;  // its session is gone
        const auto s = sessions_.find(at->second);
        if (s != sessions_.end() && s->second->id == id) on_login_result(*s->second, std::move(result), now);
    }
    log_refused_for_limit(now);
// [C_Y_B - CoA]

    std::vector<std::pair<Address, const char*>> drop;
    for (auto& [addr, s] : sessions_) {
        if (s->connection.closed_by_client()) {
            drop.emplace_back(addr, "the client quit");
            continue;
        }
        if (s->connection.timed_out(now)) {
            drop.emplace_back(addr, "timed out");
            continue;
        }
        if (s->connection.failed()) {
            drop.emplace_back(addr, "broke the protocol");
            continue;
        }
        if (!s->closing && s->control.state() < ControlChannel::State::AwaitJoin && now >= s->deadline) {
            drop.emplace_back(addr, "did not log in in time");
            continue;
        }
        try {
            if (s->player && !s->closing) {
                s->player->tick(s->connection, now);
                if (s->player->failed()) apply(*s, s->control.kick(login_text::kLoginFailed), now);
            }
            for (auto& pkt : s->connection.flush(now)) send_(addr, pkt);
            if (s->player) s->player->take_acks(s->connection);   // what flush dropped counts as lost
            if (s->closing && (now >= s->close_by || s->connection.unacked_reliable(0) == 0))
                drop.emplace_back(addr, "closed");
        } catch (const std::exception& e) {
            // One session's fault stays its own: it is dropped, the others go on.
            spdlog::error("[endpoint] {} ({}): the tick threw {}: dropping the session", addr.to_string(), s->request.character_id, e.what());
            drop.emplace_back(addr, "threw an exception");
        }
    }
    for (const auto& [addr, why] : drop) remove(addr, why);
}

void Endpoint::kick_all(const std::string& text, Clock::time_point now) {
    for (auto& [addr, s] : sessions_) apply(*s, s->control.kick(text), now);
}

void Endpoint::finish_players(const char* why) {
    for (auto& [addr, s] : sessions_) {
        if (!s->player) continue;
        try {
            s->player->finish(why);
        } catch (const std::exception& e) {
            spdlog::error("[endpoint] {} ({}): saving the position threw {}", addr.to_string(), s->request.character_id, e.what());
        }
    }
}

const PlayerSession* Endpoint::player_at(const Address& a) const {
    const auto it = sessions_.find(a);
    return it == sessions_.end() ? nullptr : it->second->player.get();
}

std::vector<PlayerHandle> Endpoint::players() {
    std::vector<std::pair<uint64_t, PlayerHandle>> found;
    for (auto& [address, s] : sessions_) {
        if (!s->player || s->closing || s->player->failed()) continue;
        found.push_back({s->id, PlayerHandle{s->player.get(), &s->connection}});
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<PlayerHandle> out;
    out.reserve(found.size());
    for (const auto& f : found) out.push_back(f.second);
    return out;
}

std::optional<Address> Endpoint::session_of(const std::string& character_id) const {
    auto it = by_character_.find(character_id);
    if (it == by_character_.end()) return std::nullopt;
    return it->second;
}

}} // namespace aoc::worldd
