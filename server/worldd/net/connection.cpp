// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/connection.cpp
// ============================================================================
#include "net/connection.h"
#include "protocol/conn/sequence.h"
#include "protocol/wire/packet_parser.h"
#include <algorithm>
#include <spdlog/spdlog.h>

namespace aoc { namespace worldd {

namespace conn = protocol::conn;
namespace emit = protocol::emit;
namespace wire = protocol::wire;

namespace {

InBunch copy_bunch(const uint8_t* data, size_t len, const wire::ParsedBunch& b) {
    InBunch in;
    in.channel = b.channel;
    in.reliable = b.is_reliable;
    in.control = b.is_control;
    in.open = b.is_open;
    in.close = b.is_close;
    in.close_reason = b.close_reason;
    // The walk only passes bunches whose data lies inside the packet (walk_bunches_ex stops at one that overruns
    // it), so write_bit_range, which does not look at the length, stays in bounds.
    if (b.data_start_bit + b.bunch_data_bits > len * 8) return in;  // cannot happen
    emit::BunchWriter w(b.bunch_data_bits / 8 + 8);
    w.write_bit_range(data, b.data_start_bit, b.bunch_data_bits);
    in.data.assign(w.data(), w.data() + w.byte_size());
    in.bits = b.bunch_data_bits;
    return in;
}

/// Appends `src`'s bits after `dst`'s, in place: the work is that of `src` alone.  (It rebuilt `dst` bit by
/// bit for every part: 600 empty parts on a 500,000-bit partial bunch took 1.4 s.)  Bytes of `data` are
/// LSB-first, the bits past `bits` are 0; a part after a byte-aligned `dst` is a plain copy, and UE sends
/// every part but the last byte-aligned (the exports of a chain are the exception).
void append_bits(InBunch& dst, const InBunch& src) {
    const size_t src_bytes = std::min((src.bits + 7) / 8, src.data.size());
    const unsigned src_tail = static_cast<unsigned>(src.bits % 8);
    auto src_byte = [&](size_t i) -> uint8_t {
        uint8_t b = src.data[i];
        if (i + 1 == (src.bits + 7) / 8 && src_tail != 0) b &= static_cast<uint8_t>((1u << src_tail) - 1);
        return b;
    };
    dst.data.resize((dst.bits + 7) / 8);
    const unsigned shift = static_cast<unsigned>(dst.bits % 8);
    dst.data.reserve(dst.data.size() + src_bytes + 1);
    for (size_t i = 0; i < src_bytes; ++i) {
        const uint8_t b = src_byte(i);
        if (shift == 0) {
            dst.data.push_back(b);
        } else {
            dst.data.back() = static_cast<uint8_t>(dst.data.back() | (b << shift));
            dst.data.push_back(static_cast<uint8_t>(b >> (8 - shift)));
        }
    }
    dst.bits += src.bits;
    dst.data.resize((dst.bits + 7) / 8);
}
// C Y B.C-O-A

}  // namespace

Connection::Connection(const ConnectionConfig& cfg, Clock::time_point now)
    : cfg_(cfg), last_receive_(now), last_send_(now) {
    // UE5 InitSequence: the notify state starts one before the peer's first
    // packet, and every channel's reliable sequence at initial seq % 1024.
    notify_.init(conn::seq_add(cfg.client_initial_seq, -1), cfg.server_initial_seq);
}

std::vector<InBunch> Connection::receive(const uint8_t* data, size_t len, Clock::time_point now) {
    std::vector<InBunch> out;
    const auto pkt = conn::read_data_packet(data, len, wire::Direction::ClientToServer);
    if (!pkt) return out;
    const int delta = notify_.update(pkt->notify, [this](uint16_t seq, bool ok) { on_packet_ack(seq, ok); });
    if (delta <= 0) {
        // While the receive window re-syncs, packets are dropped until the
        // client acks the advanced window; UE still owes the client that ack
        // (++HasDirtyAcks), or it would wait for our next keep-alive.
        if (notify_.is_waiting_for_history_flush()) dirty_acks_ = true;
        return out;
    }
    last_receive_ = now;

    std::vector<wire::ParsedBunch> bunches;
    const wire::WalkResult walk = wire::walk_bunches_ex(data, len, pkt->bunches_begin, pkt->bunches_end,
                                                        [&](const wire::ParsedBunch& b) { bunches.push_back(b); });
    for (const auto& b : bunches) {
        Queued q;
        q.bunch = copy_bunch(data, len, b);
        q.partial = b.is_partial;
        q.partial_initial = b.partial_initial;
        q.partial_final = b.partial_final;
        if (!b.is_reliable) {
            if (q.partial) spdlog::debug("[conn] dropping an unreliable partial bunch on channel {}", b.channel);
            else complete(std::move(q.bunch), out);
            continue;
        }
        const uint32_t ch = b.channel;
        int& last = in_reliable_.try_emplace(ch, cfg_.client_initial_seq % kChSeqSpace).first->second;
        const int seq = conn::make_relative(static_cast<int>(b.ch_sequence), last, kChSeqSpace);
        if (seq <= last) continue;  // already delivered: a resend
        if (seq != last + 1) {
            if (early_total_ >= kMaxEarlyBunches) {
                spdlog::warn("[conn] reliable buffer overflow on channel {}: the connection fails", ch);
                failed_ = true;
                return out;
            }
            if (in_queue_[ch].emplace(seq, std::move(q)).second) ++early_total_;
            continue;
        }
        last = seq;
        accept(std::move(q), out);
        auto qit = in_queue_.find(ch);
        while (qit != in_queue_.end() && !qit->second.empty() && qit->second.begin()->first <= last + 1) {
            auto first = qit->second.begin();
            if (first->first == last + 1) {
                last = first->first;
                accept(std::move(first->second), out);
            }
            qit->second.erase(first);
            --early_total_;
        }
    }
    if (!walk.complete) {
        // A bunch header that cannot be read, or whose data runs past the packet: UE closes the connection
        // ("Bunch header overflowed", UNetConnection::ReceivedPacket).  The bunches before it were handled.
        spdlog::warn("[conn] a packet with {} unreadable bits after {} bunches (at bit {} of {}): the connection fails",
                     pkt->bunches_end - walk.end_pos, walk.visited, walk.end_pos, pkt->bunches_end);
        failed_ = true;
        return out;
    }
    notify_.ack_seq(pkt->notify.seq);
    dirty_acks_ = true;
    return out;
}
// (C Y B | CoA)

void Connection::accept(Queued&& q, std::vector<InBunch>& out) {
    const uint32_t ch = q.bunch.channel;
    if (!q.partial) {
        complete(std::move(q.bunch), out);
        return;
    }
    if (q.partial_initial) {
        Partial& p = partial_[ch];
        p.bunch = std::move(q.bunch);
        p.parts = 1;
    } else {
        auto it = partial_.find(ch);
        if (it == partial_.end()) {
            spdlog::debug("[conn] dropping a partial bunch without its start on channel {}", ch);
            return;
        }
        if (it->second.bunch.bits + q.bunch.bits > kMaxPartialBits) {
            spdlog::warn("[conn] a partial bunch on channel {} exceeds {} bits: the connection fails", ch, kMaxPartialBits);
            partial_.erase(it);
            failed_ = true;
            return;
        }
        if (it->second.parts >= kMaxPartialParts) {
            spdlog::warn("[conn] a partial bunch on channel {} has more than {} parts: the connection fails", ch,
                         kMaxPartialParts);
            partial_.erase(it);
            failed_ = true;
            return;
        }
        append_bits(it->second.bunch, q.bunch);
        ++it->second.parts;
        it->second.bunch.close = it->second.bunch.close || q.bunch.close;
    }
    if (q.partial_final) {
        InBunch whole = std::move(partial_[ch].bunch);
        partial_.erase(ch);
        complete(std::move(whole), out);
    }
}

void Connection::complete(InBunch&& b, std::vector<InBunch>& out) {
    if (b.channel == 0 && b.control && b.close) closed_by_client_ = true;
    out.push_back(std::move(b));
}

void Connection::on_packet_ack(uint16_t seq, bool delivered) {
    auto it = in_flight_.find(seq);
    if (it == in_flight_.end()) return;
    for (auto& ob : it->second) {
        if (ob->acked) continue;
        if (delivered) {
            ob->acked = true;
            if (ob->reliable) {
                auto& n = unacked_[ob->channel];
                if (n > 0) --n;
            }
            if (ob->closes) {
                // The close is acked: the index is free (UChannel::ReceivedAcks).
                auto c = closing_.find(ob->channel);
                if (c != closing_.end() && c->second == ob) closing_.erase(c);
            }
        } else if (ob->reliable && !ob->queued) {
            ob->queued = true;
            outgoing_.push_back(ob);  // lost: send it again, same sequence
        }
        note_ack(*ob, delivered);
    }
    in_flight_.erase(it);
}

void Connection::note_ack(const OutBunch& ob, bool delivered) {
    if (ob.chain == 0) return;
    if (ack_events_.size() >= kMaxAckEvents) ack_events_.pop_front();
    AckEvent e;
    e.chain = ob.chain;
    e.channel = ob.channel;
    e.part = ob.part;
    e.export_part = ob.export_part;
    e.delivered = delivered;
    ack_events_.push_back(e);
}

std::vector<AckEvent> Connection::take_ack_events() {
    std::vector<AckEvent> out(ack_events_.begin(), ack_events_.end());
    ack_events_.clear();
    return out;
}

bool Connection::has_window(uint32_t channel, size_t count) const {
    return unacked_reliable(channel) + count <= kMaxUnackedReliable;
}

Connection::OutPtr Connection::queue_bunch(conn::BunchHeader header, const uint8_t* payload, size_t bits, PartTag tag) {
    if (header.reliable) {
        int& last = out_reliable_.try_emplace(header.channel, cfg_.server_initial_seq % kChSeqSpace).first->second;
        header.ch_sequence = static_cast<uint16_t>(++last % kChSeqSpace);
        ++unacked_[header.channel];
    }
    auto ob = std::make_shared<OutBunch>();
    ob->channel = header.channel;
    ob->reliable = header.reliable;
    ob->chain = tag.chain;
    ob->part = tag.part;
    ob->export_part = tag.export_part;
    emit::BunchWriter w(bits / 8 + 16);
    conn::write_bunch(w, header, payload, bits);
    ob->data.assign(w.data(), w.data() + w.byte_size());
    ob->bits = w.bit_pos();
    ob->queued = true;
    outgoing_.push_back(ob);
    return ob;
}

bool Connection::send(conn::BunchHeader header, const emit::BunchWriter& payload) {
    const size_t payload_bits = payload.bit_pos();
    if (conn::bunch_bits(header, payload_bits) > kMaxBunchBits) {
        spdlog::error("[conn] a {}-bit bunch does not fit in one packet", payload_bits);
        return false;
    }
    if (header.reliable && !has_window(header.channel, 1)) return false;
    queue_bunch(header, payload.data(), payload_bits);
    return true;
}

bool Connection::plan_chain(const Chain& c, size_t* parts_out, size_t* content_parts_out, bool log) const {
    if (c.channel == 0) {
        if (log) spdlog::error("[conn] channel 0 is the control channel: send_chain is for actor channels");
        return false;
    }
    if (channel_open(c.channel) == c.open) {
        if (log)
            spdlog::error("[conn] chain refused: channel {} is {} and the chain {}", c.channel,
                          c.open ? "already open" : "not open", c.open ? "opens it" : "does not open it");
        return false;
    }
    if (c.open && channel_closing(c.channel)) {
        if (log)
            spdlog::error("[conn] chain refused: channel {} is closing, its index is taken until the close is acknowledged",
                          c.channel);
        return false;
    }
    if (c.open && !c.reliable) {
        if (log) spdlog::error("[conn] chain refused: the open of channel {} must be reliable", c.channel);
        return false;
    }
    const size_t content_bits = c.payload.bit_pos();
    if (content_bits > kMaxPartialBits) {
        if (log)
            spdlog::error("[conn] chain refused: {} bits of content on channel {} exceed {} (the client fails the connection)",
                          content_bits, c.channel, kMaxPartialBits);
        return false;
    }
    for (const auto& e : c.exports) {
        if (e.bit_pos() > kMaxSingleBunchBits) {
            if (log)
                spdlog::error("[conn] chain refused: a {}-bit export bunch on channel {} exceeds {}", e.bit_pos(),
                              c.channel, kMaxSingleBunchBits);
            return false;
        }
    }
    // Content up to kMaxSingleBunchBits stays whole; more is cut into parts of
    // kMaxPartialBunchBits, the last taking the rest.
    const size_t content_parts =
        content_bits > kMaxSingleBunchBits ? (content_bits + kMaxPartialBunchBits - 1) / kMaxPartialBunchBits : 1;
    const size_t parts = c.exports.size() + content_parts;
    if (c.reliable && !has_window(c.channel, parts)) {
        if (log && parts > kMaxUnackedReliable)
            spdlog::error("[conn] chain refused: {} parts on channel {} can never fit the window of {}", parts, c.channel,
                          kMaxUnackedReliable);
        return false;
    }
    if (parts_out) *parts_out = parts;
    if (content_parts_out) *content_parts_out = content_parts;
    return true;
}
// (C | O | A : Cyb)

size_t Connection::part_count(const Chain& c) {
    const size_t content_bits = c.payload.bit_pos();
    const size_t content_parts =
        content_bits > kMaxSingleBunchBits ? (content_bits + kMaxPartialBunchBits - 1) / kMaxPartialBunchBits : 1;
    return c.exports.size() + content_parts;
}

bool Connection::can_send_chain(const Chain& c) const {
    return plan_chain(c, nullptr, nullptr, false);
}

ChainId Connection::send_chain(const Chain& c) {
    size_t parts = 0, content_parts = 0;
    if (!plan_chain(c, &parts, &content_parts, true)) return 0;
    const size_t content_bits = c.payload.bit_pos();
    const ChainId id = next_chain_++;

    conn::BunchHeader base;
    base.channel = c.channel;
    base.reliable = c.reliable;
    base.name_index = kActorChannelName;
    size_t index = 0;
    auto queue_part = [&](const uint8_t* data, size_t bits, bool exports) {
        conn::BunchHeader h = base;
        h.open = c.open && index == 0;
        h.control = h.open;  // bControl is bOpen || bClose
        h.has_exports = exports;
        h.partial = parts > 1;
        h.partial_initial = h.partial && index == 0;
        h.partial_final = h.partial && index + 1 == parts;
        PartTag tag;
        tag.chain = id;
        tag.part = static_cast<uint32_t>(index);
        tag.export_part = exports;
        ++index;
        queue_bunch(h, data, bits, tag);
    };
    for (const auto& e : c.exports) queue_part(e.data(), e.bit_pos(), true);
    for (size_t i = 0; i < content_parts; ++i) {
        const size_t first = i * kMaxPartialBunchBits;  // a multiple of 8: the part starts on a byte
        const size_t bits = content_parts == 1 ? content_bits : std::min(kMaxPartialBunchBits, content_bits - first);
        queue_part(c.payload.data() + first / 8, bits, false);
    }
    if (c.open) open_channels_.insert(c.channel);
    return id;
}

bool Connection::close_channel(uint32_t channel, uint32_t reason) {
    if (!channel_open(channel)) {
        spdlog::error("[conn] close refused: channel {} is not open", channel);
        return false;
    }
    if (reason >= kCloseReasonSpace) {
        spdlog::error("[conn] close refused: reason {} is not below {}", reason, kCloseReasonSpace);
        return false;
    }
    if (!has_window(channel, 1)) return false;
    conn::BunchHeader h;
    h.control = true;
    h.close = true;
    h.close_reason = reason;
    h.reliable = true;
    h.channel = channel;
    h.name_index = kActorChannelName;
    const OutPtr close = queue_bunch(h, nullptr, 0);
    close->closes = true;
    closing_[channel] = close;
    open_channels_.erase(channel);
    return true;
}

bool Connection::send_close(uint32_t reason) {
    conn::BunchHeader h;
    h.control = true;
    h.close = true;
    h.close_reason = reason;
    h.reliable = true;
    h.channel = 0;
    return send(h, emit::BunchWriter(1));
}

emit::BunchWriter Connection::start_packet() const {
    emit::BunchWriter w(conn::kMaxPacketBytes);
    conn::begin_server_packet(w, cfg_.outer, notify_.make_header(), cfg_.custom_field);
    return w;
}

std::vector<uint8_t> Connection::finish_packet(emit::BunchWriter& w, std::vector<OutPtr> tracked,
                                               Clock::time_point now) {
    auto bytes = conn::finish_data_packet(w);
    const uint16_t seq = notify_.commit_and_increment_out_seq();
    if (!tracked.empty()) in_flight_[seq] = std::move(tracked);
    last_send_ = now;
    dirty_acks_ = false;
    return bytes;
}
// -- C Y B . C-O-A --

std::vector<std::vector<uint8_t>> Connection::flush(Clock::time_point now) {
    std::vector<std::vector<uint8_t>> out;
    constexpr size_t kMaxBits = conn::kMaxPacketBytes * 8;
    while (!outgoing_.empty() && !notify_.is_window_full()) {
        emit::BunchWriter w = start_packet();
        std::vector<OutPtr> tracked;  // what the ack of this packet is about: reliable bunches, chain parts
        bool wrote = false;
        while (!outgoing_.empty()) {
            const OutPtr& ob = outgoing_.front();
            if (ob->acked) {  // acked in an earlier copy while queued again
                outgoing_.pop_front();
                continue;
            }
            if (wrote && w.bit_pos() + ob->bits + 2 > kMaxBits) break;
            w.write_bit_range(ob->data.data(), 0, ob->bits);
            ob->queued = false;
            if (ob->reliable || ob->chain != 0) tracked.push_back(ob);
            wrote = true;
            outgoing_.pop_front();
        }
        if (!wrote) break;
        out.push_back(finish_packet(w, std::move(tracked), now));
    }
    drop_unreliable_backlog();
    // An ack goes out kAckDelay after our last packet (not after the receive
    // that made it due), or rides a keep-alive once we have been quiet that long.
    const auto quiet = now - last_send_;
    if (out.empty() && !notify_.is_window_full() &&
        ((dirty_acks_ && quiet >= kAckDelay) || quiet >= kKeepAliveInterval)) {
        emit::BunchWriter w = start_packet();
        out.push_back(finish_packet(w, {}, now));
    }
    return out;
}

bool Connection::timed_out(Clock::time_point now) const {
    const auto limit = playing_ ? std::chrono::duration_cast<Clock::duration>(kPlayTimeout)
                                : std::chrono::duration_cast<Clock::duration>(kLoginTimeout);
    return now - last_receive_ > limit;
}

size_t Connection::queued_unreliable() const {
    size_t n = 0;
    for (const auto& ob : outgoing_)
        if (!ob->reliable) ++n;
    return n;
}

void Connection::drop_unreliable_backlog() {
    // What is left of the queue after the send loop could not go out: the packet window is full.  Reliable
    // bunches wait for it (their number is bounded by the channels' windows); unreliable ones are dropped, the
    // oldest first, down to kMaxQueuedUnreliable.
    size_t excess = queued_unreliable();
    if (excess <= kMaxQueuedUnreliable) return;
    excess -= kMaxQueuedUnreliable;
    std::unordered_set<ChainId> dropped_chains;  // the rest of a chain whose part was dropped goes with it
    std::deque<OutPtr> kept;
    size_t dropped = 0;
    for (OutPtr& ob : outgoing_) {
        const bool sibling = ob->chain != 0 && dropped_chains.count(ob->chain) != 0;
        if (ob->reliable || !(sibling || excess > 0)) {
            kept.push_back(std::move(ob));
            continue;
        }
        if (excess > 0) --excess;  // a dropped sibling is part of the excess, too
        if (ob->chain != 0) dropped_chains.insert(ob->chain);
        ob->queued = false;
        note_ack(*ob, false);  // never sent: lost, as far as the caller is concerned (and final)
        ++dropped;
    }
    outgoing_.swap(kept);
    unreliable_dropped_ += dropped;
    spdlog::debug("[conn] the packet window is full: dropped {} queued unreliable bunches", dropped);
}

size_t Connection::unacked_reliable(uint32_t channel) const {
    auto it = unacked_.find(channel);
    return it == unacked_.end() ? 0 : it->second;
}

}} // namespace aoc::worldd
