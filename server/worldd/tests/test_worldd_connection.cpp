// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_connection.cpp: net/connection against a fake client
// ============================================================================
#include "fake_client.h"
#include "net/connection.h"
#include <cstdio>
#include <iterator>
#include <utility>
#include <vector>

using namespace aoc::worldd;
namespace conn = aoc::protocol::conn;
namespace emit = aoc::protocol::emit;
using std::chrono::milliseconds;
using std::chrono::seconds;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static const conn::OuterHeader kOuter{0x500c7696u, 0, 5};
static const conn::CustomField kCf{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d};
static const Clock::time_point t0{seconds(1000)};

static ConnectionConfig config() {
    ConnectionConfig c;
    c.outer = kOuter;
    c.custom_field = kCf;
    c.server_initial_seq = 14265;
    c.client_initial_seq = 15870;
    return c;
}

static fake::Client client() { return fake::Client(kOuter, kCf, 14265, 15870); }

static conn::BunchHeader control_header() {
    conn::BunchHeader h;
    h.reliable = true;
    h.channel = 0;
    return h;
}

static emit::BunchWriter nmt_payload(conn::Nmt type) {
    emit::BunchWriter w;
    if (type == conn::Nmt::Challenge) conn::write_nmt_challenge(w, "ABCD1234");
    else conn::write_nmt_welcome(w, "/Game/L", "/Game/M", "");
    return w;
}
// CoA x C:Y:B

static void test_acks_and_keepalive() {
    std::printf("acks and keep-alives\n");
    Connection c(config(), t0);
    auto cl = client();
    const auto in = c.receive(cl.packet({cl.hello()}), t0);
    CHECK(in.size() == 1 && in[0].channel == 0 && in[0].reliable && in[0].open && in[0].bits > 0,
          "the client's Hello comes out as one reliable channel-0 bunch");
    const auto m = in.size() == 1 ? conn::parse_control_message(in[0].data.data(), in[0].data.size(), 0, in[0].bits)
                                  : std::nullopt;
    CHECK(m && m->type == conn::Nmt::Hello && m->network_version == 0xa42f624eu, "its payload is NMT_Hello");

    CHECK(c.flush(t0 + milliseconds(10)).empty(), "no ack before 50 ms");
    const auto pkts = c.flush(t0 + milliseconds(50));
    CHECK(pkts.size() == 1, "an ack-only packet at 50 ms");
    const auto r = pkts.size() == 1 ? cl.receive(pkts[0]) : fake::Received{};
    CHECK(r.valid && r.notify.seq == 14265 && r.notify.acked_seq == 15870 && r.notify.history[0] == 0x1 &&
          r.bunches.empty(),
          "seq 14265 (from the cookie), acks 15870 with history 0x1, no bunches");
    CHECK(c.flush(t0 + milliseconds(150)).empty(), "nothing more while quiet for under 200 ms");
    CHECK(c.flush(t0 + milliseconds(250)).size() == 1, "a keep-alive after 200 ms of quiet");
}

