// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_packet_notify.cpp
//
//  protocol/conn/packet_notify: the notify header on the wire, and the
//  acknowledgement state machine ported from UE5 FNetPacketNotify, checked
//  against the headers the real client wrote in the 2026-09-28 capture.
// ============================================================================
#include "protocol/conn/packet_notify.h"
#include "protocol/wire/ue5_primitives.h"
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using namespace aoc::protocol;
using namespace aoc::protocol::conn;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static std::vector<uint8_t> hex(const std::string& s) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < s.size(); i += 2)
        out.push_back(static_cast<uint8_t>(std::stoi(s.substr(i, 2), nullptr, 16)));
    return out;
}

// The client's data packets #4 (Hello), #8 (ack only) and #9 (NetSpeed).
static const char* kClientPkt4 =
    "96760c5008e0defe3d000000004ec871c350d7ff4d00fcf75f000c00043889bd900200000000000c";
static const char* kClientPkt8 = "96760c5008e4de007e000000004ec871c3509701";
static const char* kClientPkt9 =
    "96760c5008e8de01fe000000004ec871c3505713100001fe17400104400d030003";
// C O A x Cyb

static NotifyHeader header_of(const char* pkt_hex) {
    const auto b = hex(pkt_hex);
    size_t off = 38;  // after the outer header
    return read_notify_header(b.data(), b.size(), ::ue5::strip_termination(b.data(), b.size()), off)
        .value_or(NotifyHeader{});
}

static void test_wire_format() {
    std::printf("wire format\n");
    const NotifyHeader h4 = header_of(kClientPkt4);
    CHECK(h4.seq == 15870 && h4.acked_seq == 14264 && h4.history_words == 1 && h4.history[0] == 0,
          "captured #4: seq 15870, acked 14264, history 0x0");
    const NotifyHeader h9 = header_of(kClientPkt9);
    CHECK(h9.seq == 15873 && h9.acked_seq == 14266 && h9.history[0] == 0x3,
          "captured #9: seq 15873, acked 14266, history 0x3");

    emit::BunchWriter w;
    w.write_bits(0, 38);
    write_notify_header(w, h9);
    const auto want = hex(kClientPkt9);
    bool same = true;
    for (size_t bit = 0; bit < 38 + 64; ++bit) {
        size_t a = bit, b = bit;
        if (bit < 38) continue;
        if (::ue5::read_bits(w.data(), w.byte_size(), a, 1) != ::ue5::read_bits(want.data(), want.size(), b, 1))
            same = false;
    }
    CHECK(same, "write_notify_header reproduces the captured bits");

    NotifyHeader big;
    big.seq = 1; big.acked_seq = 2; big.history_words = 3;
    big.history = {0x11111111u, 0x22222222u, 0x33333333u};
    emit::BunchWriter w2;
    write_notify_header(w2, big);
    size_t off = 0;
    const auto back = read_notify_header(w2.data(), w2.byte_size(), w2.bit_pos(), off);
    CHECK(back && back->history_words == 3 && back->history[2] == 0x33333333u && off == 32 + 96,
          "three history words round-trip");
    size_t short_off = 0;
    CHECK(!read_notify_header(w2.data(), w2.byte_size(), 64, short_off),
          "a header cut short by the packet end is rejected");
}

