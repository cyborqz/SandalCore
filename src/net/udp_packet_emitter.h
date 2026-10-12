// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/udp_packet_emitter.h
//
//  Concrete replication::IPacketEmitter that serializes each call via the
//  ActorBuilder (schema-driven UE5 bunches) and wraps the result in a
//  complete UE5 S>C packet — outer header + FNetPacketNotify packed header
//  + AoC 6-byte custom field + PacketInfo + bunch payload + termination bit.
//
//  Two emitters live here:
//    1) RecordingEmitter — dependency-free, stores every call as a record.
//       Used by integration tests and anywhere we don't have a real socket.
//    2) UdpPacketEmitter — produces a full UDP-ready byte buffer per call
//       and hands it to a sender callback that owns the socket.
//
//  Per-client outer-packet state (magic header, session/client id bits,
//  sequence counters, custom field) is fetched on each emit via a lookup
//  callback supplied by GameServer.  This keeps the emitter portable
//  (no <winsock2.h>) and lets GameServer retain ownership of ClientState.
//
//  LAYER:   Net / emit-glue (bridges World to UDP)
//  SESSION: F (observer) + G (active byte-level emission)
// ============================================================================
#pragma once

#include "world/replication/packet_emitter.h"
#include "protocol/emit/actor_builder.h"
#include "protocol/emit/bunch_writer.h"
#include "protocol/emit/nmt_builder.h"
#include "protocol/schema/schema_registry.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace aoc { namespace net {

// ─── Recording emitter ─────────────────────────────────────────────────────
//
// Records every call for test assertions.  Does no bit-level work.

class RecordingPacketEmitter : public world::replication::IPacketEmitter {
public:
    struct SpawnRecord   { std::string client; uint64_t netguid; };
    struct DeltaRecord   { std::string client; uint64_t netguid;
                           std::vector<world::replication::ChangedProperty> changes; };
    struct DestroyRecord { std::string client; uint64_t netguid; };
    struct MoveRecord    { std::string client; uint64_t netguid;
                           protocol::emit::FVector3 loc;
                           protocol::emit::FVector3 vel; };

    std::vector<SpawnRecord>   spawns;
    std::vector<DeltaRecord>   deltas;
    std::vector<DestroyRecord> destroys;
    std::vector<MoveRecord>    moves;
// (C_Y_B ~ CoA)

    void emit_spawn(const std::string& c,
                    const world::simulation::SimulationActor& a) override {
        std::lock_guard<std::mutex> lk(mu_);
        spawns.push_back({c, a.netguid});
    }
    void emit_property_delta(const std::string& c,
                              const world::simulation::SimulationActor& a,
                              const std::vector<world::replication::ChangedProperty>& ch) override {
        std::lock_guard<std::mutex> lk(mu_);
        deltas.push_back({c, a.netguid, ch});
    }
    void emit_destroy(const std::string& c, uint64_t guid) override {
        std::lock_guard<std::mutex> lk(mu_);
        destroys.push_back({c, guid});
    }
    void emit_movement(const std::string& c, uint64_t guid,
                       protocol::emit::FVector3 loc,
                       protocol::emit::FVector3 vel) override {
        std::lock_guard<std::mutex> lk(mu_);
        moves.push_back({c, guid, loc, vel});
    }

    void clear() {
        std::lock_guard<std::mutex> lk(mu_);
        spawns.clear(); deltas.clear(); destroys.clear(); moves.clear();
    }

    size_t total() const {
        std::lock_guard<std::mutex> lk(mu_);
        return spawns.size() + deltas.size() + destroys.size() + moves.size();
    }

private:
    mutable std::mutex mu_;
};

// ─── Per-client outer-packet state (Session G) ─────────────────────────────
//
// The UdpPacketEmitter needs enough state to build a complete UE5 packet:
// the magic header bits, the session/client-id bits, the current out/in
// sequence counters, and the 6-byte AoC custom field established during
// handshake.  GameServer owns this in its ClientState struct; we avoid a
// direct dependency by having GameServer populate this lightweight snapshot
// on demand via a lookup callback.
//
// "Lightweight snapshot" means the caller populates fresh values just before
// each emit — no cached state inside the emitter.  Keeps thread safety
// trivial: the sequence counter increment happens inside the callback under
// the caller's lock.

struct OuterPacketState {
    uint32_t magic_header   = 0;
    uint8_t  session_id     = 0;   // 2 bits
    uint8_t  client_id_bits = 0;   // 3 bits
    uint16_t out_seq        = 0;   // 14-bit sender seq (WILL be incremented after use)
    uint16_t in_ack_seq     = 0;   // 14-bit last-seen receiver seq
    // Session H.2 — per-client reliable-channel-0 sequence for NMT bunches.
    // GameServer fills from ClientState::reliable_seq and advances it.
    // Independent from `out_seq` (which is the outer packet sequence).
    uint16_t reliable_ch_seq = 0;
    bool     custom_field_present = false;
    uint8_t  custom_field[6] = {}; // if present, appended after packed header
    // Byte-count hint to reserve in the output buffer.
    size_t   reserve_bytes_hint = 256;
};