static void test_reliable_out_and_resend() {
    std::printf("reliable bunches out\n");
    Connection c(config(), t0);
    auto cl = client();
    c.receive(cl.packet({cl.hello()}), t0);
    CHECK(c.send(control_header(), nmt_payload(conn::Nmt::Challenge)), "queue NMT_Challenge");
    auto pkts = c.flush(t0 + milliseconds(1));
    auto r = pkts.size() == 1 ? cl.receive(pkts[0]) : fake::Received{};
    CHECK(r.messages.size() == 1 && r.messages[0].type == conn::Nmt::Challenge && r.bunches[0].ch_sequence == 954,
          "sent at once with channel sequence 954 (14265 % 1024 + 1), as the capture has");
    CHECK(c.unacked_reliable(0) == 1, "one reliable bunch in flight");

    // The client's next packet acks it.
    c.receive(cl.packet({}), t0 + milliseconds(5));
    CHECK(c.unacked_reliable(0) == 0, "acknowledged: nothing in flight");

    CHECK(c.send(control_header(), nmt_payload(conn::Nmt::Welcome)), "queue NMT_Welcome");
    auto lost = c.flush(t0 + milliseconds(6));
    CHECK(lost.size() == 1, "sent (and lost on the way)");
    auto ka = c.flush(t0 + milliseconds(300));
    r = ka.size() == 1 ? cl.receive(ka[0]) : fake::Received{};
    CHECK(r.valid && r.bunches.empty(), "the client gets the next packet, a keep-alive");
    // Its next header reports: newest received = the keep-alive, the Welcome's packet missing.
    c.receive(cl.packet({}), t0 + milliseconds(310));
    auto again = c.flush(t0 + milliseconds(311));
    r = again.size() == 1 ? cl.receive(again[0]) : fake::Received{};
    CHECK(r.messages.size() == 1 && r.messages[0].type == conn::Nmt::Welcome && r.bunches[0].ch_sequence == 955,
          "the lost Welcome goes out again with the same sequence, 955");
    c.receive(cl.packet({}), t0 + milliseconds(320));
    CHECK(c.unacked_reliable(0) == 0, "and is acknowledged");
    auto quiet = c.flush(t0 + milliseconds(330));
    CHECK(quiet.size() <= 1 && (quiet.empty() || cl.receive(quiet[0]).messages.empty()),
          "no further copies");
}

static void test_in_order_delivery() {
    std::printf("the client's reliable bunches\n");
    Connection c(config(), t0);
    auto cl = client();
    auto b1 = cl.hello();
    auto b2 = cl.netspeed(1);
    auto b3 = cl.netspeed(2);
    auto first = c.receive(cl.packet({std::move(b1)}), t0);
    CHECK(first.size() == 1, "sequence 511 is delivered");
    auto early = c.receive(cl.packet({b3}), t0);
    CHECK(early.empty(), "513 before 512 waits");
    auto both = c.receive(cl.packet({b2}), t0);
    auto rate = [](const InBunch& b) {
        auto m = conn::parse_control_message(b.data.data(), b.data.size(), 0, b.bits);
        return m ? m->net_speed : 0u;
    };
    CHECK(both.size() == 2 && rate(both[0]) == 1 && rate(both[1]) == 2, "512 then delivers 512 and 513, in order");
    CHECK(c.receive(cl.packet({b2}), t0).empty(), "512 again (a resend) is dropped");

    auto pkt = cl.packet({cl.netspeed(3)});
    CHECK(c.receive(pkt, t0).size() == 1, "a new packet is delivered");
    CHECK(c.receive(pkt, t0).empty(), "the same datagram twice is dropped");
    CHECK(c.receive(pkt.data(), 5, t0).empty(), "a truncated datagram is dropped");
}

static void test_partial_and_close() {
    std::printf("partial bunches and close\n");
    Connection c(config(), t0);
    auto cl = client();
    c.receive(cl.packet({cl.hello()}), t0);
    auto part = [&](bool initial, bool final_, uint8_t byte) {
        fake::Bunch b = cl.control([byte](emit::BunchWriter& w) { w.write_uint8(byte); });
        b.header.partial = true;
        b.header.partial_initial = initial;
        b.header.partial_final = final_;
        return b;
    };
    CHECK(c.receive(cl.packet({part(true, false, 0xAA)}), t0).empty(), "the first part waits");
    CHECK(c.receive(cl.packet({part(false, false, 0xBB)}), t0).empty(), "the middle part waits");
    auto whole = c.receive(cl.packet({part(false, true, 0xCC)}), t0);
    CHECK(whole.size() == 1 && whole[0].bits == 24 && whole[0].data[0] == 0xAA && whole[0].data[1] == 0xBB &&
          whole[0].data[2] == 0xCC,
          "the last part delivers all 24 bits");
    CHECK(c.receive(cl.packet({part(false, true, 0xDD)}), t0).empty(), "a part without its start is dropped");

    CHECK(!c.closed_by_client(), "not closed yet");
    auto closed = c.receive(cl.packet({cl.close_channel()}), t0);
    CHECK(closed.size() == 1 && closed[0].close && c.closed_by_client(), "the client's close of channel 0");
}

