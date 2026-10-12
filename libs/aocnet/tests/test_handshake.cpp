// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_handshake.cpp
//
//  protocol/conn/handshake against the four handshake packets of the
//  2026-09-28 capture: parse, byte-exact rebuild, the extension layout, and
//  the initial sequences the client takes from the cookie.
// ============================================================================
#include "capture_packets.h"
#include "protocol/conn/handshake.h"
#include "protocol/conn/packet_framing.h"
#include "protocol/wire/ue5_primitives.h"
#include <cstdio>
#include <cstring>

using namespace aoc::protocol::conn;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static std::optional<HandshakePacket> parse(const char* hex) {
    const auto b = capture::bytes(hex);
    return parse_handshake(b.data(), b.size());
}

static void test_parse() {
    std::printf("parse\n");
    const auto init = parse(capture::kInitial);
    CHECK(init && init->type == HandshakeType::Initial && init->sent_count == 1 && init->secret_id == 0,
          "Initial: type 0, first send, secret 0");
    CHECK(init && init->ext && init->ext->has_initial_marker(), "Initial carries the extension marker");
    CHECK(init && init->ext &&
          ::ue5::guid_to_hex(init->ext->character_id().data()) == "bbec5c86eb101fe5f38c87f0fa22e7d0",
          "Initial's character id is cyb's");
    CHECK(init && init->ext &&
          ::ue5::guid_to_hex(init->ext->ticket().data()) == "9ddc8eac805cc2fc3e75df52b74613c8",
          "Initial's ticket reads as 32 hex digits");
    CHECK(init && init->magic_header == 0x500c7696u, "magic header 0x500c7696");

    const auto ch = parse(capture::kChallenge);
    CHECK(ch && ch->type == HandshakeType::Challenge && ch->secret_id == 1, "Challenge: type 1, secret 1");
    CHECK(ch && ch->padding.size() == kChallengePaddingBytes, "Challenge has 15 padding bytes (153 in all)");
    const std::array<uint8_t, 6> cf = {0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d};
    CHECK(ch && ch->ext && ch->ext->custom_field() == cf, "the custom field is the extension's first 6 bytes");
    CHECK(ch && ch->ext && ch->ext->spawn()[0] == -777664.5 && ch->ext->spawn()[2] == 15952.0,
          "spawn location (-777664.5, 616578.7, 15952.0)");
    CHECK(ch && ch->ext && ch->ext->character_id() == init->ext->character_id() &&
          ch->ext->ticket() == init->ext->ticket(),
          "the Challenge echoes the character id and ticket");

    const auto resp = parse(capture::kResponse);
    CHECK(resp && resp->type == HandshakeType::Response && resp->timestamp == ch->timestamp &&
          resp->cookie == ch->cookie && resp->secret_id == ch->secret_id,
          "Response echoes timestamp, cookie and secret");
    CHECK(resp && resp->ext && resp->ext->bytes == ch->ext->bytes, "Response echoes the whole extension");

    const auto ack = parse(capture::kAck);
    CHECK(ack && ack->type == HandshakeType::Ack && ack->timestamp == -1.0 && ack->cookie == ch->cookie,
          "Ack: timestamp -1, the Challenge's cookie");
    CHECK(ack && ack->padding.size() == kAckPaddingBytes, "Ack has 9 padding bytes (147 in all)");
// ~ C.O.A - C Y B ~

    const auto seqs = initial_sequences(ch->cookie);
    CHECK(seqs.server == 14265 && seqs.client == 15870, "cookie b9 37 fe bd: server 14265, client 15870");
}

static void test_rebuild() {
    std::printf("rebuild\n");
    for (const char* hex : {capture::kInitial, capture::kChallenge, capture::kResponse, capture::kAck}) {
        const auto want = capture::bytes(hex);
        const auto p = parse_handshake(want.data(), want.size());
        CHECK(p && build_handshake(*p) == want, "build(parse(packet)) is byte-identical");
    }
    const auto init = parse(capture::kInitial);
    const auto ch = parse(capture::kChallenge);
    std::array<uint8_t, 38> server_bytes{};
    std::memcpy(server_bytes.data(), ch->ext->bytes.data(), 22);
    std::memcpy(server_bytes.data() + 22, ch->ext->bytes.data() + 38, 16);
    const auto ext = IntrepidExt::for_server(*init->ext, server_bytes, ch->ext->spawn());
    CHECK(ext.bytes == ch->ext->bytes, "for_server rebuilds the captured Challenge extension");
}

static void test_reject() {
    std::printf("reject\n");
    const auto hello = capture::bytes(capture::kClientHello);
    CHECK(!is_handshake(hello.data(), hello.size()), "a data packet is not a handshake");
    CHECK(!parse_handshake(hello.data(), hello.size()), "parse_handshake refuses a data packet");
    auto init = capture::bytes(capture::kInitial);
    CHECK(is_handshake(init.data(), init.size()), "the Initial is a handshake");
    CHECK(!parse_handshake(init.data(), 30), "a packet cut before the cookie ends is refused");
    auto zero_tail = init;
    zero_tail.back() = 0;
    CHECK(!parse_handshake(zero_tail.data(), zero_tail.size()), "a packet without a termination bit is refused");
    CHECK(!parse_handshake(nullptr, 0), "an empty packet is refused");
    CHECK(!parse_handshake(init.data(), 12), "a 12-byte prefix (non-zero last byte) is refused");
    const auto login = capture::bytes(capture::kClientLogin);
    CHECK(!parse_handshake(login.data(), login.size()), "a long data packet is refused by its handshake bit");

    // Datagrams never exceed kMaxPacketBytes; a longer one is refused before parsing.
    HandshakePacket big = *parse(capture::kInitial);
    big.padding.clear();
    big.padding.assign(kMaxPacketBytes - build_handshake(big).size(), 0xAA);
    const auto at_max = build_handshake(big);
    CHECK(at_max.size() == kMaxPacketBytes && parse_handshake(at_max.data(), at_max.size()),
          "a handshake packet of exactly 1028 bytes is accepted");
    big.padding.push_back(0xAA);
    const auto over = build_handshake(big);
    CHECK(over.size() == kMaxPacketBytes + 1 && !parse_handshake(over.data(), over.size()),
          "one byte more is refused");
}

static void test_edges() {
    std::printf("edges\n");
    const auto init = capture::bytes(capture::kInitial);
    std::vector<uint8_t> b343(init.begin(), init.begin() + 43);
    b343.back() = 0x80;  // 343 effective bits
    CHECK(!parse_handshake(b343.data(), b343.size()), "343 bits: one short of the cookie is refused");
    std::vector<uint8_t> b344(init.begin(), init.begin() + 43);
    b344.push_back(0x01);  // exactly 344 bits
    const auto q = parse_handshake(b344.data(), b344.size());
    CHECK(q && !q->ext && q->padding.empty(), "344 bits: cookie complete, no extension");

    auto p = *parse(capture::kInitial);
    p.padding.clear();
    const auto exact = build_handshake(p);
    const auto e = parse_handshake(exact.data(), exact.size());
    CHECK(exact.size() == 138 && e && e->ext && e->ext->bytes == p.ext->bytes && e->padding.empty(),
          "cookie + extension with no padding keeps the extension");
    HandshakePacket r = p;
    r.restart = true;
    const auto rb = build_handshake(r);
    const auto rp = parse_handshake(rb.data(), rb.size());
    CHECK(rp && rp->restart, "the restart bit round-trips");
}

int main() {
    test_parse();
    test_rebuild();
    test_reject();
    test_edges();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
