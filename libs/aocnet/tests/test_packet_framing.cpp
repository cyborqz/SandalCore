// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_packet_framing.cpp
//
//  protocol/conn/packet_framing + nmt against the 2026-09-28 capture: the
//  server's NMT_Challenge, NMT_Welcome and keep-alive rebuilt byte for byte,
//  and the client's control messages read back.
// ============================================================================
#include "capture_packets.h"
#include "protocol/conn/handshake.h"
#include "protocol/conn/nmt.h"
#include "protocol/conn/packet_framing.h"
#include "protocol/wire/packet_parser.h"
#include "protocol/wire/ue5_primitives.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace aoc::protocol;
using namespace aoc::protocol::conn;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static OuterHeader g_outer;
static CustomField g_cf{};

static void setup() {
    const auto ch = capture::bytes(capture::kChallenge);
    const auto p = parse_handshake(ch.data(), ch.size());
    g_outer = outer_header_of(*p);
    g_cf = p->ext->custom_field();
}

// The legacy server acked with an all-ones history word.
static NotifyHeader legacy_notify(uint16_t seq, uint16_t acked) {
    NotifyHeader h;
    h.seq = seq;
    h.acked_seq = acked;
    h.history[0] = 0xFFFFFFFFu;
    return h;
}

static std::vector<uint8_t> server_nmt(uint16_t seq, uint16_t acked, uint16_t ch_seq,
                                       const emit::BunchWriter& payload) {
    emit::BunchWriter w;
    begin_server_packet(w, g_outer, legacy_notify(seq, acked), g_cf);
    BunchHeader b;
    b.reliable = true;
    b.ch_sequence = ch_seq;
    write_bunch(w, b, payload.data(), payload.bit_pos());
    return finish_data_packet(w);
}

static void test_server_packets() {
    std::printf("server packets\n");
    emit::BunchWriter challenge;
    write_nmt_challenge(challenge, "569D9F10");
    CHECK(server_nmt(14265, 15870, 954, challenge) == capture::bytes(capture::kServerNmtChallenge),
          "NMT_Challenge is byte-identical to the capture");

    emit::BunchWriter welcome;
    write_nmt_welcome(welcome, "/Game/Levels/Verra_World_Master/Verra_World_Master",
                      "/Game/GameBlueprints/AoCGameModeBaseBP.AoCGameModeBaseBP_C", "");
    CHECK(server_nmt(14266, 15871, 955, welcome) == capture::bytes(capture::kServerWelcome),
          "NMT_Welcome is byte-identical to the capture");

    emit::BunchWriter ka;
    begin_server_packet(ka, g_outer, legacy_notify(14267, 15873), g_cf);
    CHECK(finish_data_packet(ka) == capture::bytes(capture::kServerKeepAlive),
          "the keep-alive is byte-identical to the capture");
}
// (C Y B : C-O-A)

struct Read {
    std::vector<uint8_t> bytes;
    std::optional<DataPacket> packet;
    std::vector<wire::ParsedBunch> bunches;
};

static Read read(const char* hex) {
    Read r;
    r.bytes = capture::bytes(hex);
    r.packet = read_data_packet(r.bytes.data(), r.bytes.size(), wire::Direction::ClientToServer);
    if (r.packet) r.bunches = packet_bunches(r.bytes.data(), r.bytes.size(), *r.packet);
    return r;
}

static std::optional<ControlMessage> message(const Read& r, size_t i = 0) {
    const auto& b = r.bunches.at(i);
    return parse_control_message(r.bytes.data(), r.bytes.size(), b.data_start_bit, b.bunch_data_bits);
}

