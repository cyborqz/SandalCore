// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/conn/packet_notify.h
//
//  Packet acknowledgement, ported from UE5 FNetPacketNotify.
//
//  Every data packet starts (after the 38-bit outer header) with a notify
//  header: a packed uint32 (Seq:14 | AckedSeq:14 | HistoryWordCount-1:4)
//  followed by HistoryWordCount 32-bit words.  AckedSeq is the newest packet
//  the sender has received; bit i of the history says whether packet
//  AckedSeq - i arrived.  From the peer's headers a connection learns which
//  of its own packets were delivered (so reliable data in lost ones is sent
//  again), and it writes its own receive history into every packet it sends.
//
//  Confirmed against the 2026-09-28 capture: the client's first packets carry
//  AckedSeq = server initial seq - 1 and history 0x0, then 0x1 and 0x3 as the
//  server's packets arrive (test_packet_notify).
//
//  LAYER: Protocol / conn
// ============================================================================
#pragma once

#include "protocol/conn/sequence.h"
#include "protocol/emit/bunch_writer.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

namespace aoc { namespace protocol { namespace conn {

constexpr size_t kHistoryWordBits = 32;
constexpr size_t kMaxHistoryWords = 8;                                 // 256 packets
constexpr size_t kMaxHistoryBits  = kMaxHistoryWords * kHistoryWordBits;

/// The notify header of one packet.
struct NotifyHeader {
    uint16_t seq = 0;          // this packet's sequence number
    uint16_t acked_seq = 0;    // newest packet the sender has received
    uint8_t  history_words = 1;                    // 1..8
    std::array<uint32_t, kMaxHistoryWords> history{};

    /// True if packet `acked_seq - index` was received.
    bool is_delivered(size_t index) const {
        return (history[index / kHistoryWordBits] >> (index % kHistoryWordBits)) & 1u;
    }
};
// CoA.C:Y:B

/// Writes the packed header and its history words.
void write_notify_header(emit::BunchWriter& w, const NotifyHeader& h);

/// Reads a notify header at bit `off` (advancing it); nullopt if the packet
/// ends first.
std::optional<NotifyHeader> read_notify_header(const uint8_t* data, size_t len,
                                               size_t end_bit, size_t& off);

/// The 256-packet receive history: bit 0 is the newest packet.
class SequenceHistory {
public:
    void reset() { words_.fill(0); }
    /// Shifts the history by one packet and records whether it arrived.
    void add_delivery_status(bool delivered);
    bool is_delivered(size_t index) const {
        return (words_[index / kHistoryWordBits] >> (index % kHistoryWordBits)) & 1u;
    }
    uint32_t word(size_t i) const { return words_[i]; }

private:
    std::array<uint32_t, kMaxHistoryWords> words_{};
};

/// One side's packet sequencing and acknowledgement state.
class PacketNotify {
public:
    /// `initial_in_seq` is the peer's initial sequence minus one (nothing
    /// received yet); `initial_out_seq` is the first sequence this side sends.
    void init(uint16_t initial_in_seq, uint16_t initial_out_seq);

    /// Checks a received header.  Returns how far it advances the incoming
    /// sequence (> 0: a new packet to process; 0: a duplicate, stale or
    /// invalid packet to drop).  For an accepted packet, `on_ack(seq,
    /// delivered)` is called for each of this side's packets that the header
    /// newly reports, oldest first.
    template <class OnAck>
    int update(const NotifyHeader& h, OnAck&& on_ack) {
        const int delta = sequence_delta(h);
        if (delta <= 0) return 0;
        process_received_acks(h, on_ack);
        return internal_update(h, delta);
    }

    /// True while the receive window is being re-synchronised after a jump
    /// of more than 256 packets: update() drops packets (returning 0) until
    /// the peer acks the header that carried the advanced window.
    bool is_waiting_for_history_flush() const {
        return seq_greater(waiting_for_flush_seq_ack_, out_ack_seq_);
    }

    /// Records the packet accepted by the last update() as received; packets
    /// skipped since the previous one are recorded as lost.
    void ack_seq(uint16_t seq);

    /// The header for the next packet this side sends.
    NotifyHeader make_header() const;

    /// Call once per packet sent with make_header(); returns the sequence
    /// that packet used.  The ack this packet carries is recorded here, from
    /// in_ack_seq() at this moment, so ack_seq() must not run between
    /// make_header() and this call: the peer would see a different ack than
    /// the one recorded.
    uint16_t commit_and_increment_out_seq();

    /// True while 256 packets are unacknowledged: sending more would leave the
    /// peer unable to report on them.
    bool is_window_full() const {
        return seq_diff(out_seq_, out_ack_seq_) > static_cast<int>(kMaxHistoryBits);
    }

    uint16_t in_seq() const { return in_seq_; }
    uint16_t in_ack_seq() const { return in_ack_seq_; }
    uint16_t out_seq() const { return out_seq_; }
    uint16_t out_ack_seq() const { return out_ack_seq_; }

private:
    struct SentAck { uint16_t out_seq; uint16_t in_ack_seq; };

    int sequence_delta(const NotifyHeader& h) const;

    template <class OnAck>
    void process_received_acks(const NotifyHeader& h, OnAck& on_ack) {
        if (!seq_greater(h.acked_seq, out_ack_seq_)) return;
        int ack_count = seq_diff(h.acked_seq, out_ack_seq_);
        // The window may have been re-based past the record (internal_update).
        const uint16_t new_in_ack_seq_ack = update_in_ack_seq_ack(ack_count, h.acked_seq);
        if (seq_greater(new_in_ack_seq_ack, in_ack_seq_ack_)) in_ack_seq_ack_ = new_in_ack_seq_ack;
        uint16_t current = seq_add(out_ack_seq_, 1);
        const int history_bits = static_cast<int>(h.history_words * kHistoryWordBits);
        // Packets older than the history the peer sent are lost.
        while (ack_count > history_bits) {
            --ack_count;
            on_ack(current, false);
            current = seq_add(current, 1);
        }
        while (ack_count > 0) {
            --ack_count;
            on_ack(current, h.is_delivered(static_cast<size_t>(ack_count)));
            current = seq_add(current, 1);
        }
        out_ack_seq_ = h.acked_seq;
        if (seq_greater(out_ack_seq_, waiting_for_flush_seq_ack_)) waiting_for_flush_seq_ack_ = out_ack_seq_;
    }
// [C | O | A - C_Y_B]

    uint16_t update_in_ack_seq_ack(int ack_count, uint16_t acked_seq);
    int internal_update(const NotifyHeader& h, int delta);
    bool will_sequence_fit_in_history(uint16_t seq) const;
    bool has_unacknowledged_acks() const;
    void ack_seq(uint16_t seq, bool delivered);
    size_t history_length() const;

    SequenceHistory in_history_;
    std::deque<SentAck> ack_record_;
    uint16_t in_seq_ = 0;          // newest packet received
    uint16_t in_ack_seq_ = 0;      // newest packet recorded in in_history_
    uint16_t in_ack_seq_ack_ = 0;  // newest in_ack_seq the peer has seen us send
    uint16_t out_seq_ = 0;         // next packet to send
    uint16_t out_ack_seq_ = 0;     // newest of ours the peer reported on
    uint16_t waiting_for_flush_seq_ack_ = 0;  // our packet whose ack ends a re-sync
};

}}} // namespace aoc::protocol::conn