static void test_timeouts_and_limits() {
    std::printf("timeouts and limits\n");
    Connection c(config(), t0);
    CHECK(!c.timed_out(t0 + seconds(120)), "no timeout at 120 s before Join");
    CHECK(c.timed_out(t0 + seconds(121)), "timed out after 120 s before Join");
    c.set_playing();
    CHECK(c.timed_out(t0 + seconds(61)), "after Join the limit is 60 s");

    Connection d(config(), t0);
    emit::BunchWriter big;
    for (int i = 0; i < 1100; ++i) big.write_uint8(0);
    CHECK(!d.send(control_header(), big), "a bunch bigger than a packet is refused");
    emit::BunchWriter small;
    small.write_uint8(1);
    bool all = true;
    for (size_t i = 0; i < Connection::kMaxUnackedReliable; ++i) all = all && d.send(control_header(), small);
    CHECK(all && !d.send(control_header(), small), "256 unacknowledged reliable bunches, then refused");
    const auto pkts = d.flush(t0);
    size_t bytes_ok = 0;
    for (const auto& p : pkts) bytes_ok += p.size() <= conn::kMaxPacketBytes ? 1 : 0;
    CHECK(pkts.size() > 1 && bytes_ok == pkts.size(), "they go out over several packets of at most 1028 bytes");
// CoA + C_Y_B

    Connection e(config(), t0);
    emit::BunchWriter w970, w1000;
    for (int i = 0; i < 970; ++i) w970.write_uint8(1);
    for (int i = 0; i < 1000; ++i) w1000.write_uint8(1);
    CHECK(e.send(control_header(), w970) && !e.send(control_header(), w1000),
          "a 970-byte payload fits one packet, a 1000-byte one is refused");
    const auto big_pkts = e.flush(t0);
    CHECK(big_pkts.size() == 1 && big_pkts[0].size() <= conn::kMaxPacketBytes, "and that packet stays within 1028 bytes");
}

static void test_duplicates_and_timeout_refresh() {
    std::printf("duplicate datagrams and the idle timeout\n");
    Connection c(config(), t0);
    auto cl = client();
    c.receive(cl.packet({cl.hello()}), t0);
    fake::Bunch u;  // an unreliable bunch on channel 0
    u.header.reliable = false;
    u.header.channel = 0;
    u.payload.write_uint8(0x77);
    auto pkt = cl.packet({u});
    CHECK(c.receive(pkt, t0 + seconds(1)).size() == 1, "an unreliable bunch is delivered once");
    CHECK(c.receive(pkt, t0 + seconds(100)).empty(), "the same datagram again delivers nothing");
    CHECK(c.timed_out(t0 + seconds(121) + milliseconds(1)), "and a duplicate does not refresh the idle timeout");
    auto last = cl.netspeed(5);  // 512
    CHECK(c.receive(cl.packet({last}), t0 + seconds(2)).size() == 1, "512 is delivered");
    CHECK(c.receive(cl.packet({last}), t0 + seconds(3)).empty(),
          "a resend of the most recently delivered bunch (512, in a new packet) is dropped");
    fake::Bunch up;  // an unreliable partial bunch: not supported, must not be delivered
    up.header.reliable = false;
    up.header.channel = 0;
    up.header.partial = true;
    up.header.partial_initial = true;
    up.header.partial_final = true;
    up.payload.write_uint8(0x66);
    CHECK(c.receive(cl.packet({up}), t0 + seconds(4)).empty(), "an unreliable partial bunch is dropped");
    fake::Bunch other;  // a reliable control close on channel 5: not the control channel
    other.header.reliable = true;
    other.header.control = true;
    other.header.close = true;
    other.header.channel = 5;
    other.header.ch_sequence = 511;
    auto closed5 = c.receive(cl.packet({other}), t0 + seconds(5));
    CHECK(closed5.size() == 1 && closed5[0].close && !c.closed_by_client(),
          "a close on channel 5 is delivered but does not end the connection");

    Connection d(config(), t0);
    auto cd = client();
    d.receive(cd.packet({cd.hello()}), t0 + seconds(100));
    CHECK(!d.timed_out(t0 + seconds(121)) && d.timed_out(t0 + seconds(221)),
          "an accepted packet at 100 s moves the timeout to 220 s");
}