// The client's side of the capture: its headers must come out as it wrote them.
static void test_capture_timeline_client_side() {
    std::printf("capture timeline (client side)\n");
    PacketNotify n;
    n.init(14264, 15870);  // server's initial seq 14265 - 1; client's own 15870
    NotifyHeader h = n.make_header();
    CHECK(h.seq == 15870 && h.acked_seq == 14264 && h.history[0] == 0 && h.history_words == 1,
          "#4 Hello: seq 15870, acked 14264, history 0x0");
    n.commit_and_increment_out_seq();
    h = n.make_header();
    CHECK(h.seq == 15871 && h.acked_seq == 14264, "#6 Login: seq 15871, still acked 14264");
    n.commit_and_increment_out_seq();

    // #5 NMT_Challenge arrives: seq 14265, acking our 15870 (legacy history all ones).
    NotifyHeader in5; in5.seq = 14265; in5.acked_seq = 15870; in5.history[0] = 0xFFFFFFFFu;
    std::vector<std::pair<uint16_t, bool>> acks;
    const int d = n.update(in5, [&](uint16_t s, bool ok) { acks.push_back({s, ok}); });
    CHECK(d == 1, "#5 is one packet ahead");
    CHECK(acks.size() == 1 && acks[0].first == 15870 && acks[0].second, "#5 acknowledges #4");
    n.ack_seq(14265);
    h = n.make_header();
    CHECK(h.seq == 15872 && h.acked_seq == 14265 && h.history[0] == 0x1 && h.history_words == 1,
          "#8: seq 15872, acked 14265, history 0x1 (as captured)");
    CHECK(h.history[0] == header_of(kClientPkt8).history[0], "#8 history equals the captured word");
    n.commit_and_increment_out_seq();

    NotifyHeader in7; in7.seq = 14266; in7.acked_seq = 15871; in7.history[0] = 0xFFFFFFFFu;
    n.update(in7, [](uint16_t, bool) {});
    n.ack_seq(14266);
    h = n.make_header();
    CHECK(h.seq == 15873 && h.acked_seq == 14266 && h.history[0] == 0x3,
          "#9: seq 15873, acked 14266, history 0x3 (as captured)");
}

static void test_server_first_header() {
    std::printf("server side\n");
    PacketNotify n;
    n.init(15869, 14265);  // client's initial 15870 - 1; ours 14265
    const NotifyHeader c4 = header_of(kClientPkt4);
    int reports = 0;
    const int d = n.update(c4, [&](uint16_t, bool) { ++reports; });
    CHECK(d == 1 && reports == 0, "the client's first packet is accepted and acks nothing yet");
    n.ack_seq(c4.seq);
    const NotifyHeader h = n.make_header();
    CHECK(h.seq == 14265 && h.acked_seq == 15870 && h.history[0] == 0x1,
          "our first header: seq 14265, acked 15870, history 0x1");
}

static void test_loss_and_gaps() {
    std::printf("loss, gaps, duplicates\n");
    PacketNotify n;
    n.init(99, 1000);
    for (int i = 0; i < 3; ++i) n.commit_and_increment_out_seq();  // sent 1000, 1001, 1002
    NotifyHeader in; in.seq = 100; in.acked_seq = 1002; in.history[0] = 0x5;  // 1001 lost
    std::vector<std::pair<uint16_t, bool>> acks;
    CHECK(n.update(in, [&](uint16_t s, bool ok) { acks.push_back({s, ok}); }) == 1, "accepted");
    CHECK(acks.size() == 3 && acks[0] == std::make_pair(uint16_t(1000), true) &&
          acks[1] == std::make_pair(uint16_t(1001), false) &&
          acks[2] == std::make_pair(uint16_t(1002), true),
          "1000 delivered, 1001 lost, 1002 delivered, oldest first");
    n.ack_seq(100);

    CHECK(n.update(in, [](uint16_t, bool) {}) == 0, "the same packet again is dropped");

    NotifyHeader gap; gap.seq = 103; gap.acked_seq = 1002;
    CHECK(n.update(gap, [](uint16_t, bool) {}) == 3, "a packet three ahead is accepted");
    n.ack_seq(103);
    CHECK((n.make_header().history[0] & 0xF) == 0x9, "history 1001b: 103 and 100 received, 101 and 102 lost");
// [C O A - Cyb]

    NotifyHeader stale; stale.seq = 102; stale.acked_seq = 1002;
    CHECK(n.update(stale, [](uint16_t, bool) {}) == 0, "an older packet arriving late is dropped");

    NotifyHeader bogus; bogus.seq = 104; bogus.acked_seq = 1003;  // we never sent 1003
    CHECK(n.update(bogus, [](uint16_t, bool) {}) == 0, "an ack of a packet we never sent is dropped");
}

