// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  fake_client.h: the client's side of a connection, for worldd's tests.
//
//  Builds client data packets (its own packet sequence and acks, reliable
//  channel-0 bunches, bunches on actor channels) with libaocnet, and reads the
//  server's packets back: the bunches of each packet, and the chains the client
//  has received whole (Delivered: reliable bunches in channel-sequence order,
//  a chain's parts put together, its export bunches kept apart, as UE's client
//  takes them).
// ============================================================================
#pragma once

#include "protocol/conn/nmt.h"
#include "protocol/conn/packet_framing.h"
#include "protocol/conn/packet_notify.h"
#include "protocol/conn/sequence.h"
#include "protocol/emit/bunch_writer.h"
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fake {

namespace conn = aoc::protocol::conn;
namespace emit = aoc::protocol::emit;
namespace wire = aoc::protocol::wire;

struct Bunch {
    conn::BunchHeader header;
    emit::BunchWriter payload;
};

/// One chain of an actor channel as the client has it once every part has arrived.
struct Delivered {
    uint32_t channel = 0;
    bool reliable = false;
    bool open = false;                           // the chain opens the channel
    bool close = false;                          // a close bunch (no content)
    std::vector<emit::BunchWriter> exports;      // the export bunches, each a bunch payload
    emit::BunchWriter content;                   // the content bits: SerializeNewActor and blocks, or an RPC
};

/// What the client got in one server packet.
struct Received {
    conn::NotifyHeader notify;
    std::vector<wire::ParsedBunch> bunches;
    std::vector<conn::ControlMessage> messages;  // channel-0 payloads
    std::vector<Delivered> delivered;            // actor-channel chains this packet completed
    bool closed = false;                         // channel 0 closed
    bool valid = false;
};

class Client {
public:
    Client(conn::OuterHeader outer, conn::CustomField cf, uint16_t server_initial, uint16_t client_initial)
        : outer_(outer), cf_(cf), out_reliable_(client_initial % 1024), client_seq0_(client_initial % 1024),
          server_seq0_(server_initial % 1024) {
        notify_.init(conn::seq_add(server_initial, -1), client_initial);
    }

    /// A packet carrying `bunches`, with this client's next sequence and acks.
    std::vector<uint8_t> packet(std::vector<Bunch> bunches) {
        emit::BunchWriter w;
        conn::begin_client_packet(w, outer_, notify_.make_header(), cf_);
        for (auto& b : bunches) conn::write_bunch(w, b.header, b.payload.data(), b.payload.bit_pos());
        notify_.commit_and_increment_out_seq();
        return conn::finish_data_packet(w);
    }
// [Cyb - C.O.A]

    /// A reliable channel-0 bunch with the next channel sequence; the first
    /// one opens the channel, as the retail client's NMT_Hello does.
    Bunch control(std::function<void(emit::BunchWriter&)> write) {
        Bunch b;
        b.header.reliable = true;
        b.header.channel = 0;
        b.header.ch_sequence = static_cast<uint16_t>(++out_reliable_ % 1024);
        if (!opened_) {
            b.header.control = true;
            b.header.open = true;
            opened_ = true;
        }
        write(b.payload);
        return b;
    }

    Bunch nmt(conn::Nmt type) {
        return control([type](emit::BunchWriter& w) { w.write_uint8(static_cast<uint8_t>(type)); });
    }

    Bunch hello() {
        return control([](emit::BunchWriter& w) {
            w.write_uint8(static_cast<uint8_t>(conn::Nmt::Hello));
            w.write_uint8(1);
            w.write_uint32(0xa42f624eu);
            w.write_int32(0);
        });
    }

    Bunch login() {
        return control([](emit::BunchWriter& w) {
            w.write_uint8(static_cast<uint8_t>(conn::Nmt::Login));
            w.write_fstring_ansi("0");
            w.write_fstring_ansi("?Name=TEST");
        });
    }

    /// A Login as the retail client sends it (e2e capture): the ?Name= option, then the unique id (FUniqueNetIdRepl,
    /// flags 8: the id as an FString) and the platform name.
    Bunch login(const std::string& name, const std::string& unique_id) {
        return control([&](emit::BunchWriter& w) {
            w.write_uint8(static_cast<uint8_t>(conn::Nmt::Login));
            w.write_fstring_ansi("0");
            w.write_fstring_ansi("?Name=" + name);
            w.write_uint8(8);
            w.write_fstring_ansi(unique_id);
            w.write_fstring_ansi("NULL");
        });
    }

    /// A bunch of `bits` bits of `data` (LSB-first) on actor channel `channel`: reliable ones take the channel's next
    /// sequence (the first is the client's initial sequence % 1024 + 1, as the capture's), an unreliable one none.
    Bunch actor(uint32_t channel, const uint8_t* data, size_t bits, bool reliable) {
        Bunch b;
        b.header.channel = channel;
        b.header.reliable = reliable;
        if (reliable) {
            auto it = actor_seq_.try_emplace(channel, client_seq0_).first;
            b.header.ch_sequence = static_cast<uint16_t>(++it->second % 1024);
            b.header.name_index = 102;   // EName Actor
        }
        b.payload.write_bit_range(data, 0, bits);
        return b;
    }
    Bunch actor(uint32_t channel, const emit::BunchWriter& payload, bool reliable) {
        return actor(channel, payload.data(), payload.bit_pos(), reliable);
    }

    Bunch netspeed(uint32_t rate) {
        return control([rate](emit::BunchWriter& w) {
            w.write_uint8(static_cast<uint8_t>(conn::Nmt::Netspeed));
            w.write_uint32(rate);
        });
    }

    Bunch close_channel() {
        Bunch b;
        b.header.control = true;
        b.header.close = true;
        b.header.reliable = true;
        b.header.channel = 0;
        b.header.ch_sequence = static_cast<uint16_t>(++out_reliable_ % 1024);
        return b;
    }

    /// Reads a server packet: acks it and returns what it carried.
    Received receive(const std::vector<uint8_t>& pkt) {
        Received r;
        const auto p = conn::read_data_packet(pkt.data(), pkt.size(), wire::Direction::ServerToClient);
        if (!p) return r;
        r.notify = p->notify;
        if (notify_.update(p->notify, [](uint16_t, bool) {}) <= 0) return r;
        notify_.ack_seq(p->notify.seq);
        r.valid = true;
        r.bunches = conn::packet_bunches(pkt.data(), pkt.size(), *p);
        for (const auto& b : r.bunches) {
            if (b.channel != 0) {
                take(b, pkt.data(), r.delivered);
                continue;
            }
            if (b.is_control && b.is_close) r.closed = true;
            if (b.bunch_data_bits == 0) continue;
            if (auto m = conn::parse_control_message(pkt.data(), pkt.size(), b.data_start_bit, b.bunch_data_bits))
                r.messages.push_back(*m);
        }
        return r;
    }

    conn::PacketNotify& notify() { return notify_; }
// built with C:Y:B and CoA

private:
    /// One part of a chain, as it arrived.
    struct Part {
        bool reliable = false, partial = false, initial = false, final = false, exports = false, open = false, close = false;
        emit::BunchWriter bits;
    };
    /// The chain being put together on a channel.
    struct Assembling {
        bool open = false, close = false;
        std::vector<emit::BunchWriter> exports;
        emit::BunchWriter content;
        Assembling() {}   // BunchWriter's default constructor is explicit: no `Assembling{}`
    };

    /// A bunch of an actor channel: reliable ones are held until they are next in the channel's sequence.
    void take(const wire::ParsedBunch& b, const uint8_t* pkt, std::vector<Delivered>& out) {
        Part p;
        p.reliable = b.is_reliable;
        p.partial = b.is_partial;
        p.initial = b.partial_initial;
        p.final = b.partial_final;
        p.exports = b.has_package_map_exports;
        p.open = b.is_open;
        p.close = b.is_close;
        p.bits.write_bit_range(pkt, b.data_start_bit, b.bunch_data_bits);
        if (!b.is_reliable) {
            assemble(b.channel, std::move(p), out);
            return;
        }
        int& last = last_.try_emplace(b.channel, server_seq0_).first->second;
        const int seq = conn::make_relative(static_cast<int>(b.ch_sequence), last, 1024);
        if (seq <= last) return;   // a resend of what the client has
        auto& queue = early_[b.channel];
        queue.emplace(seq, std::move(p));
        while (!queue.empty() && queue.begin()->first == last + 1) {
            ++last;
            assemble(b.channel, std::move(queue.begin()->second), out);
            queue.erase(queue.begin());
        }
    }

    void assemble(uint32_t channel, Part&& p, std::vector<Delivered>& out) {
        auto emit_chain = [&](Assembling&& a, bool reliable) {
            Delivered d;
            d.channel = channel;
            d.reliable = reliable;
            d.open = a.open;
            d.close = a.close;
            d.exports = std::move(a.exports);
            d.content = std::move(a.content);
            out.push_back(std::move(d));
        };
        Assembling& a = assembling_[channel];
        if (!p.partial) {   // one bunch is the whole chain (a chain with exports is always partial)
            Assembling single;
            single.open = p.open;
            single.close = p.close;
            single.content = std::move(p.bits);
            emit_chain(std::move(single), p.reliable);
            return;
        }
        if (p.initial) a = Assembling();
        a.open = a.open || p.open;
        a.close = a.close || p.close;
        if (p.exports) {
            a.exports.push_back(std::move(p.bits));
        } else {
            a.content.write_bit_range(p.bits.data(), 0, p.bits.bit_pos());
        }
        if (p.final) {
            emit_chain(std::move(a), p.reliable);
            a = Assembling();
        }
    }

    conn::OuterHeader outer_;
    conn::CustomField cf_;
    conn::PacketNotify notify_;
    int out_reliable_;
    int client_seq0_;
    int server_seq0_;
    bool opened_ = false;
    std::unordered_map<uint32_t, int> actor_seq_;                // channel -> last reliable sequence sent
    std::unordered_map<uint32_t, int> last_;                     // channel -> last reliable sequence delivered
    std::unordered_map<uint32_t, std::map<int, Part>> early_;    // reliable parts waiting for their turn
    std::unordered_map<uint32_t, Assembling> assembling_;
};

}  // namespace fake