static void test_window_full() {
    std::printf("the 256-packet window\n");
    Connection c(config(), t0);
    auto cl = client();
    c.receive(cl.packet({cl.hello()}), t0);
    auto ack = c.flush(t0 + milliseconds(50));
    CHECK(ack.size() == 1 && cl.receive(ack[0]).valid, "the ack (packet 14265) reaches the client");

    // The client is silent (loading the map); keep-alives every 200 ms.
    std::vector<std::vector<uint8_t>> kas;
    for (int i = 1; i <= 300; ++i)
        for (auto& p : c.flush(t0 + milliseconds(50 + 200 * i))) kas.push_back(std::move(p));
    CHECK(kas.size() == 255, "255 keep-alives, then the window of 256 unacknowledged packets is full");
    CHECK(c.flush(t0 + seconds(200)).empty(), "nothing more goes out while it is full, not even a keep-alive");
    CHECK(c.notify().is_window_full(), "(is_window_full)");

    for (const auto& p : kas) cl.receive(p);
    auto in = c.receive(cl.packet({cl.netspeed(7)}), t0 + seconds(200) + milliseconds(1));
    CHECK(in.size() == 1 && !c.notify().is_window_full(), "the client's next packet is accepted and opens the window");
    CHECK(c.flush(t0 + seconds(200) + milliseconds(60)).size() == 1, "and its ack goes out");
}

static void test_cadence() {
    std::printf("keep-alive cadence and ack timing\n");
    Connection c(config(), t0);
    size_t n = 0;
    for (int t = 0; t <= 2000; t += 100) n += c.flush(t0 + milliseconds(t)).size();
    CHECK(n == 10, "a keep-alive every 200 ms with no traffic: 10 in 2 s");
    auto cl = client();
    c.receive(cl.packet({cl.hello()}), t0 + milliseconds(2030));
    CHECK(c.flush(t0 + milliseconds(2049)).empty() && c.flush(t0 + milliseconds(2050)).size() == 1,
          "the ack goes out 50 ms after the last send (at 2050), 20 ms after the receive");
    Connection d(config(), t0);
    CHECK(d.flush(t0 + milliseconds(199)).empty() && d.flush(t0 + milliseconds(200)).size() == 1,
          "the keep-alive comes at exactly 200 ms");
}

static void test_early_queue_bound() {
    std::printf("a gap the client never fills\n");
    Connection c(config(), t0);
    auto cl = client();
    c.receive(cl.packet({cl.hello()}), t0);  // 511
    cl.netspeed(1);                          // 512 is never sent
    Clock::time_point t = t0;
    size_t delivered = 0, sent = 0;
    while (sent < 300 && !c.failed()) {
        t += milliseconds(1);
        delivered += c.receive(cl.packet({cl.netspeed(static_cast<uint32_t>(2 + sent))}), t).size();
        ++sent;
        // The server's acks flow back every 50 packets, so the packet window stays open.
        if (sent % 50 == 0)
            for (const auto& p : c.flush(t += milliseconds(60))) cl.receive(p);
    }
    CHECK(delivered == 0, "the early bunches all wait for 512");
    CHECK(c.failed() && sent == 257,
          "the 257th early bunch fails the connection instead of being dropped while acknowledged (UE: reliable buffer overflow)");

    Connection ok(config(), t0);
    auto cok = client();
    ok.receive(cok.packet({cok.hello()}), t0);
    auto gap = cok.netspeed(1);  // 512, held back
    std::vector<fake::Bunch> after;
    for (uint32_t i = 0; i < 256; ++i) after.push_back(cok.netspeed(2 + i));  // 513..768: exactly the buffer
    t = t0;
    for (size_t i = 0; i < after.size(); ++i) {
        t += milliseconds(1);
        ok.receive(cok.packet({after[i]}), t);
        if (i % 50 == 49)
            for (const auto& p : ok.flush(t += milliseconds(60))) cok.receive(p);
    }
    CHECK(!ok.failed() && ok.receive(cok.packet({gap}), t).size() == 257,
          "256 early bunches are fine: 512 releases all of them");
}
// (Cyb : C.O.A)