static void test_wrap_and_history() {
    std::printf("wrap-around and history size\n");
    PacketNotify n;
    n.init(16382, 16383);
    n.commit_and_increment_out_seq();
    CHECK(n.out_seq() == 0, "the outgoing sequence wraps from 16383 to 0");
    NotifyHeader in; in.seq = 16383; in.acked_seq = 16383; in.history[0] = 1;
    CHECK(n.update(in, [](uint16_t, bool) {}) == 1, "16383 after 16382 is accepted");
    n.ack_seq(16383);
    NotifyHeader in2; in2.seq = 0; in2.acked_seq = 16383;
    CHECK(n.update(in2, [](uint16_t, bool) {}) == 1, "0 after 16383 is one ahead");
    n.ack_seq(0);

    PacketNotify m;
    m.init(0, 500);
    for (uint16_t s = 1; s <= 40; ++s) {
        NotifyHeader p; p.seq = s; p.acked_seq = 499;
        m.update(p, [](uint16_t, bool) {});
        m.ack_seq(s);
    }
    CHECK(m.make_header().history_words == 2, "40 unreported packets need two history words");
    m.commit_and_increment_out_seq();                  // our 500 carries in_ack_seq 40
    NotifyHeader ack; ack.seq = 41; ack.acked_seq = 500; ack.history[0] = 1;
    m.update(ack, [](uint16_t, bool) {});
    m.ack_seq(41);
    CHECK(m.make_header().history_words == 1, "once the peer saw our ack of 40, one word is enough");

    PacketNotify w;
    w.init(0, 0);
    for (int i = 0; i < 255; ++i) w.commit_and_increment_out_seq();
    CHECK(!w.is_window_full(), "255 unacknowledged packets leave room");
    w.commit_and_increment_out_seq();
    CHECK(w.is_window_full(), "the 256th unacknowledged packet fills the window");

    PacketNotify old;
    old.init(0, 0);
    for (int i = 0; i < 40; ++i) old.commit_and_increment_out_seq();
    NotifyHeader one_word; one_word.seq = 1; one_word.acked_seq = 39; one_word.history[0] = 0xFFFFFFFFu;
    int lost = 0, delivered = 0;
    old.update(one_word, [&](uint16_t, bool ok) { (ok ? delivered : lost)++; });
    CHECK(lost == 8 && delivered == 32, "packets older than a one-word history count as lost");
}

// UE5 FNetPacketNotify::InternalUpdate: the receive window after the peer's
// sequence jumps more than 256 packets past the newest ack it has confirmed.
static void test_window_recovery() {
    std::printf("receive window after a jump of more than 256 packets\n");
    // Nothing unconfirmed in our history: the peer's sequence jumps 300 past
    // the last packet we acked (worldd did not read the socket for a while).
    PacketNotify n;
    n.init(0, 1000);
    n.make_header();
    uint16_t sent = n.commit_and_increment_out_seq();
    NotifyHeader h; h.seq = 300; h.acked_seq = sent; h.history[0] = 1;
    CHECK(n.update(h, [](uint16_t, bool) {}) == 300 && !n.is_waiting_for_history_flush(),
          "with no unconfirmed acks the window is re-based and the packet accepted");
    n.ack_seq(300);
    CHECK(n.make_header().acked_seq == 300 && n.make_header().history_words == 1,
          "the next header acks it with one history word");

    // Acks the peer has not confirmed seeing: the window is advanced with
    // losses (flush-wait) and the connection recovers once the peer acks the
    // header that carried them.
    PacketNotify m;
    m.init(0, 1000);
    m.make_header();
    sent = m.commit_and_increment_out_seq();                      // 1000 carries in_ack_seq 0
    for (uint16_t s = 1; s <= 10; ++s) {
        NotifyHeader p; p.seq = s; p.acked_seq = 999;
        m.update(p, [](uint16_t, bool) {});
        m.ack_seq(s);
    }
    NotifyHeader j; j.seq = 300; j.acked_seq = sent; j.history[0] = 1;   // acks 1000, jumps to 300
    CHECK(m.update(j, [](uint16_t, bool) {}) == 0 && m.is_waiting_for_history_flush() && m.in_seq() == 246,
          "with acks pending the packet is dropped and the window advances to 246 as lost");
    NotifyHeader out = m.make_header();
    CHECK(out.acked_seq == 246 && out.history_words == 8 && out.history[0] == 0 && out.history[7] == 0x003FF000u,
          "the next header carries 246 with the full history: 236 lost, then the 10 received (bits 236..245)");
    sent = m.commit_and_increment_out_seq();                      // 1001 carries in_ack_seq 246
    NotifyHeader k; k.seq = 301; k.acked_seq = sent; k.history[0] = 1;
    CHECK(m.update(k, [](uint16_t, bool) {}) == 55 && !m.is_waiting_for_history_flush() && m.in_seq() == 301,
          "once the peer acks that header, 301 is accepted (delta 55)");
    m.ack_seq(301);
    CHECK(m.make_header().history_words == 2, "and the history shrinks to the 55 packets since 246");
}

