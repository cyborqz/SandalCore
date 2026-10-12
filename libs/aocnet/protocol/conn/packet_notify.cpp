// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/conn/packet_notify.cpp
// ============================================================================
#include "protocol/conn/packet_notify.h"
#include "protocol/wire/ue5_primitives.h"

namespace aoc { namespace protocol { namespace conn {

void write_notify_header(emit::BunchWriter& w, const NotifyHeader& h) {
    // history_words is 1..8; a stray value must not read past the array.
    const size_t words = h.history_words < 1 ? 1 : (h.history_words > kMaxHistoryWords ? kMaxHistoryWords : h.history_words);
    const uint32_t packed = (static_cast<uint32_t>(h.seq & kSeqMask) << 18) |
                            (static_cast<uint32_t>(h.acked_seq & kSeqMask) << 4) |
                            static_cast<uint32_t>(words - 1);
    w.write_uint32(packed);
    for (size_t i = 0; i < words; ++i) w.write_uint32(h.history[i]);
}

std::optional<NotifyHeader> read_notify_header(const uint8_t* data, size_t len,
                                               size_t end_bit, size_t& off) {
    if (off + 32 > end_bit) return std::nullopt;
    const uint32_t packed = static_cast<uint32_t>(::ue5::read_bits(data, len, off, 32));
    NotifyHeader h;
    h.seq = static_cast<uint16_t>((packed >> 18) & kSeqMask);
    h.acked_seq = static_cast<uint16_t>((packed >> 4) & kSeqMask);
    h.history_words = static_cast<uint8_t>((packed & 0x0F) + 1);
    if (h.history_words > kMaxHistoryWords) return std::nullopt;
    if (off + h.history_words * kHistoryWordBits > end_bit) return std::nullopt;
    for (size_t i = 0; i < h.history_words; ++i)
        h.history[i] = static_cast<uint32_t>(::ue5::read_bits(data, len, off, 32));
    return h;
}

void SequenceHistory::add_delivery_status(bool delivered) {
    for (size_t i = kMaxHistoryWords - 1; i > 0; --i)
        words_[i] = (words_[i] << 1) | (words_[i - 1] >> (kHistoryWordBits - 1));
    words_[0] = (words_[0] << 1) | (delivered ? 1u : 0u);
}

void PacketNotify::init(uint16_t initial_in_seq, uint16_t initial_out_seq) {
    in_history_.reset();
    ack_record_.clear();
    in_seq_ = in_ack_seq_ = in_ack_seq_ack_ = initial_in_seq & kSeqMask;
    out_seq_ = initial_out_seq & kSeqMask;
    out_ack_seq_ = seq_add(out_seq_, -1);
    waiting_for_flush_seq_ack_ = out_ack_seq_;
}
// -- C | Y | B & C O A --

int PacketNotify::sequence_delta(const NotifyHeader& h) const {
    // A newer packet whose ack lies within what we have actually sent.
    const bool ack_valid = !seq_greater(out_ack_seq_, h.acked_seq) &&
                           seq_greater(out_seq_, h.acked_seq);
    if (seq_greater(h.seq, in_seq_) && ack_valid) return seq_diff(h.seq, in_seq_);
    return 0;
}

void PacketNotify::ack_seq(uint16_t seq) { ack_seq(seq, true); }

void PacketNotify::ack_seq(uint16_t seq, bool delivered) {
    while (seq_greater(seq, in_ack_seq_)) {
        in_ack_seq_ = seq_add(in_ack_seq_, 1);
        in_history_.add_delivery_status(in_ack_seq_ == seq ? delivered : false);
    }
}

size_t PacketNotify::history_length() const {
    if (!seq_greater(in_ack_seq_ack_, in_ack_seq_)) {
        const size_t n = static_cast<size_t>(seq_diff(in_ack_seq_, in_ack_seq_ack_));
        return n < kMaxHistoryBits ? n : kMaxHistoryBits;
    }
    return kMaxHistoryBits;
}

bool PacketNotify::will_sequence_fit_in_history(uint16_t seq) const {
    return !seq_greater(in_ack_seq_ack_, seq) &&
           seq_diff(seq, in_ack_seq_ack_) <= static_cast<int>(kMaxHistoryBits);
}

bool PacketNotify::has_unacknowledged_acks() const {
    for (size_t i = 0, n = history_length(); i < n; ++i)
        if (in_history_.is_delivered(i)) return true;
    return false;
}

// UE5 FNetPacketNotify::InternalUpdate.  A packet more than 256 past the
// newest ack the peer has confirmed cannot be recorded in the history.  If
// the history holds nothing the peer still needs, the window is re-based
// (the peer synthesises the losses); otherwise the window is advanced with
// losses as far as it goes and packets are dropped until the peer acks the
// header carrying that, after which normal acceptance resumes.
int PacketNotify::internal_update(const NotifyHeader& h, int delta) {
    if (!is_waiting_for_history_flush() && !will_sequence_fit_in_history(h.seq)) {
        if (has_unacknowledged_acks()) waiting_for_flush_seq_ack_ = out_seq_;
        else in_ack_seq_ack_ = seq_add(h.seq, -1);
    }
    if (!is_waiting_for_history_flush()) {
        in_seq_ = h.seq;
        return delta;
    }
    uint16_t new_in_seq_to_ack = h.seq;
    if (!will_sequence_fit_in_history(h.seq) && has_unacknowledged_acks())
        new_in_seq_to_ack = seq_add(in_ack_seq_ack_, static_cast<int>(kMaxHistoryBits - history_length()));
    if (!seq_greater(in_seq_, new_in_seq_to_ack)) {
        in_seq_ = new_in_seq_to_ack;
        ack_seq(new_in_seq_to_ack, false);
    }
    return 0;  // the packet is dropped either way; the window has moved on
}

NotifyHeader PacketNotify::make_header() const {
    NotifyHeader h;
    h.seq = out_seq_;
    h.acked_seq = in_ack_seq_;
    size_t words = (history_length() + kHistoryWordBits - 1) / kHistoryWordBits;
    if (words < 1) words = 1;
    if (words > kMaxHistoryWords) words = kMaxHistoryWords;
    h.history_words = static_cast<uint8_t>(words);
    for (size_t i = 0; i < words; ++i) h.history[i] = in_history_.word(i);
    return h;
}

uint16_t PacketNotify::commit_and_increment_out_seq() {
    const uint16_t sent = out_seq_;
    ack_record_.push_back({sent, in_ack_seq_});
    out_seq_ = seq_add(out_seq_, 1);
    return sent;
}

uint16_t PacketNotify::update_in_ack_seq_ack(int ack_count, uint16_t acked_seq) {
    // Drop the records of packets the header has now reported on; the last
    // one says which of our acks the peer has seen.
    while (ack_count > 1 && !ack_record_.empty()) {
        ack_record_.pop_front();
        --ack_count;
    }
    if (!ack_record_.empty()) {
        const SentAck rec = ack_record_.front();
        ack_record_.pop_front();
        if (rec.out_seq == acked_seq) return rec.in_ack_seq;
    }
    // No record for the acked packet: assume the peer has seen none of our
    // acks, so the next headers carry the full 256-packet history.
    return seq_add(in_ack_seq_, -static_cast<int>(kMaxHistoryBits));
}

}}} // namespace aoc::protocol::conn