// ─── UdpPacketEmitter ─────────────────────────────────────────────────────
//
// Produces a complete, UDP-ready byte buffer per IPacketEmitter call.
// Session G scope: we emit real bytes.  Whether they're actually SENT is
// decided by the caller's send callback — that callback is what owns the
// socket.  In dry-run mode (the callback logs and discards), the emitter
// still does all the work; nothing goes on the wire.

class UdpPacketEmitter : public world::replication::IPacketEmitter {
public:
    // Callback: fetch the per-client outer-packet state for the given key.
    // Returns nullopt if the client isn't connected (emit is then dropped).
    // The callback MUST advance the client's out_seq before returning so
    // multiple emits in quick succession don't collide.
    using OuterStateFn =
        std::function<std::optional<OuterPacketState>(const std::string&)>;

    // Callback: send a fully-formed UDP payload to the given client key.
    // In Session G's default (dry-run) mode, this just logs + counts.
    // When --session-g-send is on, GameServer wires this to real sendto.
    using SendFn =
        std::function<void(const std::string& client_key,
                            const uint8_t* data, size_t len)>;

    UdpPacketEmitter(const protocol::schema::SchemaRegistry& schemas,
                     OuterStateFn outer_fn,
                     SendFn send_fn)
        : schemas_(schemas),
          outer_(std::move(outer_fn)),
          send_(std::move(send_fn)) {}

    // ─── IPacketEmitter implementation ────────────────────────────────
    void emit_spawn(const std::string& c,
                    const world::simulation::SimulationActor& a) override;
    void emit_property_delta(const std::string& c,
                              const world::simulation::SimulationActor& a,
                              const std::vector<world::replication::ChangedProperty>& ch) override;
    void emit_destroy(const std::string& c, uint64_t guid) override;
    void emit_movement(const std::string& c, uint64_t guid,
                       protocol::emit::FVector3 loc,
                       protocol::emit::FVector3 vel) override;

    // ─── Session H.2 — NMT control-channel emitters ───────────────────
    // These call NmtBuilder + wrap_and_send to produce complete UE5 S>C
    // packets for the client's control channel 0.  Triggered by
    // LiveWorld when the dispatcher accepts an incoming NMT that warrants
    // a server reply (e.g. NMT_Login → reply with NMT_Welcome).
    //
    // `opens_channel=true` marks this as the FIRST NMT we're sending on
    // this client (will set bControl=1 + bOpen=1 to open the channel).
    // The BOOTSTRAPPING NMT_Challenge typically opens; Welcome follows
    // on the already-open channel.
    void send_nmt_welcome(const std::string& client_key,
                           const std::string& level,
                           const std::string& gamemode,
                           const std::string& redirect_url = "",
                           bool opens_channel = false);
// [Cyb :: C O A]

    void send_nmt_challenge(const std::string& client_key,
                              const std::string& challenge,
                              bool opens_channel = true);

    // Diagnostics — atomic counters (atomics can't be aggregate-initialized
    // on MSVC; ctor explicitly zeroes them).
    struct Counters {
        std::atomic<uint64_t> spawns;
        std::atomic<uint64_t> deltas;
        std::atomic<uint64_t> destroys;
        std::atomic<uint64_t> moves;
        std::atomic<uint64_t> outer_lookup_failed;
        std::atomic<uint64_t> bytes_produced;
        Counters() noexcept
            : spawns(0), deltas(0), destroys(0),
              moves(0), outer_lookup_failed(0), bytes_produced(0) {}
    };
    const Counters& counters() const { return counters_; }

private:
    const protocol::schema::SchemaRegistry& schemas_;
    protocol::emit::ActorBuilder builder_;
    OuterStateFn outer_;
    SendFn       send_;
    Counters     counters_;

    // Assemble: outer header + packed header + custom field + PacketInfo
    // + bunch bits + termination, and push through the send callback.
    // Does its own outer_() lookup (which ADVANCES out_seq once per call).
    void wrap_and_send(const std::string& client_key,
                        const protocol::emit::BunchWriter& bw);

    // Same but with a pre-fetched outer state — use when the caller has
    // already done the outer_() lookup (e.g. NMT emitters need the state
    // BEFORE building the bunch so the ch_sequence matches).  Does NOT
    // re-call outer_().
    void wrap_and_send_with_state(const std::string& client_key,
                                     const OuterPacketState& outer,
                                     const protocol::emit::BunchWriter& bw);
};

}} // namespace aoc::net