static void test_bounds_under_a_hostile_client() {
    std::printf("bounds under a hostile client\n");
    Clock::time_point t = t0;
    auto pump = [&](Connection& con, fake::Client& cli) {
        t += milliseconds(60);
        for (const auto& p : con.flush(t)) cli.receive(p);
    };

    Connection c(config(), t0);
    auto cl = client();
    c.receive(cl.packet({cl.hello()}), t0);
    auto big = [](uint32_t channel, uint16_t chseq) {
        fake::Bunch b;
        b.header.reliable = true;
        b.header.channel = channel;
        b.header.ch_sequence = chseq;
        for (int i = 0; i < 900; ++i) b.payload.write_uint8(static_cast<uint8_t>(i));
        return b;
    };
    const uint16_t early = static_cast<uint16_t>((15870 % 1024 + 2) % 1024);  // one past the next expected
    uint32_t ch = 1;
    for (; ch <= 4000 && !c.failed(); ++ch) {
        t += milliseconds(1);
        c.receive(cl.packet({big(ch, early)}), t);
        if (ch % 50 == 0) pump(c, cl);
    }
    CHECK(c.failed() && ch - 1 == 257,
          "early bunches spread over channels count against one bound: the 257th fails the connection");

    Connection d(config(), t0);
    auto cd = client();
    d.receive(cd.packet({cd.hello()}), t0);
    auto part = [&](fake::Client& from, bool initial, bool final_) {
        fake::Bunch b = from.control([](emit::BunchWriter& w) { for (int i = 0; i < 900; ++i) w.write_uint8(0x5A); });
        b.header.partial = true;
        b.header.partial_initial = initial;
        b.header.partial_final = final_;
        return b;
    };
    d.receive(cd.packet({part(cd, true, false)}), t);
    int parts = 1;
    for (int i = 0; i < 300 && !d.failed(); ++i, ++parts) {
        t += milliseconds(1);
        d.receive(cd.packet({part(cd, false, false)}), t);
        if (i % 50 == 49) pump(d, cd);
    }
    CHECK(d.failed() && static_cast<size_t>(parts - 1) * 900 * 8 <= Connection::kMaxPartialBits &&
              static_cast<size_t>(parts) * 900 * 8 > Connection::kMaxPartialBits,
          "a partial bunch fails the connection as soon as it would exceed 64 KB");

    Connection e(config(), t0);
    auto ce = client();
    e.receive(ce.packet({ce.hello()}), t0);
    t = t0;
    e.receive(ce.packet({part(ce, true, false)}), t);
    for (int i = 0; i < 60; ++i) {  // 61 parts: 55 KB
        t += milliseconds(1);
        e.receive(ce.packet({part(ce, false, false)}), t);
        if (i % 50 == 49) pump(e, ce);
    }
    auto whole = e.receive(ce.packet({part(ce, false, true)}), t);
    CHECK(!e.failed() && whole.size() == 1 && whole[0].bits == 62u * 900 * 8, "a 55 KB partial bunch is reassembled");
}