static void test_client_packets() {
    std::printf("client packets\n");
    const Read hello = read(capture::kClientHello);
    CHECK(hello.packet && hello.packet->outer.magic_header == g_outer.magic_header &&
          hello.packet->custom_field == g_cf,
          "Hello: same magic header and custom field as the handshake");
    CHECK(hello.packet && hello.packet->notify.seq == 15870 && hello.packet->info.has_pkt_info &&
          hello.packet->info.jitter_ms == 1023,
          "Hello: seq 15870, packet info with jitter 1023");
    CHECK(hello.bunches.size() == 1 && hello.bunches[0].is_control && hello.bunches[0].is_open &&
          hello.bunches[0].is_reliable && hello.bunches[0].channel == 0 && hello.bunches[0].ch_sequence == 511,
          "Hello opens channel 0, reliable, channel sequence 511");
    const auto h = hello.bunches.size() == 1 ? message(hello) : std::nullopt;
    CHECK(h && h->type == Nmt::Hello && h->little_endian == 1 && h->network_version == 0xa42f624eu &&
          h->encryption_token.empty(),
          "NMT_Hello: little endian, network version 0xa42f624e, no encryption token");

    const Read login = read(capture::kClientLogin);
    const auto l = login.bunches.size() == 1 ? message(login) : std::nullopt;
    CHECK(login.bunches.size() == 1 && login.bunches[0].ch_sequence == 512, "Login: channel sequence 512");
    CHECK(l && l->type == Nmt::Login && l->request_url.rfind("?Name=DESKTOP-EXAMPL2-", 0) == 0,
          "NMT_Login: the request URL carries the machine name");

    const Read ack = read(capture::kClientAckOnly);
    CHECK(ack.packet && !ack.packet->info.has_pkt_info && ack.bunches.empty(),
          "an ack-only packet: no packet info, no bunches");

    const Read speed = read(capture::kClientNetSpeed);
    const auto s = speed.bunches.size() == 1 ? message(speed) : std::nullopt;
    CHECK(s && s->type == Nmt::Netspeed && s->net_speed == 200000 && speed.bunches[0].ch_sequence == 513,
          "NMT_Netspeed 200000, channel sequence 513");

    const Read join = read(capture::kClientJoin);
    const auto j = join.bunches.size() == 1 ? message(join) : std::nullopt;
    CHECK(j && j->type == Nmt::Join && join.bunches[0].ch_sequence == 514, "NMT_Join, channel sequence 514");

    const Read quit = read(capture::kClientClose);
    CHECK(quit.bunches.size() == 1 && quit.bunches[0].channel == 0 && quit.bunches[0].is_control &&
          quit.bunches[0].is_close,
          "the client's quit closes channel 0");
}

static void test_server_packets_read_back() {
    std::printf("server packets read back\n");
    Read welcome;
    welcome.bytes = capture::bytes(capture::kServerWelcome);
    welcome.packet = read_data_packet(welcome.bytes.data(), welcome.bytes.size(), wire::Direction::ServerToClient);
    if (welcome.packet) welcome.bunches = packet_bunches(welcome.bytes.data(), welcome.bytes.size(), *welcome.packet);
    CHECK(welcome.packet && welcome.packet->notify.seq == 14266 && welcome.bunches.size() == 1 &&
          welcome.bunches[0].ch_sequence == 955,
          "the captured Welcome: seq 14266, one reliable bunch, channel sequence 955");
    const auto w = welcome.bunches.size() == 1 ? message(welcome) : std::nullopt;
    CHECK(w && w->type == Nmt::Welcome && w->level == "/Game/Levels/Verra_World_Master/Verra_World_Master" &&
          w->game_mode == "/Game/GameBlueprints/AoCGameModeBaseBP.AoCGameModeBaseBP_C" && w->redirect_url.empty(),
          "NMT_Welcome: level, game mode, no redirect");

    emit::BunchWriter failure;
    write_nmt_failure(failure, "no entry");
    const auto bytes = server_nmt(1, 2, 3, failure);
    const auto p = read_data_packet(bytes.data(), bytes.size(), wire::Direction::ServerToClient);
    const auto b = p ? packet_bunches(bytes.data(), bytes.size(), *p) : std::vector<wire::ParsedBunch>{};
    const auto f = b.size() == 1
        ? parse_control_message(bytes.data(), bytes.size(), b[0].data_start_bit, b[0].bunch_data_bits)
        : std::nullopt;
    CHECK(f && f->type == Nmt::Failure && f->error == "no entry", "NMT_Failure round-trips its text");
}
// [C | O | A :: C Y B]

