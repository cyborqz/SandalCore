// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/live_world.cpp
// ============================================================================
#include "net/live_world.h"
#include <spdlog/spdlog.h>

namespace aoc { namespace net {

// ─── Helper: pick the right emitter ────────────────────────────────────────
//
// If force_recording OR either callback is null, use a RecordingPacketEmitter.
// Otherwise construct a UdpPacketEmitter that produces complete UE5 packets
// and forwards through the send callback.
static world::replication::IPacketEmitter*
install_emitter(bool force_recording,
                const protocol::schema::SchemaRegistry& schemas,
                UdpPacketEmitter::OuterStateFn outer_fn,
                UdpPacketEmitter::SendFn send_fn,
                std::unique_ptr<RecordingPacketEmitter>& out_rec,
                std::unique_ptr<UdpPacketEmitter>& out_udp) {
    const bool use_recording = force_recording || !outer_fn || !send_fn;
    if (use_recording) {
        out_rec = std::make_unique<RecordingPacketEmitter>();
        return out_rec.get();
    }
    out_udp = std::make_unique<UdpPacketEmitter>(
        schemas, std::move(outer_fn), std::move(send_fn));
    return out_udp.get();
}

// ─── Construction ──────────────────────────────────────────────────────────

LiveWorld::LiveWorld(const protocol::schema::SchemaRegistry& schemas,
                      UdpPacketEmitter::OuterStateFn outer_fn,
                      UdpPacketEmitter::SendFn send_fn,
                      LiveWorldConfig config)
    : config_(config),
      events_(),
      clock_(),
      actors_(events_, clock_),
      visibility_(),
      recording_(nullptr),
      udp_(nullptr),
      emitter_(install_emitter(config.use_recording_emitter,
                                schemas, std::move(outer_fn), std::move(send_fn),
                                recording_, udp_)),
      broadcast_(events_, actors_, visibility_, *emitter_),
      netguid_(),
      sessions_(),
      dispatcher_(sessions_, &netguid_, &events_)
{
    broadcast_.install_subscriptions();
    spdlog::info("[LiveWorld] Initialized (emitter={}, replication_hz={})",
                 recording_ ? "recording" : "udp",
                 config_.replication_hz);
}

LiveWorld::~LiveWorld() {
    stop();
}

// ─── Lifecycle ─────────────────────────────────────────────────────────────
// Cyb / C | O | A

void LiveWorld::start() {
    if (running_.exchange(true)) return;  // already running
    {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.started_at = std::chrono::steady_clock::now();
    }
    tick_thread_ = std::thread([this]() { tick_loop(); });
    spdlog::info("[LiveWorld] Started replication-tick thread @ {}Hz",
                 config_.replication_hz);
}

void LiveWorld::stop() {
    if (!running_.exchange(false)) return;  // already stopped
    if (tick_thread_.joinable()) tick_thread_.join();
    spdlog::info("[LiveWorld] Stopped");
}

// ─── Client hooks ──────────────────────────────────────────────────────────

void LiveWorld::on_client_connected(const std::string& client_key) {
    // Ensure a session exists (dispatcher also does get_or_create, but that
    // path is triggered by packets; here we pre-populate so a client that
    // finishes handshake via legacy path still appears in our registry).
    ClientSession* cs = sessions_.get_or_create(client_key);
    visibility_.add_client(client_key);

    // Session H.1: the StatelessConnect handshake is handled by the legacy
    // path in GameServer before this hook fires.  Jump the LiveWorld
    // session past AWAITING_HANDSHAKE / HANDSHAKE_IN_PROGRESS directly to
    // NMT_NEGOTIATING so the dispatcher's NMT handlers accept subsequent
    // real-client traffic.
    if (cs && cs->phase == ClientPhase::AWAITING_HANDSHAKE) {
        cs->transition_to(ClientPhase::NMT_NEGOTIATING);
    }

    spdlog::info("[LiveWorld] Client connected: {}  (sessions={}, phase={})",
                 client_key, sessions_.size(),
                 cs ? to_string(cs->phase) : "nullptr");
}

void LiveWorld::on_client_disconnected(const std::string& client_key) {
    // Mark the session DISCONNECTING so any in-flight dispatch fails cleanly,
    // then release resources.
    if (ClientSession* cs = sessions_.get(client_key)) {
        cs->transition_to(ClientPhase::DISCONNECTING);
        if (cs->netguid_block.is_valid()) {
            netguid_.release_player_block(client_key);
        }
    }
    visibility_.remove_client(client_key);
    sessions_.remove(client_key);
    spdlog::info("[LiveWorld] Client disconnected: {}  (sessions={})",
                 client_key, sessions_.size());
}

// ─── Packet entry ──────────────────────────────────────────────────────────

DispatchResult LiveWorld::on_packet(const DispatchPacket& pkt) {
    {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.total_packets_in++;
    }
    DispatchResult r = dispatcher_.dispatch(pkt);
    if (!r.accepted) {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.total_rejects++;
    } else {
        // Session H.2 — accepted dispatch may trigger a server reply.
        maybe_send_nmt_reply(pkt, r);
    }
    return r;
}

// ─── Session H.2 dispatcher post-hook ──────────────────────────────────────

void LiveWorld::maybe_send_nmt_reply(const DispatchPacket& pkt,
                                        const DispatchResult& r) {
    if (!udp_) return;  // recording-mode LiveWorld has no UDP emitter

    // Rule 1: NMT_Hello → reply with NMT_Challenge.
    //   The real AoC client sends NMT_Hello once per connection, and
    //   the server replies with NMT_Challenge containing a challenge
    //   string.  NMT_Hello stays in NMT_NEGOTIATING (no phase change)
    //   so we key on the opcode, not the transition.
    if (pkt.op == DispatchOp::NMT_HELLO) {
        spdlog::info("[LiveWorld/H.2] NMT_Hello observed for {}; "
                      "emitting NMT_Challenge (challenge=\"{}\")",
                      pkt.client_key, nmt_reply_cfg_.challenge_string);
        udp_->send_nmt_challenge(pkt.client_key,
                                   nmt_reply_cfg_.challenge_string,
                                   /*opens_channel=*/false);
        return;
    }

    // Rule 2: NMT_Login accepted → AUTHENTICATED → reply with NMT_Welcome.
    if (pkt.op == DispatchOp::NMT_LOGIN &&
        r.new_phase == ClientPhase::AUTHENTICATED) {
        spdlog::info("[LiveWorld/H.2] NMT_Login accepted for {}; "
                      "emitting NMT_Welcome (level=\"{}\")",
                      pkt.client_key, nmt_reply_cfg_.welcome_level);
        udp_->send_nmt_welcome(pkt.client_key,
                                 nmt_reply_cfg_.welcome_level,
                                 nmt_reply_cfg_.welcome_gamemode,
                                 nmt_reply_cfg_.welcome_redirect,
                                 /*opens_channel=*/false);
        return;
    }
}

void LiveWorld::on_data_packet_observed(const std::string& client_key) {
    if (ClientSession* cs = sessions_.get(client_key)) {
        cs->packets_received++;
        cs->touch();
    }
    std::lock_guard<std::mutex> lk(stats_mu_);
    stats_.total_packets_in++;
}

// ─── Tick loop ─────────────────────────────────────────────────────────────

void LiveWorld::tick_loop() {
    const auto interval_us =
        world::simulation::WorldClock::interval_us(config_.replication_hz);
    const std::chrono::microseconds interval(interval_us);
    auto next = std::chrono::steady_clock::now() + interval;

    // Heartbeat: emit an info-level summary every ~5 seconds so an operator
    // watching the log can see the pipeline is alive.  5s is a compromise
    // between "useful feedback" and "not drowning the log".
    auto last_heartbeat = std::chrono::steady_clock::now();
    const auto heartbeat_interval = std::chrono::seconds(5);

    while (running_.load(std::memory_order_acquire)) {
        // Sleep until the next tick boundary, but wake early if stop() flips
        // running_ — cheap polling every 10ms keeps shutdown snappy without
        // a condition variable.  A real prod version would use CV.
        while (running_.load(std::memory_order_acquire)) {
            auto now = std::chrono::steady_clock::now();
            if (now >= next) break;
            auto delta = std::min<std::chrono::microseconds>(
                std::chrono::duration_cast<std::chrono::microseconds>(next - now),
                std::chrono::microseconds(10'000));
            std::this_thread::sleep_for(delta);
        }
        if (!running_.load(std::memory_order_acquire)) break;
// C | O | A with C:Y:B

        try {
            broadcast_.tick();
        } catch (const std::exception& e) {
            spdlog::error("[LiveWorld] tick() threw: {}", e.what());
        }

        uint64_t ticks_now = 0;
        uint64_t packets_now = 0;
        uint64_t rejects_now = 0;
        {
            std::lock_guard<std::mutex> lk(stats_mu_);
            stats_.tick_count++;
            ticks_now = stats_.tick_count;
            packets_now = stats_.total_packets_in;
            rejects_now = stats_.total_rejects;
        }

        // Heartbeat — skip the VERY first one (0 ticks = boring).
        auto now_hb = std::chrono::steady_clock::now();
        if (now_hb - last_heartbeat >= heartbeat_interval) {
            spdlog::info("[LiveWorld] heartbeat: ticks={} sessions={} "
                          "packets_in={} rejects={} actors={}",
                          ticks_now, sessions_.size(),
                          packets_now, rejects_now,
                          actors_.size());
            last_heartbeat = now_hb;
        }

        next += interval;
        // If the tick ran long and we're way behind, resync instead of
        // chasing phantom ticks.
        auto now = std::chrono::steady_clock::now();
        if (now > next + interval * 5) next = now + interval;
    }
}

// ─── Stats ─────────────────────────────────────────────────────────────────

LiveWorld::Stats LiveWorld::stats() const {
    std::lock_guard<std::mutex> lk(stats_mu_);
    return stats_;
}

}} // namespace aoc::net