static void test_sequence_arithmetic() {
    std::printf("sequence arithmetic\n");
    CHECK(seq_diff(0, 1) == -1 && seq_diff(1, 0) == 1 && seq_diff(0, 16383) == 1 && seq_diff(16383, 0) == -1 &&
          seq_diff(8191, 0) == 8191 && seq_diff(8192, 0) == -8192,
          "seq_diff is signed and wraps at 16384");
    CHECK(seq_greater(8191, 0) && !seq_greater(8192, 0) && seq_greater(0, 8193),
          "seq_greater flips at half the space");
    CHECK(make_relative(5, 1020, 1024) == 1029 && make_relative(1020, 5, 1024) == -4 &&
          best_signed_difference(5, 1020, 1024) == 9,
          "make_relative expands a 10-bit channel sequence toward the reference");
}

static void test_header_bounds() {
    std::printf("notify header bounds\n");
    emit::BunchWriter w;
    NotifyHeader h; h.seq = 5; h.acked_seq = 4; h.history_words = 3;
    write_notify_header(w, h);
    size_t off = 0;
    CHECK(!read_notify_header(w.data(), w.byte_size(), 20, off),
          "a packet shorter than the packed header is rejected");
    // The receive path's only memory-safety check: history has 8 words.
    emit::BunchWriter w16;
    w16.write_uint32((5u << 18) | (4u << 4) | 15u);   // history nibble 15: 16 words
    for (int i = 0; i < 16; ++i) w16.write_uint32(0xFFFFFFFFu);
    off = 0;
    CHECK(!read_notify_header(w16.data(), w16.byte_size(), w16.bit_pos(), off),
          "a header claiming 16 history words is rejected");

    // write_notify_header clamps a stray word count to 1..8.
    NotifyHeader none; none.seq = 5; none.acked_seq = 4; none.history_words = 0;
    emit::BunchWriter wz;
    write_notify_header(wz, none);
    off = 0;
    auto rz = read_notify_header(wz.data(), wz.byte_size(), wz.bit_pos(), off);
    CHECK(wz.bit_pos() == 64 && rz && rz->history_words == 1, "a header with no history words is written with one");
    NotifyHeader many; many.seq = 5; many.acked_seq = 4; many.history_words = 200;
    emit::BunchWriter wm;
    write_notify_header(wm, many);
    off = 0;
    auto rm = read_notify_header(wm.data(), wm.byte_size(), wm.bit_pos(), off);
    CHECK(wm.bit_pos() == 32 + 8 * 32 && rm && rm->history_words == 8 && rm->seq == 5 && rm->acked_seq == 4,
          "a header with 200 history words is written with eight");
}
// [C.Y.B + CoA]

static void test_history_words() {
    std::printf("history across words\n");
    PacketNotify m;
    m.init(0, 500);
    bool each_ahead = true;
    for (uint16_t s = 1; s <= 40; ++s) {
        NotifyHeader p; p.seq = s; p.acked_seq = 499;
        each_ahead = each_ahead && m.update(p, [](uint16_t, bool) {}) == 1;
        if (s != 3 && s != 35) m.ack_seq(s);   // 3 and 35 are never acked: lost
    }
    CHECK(each_ahead, "each packet is one ahead");
    const NotifyHeader h = m.make_header();
    CHECK(h.history_words == 2 && h.history[0] == 0xFFFFFFDFu && h.history[1] == 0xDFu,
          "40 packets: word 0 has 35 missing (bit 5), word 1 has 3 missing (bit 37)");
    PacketNotify k;
    k.init(0, 500);
    for (uint16_t s = 1; s <= 32; ++s) {
        NotifyHeader p; p.seq = s; p.acked_seq = 499;
        k.update(p, [](uint16_t, bool) {});
        k.ack_seq(s);
    }
    CHECK(k.make_header().history_words == 1, "32 unreported packets fit one word");
    NotifyHeader p33; p33.seq = 33; p33.acked_seq = 499;
    k.update(p33, [](uint16_t, bool) {});
    k.ack_seq(33);
    CHECK(k.make_header().history_words == 2, "the 33rd needs a second word");
}