static void test_round_trip() {
    std::printf("round trip\n");
    NotifyHeader n;
    n.seq = 100;
    n.acked_seq = 200;
    n.history[0] = 0x5;
    emit::BunchWriter payload;
    payload.write_uint8(static_cast<uint8_t>(Nmt::Netspeed));
    payload.write_uint32(12345);
    emit::BunchWriter w;
    begin_client_packet(w, g_outer, n, g_cf, 21);
    BunchHeader b;
    b.control = true;
    b.open = true;
    b.reliable = true;
    b.ch_sequence = 1030;  // written modulo 1024
    write_bunch(w, b, payload.data(), payload.bit_pos());
    const auto bytes = finish_data_packet(w);
    const auto p = read_data_packet(bytes.data(), bytes.size(), wire::Direction::ClientToServer);
    CHECK(p && p->notify.seq == 100 && p->notify.acked_seq == 200 && p->info.jitter_ms == 21,
          "headers read back");
    const auto bunches = p ? packet_bunches(bytes.data(), bytes.size(), *p) : std::vector<wire::ParsedBunch>{};
    CHECK(bunches.size() == 1 && bunches[0].ch_sequence == 6 && bunches[0].is_open,
          "the bunch reads back with its sequence modulo 1024");
    const auto m = bunches.size() == 1
        ? parse_control_message(bytes.data(), bytes.size(), bunches[0].data_start_bit, bunches[0].bunch_data_bits)
        : std::nullopt;
    CHECK(m && m->type == Nmt::Netspeed && m->net_speed == 12345, "the payload reads back");

    for (size_t bits : {0, 8, 31, 32, 200}) {
        emit::BunchWriter tmp;
        std::vector<uint8_t> zeros(32, 0);
        write_bunch(tmp, b, zeros.data(), bits);
        CHECK(tmp.bit_pos() == bunch_bits(b, bits), "bunch_bits matches what write_bunch writes");
    }
    // BunchDataBits is a SerializeInt, whose width changes at 8192.
    for (size_t bits : {8191, 8192, 8223}) {
        emit::BunchWriter tmp;
        std::vector<uint8_t> zeros(kMaxPacketBytes, 0);
        write_bunch(tmp, b, zeros.data(), bits);
        CHECK(tmp.bit_pos() == bunch_bits(b, bits), "bunch_bits matches what write_bunch writes for a full-size payload");
    }

    const auto init = capture::bytes(capture::kInitial);
    CHECK(!read_data_packet(init.data(), init.size(), wire::Direction::ClientToServer), "a handshake packet is not a data packet");
    CHECK(!read_data_packet(bytes.data(), 10, wire::Direction::ClientToServer), "a truncated packet is refused");
}

static std::vector<uint8_t> client_packet(const BunchHeader& b, const uint8_t* payload, size_t bits) {
    NotifyHeader n;
    n.seq = 7;
    n.acked_seq = 6;
    emit::BunchWriter w;
    begin_client_packet(w, g_outer, n, g_cf);
    write_bunch(w, b, payload, bits);
    return finish_data_packet(w);
}

static std::vector<wire::ParsedBunch> bunches_of(const std::vector<uint8_t>& bytes, wire::Direction dir) {
    const auto p = read_data_packet(bytes.data(), bytes.size(), dir);
    return p ? packet_bunches(bytes.data(), bytes.size(), *p) : std::vector<wire::ParsedBunch>{};
}

