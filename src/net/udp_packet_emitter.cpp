// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/udp_packet_emitter.cpp
//
//  Session F: emit raw bunch bits via send callback.
//  Session G: produce COMPLETE UE5 S>C packets, then hand to send callback.
//
//  Complete-packet layout (matches handle_game_data's parse path in reverse):
//
//    [MagicHeader 32b][SessionID 2b][ClientID 3b][HandshakeBit=0 1b]   (38b)
//    [FNetPacketNotify packed header 32b]                              (70b)
//    [one history word 32b]                                           (102b)
//    [AoC custom field 48b]                                           (150b)
//    [bHasPktInfo=1 1b][jitter 10b][bHasSrvFrame=0 1b]                (162b)
//    [bunch payload ... n bits]
//    [termination bit 1]
//    [zero pad to byte boundary]
//
//  The AoC client rejects S>C packets that skew any of these fields.
// ============================================================================
#include "net/udp_packet_emitter.h"
#include "protocol/wire/ue5_primitives.h"
#include <spdlog/spdlog.h>

namespace aoc { namespace net {

using namespace protocol;
using namespace world;

// ─── Helper: allocate a simple channel id per (client, actor) pair ────────
//
// UE5 uses 10-bit channel indices per client.  Session G uses a stable hash
// so the same (client, actor) pair always lands on the same channel.
// Session H will replace this with a proper channel-open/close table per
// client session (channels are reusable after close).

static uint32_t channel_for(const std::string& client_key, uint64_t netguid) {
    uint64_t h = std::hash<std::string>{}(client_key);
    h ^= netguid * 2654435761ull;
    return static_cast<uint32_t>(h % 1000u + 10u);  // avoid reserved 0..9
}
// Cyb captures, C | O | A code

// ─── emit_spawn ────────────────────────────────────────────────────────────

void UdpPacketEmitter::emit_spawn(const std::string& c,
                                    const simulation::SimulationActor& a) {
    counters_.spawns.fetch_add(1, std::memory_order_relaxed);

    const schema::ActorSchema* sch = schemas_.get_schema(a.type);
    if (!sch) {
        spdlog::warn("[UdpEmitter] No schema for type={} on actor {:#x}",
                     static_cast<int>(a.type), a.netguid);
        return;
    }

    emit::BunchWriter bw;
    emit::EmitContext ctx;
    ctx.channel         = channel_for(c, a.netguid);
    ctx.ch_sequence     = 1;
    ctx.is_reliable     = true;
    ctx.partial_initial = true;
    ctx.partial_final   = true;

    size_t bits = builder_.build_spawn(*sch, a.runtime, ctx, bw);
    if (bits == 0) {
        spdlog::warn("[UdpEmitter] build_spawn returned 0 bits for actor {:#x}",
                     a.netguid);
        return;
    }
    wrap_and_send(c, bw);
}

// ─── emit_property_delta ───────────────────────────────────────────────────

void UdpPacketEmitter::emit_property_delta(const std::string& c,
                                             const simulation::SimulationActor& a,
                                             const std::vector<replication::ChangedProperty>& ch) {
    counters_.deltas.fetch_add(1, std::memory_order_relaxed);

    const schema::ActorSchema* sch = schemas_.get_schema(a.type);
    if (!sch) return;

    // Split changes into root vs component.  Component-scoped deltas need a
    // build_component_delta extension (Session H); for now we emit root
    // deltas and log the skipped component changes.
    std::vector<uint32_t> root_handles;
    size_t skipped_component_changes = 0;
    for (const auto& change : ch) {
        if (change.component_index < 0) root_handles.push_back(change.handle);
        else ++skipped_component_changes;
    }
    if (skipped_component_changes > 0) {
        spdlog::debug("[UdpEmitter] Skipped {} component-scoped deltas for "
                      "actor {:#x} (component deltas deferred to Session H)",
                      skipped_component_changes, a.netguid);
    }
    if (root_handles.empty()) return;

    emit::BunchWriter bw;
    emit::EmitContext ctx;
    ctx.channel         = channel_for(c, a.netguid);
    ctx.ch_sequence     = 2;
    ctx.is_reliable     = true;
    ctx.partial_initial = true;
    ctx.partial_final   = true;

    size_t bits = builder_.build_delta(*sch, a.runtime, root_handles, ctx, bw);
    if (bits == 0) return;
    wrap_and_send(c, bw);
}

// ─── emit_destroy ──────────────────────────────────────────────────────────

void UdpPacketEmitter::emit_destroy(const std::string& c, uint64_t guid) {
    counters_.destroys.fetch_add(1, std::memory_order_relaxed);

    emit::BunchWriter bw;
    emit::EmitContext ctx;
    ctx.channel         = channel_for(c, guid);
    ctx.ch_sequence     = 3;
    ctx.is_reliable     = true;
    ctx.partial_initial = true;
    ctx.partial_final   = true;

    size_t bits = builder_.build_destroy(ctx, bw);
    if (bits == 0) return;
    wrap_and_send(c, bw);
}

// ─── emit_movement ─────────────────────────────────────────────────────────

void UdpPacketEmitter::emit_movement(const std::string& c, uint64_t guid,
                                       emit::FVector3 loc, emit::FVector3 vel) {
    counters_.moves.fetch_add(1, std::memory_order_relaxed);
    // Session H: emit a real movement delta via FFastActorLocationArray.
    // For now the counter increments so observers see cadence.
    (void)c; (void)guid; (void)loc; (void)vel;
}

// ─── Session H.2 NMT emitters ─────────────────────────────────────────────

void UdpPacketEmitter::send_nmt_welcome(const std::string& client_key,
                                           const std::string& level,
                                           const std::string& gamemode,
                                           const std::string& redirect_url,
                                           bool opens_channel) {
    if (!outer_) {
        counters_.outer_lookup_failed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Single outer_() call — it reads AND advances client seq counters
    // on GameServer's side.  We use the same snapshot for ch_sequence
    // (NMT bunch header) AND outer-packet framing to stay atomic.
    auto outer_opt = outer_(client_key);
    if (!outer_opt) {
        counters_.outer_lookup_failed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    emit::BunchWriter bw;
    emit::NmtBunchContext ctx;
    ctx.ch_sequence   = outer_opt->reliable_ch_seq;
    ctx.opens_channel = opens_channel;
    size_t bits = emit::NmtBuilder::build_welcome(bw, ctx, level, gamemode, redirect_url);
    if (bits == 0) {
        spdlog::warn("[UdpEmitter] build_welcome returned 0 bits");
        return;
    }
    wrap_and_send_with_state(client_key, *outer_opt, bw);
}
// (C-O-A ~ C.Y.B)

void UdpPacketEmitter::send_nmt_challenge(const std::string& client_key,
                                             const std::string& challenge,
                                             bool opens_channel) {
    if (!outer_) {
        counters_.outer_lookup_failed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto outer_opt = outer_(client_key);
    if (!outer_opt) {
        counters_.outer_lookup_failed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    emit::BunchWriter bw;
    emit::NmtBunchContext ctx;
    ctx.ch_sequence   = outer_opt->reliable_ch_seq;
    ctx.opens_channel = opens_channel;
    size_t bits = emit::NmtBuilder::build_challenge(bw, ctx, challenge);
    if (bits == 0) {
        spdlog::warn("[UdpEmitter] build_challenge returned 0 bits");
        return;
    }
    wrap_and_send_with_state(client_key, *outer_opt, bw);
}

// ─── wrap_and_send ────────────────────────────────────────────────────────
//
// Assemble the full S>C packet and hand it to the send callback.  This is
// the core of Session G: we produce bytes the real client could consume.

void UdpPacketEmitter::wrap_and_send(const std::string& client_key,
                                       const emit::BunchWriter& bw) {
    if (!outer_ || !send_) {
        counters_.outer_lookup_failed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto outer_opt = outer_(client_key);
    if (!outer_opt) {
        counters_.outer_lookup_failed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    wrap_and_send_with_state(client_key, *outer_opt, bw);
}

void UdpPacketEmitter::wrap_and_send_with_state(const std::string& client_key,
                                                   const OuterPacketState& outer,
                                                   const emit::BunchWriter& bw) {
    if (!send_) {
        counters_.outer_lookup_failed.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Reserve a buffer big enough for outer fields + bunch bits + termination.
    // Outer header is <30B; bunch is bw.byte_size(); pad of ~2B for
    // termination + byte alignment.  Round up generously.
    const size_t bunch_bytes = bw.byte_size();
    std::vector<uint8_t> buf;
    buf.resize(std::max<size_t>(outer.reserve_bytes_hint,
                                  bunch_bytes + 64));
    size_t off = 0;

    // 1. Outer header: Magic(32) + SessionID(2) + ClientID(3) + HandshakeBit=0(1)
    ::ue5::write_bits(buf.data(), buf.size(), off, outer.magic_header, 32);
    ::ue5::write_bits(buf.data(), buf.size(), off, outer.session_id, 2);
    ::ue5::write_bits(buf.data(), buf.size(), off, outer.client_id_bits, 3);
    ::ue5::write_bits(buf.data(), buf.size(), off, 0u, 1);  // handshake_bit=0 (game data)
// (C:Y:B) (C | O | A)

    // 2. FNetPacketNotify packed header — 32 bits.
    //    Layout: [out_seq 14][in_ack_seq 14][hist_count-1 4]
    //    hist_count=1 → field value 0; one history word follows.
    const uint16_t hist_count = 1;
    uint32_t packed =
        (static_cast<uint32_t>(outer.out_seq    & 0x3FFF) << 18) |
        (static_cast<uint32_t>(outer.in_ack_seq & 0x3FFF) <<  4) |
        (static_cast<uint32_t>((hist_count - 1) & 0x0F));
    ::ue5::write_bits(buf.data(), buf.size(), off, packed, 32);

    // 3. History words (all acked = 0xFFFFFFFF for hist_count=1).
    for (uint16_t i = 0; i < hist_count; ++i) {
        ::ue5::write_bits(buf.data(), buf.size(), off, 0xFFFFFFFFu, 32);
    }

    // 4. AoC custom field (6 bytes / 48 bits) if present.
    if (outer.custom_field_present) {
        for (int i = 0; i < 6; ++i) {
            ::ue5::write_bits(buf.data(), buf.size(), off,
                              outer.custom_field[i], 8);
        }
    }

    // 5. PacketInfo: bHasPktInfo=1 + 10-bit jitter + bHasSrvFrame=0
    //    Real AoC server uses jitter=1023 (max) for synthesized packets.
    //    bHasSrvFrame=0 means no 8-bit frame-time byte follows.
    ::ue5::write_bits(buf.data(), buf.size(), off, 1u, 1);      // bHasPktInfo
    ::ue5::write_bits(buf.data(), buf.size(), off, 1023u, 10);  // jitter
    ::ue5::write_bits(buf.data(), buf.size(), off, 0u, 1);      // bHasSrvFrame

    // 6. Bunch payload — write the bunch's bits verbatim.
    const size_t bunch_bits = bw.bit_pos();
    // Grow buffer if the bunch is bigger than our initial reserve.
    size_t total_bits_after_bunch = off + bunch_bits + 1 /*term*/;
    size_t needed_bytes = (total_bits_after_bunch + 7) / 8 + 2;
    if (buf.size() < needed_bytes) buf.resize(needed_bytes, 0);

    bw.bytes();  // unused result; ensures internal state is stable
    // Copy bit-by-bit from bw's buffer into our packet buffer.
    const uint8_t* bw_data = bw.data();
    for (size_t i = 0; i < bunch_bits; ++i) {
        int bit = (bw_data[i >> 3] >> (i & 7)) & 1;
        ::ue5::write_bits(buf.data(), buf.size(), off, bit, 1);
    }

    // 7. AoC PacketHandler sentinel + termination bit.
    //    Phase B.0p5 (2026-04-27): without the AoC sentinel bit BEFORE
    //    the standard termination, the client logs "0's in last byte"
    //    and silently drops the packet.  Same bug pattern as
    //    send_bunch_packet, send_ch0_reliable_payload, etc.
    ::ue5::write_bits(buf.data(), buf.size(), off, 1u, 1);  // AoC sentinel
    ::ue5::write_bits(buf.data(), buf.size(), off, 1u, 1);  // UE5 termination

    // 8. Final byte length — round bit position up to byte boundary.
    size_t final_bytes = (off + 7) / 8;
    buf.resize(final_bytes);

    counters_.bytes_produced.fetch_add(final_bytes, std::memory_order_relaxed);
    send_(client_key, buf.data(), buf.size());
}

}} // namespace aoc::net