static void test_peer_history_upper_bits() {
    std::printf("peer history, upper bits of a word\n");
    PacketNotify n;
    n.init(0, 1000);
    for (int i = 0; i < 32; ++i) n.commit_and_increment_out_seq();   // 1000..1031
    NotifyHeader in; in.seq = 1; in.acked_seq = 1031; in.history[0] = 0x80000001u;
    std::vector<std::pair<uint16_t, bool>> acks;
    CHECK(n.update(in, [&](uint16_t s, bool ok) { acks.push_back({s, ok}); }) == 1, "accepted");
    CHECK(acks.size() == 32 && acks[0] == std::make_pair(uint16_t(1000), true) &&
          acks[1] == std::make_pair(uint16_t(1001), false) && acks[30] == std::make_pair(uint16_t(1030), false) &&
          acks[31] == std::make_pair(uint16_t(1031), true),
          "bit 31 is the oldest packet of the word, bit 0 the newest");
    CHECK(n.out_ack_seq() == 1031, "out_ack_seq follows the header");
}

static void test_rejections_have_no_side_effects() {
    std::printf("rejected packets\n");
    PacketNotify n;
    n.init(99, 1000);
    for (int i = 0; i < 3; ++i) n.commit_and_increment_out_seq();   // 1000..1002
    NotifyHeader in; in.seq = 100; in.acked_seq = 1002; in.history[0] = 0x7;
    CHECK(n.update(in, [](uint16_t, bool) {}) == 1, "accepted");
    n.ack_seq(100);
    NotifyHeader older_ack; older_ack.seq = 101; older_ack.acked_seq = 1000;
    CHECK(n.update(older_ack, [](uint16_t, bool) {}) == 0,
          "a newer packet whose ack is older than what the peer already reported is dropped");
    NotifyHeader bogus; bogus.seq = 101; bogus.acked_seq = 1003;   // not sent
    CHECK(n.update(bogus, [](uint16_t, bool) {}) == 0 && n.out_ack_seq() == 1002 && n.in_seq() == 100,
          "an ack of an unsent packet changes nothing");
    NotifyHeader good; good.seq = 101; good.acked_seq = 1002;
    CHECK(n.update(good, [](uint16_t, bool) {}) == 1, "and the next valid packet is still accepted");
}

static void test_header_uses_acked_not_received() {
    std::printf("acked vs received\n");
    PacketNotify n;
    n.init(99, 1000);
    NotifyHeader in; in.seq = 100; in.acked_seq = 999;
    CHECK(n.update(in, [](uint16_t, bool) {}) == 1, "accepted but not yet acked");
    CHECK(n.make_header().acked_seq == 99, "the header acks only what ack_seq recorded");
    CHECK(n.commit_and_increment_out_seq() == 1000 && n.out_seq() == 1001,
          "commit returns the sequence just sent");

    // The record holds in_ack_seq at commit time, not in_seq: an ack_seq()
    // between make_header() and commit would break this (see packet_notify.h).
    PacketNotify c;
    c.init(0, 1000);
    for (uint16_t s = 1; s <= 31; ++s) {
        NotifyHeader p; p.seq = s; p.acked_seq = 999;
        c.update(p, [](uint16_t, bool) {});
        c.ack_seq(s);
    }
    NotifyHeader p32; p32.seq = 32; p32.acked_seq = 999;
    c.update(p32, [](uint16_t, bool) {});               // received, not acked
    c.commit_and_increment_out_seq();                   // 1000 carries in_ack_seq 31
    c.ack_seq(32);
    for (uint16_t s = 33; s <= 63; ++s) {
        NotifyHeader p; p.seq = s; p.acked_seq = 999;
        c.update(p, [](uint16_t, bool) {});
        c.ack_seq(s);
    }
    NotifyHeader p64; p64.seq = 64; p64.acked_seq = 1000; p64.history[0] = 1;
    c.update(p64, [](uint16_t, bool) {});
    c.ack_seq(64);
    CHECK(c.make_header().history_words == 2, "the peer saw our ack of 31, so 33 packets need two words");
}

int main() {
    test_wire_format();
    test_capture_timeline_client_side();
    test_server_first_header();
    test_loss_and_gaps();
    test_wrap_and_history();
    test_sequence_arithmetic();
    test_header_bounds();
    test_history_words();
    test_peer_history_upper_bits();
    test_rejections_have_no_side_effects();
    test_header_uses_acked_not_received();
    test_window_recovery();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