static void test_framing_edges() {
    std::printf("framing edges\n");
    BunchHeader close;
    close.control = true;
    close.close = true;
    close.reliable = true;
    close.close_reason = 7;
    close.ch_sequence = 515;
    auto bytes = client_packet(close, nullptr, 0);
    auto bs = bunches_of(bytes, wire::Direction::ClientToServer);
    CHECK(bs.size() == 1 && bs[0].is_close && bs[0].close_reason == 7 && bs[0].ch_sequence == 515 &&
          bs[0].bunch_data_bits == 0,
          "a close bunch with reason 7 reads back");
    emit::BunchWriter cw;
    write_bunch(cw, close, nullptr, 0);
    CHECK(cw.bit_pos() == bunch_bits(close, 0), "bunch_bits counts the close reason");

    const uint8_t payload[2] = {0xA5, 0x3C};
    BunchHeader unreliable;
    unreliable.channel = 3;
    bytes = client_packet(unreliable, payload, 16);
    bs = bunches_of(bytes, wire::Direction::ClientToServer);
    CHECK(bs.size() == 1 && !bs[0].is_reliable && !bs[0].is_control && bs[0].channel == 3 &&
          bs[0].bunch_data_bits == 16 &&
          ::ue5::read_bits_at(bytes.data(), bytes.size(), bs[0].data_start_bit, 16) == 0x3CA5u,
          "an unreliable bunch has no sequence and no name");
    BunchHeader open_unreliable;
    open_unreliable.control = true;
    open_unreliable.open = true;
    open_unreliable.channel = 3;
    bytes = client_packet(open_unreliable, payload, 16);
    bs = bunches_of(bytes, wire::Direction::ClientToServer);
    CHECK(bs.size() == 1 && bs[0].is_open && !bs[0].is_reliable && bs[0].channel_name == "EName[255]" &&
          bs[0].bunch_data_bits == 16,
          "an unreliable open bunch still carries the channel name");
// [CoA + C | Y | B]

    const auto hello = capture::bytes(capture::kClientHello);
    const size_t eff = ::ue5::strip_termination(hello.data(), hello.size());
    const auto hp = read_data_packet(hello.data(), hello.size(), wire::Direction::ClientToServer);
    CHECK(hp && hp->bunches_end == eff - 1, "bunches_end excludes the sentinel bit");
    auto zero_tail = hello;
    zero_tail.push_back(0);
    CHECK(!read_data_packet(zero_tail.data(), zero_tail.size(), wire::Direction::ClientToServer),
          "a data packet whose last byte is zero is refused");

    // A server packet with a frame-time byte before the bunches.
    NotifyHeader n;
    n.seq = 7;
    n.acked_seq = 6;
    emit::BunchWriter w;
    w.write_uint32(g_outer.magic_header);
    w.write_bits(g_outer.session_id, 2);
    w.write_bits(g_outer.client_id, 3);
    w.write_bit(0);
    write_notify_header(w, n);
    for (uint8_t c : g_cf) w.write_uint8(c);
    w.write_bit(1);
    w.write_serialize_int(1023, 1024);
    w.write_bit(1);
    w.write_uint8(0xAB);  // bHasServerFrameTime + FrameTimeByte
    write_bunch(w, unreliable, payload, 16);
    bytes = finish_data_packet(w);
    const auto sp = read_data_packet(bytes.data(), bytes.size(), wire::Direction::ServerToClient);
    bs = sp ? packet_bunches(bytes.data(), bytes.size(), *sp) : std::vector<wire::ParsedBunch>{};
    CHECK(sp && sp->info.has_srv_frame && sp->info.frame_time_byte == 0xAB && bs.size() == 1 && bs[0].channel == 3,
          "a server packet's frame-time byte is skipped before the bunches");
}

static void test_nmt_edges() {
    std::printf("nmt edges\n");
    emit::BunchWriter w;
    write_nmt_challenge(w, "569D9F10");
    const auto m = parse_control_message(w.data(), w.byte_size(), 0, w.bit_pos());
    CHECK(m && m->type == Nmt::Challenge && m->challenge == "569D9F10", "a written Challenge parses back");
    const uint8_t netspeed[4] = {4, 1, 2, 3};
    CHECK(!parse_control_message(netspeed, sizeof(netspeed), 0, 24), "a Netspeed cut short is refused");
    const uint8_t failure[6] = {6, 5, 0, 0, 0, 'a'};
    CHECK(!parse_control_message(failure, sizeof(failure), 0, 48), "a Failure whose text is cut short is refused");
    CHECK(std::string(nmt_name(Nmt::Join)) == "Join" && std::string(nmt_name(Nmt::Failure)) == "Failure" &&
          std::string(nmt_name(static_cast<Nmt>(200))) == "?",
          "nmt_name");
}

int main() {
    setup();
    test_server_packets();
    test_client_packets();
    test_server_packets_read_back();
    test_round_trip();
    test_framing_edges();
    test_nmt_edges();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