// UE's NetConnection drops a packet while PacketNotify waits for a history
// flush but still sends an ack for it on the next tick (++HasDirtyAcks).
static void test_ack_while_resyncing() {
    std::printf("the ack while the receive window re-syncs\n");
    Connection c(config(), t0);
    auto cl = client();
    auto ka = c.flush(t0 + milliseconds(200));  // packet 14265 carries the ack of 15869: nothing yet
    CHECK(ka.size() == 1 && cl.receive(ka[0]).valid, "a keep-alive reaches the client");
    // Ten packets we acknowledge without the client having seen that ack.
    CHECK(c.receive(cl.packet({cl.hello()}), t0 + milliseconds(210)).size() == 1, "the client's Hello arrives");
    for (int i = 1; i < 10; ++i) c.receive(cl.packet({}), t0 + milliseconds(210));
    CHECK(c.flush(t0 + milliseconds(249)).empty() && c.flush(t0 + milliseconds(250)).size() == 1,
          "the ack of 15879 goes out 50 ms after the keep-alive (and is lost)");
    // The client's sequence jumps 300 past what we last confirmed to it.
    for (int i = 0; i < 290; ++i) cl.notify().commit_and_increment_out_seq();
    CHECK(c.receive(cl.packet({}), t0 + milliseconds(260)).empty() && c.notify().is_waiting_for_history_flush(),
          "the packet is dropped: the window waits for a flush");
    CHECK(c.flush(t0 + milliseconds(299)).empty(), "no ack before 50 ms after the last send");
    const auto acks = c.flush(t0 + milliseconds(300));
    const auto r = acks.size() == 1 ? cl.receive(acks[0]) : fake::Received{};
    CHECK(r.valid && r.notify.acked_seq == 15869 + 256 - 10,
          "then a packet goes out that acks the advanced window (15869 + 256 - 10), so the client can flush it");
    const auto in = c.receive(cl.packet({cl.netspeed(1)}), t0 + milliseconds(310));
    CHECK(in.size() == 1 && !c.notify().is_waiting_for_history_flush(),
          "and once the client acks that, its next packet is accepted");
}
// (C-Y-B) (C O A)

// -- the packet walk and the partial reassembly (review B: W1, W3, M5) ----------------------------------

static std::vector<uint8_t> random_bits(size_t n, uint32_t seed) {
    std::vector<uint8_t> v;
    uint32_t x = seed * 2654435761u + 12345u;
    for (size_t i = 0; i < n; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        v.push_back(static_cast<uint8_t>(x & 1));
    }
    return v;
}

/// A reliable bunch on actor channel `ch` carrying `bits`, numbered from `seq` (the client's counter for that channel).
static fake::Bunch actor_part(int& seq, uint32_t ch, const std::vector<uint8_t>& bits, bool partial = false,
                              bool initial = false, bool final_ = false) {
    fake::Bunch b;
    b.header.reliable = true;
    b.header.channel = ch;
    b.header.ch_sequence = static_cast<uint16_t>(++seq % 1024);
    b.header.name_index = 102;
    b.header.partial = partial;
    b.header.partial_initial = initial;
    b.header.partial_final = final_;
    for (uint8_t bit : bits) b.payload.write_bit(bit);
    return b;
}

static std::vector<uint8_t> bits_of(const InBunch& b) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i < b.bits; ++i) out.push_back((b.data[i / 8] >> (i % 8)) & 1);
    return out;
}

/// A client packet whose bunches are followed by `tail`: bits of a header that is cut short or claims more data
/// than the packet has (the Client class only writes whole bunches).
static std::vector<uint8_t> packet_with_tail(fake::Client& cl, std::vector<fake::Bunch> bunches, const emit::BunchWriter& tail,
                                             size_t tail_bits) {
    emit::BunchWriter w;
    conn::begin_client_packet(w, kOuter, cl.notify().make_header(), kCf);
    for (auto& b : bunches) conn::write_bunch(w, b.header, b.payload.data(), b.payload.bit_pos());
    w.write_bit_range(tail.data(), 0, tail_bits);
    cl.notify().commit_and_increment_out_seq();
    return conn::finish_data_packet(w);
}

static void test_many_bunches_in_a_packet() {
    std::printf("70 bunches in one packet (wire::walk_bunches stopped at 64)\n");
    Connection c(config(), t0);
    auto cl = client();
    int seq = 15870 % 1024;
    std::vector<fake::Bunch> many;
    for (int i = 0; i < 70; ++i) many.push_back(actor_part(seq, 5, random_bits(1, i)));
    const auto pkt = cl.packet(std::move(many));
    const auto got = c.receive(pkt, t0);
    CHECK(got.size() == 70 && !c.failed(), "all 70 reliable bunches are delivered (UE has no limit of 64 per packet)");
    bool in_order = got.size() == 70;
    for (size_t i = 0; i < got.size(); ++i) in_order = in_order && bits_of(got[i]) == random_bits(1, static_cast<uint32_t>(i));
    CHECK(in_order, "in order, each with its own bit");
    std::vector<fake::Bunch> next;
    next.push_back(actor_part(seq, 5, random_bits(1, 70)));
    CHECK(c.receive(cl.packet(std::move(next)), t0 + milliseconds(10)).size() == 1,
          "and the next reliable bunch on the channel is delivered, not held as early for ever");
    // The most a packet of 1028 bytes holds is a few under 293 bunches of 28 bits (unreliable, no data): the walk does
    // not stop at any smaller number.
    Connection d(config(), t0);
    auto cd = client();
    std::vector<fake::Bunch> dense;
    for (int i = 0; i < 250; ++i) {
        fake::Bunch b;
        b.header.channel = 5;   // unreliable, 28 bits
        dense.push_back(std::move(b));
    }
    const auto dense_pkt = cd.packet(std::move(dense));
    CHECK(dense_pkt.size() <= conn::kMaxPacketBytes && d.receive(dense_pkt, t0).size() == 250,
          "a packet of 250 empty unreliable bunches delivers all 250");
}
// (CoA ~ C | Y | B)

static void test_corrupt_packet_fails() {
    std::printf("a packet whose walk stops early fails the connection (UE: bunch header overflowed)\n");
    int seq;
    {   // a header cut short: 12 zero bits where a header needs 28 (PacketReader answers a read past the end with 0 and does
        // not advance: before the strict walk this read as a bunch of no data)
        Connection c(config(), t0);
        auto cl = client();
        seq = 15870 % 1024;
        std::vector<fake::Bunch> v;
        v.push_back(actor_part(seq, 5, random_bits(40, 1)));
        emit::BunchWriter tail;
        for (int i = 0; i < 12; ++i) tail.write_bit(0);
        const auto got = c.receive(packet_with_tail(cl, std::move(v), tail, 12), t0);
        CHECK(got.size() == 1 && bits_of(got[0]) == random_bits(40, 1), "the bunch before the damage is delivered");
        CHECK(c.failed(), "and the connection fails: a header that does not read");
    }
    {   // a header that reads, with 100 bits of data of which the packet has 10
        Connection c(config(), t0);
        auto cl = client();
        seq = 15870 % 1024;
        std::vector<fake::Bunch> v;
        v.push_back(actor_part(seq, 5, random_bits(40, 2)));
        emit::BunchWriter cut;
        const fake::Bunch big = actor_part(seq, 5, random_bits(100, 3));
        conn::write_bunch(cut, big.header, big.payload.data(), big.payload.bit_pos());
        const size_t keep = cut.bit_pos() - 90;
        const auto got = c.receive(packet_with_tail(cl, std::move(v), cut, keep), t0);
        CHECK(got.size() == 1 && c.failed(), "a bunch whose data runs past the packet fails it as well, the bunch before it is delivered");
    }
    {   // the same two bunches, whole: no failure
        Connection c(config(), t0);
        auto cl = client();
        seq = 15870 % 1024;
        std::vector<fake::Bunch> v;
        v.push_back(actor_part(seq, 5, random_bits(40, 2)));
        v.push_back(actor_part(seq, 5, random_bits(100, 3)));
        CHECK(c.receive(cl.packet(std::move(v)), t0).size() == 2 && !c.failed(), "the same packet, whole: both delivered, no failure");
    }
    {   // a packet with nothing after the headers is fine
        Connection c(config(), t0);
        auto cl = client();
        c.receive(cl.packet({}), t0);
        CHECK(!c.failed(), "an empty packet is no failure");
    }
}

static void test_partial_reassembly() {
    std::printf("partial bunches: appended in place, bit for bit, and bounded in parts\n");
    {   // odd sizes, unaligned parts (the exports in front of a chain are): the bits come out as they went in
        Connection c(config(), t0);
        auto cl = client();
        int seq = 15870 % 1024;
        const size_t sizes[] = {873, 7, 7616, 0, 1, 15, 64, 3, 8, 5};
        std::vector<uint8_t> expect;
        Clock::time_point t = t0;
        std::vector<InBunch> got;
        for (size_t i = 0; i < std::size(sizes); ++i) {
            const auto bits = random_bits(sizes[i], static_cast<uint32_t>(40 + i));
            expect.insert(expect.end(), bits.begin(), bits.end());
            std::vector<fake::Bunch> v;
            v.push_back(actor_part(seq, 7, bits, true, i == 0, i + 1 == std::size(sizes)));
            t += milliseconds(1);
            for (auto& b : c.receive(cl.packet(std::move(v)), t)) got.push_back(std::move(b));
        }
        CHECK(got.size() == 1 && got[0].bits == expect.size() && bits_of(got[0]) == expect,
              "ten parts of 873, 7, 7616, 0, 1, 15, 64, 3, 8 and 5 bits give one bunch of their 8592 bits, in order");
    }
    {   // byte-aligned parts (the fast path) and a final part that is not
        Connection c(config(), t0);
        auto cl = client();
        int seq = 15870 % 1024;
        std::vector<uint8_t> expect;
        Clock::time_point t = t0;
        std::vector<InBunch> got;
        const size_t sizes[] = {7616, 7616, 7616, 13};
        for (size_t i = 0; i < std::size(sizes); ++i) {
            const auto bits = random_bits(sizes[i], static_cast<uint32_t>(60 + i));
            expect.insert(expect.end(), bits.begin(), bits.end());
            std::vector<fake::Bunch> v;
            v.push_back(actor_part(seq, 7, bits, true, i == 0, i + 1 == std::size(sizes)));
            t += milliseconds(1);
            for (auto& b : c.receive(cl.packet(std::move(v)), t)) got.push_back(std::move(b));
        }
        CHECK(got.size() == 1 && bits_of(got[0]) == expect, "three 7616-bit parts and a final of 13 bits: 22,861 bits, in order");
    }
    // The part cap (kMaxPartialParts = 256): a partial bunch has at most 256 parts (the sender's window holds no more); a peer
    // that sends empty parts for ever fails the connection at the next one instead of costing CPU for ever.
    auto partial_of = [&](size_t parts) {
        Connection c(config(), t0);
        auto cl = client();
        int seq = 15870 % 1024;
        Clock::time_point t = t0;
        std::vector<InBunch> got;
        size_t sent = 0;
        while (sent < parts && !c.failed()) {
            std::vector<fake::Bunch> v;
            for (int k = 0; k < 60 && sent < parts; ++k, ++sent) {
                const bool edge = sent == 0 || sent + 1 == parts;   // the first and last parts carry a byte, the rest nothing
                v.push_back(actor_part(seq, 7, edge ? random_bits(8, static_cast<uint32_t>(sent)) : std::vector<uint8_t>{}, true,
                                       sent == 0, sent + 1 == parts));
            }
            t += milliseconds(60);
            for (auto& b : c.receive(cl.packet(std::move(v)), t)) got.push_back(std::move(b));
            for (const auto& pkt : c.flush(t)) cl.receive(pkt);
        }
        return std::make_pair(std::move(got), c.failed());
    };
    const auto at_cap = partial_of(256);
    CHECK(!at_cap.second && at_cap.first.size() == 1 && at_cap.first[0].bits == 16,
          "a partial bunch of exactly 256 parts (one of 8 bits, 254 empty, one of 8) is reassembled");
    const auto over = partial_of(257);
    CHECK(over.second && over.first.empty(), "one part more fails the connection, and nothing is delivered");
    const auto flood = partial_of(600);
    CHECK(flood.second && flood.first.empty(), "600 empty parts fail it too (they cost a copy of the bunch so far each, before)");
}

int main() {
    test_acks_and_keepalive();
    test_reliable_out_and_resend();
    test_in_order_delivery();
    test_partial_and_close();
    test_timeouts_and_limits();
    test_duplicates_and_timeout_refresh();
    test_window_full();
    test_cadence();
    test_early_queue_bound();
    test_bounds_under_a_hostile_client();
    test_ack_while_resyncing();
    test_many_bunches_in_a_packet();
    test_corrupt_packet_fails();
    test_partial_reassembly();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
